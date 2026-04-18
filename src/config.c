/* config.c — .evilwayrc parser
 *
 * File format is identical to evilwm's .evilwmrc:
 * one option per line, leading dashes omitted.  Options are the same
 * as evilwm command-line options.
 *
 * DECISION: we change the default mask1 from control+alt to super.
 * On Apple hardware Command→Super.  Everything else follows evilwm 1.5.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/types/wlr_keyboard.h>

#include "evilway.h"

/* ── Color helpers ──────────────────────────────────────────────────── */

static void color_from_hex(const char *hex, float out[4]) {
	unsigned long v;
	if (hex[0] == '#') hex++;
	v = strtoul(hex, NULL, 16);
	if (strlen(hex) <= 6) {
		out[0] = ((v >> 16) & 0xff) / 255.0f;
		out[1] = ((v >> 8)  & 0xff) / 255.0f;
		out[2] = (v         & 0xff) / 255.0f;
		out[3] = 1.0f;
	} else {
		out[0] = ((v >> 24) & 0xff) / 255.0f;
		out[1] = ((v >> 16) & 0xff) / 255.0f;
		out[2] = ((v >> 8)  & 0xff) / 255.0f;
		out[3] = (v         & 0xff) / 255.0f;
	}
}

/* ── Modifier parsing ───────────────────────────────────────────────── */

static uint32_t parse_single_modifier(const char *s) {
	if (!strcasecmp(s, "shift"))    return WLR_MODIFIER_SHIFT;
	if (!strcasecmp(s, "control"))  return WLR_MODIFIER_CTRL;
	if (!strcasecmp(s, "ctrl"))     return WLR_MODIFIER_CTRL;
	if (!strcasecmp(s, "alt"))      return WLR_MODIFIER_ALT;
	if (!strcasecmp(s, "mod1"))     return WLR_MODIFIER_ALT;
	if (!strcasecmp(s, "mod2"))     return WLR_MODIFIER_MOD2;
	if (!strcasecmp(s, "mod3"))     return WLR_MODIFIER_MOD3;
	if (!strcasecmp(s, "super"))    return WLR_MODIFIER_LOGO;
	if (!strcasecmp(s, "mod4"))     return WLR_MODIFIER_LOGO;
	if (!strcasecmp(s, "mod5"))     return WLR_MODIFIER_MOD5;
	return 0;
}

/* Parse "mod+mod+mod" into a WLR_MODIFIER bitmask */
static uint32_t parse_modifier_spec(const char *spec) {
	uint32_t mods = 0;
	char buf[256];
	snprintf(buf, sizeof(buf), "%s", spec);
	char *tok = strtok(buf, "+");
	while (tok) {
		mods |= parse_single_modifier(tok);
		tok = strtok(NULL, "+");
	}
	return mods;
}

/* ── Bind parsing ───────────────────────────────────────────────────── */

static enum ew_func_id parse_func_name(const char *s) {
	if (!strcasecmp(s, "spawn"))   return EW_FUNC_SPAWN;
	if (!strcasecmp(s, "delete"))  return EW_FUNC_DELETE;
	if (!strcasecmp(s, "kill"))    return EW_FUNC_KILL;
	if (!strcasecmp(s, "lower"))   return EW_FUNC_LOWER;
	if (!strcasecmp(s, "raise"))   return EW_FUNC_RAISE;
	if (!strcasecmp(s, "next"))    return EW_FUNC_NEXT;
	if (!strcasecmp(s, "move"))    return EW_FUNC_MOVE;
	if (!strcasecmp(s, "resize"))  return EW_FUNC_RESIZE;
	if (!strcasecmp(s, "fix"))     return EW_FUNC_FIX;
	if (!strcasecmp(s, "dock"))    return EW_FUNC_DOCK;
	if (!strcasecmp(s, "info"))    return EW_FUNC_INFO;
	if (!strcasecmp(s, "vdesk"))   return EW_FUNC_VDESK;
	if (!strcasecmp(s, "quit"))    return EW_FUNC_QUIT;
	return EW_FUNC_NONE;
}

