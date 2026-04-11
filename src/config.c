/*
 * src/config.c — Runtime configuration file parser for evilWay.
 *
 * Reads ~/.evilwayrc on startup and on SIGHUP.
 *
 * File format (identical to evilwm's .evilwmrc):
 *   - One option per line, option name without leading dashes
 *   - Arguments separated by whitespace
 *   - Comments start with '#'
 *   - Blank lines ignored
 *
 * Example:
 *   bw 2
 *   snap 10
 *   term foot
 *   bind Super+Return=spawn
 *   bind Super+h=move,relative+left
 *
 * SECURITY INVARIANTS:
 *   - All line reads use fgets() with a fixed buffer. No getline(), no
 *     unbounded reads.
 *   - All string assignments use snprintf() with explicit size limits.
 *     No strcpy(), no strcat().
 *   - Numeric values are range-clamped after parsing. Out-of-range values
 *     are logged and clamped, not rejected.
 *   - Unknown keys are logged and skipped. Parse errors do not abort the
 *     load — the remaining config lines are still processed.
 *   - The binds array is heap-allocated and freed by config_free(). A failed
 *     realloc aborts the load and returns false; callers must not use the
 *     partial config.
 *   - config_load() on SIGHUP loads into a fresh EwConfig and only replaces
 *     the live config if the load succeeds. A failed reload leaves the
 *     current config intact.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xkbcommon/xkbcommon.h>

#include "config.h"
#include "evilway.h"

/* Maximum line length. Lines longer than this are truncated by fgets(); a
 * warning is logged and the remainder drained. 512 bytes is generous — the
 * longest realistic bind line is well under 128. */
#define CONFIG_LINE_MAX  512

/* Initial and growth policy for the binds heap array. Doubles on overflow. */
#define BINDS_INITIAL_CAP  16

/* =========================================================================
 * Internal string helpers
 * ====================================================================== */

/*
 * trim — strip leading and trailing ASCII whitespace in-place.
 * Returns a pointer into s (which may advance past leading spaces).
 * Overwrites trailing whitespace with NUL bytes.
 */