static unsigned parse_flags(const char *flagstr) {
	unsigned flags = 0;
	char buf[256];
	snprintf(buf, sizeof(buf), "%s", flagstr);
	char *tok = strtok(buf, "+");
	while (tok) {
		if (!strcasecmp(tok, "relative"))    flags |= FL_RELATIVE;
		else if (!strcasecmp(tok, "toggle")) flags |= FL_TOGGLE;
		else if (!strcasecmp(tok, "up"))     flags |= FL_UP;
		else if (!strcasecmp(tok, "down"))   flags |= FL_DOWN;
		else if (!strcasecmp(tok, "left"))   flags |= FL_LEFT;
		else if (!strcasecmp(tok, "right"))  flags |= FL_RIGHT;
		else if (!strcasecmp(tok, "top"))    flags |= FL_TOP;
		else if (!strcasecmp(tok, "bottom")) flags |= FL_BOTTOM;
		else if (!strcasecmp(tok, "v"))      flags |= FL_VERT;
		else if (!strcasecmp(tok, "h"))      flags |= FL_HORZ;
		else if (!strcasecmp(tok, "horizontal")) flags |= FL_HORZ;
		else if (!strcasecmp(tok, "vertical"))   flags |= FL_VERT;
		else {
			/* Try as a numeric value (vdesk number) */
			char *end;
			long val = strtol(tok, &end, 10);
			if (end != tok && *end == '\0' && val >= 0 && val <= 255)
				flags = (flags & ~FL_VALUEMASK) | (val & FL_VALUEMASK);
		}
		tok = strtok(NULL, "+");
	}
	return flags;
}

/* Parse "bind key[+mod]...=func,flag+flag+..."
 * or   "bind button=func,flag+flag+..."
 *
 * The control spec (before '=') uses '+' as separator.
 * The func spec (after '=') uses ',' to separate func from flags,
 * and '+' within flags.
 *
 * Modifiers 'mask1', 'mask2', 'altmask' refer to the config's current
 * values, just like evilwm. */
static void config_parse_bind(struct ew_config *cfg, const char *spec) {
	if (cfg->num_binds >= EW_MAX_BINDS) {
		wlr_log(WLR_ERROR, "too many binds, max %d", EW_MAX_BINDS);
		return;
	}

	char buf[512];
	snprintf(buf, sizeof(buf), "%s", spec);

	char *eq = strchr(buf, '=');
	if (!eq) {
		wlr_log(WLR_ERROR, "bind missing '=': %s", spec);
		return;
	}
	*eq = '\0';
	char *ctlspec = buf;
	char *funcspec = eq + 1;

	/* Parse the function + flags side */
	struct ew_bind bind = {0};

	if (*funcspec) {
		char *comma = strchr(funcspec, ',');
		if (comma) {
			*comma = '\0';
			bind.flags = parse_flags(comma + 1);
		}
		bind.func = parse_func_name(funcspec);
		if (bind.func == EW_FUNC_NONE) {
			wlr_log(WLR_ERROR, "unknown bind function: %s", funcspec);
			return;
		}
	} else {
		/* Empty function = remove bind.  We just skip it. */
		return;
	}

	/* Parse the control spec (modifiers + key/button) */
	/* Split on '+', last token is the key/button, rest are modifiers */
	char *tokens[32];
	int ntok = 0;
	char *t = strtok(ctlspec, "+");
	while (t && ntok < 32) {
		tokens[ntok++] = t;
		t = strtok(NULL, "+");
	}
	if (ntok == 0) return;

	/* Last token is the key or button name */
	char *keyname = tokens[ntok - 1];

	/* Everything before it is modifiers */
	uint32_t mods = 0;
	for (int i = 0; i < ntok - 1; i++) {
		if (!strcasecmp(tokens[i], "mask1"))
			mods |= cfg->mask1;
		else if (!strcasecmp(tokens[i], "mask2"))
			mods |= cfg->mask2;
		else if (!strcasecmp(tokens[i], "altmask"))
			mods |= cfg->altmask;
		else
			mods |= parse_single_modifier(tokens[i]);
	}
	bind.modifiers = mods;

	/* Determine if button or key */
	if (!strncasecmp(keyname, "button", 6)) {
		bind.type = EW_BIND_BUTTON;
		int bnum = atoi(keyname + 6);
		switch (bnum) {
		case 1: bind.button = BTN_LEFT; break;
		case 2: bind.button = BTN_MIDDLE; break;
		case 3: bind.button = BTN_RIGHT; break;
		case 4: bind.button = BTN_SIDE; break;
		case 5: bind.button = BTN_EXTRA; break;
		default: bind.button = BTN_LEFT; break;
		}
	} else {
		bind.type = EW_BIND_KEY;
		bind.keysym = xkb_keysym_from_name(keyname, XKB_KEYSYM_CASE_INSENSITIVE);
		if (bind.keysym == XKB_KEY_NoSymbol) {
			wlr_log(WLR_ERROR, "unknown keysym: %s", keyname);
			return;
		}
	}

	cfg->binds[cfg->num_binds++] = bind;
}