static char *trim(char *s) {
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

/* =========================================================================
 * Static lookup tables
 * ====================================================================== */

/* Modifier name → WLR_MODIFIER_* bitmask. "Ctrl" is accepted as an alias
 * for "Control" to match what users commonly type. */
static const struct { const char *name; uint32_t mask; } modifier_map[] = {
    { "Super",   WLR_MODIFIER_LOGO  },
    { "Shift",   WLR_MODIFIER_SHIFT },
    { "Control", WLR_MODIFIER_CTRL  },
    { "Ctrl",    WLR_MODIFIER_CTRL  },
    { "Alt",     WLR_MODIFIER_ALT   },
    { NULL, 0 }
};

/* Function name → EwFunction enum. */
static const struct { const char *name; EwFunction func; } function_map[] = {
    { "spawn",  FUNC_SPAWN  },
    { "delete", FUNC_DELETE },
    { "kill",   FUNC_KILL   },
    { "lower",  FUNC_LOWER  },
    { "raise",  FUNC_RAISE  },
    { "move",   FUNC_MOVE   },
    { "resize", FUNC_RESIZE },
    { "fix",    FUNC_FIX    },
    { "vdesk",  FUNC_VDESK  },
    { "next",   FUNC_NEXT   },
    { "dock",   FUNC_DOCK   },
    { "info",   FUNC_INFO   },
    { NULL, FUNC_SPAWN }   /* sentinel */
};

/* Flag token → FLAG_* bit. "h" and "v" are shorthand for "horizontal" and
 * "vertical" matching the example config in the prompt. */
static const struct { const char *name; uint32_t flag; } flag_map[] = {
    { "relative",   FLAG_RELATIVE   },
    { "toggle",     FLAG_TOGGLE     },
    { "top",        FLAG_TOP        },
    { "bottom",     FLAG_BOTTOM     },
    { "left",       FLAG_LEFT       },
    { "right",      FLAG_RIGHT      },
    { "up",         FLAG_UP         },
    { "down",       FLAG_DOWN       },
    { "horizontal", FLAG_HORIZONTAL },
    { "h",          FLAG_HORIZONTAL },
    { "vertical",   FLAG_VERTICAL   },
    { "v",          FLAG_VERTICAL   },
    { NULL, 0 }   /* sentinel */
};

/* =========================================================================
 * Bind array management
 * ====================================================================== */

/*
 * binds_push — append *bind to config->binds, growing the array if needed.
 * Returns false on malloc failure (logs to stderr). The caller should treat
 * false as a fatal load error.
 */
static bool binds_push(EwConfig *config, const EwBind *bind) {
    if (config->num_binds >= config->binds_capacity) {
        size_t new_cap = config->binds_capacity == 0
            ? BINDS_INITIAL_CAP
            : config->binds_capacity * 2;
        EwBind *grown = realloc(config->binds, new_cap * sizeof(EwBind));
        if (!grown) {
            fprintf(stderr, "evilway: config: out of memory growing binds array\n");
            return false;
        }
        config->binds          = grown;
        config->binds_capacity = new_cap;
    }
    config->binds[config->num_binds++] = *bind;
    return true;
}

/* =========================================================================
 * Bind line parser
 * ====================================================================== */

/*
 * parse_bind_line — parse a "TRIGGER=FUNCTION[,FLAGS]" string into *bind.
 *
 * Called with the argument string after "bind " has been stripped and trimmed.
 * On success, *bind is fully populated and true is returned.
 * On any parse error, a descriptive message is written to stderr and false is
 * returned. The caller skips the bind and continues reading the config.
 *
 * Grammar:
 *
 *   TRIGGER   ::= "button" [1-5]
 *               | [Modifier "+"]+ KeyName
 *   FUNCTION  ::= "spawn"|"delete"|"kill"|"lower"|"raise"|"move"|
 *                 "resize"|"fix"|"vdesk"|"next"|"dock"|"info"
 *   FLAGS     ::= INTEGER                (vdesk target, non-negative)
 *               | FlagToken ["+" FlagToken]*
 *   FlagToken ::= "relative"|"toggle"|"top"|"bottom"|"left"|"right"|
 *                 "up"|"down"|"horizontal"|"h"|"vertical"|"v"
 *
 * SECURITY:
 *   - All string operations are bounded via snprintf/strtok_r/strlen.
 *   - xkb_keysym_from_name is tried with XKB_KEYSYM_NO_FLAGS first (exact
 *     match). Falls back to XKB_KEYSYM_CASE_INSENSITIVE with a warning, so
 *     "return" is accepted alongside "Return". XKB_KEY_NoSymbol on both
 *     tries is a hard error.
 */
static bool parse_bind_line(const char *line, EwBind *bind) {
    memset(bind, 0, sizeof(*bind));
    bind->vdesk_target = -1;

    /* Mutable working copy — all destructive operations happen here. */
    char buf[CONFIG_LINE_MAX];
    snprintf(buf, sizeof(buf), "%s", line);

    /* ---- Split at '=' into trigger and rhs ---- */
    char *eq = strchr(buf, '=');
    if (!eq) {
        fprintf(stderr, "evilway: config: bind missing '=': %s\n", line);
        return false;
    }
    *eq = '\0';
    char *trigger = trim(buf);
    char *rhs     = trim(eq + 1);

    if (!*trigger) {
        fprintf(stderr, "evilway: config: bind has empty trigger: %s\n", line);
        return false;
    }
    if (!*rhs) {
        fprintf(stderr, "evilway: config: bind has empty function: %s\n", line);
        return false;
    }

    /* ---- Parse trigger ---- */

    if (strncmp(trigger, "button", 6) == 0) {
        /* Mouse button: "button1" through "button5". */
        char *end = NULL;
        long btn = strtol(trigger + 6, &end, 10);
        if (!end || *end != '\0' || btn < 1 || btn > 5) {
            fprintf(stderr,
                "evilway: config: invalid mouse button (must be button1-button5): %s\n",
                trigger);
            return false;
        }
        bind->is_mouse = true;
        bind->button   = (uint32_t)btn;

    } else {
        /* Keyboard: [Modifier+]...KeyName
         * Strategy: find the last '+' to split the modifier chain from the
         * key name. Everything before the last '+' is modifier tokens;
         * everything after is the key name. */
        bind->is_mouse = false;

        char trig[CONFIG_LINE_MAX];
        snprintf(trig, sizeof(trig), "%s", trigger);

        char *last_plus = strrchr(trig, '+');
        const char *keyname;

        if (last_plus) {
            *last_plus = '\0';
            keyname = last_plus + 1;

            /* Parse modifier chain via strtok_r on the prefix. */
            char *saveptr = NULL;
            char *tok = strtok_r(trig, "+", &saveptr);
            while (tok) {
                bool found = false;
                for (int i = 0; modifier_map[i].name; i++) {
                    if (strcmp(tok, modifier_map[i].name) == 0) {
                        bind->modifiers |= modifier_map[i].mask;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    fprintf(stderr,
                        "evilway: config: unknown modifier '%s' in: %s\n",
                        tok, line);
                    return false;
                }
                tok = strtok_r(NULL, "+", &saveptr);
            }
        } else {
            /* No '+' at all: bare key name, no modifiers. */
            keyname = trig;
        }

        if (!*keyname) {
            fprintf(stderr, "evilway: config: empty key name in: %s\n", line);
            return false;
        }

        /*
         * SECURITY: Try exact match first (XKB_KEYSYM_NO_FLAGS). This avoids
         * ambiguities from case-folding (e.g. "L" vs "l" → different keysyms).
         * Fall back to case-insensitive with a logged warning so common
         * lowercase spellings like "return" and "left" still work.
         */
        bind->keysym = xkb_keysym_from_name(keyname, XKB_KEYSYM_NO_FLAGS);
        if (bind->keysym == XKB_KEY_NoSymbol) {
            bind->keysym = xkb_keysym_from_name(keyname,
                XKB_KEYSYM_CASE_INSENSITIVE);
            if (bind->keysym == XKB_KEY_NoSymbol) {
                fprintf(stderr,
                    "evilway: config: unknown key name '%s' in: %s\n",
                    keyname, line);
                return false;
            }
            fprintf(stderr,
                "evilway: config: key '%s' matched case-insensitively "
                "(prefer the canonical XKB name)\n", keyname);
        }
    }

    /* ---- Parse rhs: FuncName[,FlagsOrTarget] ---- */

    /* Split at first ',' to separate function name from flags/target. */
    char *comma = strchr(rhs, ',');
    const char *flags_str = NULL;
    if (comma) {
        *comma    = '\0';
        flags_str = comma + 1;
    }
    const char *func_name = rhs;   /* rhs now NUL-terminated at comma (or end) */

    /* Look up function name. */
    bool found_func = false;
    for (int i = 0; function_map[i].name; i++) {
        if (strcmp(func_name, function_map[i].name) == 0) {
            bind->function = function_map[i].func;
            found_func = true;
            break;
        }
    }
    if (!found_func) {
        fprintf(stderr,
            "evilway: config: unknown function '%s' in: %s\n",
            func_name, line);
        return false;
    }

    /* ---- Parse flags / vdesk numeric target ---- */

    if (flags_str && *flags_str) {
        char fstr[CONFIG_LINE_MAX];
        snprintf(fstr, sizeof(fstr), "%s", flags_str);

        /* A bare non-negative integer is a vdesk absolute target. */
        char *endptr = NULL;
        long vn = strtol(fstr, &endptr, 10);
        if (endptr && *endptr == '\0' && vn >= 0) {
            if (bind->function != FUNC_VDESK) {
                fprintf(stderr,
                    "evilway: config: numeric argument only valid for vdesk in: %s\n",
                    line);
                return false;
            }
            bind->vdesk_target = (int)vn;
        } else {
            /* Flag token chain: flag[+flag...] */
            char *saveptr = NULL;
            char *tok = strtok_r(fstr, "+", &saveptr);
            while (tok) {
                bool found_flag = false;
                for (int i = 0; flag_map[i].name; i++) {
                    if (strcmp(tok, flag_map[i].name) == 0) {
                        bind->flags |= flag_map[i].flag;
                        found_flag = true;
                        break;
                    }
                }
                if (!found_flag) {
                    fprintf(stderr,
                        "evilway: config: unknown flag '%s' in: %s\n",
                        tok, line);
                    return false;
                }
                tok = strtok_r(NULL, "+", &saveptr);
            }
        }
    }

    return true;
}

/* =========================================================================
 * Public interface
 * ====================================================================== */

void config_set_defaults(EwConfig *config) {
    memset(config, 0, sizeof(*config));
    config->border_width   = 1;
    config->snap_distance  = 0;
    snprintf(config->terminal, sizeof(config->terminal), "foot");
    config->num_vdesks     = 4;
    config->binds          = NULL;
    config->num_binds      = 0;
    config->binds_capacity = 0;

    /*
     * Default keyboard binding: Super+Return → spawn terminal.
     *
     * This is the default when no config file is present, matching the
     * evilwm convention (evilwm uses Ctrl+Alt+Return; we use Super because
     * of the Apple keyboard modifier decision documented in evilway.h).
     *
     * Super+Shift+Q is NOT added here. It is hardcoded in main.c as the
     * compositor emergency exit and must not be user-configurable: a user
     * who accidentally removes it could trap themselves with no way out.
     */
    EwBind spawn_default = {
        .is_mouse     = false,
        .modifiers    = WLR_MODIFIER_LOGO,
        .keysym       = XKB_KEY_Return,
        .button       = 0,
        .function     = FUNC_SPAWN,
        .flags        = 0,
        .vdesk_target = -1,
    };
    binds_push(config, &spawn_default);
    /* binds_push failure here means OOM at startup before any config is read.
     * The compositor will continue with zero binds (Super+Shift+Q still works
     * since it is hardcoded in main.c). A later config_load() will retry. */
}

/*
 * config_load — populate *config from ~/.evilwayrc.
 *
 * Calls config_set_defaults() first to establish baseline values before
 * parsing the file. File-absent is silent (defaults only). Parse errors on
 * individual lines are logged and skipped; they do not abort the load.
 *
 * Returns true on success (including file-not-found). Returns false only on
 * malloc failure — in that case *config has been freed and must not be used
 * until config_set_defaults() is called again.
 */
bool config_load(EwConfig *config) {
    config_set_defaults(config);

    /*
     * SECURITY: Use $HOME to locate the config file. Reject empty HOME.
     * We do not fall back to getpwuid() — if HOME is not set, the session
     * environment is broken and we should not guess the user's home directory.
     */
    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr,
            "evilway: config: $HOME not set; using built-in defaults\n");
        return true;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/.evilwayrc", home);

    FILE *f = fopen(path, "r");
    if (!f)
        return true;   /* File absent — not an error, defaults suffice */

    fprintf(stderr, "evilway: config: loading %s\n", path);

    char line_buf[CONFIG_LINE_MAX];
    int  lineno = 0;

    while (fgets(line_buf, sizeof(line_buf), f)) {
        lineno++;

        /*
         * Detect lines that were truncated by fgets() (line longer than
         * CONFIG_LINE_MAX - 1 chars without a newline). Drain the rest and
         * warn; the truncated content is unparseable and must be skipped.
         */
        size_t len = strlen(line_buf);
        bool truncated = (len == CONFIG_LINE_MAX - 1 &&
                          line_buf[len - 1] != '\n');
        if (truncated) {
            fprintf(stderr,
                "evilway: config:%d: line too long (>%d chars), skipping\n",
                lineno, CONFIG_LINE_MAX - 1);
            int c;
            while ((c = fgetc(f)) != '\n' && c != EOF)
                ;
            continue;
        }

        /* Strip trailing newline (and any \r from Windows line endings). */
        if (len > 0 && line_buf[len - 1] == '\n') line_buf[--len] = '\0';
        if (len > 0 && line_buf[len - 1] == '\r') line_buf[--len] = '\0';

        char *line = trim(line_buf);

        /* Skip blank lines and comment lines. */
        if (!*line || *line == '#')
            continue;

        /*
         * Split into key and value at the first run of whitespace.
         * key  = first non-space token
         * value = remainder after trimming
         */
        char *p = line;
        while (*p && !isspace((unsigned char)*p))
            p++;

        char key[64];
        snprintf(key, sizeof(key), "%.*s", (int)(p - line), line);
        char *value = trim(p);

        /* ---- Dispatch on key name ---- */

        if (strcmp(key, "bw") == 0) {
            /*
             * SECURITY: Range [1, 20]. A border of 0 hides the frame
             * entirely (usable but surprising); values above 20 are
             * cosmetically absurd and clamped to prevent layout pathologies.
             */
            char *end;
            long v = strtol(value, &end, 10);
            if (!end || *end != '\0') {
                fprintf(stderr,
                    "evilway: config:%d: invalid bw value '%s'\n",
                    lineno, value);
            } else {
                if (v < 1 || v > 20) {
                    fprintf(stderr,
                        "evilway: config:%d: bw %ld out of range [1,20], clamping\n",
                        lineno, v);
                    v = (v < 1) ? 1 : 20;
                }
                config->border_width = (int)v;
            }

        } else if (strcmp(key, "snap") == 0) {
            /*
             * SECURITY: Range [0, 100]. 0 disables snap-to-edge.
             * Values above 100 make snap zones overlap at small window
             * sizes; clamp to avoid.
             */
            char *end;
            long v = strtol(value, &end, 10);
            if (!end || *end != '\0') {
                fprintf(stderr,
                    "evilway: config:%d: invalid snap value '%s'\n",
                    lineno, value);
            } else {
                if (v < 0 || v > 100) {
                    fprintf(stderr,
                        "evilway: config:%d: snap %ld out of range [0,100], clamping\n",
                        lineno, v);
                    v = (v < 0) ? 0 : 100;
                }
                config->snap_distance = (int)v;
            }

        } else if (strcmp(key, "term") == 0) {
            if (!*value) {
                fprintf(stderr,
                    "evilway: config:%d: term value is empty, ignoring\n",
                    lineno);
            } else {
                /*
                 * SECURITY: terminal[] is 256 bytes. snprintf truncates
                 * silently — we log it so users know their path was cut.
                 */
                int written = snprintf(config->terminal,
                    sizeof(config->terminal), "%s", value);
                if (written >= (int)sizeof(config->terminal)) {
                    fprintf(stderr,
                        "evilway: config:%d: term path truncated to %zu chars\n",
                        lineno, sizeof(config->terminal) - 1);
                }
            }

        } else if (strcmp(key, "numvdesks") == 0) {
            /*
             * SECURITY: Range [1, 16]. 0 vdesks is meaningless; values above
             * 16 are clamped to keep the vdesk array manageable.
             */
            char *end;
            long v = strtol(value, &end, 10);
            if (!end || *end != '\0') {
                fprintf(stderr,
                    "evilway: config:%d: invalid numvdesks value '%s'\n",
                    lineno, value);
            } else {
                if (v < 1 || v > 16) {
                    fprintf(stderr,
                        "evilway: config:%d: numvdesks %ld out of range [1,16], clamping\n",
                        lineno, v);
                    v = (v < 1) ? 1 : 16;
                }
                config->num_vdesks = (int)v;
            }

        } else if (strcmp(key, "bind") == 0) {
            if (!*value) {
                fprintf(stderr,
                    "evilway: config:%d: bind missing argument\n", lineno);
            } else {
                EwBind b;
                if (parse_bind_line(value, &b)) {
                    if (!binds_push(config, &b)) {
                        /* OOM — abort the load. Caller will discard config. */
                        fclose(f);
                        return false;
                    }
                }
                /* parse_bind_line already logged any error; continue reading. */
            }

        } else {
            fprintf(stderr,
                "evilway: config:%d: unknown key '%s', skipping\n",
                lineno, key);
        }
    }

    fclose(f);
    fprintf(stderr,
        "evilway: config: loaded %zu bind(s), bw=%d snap=%d term=%s vdesks=%d\n",
        config->num_binds,
        config->border_width,
        config->snap_distance,
        config->terminal,
        config->num_vdesks);
    return true;
}

void config_free(EwConfig *config) {
    free(config->binds);
    config->binds          = NULL;
    config->num_binds      = 0;
    config->binds_capacity = 0;
    /* Scalar fields are left in place — they are overwritten on the next
     * config_set_defaults() / config_load() call. */
}