/* ── Default binds ──────────────────────────────────────────────────── */

static void config_set_default_binds(struct ew_config *cfg) {
	cfg->num_binds = 0;

	/* These match evilwm 1.5 defaults but reference mask1/mask2/altmask
	 * which we've changed to Super instead of Ctrl+Alt */

	static const char *defaults[] = {
		/* Key binds */
		"mask1+Return=spawn",
		"mask1+Escape=delete",
		"mask1+altmask+Escape=kill",
		"mask1+Insert=lower",
		"mask1+KP_Insert=lower",
		"mask1+i=info",
		"mask2+Tab=next",

		/* Move relative */
		"mask1+h=move,relative+left",
		"mask1+j=move,relative+down",
		"mask1+k=move,relative+up",
		"mask1+l=move,relative+right",

		/* Move to corner */
		"mask1+y=move,top+left",
		"mask1+u=move,top+right",
		"mask1+b=move,bottom+left",
		"mask1+n=move,bottom+right",

		/* Resize relative */
		"mask1+altmask+h=resize,relative+left",
		"mask1+altmask+j=resize,relative+down",
		"mask1+altmask+k=resize,relative+up",
		"mask1+altmask+l=resize,relative+right",

		/* Maximize toggles */
		"mask1+equal=resize,toggle+v",
		"mask1+altmask+equal=resize,toggle+h",
		"mask1+x=resize,toggle+v+h",

		/* Dock / fix */
		"mask1+d=dock,toggle",
		"mask1+f=fix,toggle",

		/* Virtual desktops */
		"mask1+1=vdesk,0",
		"mask1+2=vdesk,1",
		"mask1+3=vdesk,2",
		"mask1+4=vdesk,3",
		"mask1+5=vdesk,4",
		"mask1+6=vdesk,5",
		"mask1+7=vdesk,6",
		"mask1+8=vdesk,7",
		"mask1+Left=vdesk,relative+left",
		"mask1+Right=vdesk,relative+right",
		"mask1+Up=vdesk,relative+up",
		"mask1+Down=vdesk,relative+down",
		"mask1+a=vdesk,toggle",

		/* evilWay addition: compositor quit */
		"mask1+altmask+q=quit",

		/* Mouse binds */
		"button1=move",
		"button2=resize",
		"button3=lower",

		NULL
	};

	for (int i = 0; defaults[i]; i++) {
		config_parse_bind(cfg, defaults[i]);
	}
}

/* ── App rule parsing ───────────────────────────────────────────────── */

static void config_parse_geometry(const char *spec, int *x, int *y,
                                  int *w, int *h) {
	/* Standard X geometry: WxH+X+Y or WxH-X-Y etc. */
	*x = *y = 0;
	*w = *h = 0;
	sscanf(spec, "%dx%d%d%d", w, h, x, y);
}

/* ── Init / load / destroy ──────────────────────────────────────────── */

void config_init(struct ew_config *cfg) {
	memset(cfg, 0, sizeof(*cfg));

	cfg->term = strdup(EW_DEFAULT_TERM);
	cfg->border_width = EW_DEFAULT_BW;
	cfg->snap = EW_DEFAULT_SNAP;
	cfg->move_step = EW_DEFAULT_MOVE_STEP;
	cfg->nosoliddrag = false;
	cfg->wholescreen = false;
	cfg->vdesks_cols = EW_DEFAULT_VDESKS_C;
	cfg->vdesks_rows = EW_DEFAULT_VDESKS_R;
	/* Default colors — evilwm defaults */
	color_from_hex("#DAA520", cfg->fg);  /* goldenrod, active */
	color_from_hex("#4682B4", cfg->fc);  /* steelblue, fixed */
	color_from_hex("#404040", cfg->bg);  /* dark grey, inactive */

	/* DECISION: Super for modifiers on Apple hardware */
	cfg->mask1   = WLR_MODIFIER_LOGO;
	cfg->mask2   = WLR_MODIFIER_LOGO;
	cfg->altmask = WLR_MODIFIER_SHIFT;

	config_set_default_binds(cfg);
}

void config_load(struct ew_config *cfg, const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		wlr_log(WLR_INFO, "no config file at %s, using defaults", path);
		return;
	}

	wlr_log(WLR_INFO, "loading config from %s", path);

	/* Track current app rule index for multi-line app matching */
	int current_app = -1;

	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		/* Strip trailing newline/whitespace */
		int len = strlen(line);
		while (len > 0 && isspace((unsigned char)line[len - 1]))
			line[--len] = '\0';

		/* Skip empty lines and comments */
		char *p = line;
		while (isspace((unsigned char)*p)) p++;
		if (*p == '\0' || *p == '#') continue;

		/* Split into option and value */
		char *opt = p;
		char *val = NULL;
		while (*p && !isspace((unsigned char)*p)) p++;
		if (*p) {
			*p = '\0';
			p++;
			while (isspace((unsigned char)*p)) p++;
			if (*p) val = p;
		}

		/* Process options — same names as evilwm, dashes omitted */
		if (!strcmp(opt, "term") && val) {
			free(cfg->term);
			cfg->term = strdup(val);
		} else if (!strcmp(opt, "fg") && val) {
			color_from_hex(val, cfg->fg);
		} else if (!strcmp(opt, "fc") && val) {
			color_from_hex(val, cfg->fc);
		} else if (!strcmp(opt, "bg") && val) {
			color_from_hex(val, cfg->bg);
		} else if (!strcmp(opt, "bw") && val) {
			cfg->border_width = atoi(val);
		} else if (!strcmp(opt, "snap") && val) {
			cfg->snap = atoi(val);
		} else if (!strcmp(opt, "wholescreen")) {
			cfg->wholescreen = true;
		} else if (!strcmp(opt, "numvdesks") && val) {
			char *x = strchr(val, 'x');
			if (x) {
				*x = '\0';
				cfg->vdesks_cols = atoi(val);
				cfg->vdesks_rows = atoi(x + 1);
			} else {
				cfg->vdesks_cols = atoi(val);
				cfg->vdesks_rows = 1;
			}
		} else if (!strcmp(opt, "nosoliddrag")) {
			cfg->nosoliddrag = true;
		} else if (!strcmp(opt, "mask1") && val) {
			cfg->mask1 = parse_modifier_spec(val);
			/* Rebind defaults with new mask */
			config_set_default_binds(cfg);
		} else if (!strcmp(opt, "mask2") && val) {
			cfg->mask2 = parse_modifier_spec(val);
			config_set_default_binds(cfg);
		} else if (!strcmp(opt, "altmask") && val) {
			cfg->altmask = parse_modifier_spec(val);
			config_set_default_binds(cfg);
		} else if (!strcmp(opt, "bind") && val) {
			config_parse_bind(cfg, val);
		} else if (!strcmp(opt, "app") && val) {
			/* Start a new app rule */
			if (cfg->num_app_rules < EW_MAX_APP_RULES) {
				current_app = cfg->num_app_rules++;
				struct ew_app_rule *r = &cfg->app_rules[current_app];
				memset(r, 0, sizeof(*r));
				r->app_id = strdup(val);
				r->vdesk = -1;
			}
		} else if (!strcmp(opt, "geometry") && val && current_app >= 0) {
			struct ew_app_rule *r = &cfg->app_rules[current_app];
			config_parse_geometry(val, &r->gx, &r->gy, &r->gw, &r->gh);
			r->has_geometry = true;
		} else if ((!strcmp(opt, "g")) && val && current_app >= 0) {
			struct ew_app_rule *r = &cfg->app_rules[current_app];
			config_parse_geometry(val, &r->gx, &r->gy, &r->gw, &r->gh);
			r->has_geometry = true;
		} else if (!strcmp(opt, "vdesk") && val && current_app >= 0) {
			struct ew_app_rule *r = &cfg->app_rules[current_app];
			r->vdesk = atoi(val);
		} else if (!strcmp(opt, "fixed") && current_app >= 0) {
			cfg->app_rules[current_app].fixed = true;
		} else if (!strcmp(opt, "dock") && current_app >= 0) {
			cfg->app_rules[current_app].is_dock = true;
		} else if (!strcmp(opt, "ignore-position") && current_app >= 0) {
			cfg->app_rules[current_app].ignore_position = true;
		} else if (!strcmp(opt, "ignore-border") && current_app >= 0) {
			cfg->app_rules[current_app].ignore_border = true;
		} else {
			wlr_log(WLR_INFO, "unknown config option: %s", opt);
		}
	}

	fclose(f);
}

void config_destroy(struct ew_config *cfg) {
	free(cfg->term);
	for (int i = 0; i < cfg->num_app_rules; i++) {
		free(cfg->app_rules[i].app_id);
	}
}
