/* config.h — public interface for the evilWay runtime config parser.
 *
 * Reads ~/.evilwayrc on startup and on SIGHUP.
 * Format: one option per line, option name (no leading dashes), whitespace-
 * separated arguments. Comments start with '#'. Blank lines ignored.
 * Identical format to evilwm's .evilwmrc — see evilwm manual for examples.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include "evilway.h"
/* stdbool.h and stddef.h are pulled in transitively via evilway.h */

/*
 * config_set_defaults — populate *config with built-in defaults.
 *
 * Must be called before config_load(). config_load() calls this internally,
 * so callers that only use config_load() do not need to call it separately.
 * Exposed so tests and future callers can get a clean default state without
 * touching the filesystem.
 *
 * The default bind list includes Super+Return=spawn. Super+Shift+Q is NOT
 * included — it is hardcoded in main.c as the compositor emergency exit and
 * is not user-configurable.
 */
void config_set_defaults(EwConfig *config);

/*
 * config_load — load ~/.evilwayrc into *config.
 *
 * Calls config_set_defaults() first. File-absent is not an error — silently
 * uses defaults. Unknown keys and malformed bind lines are logged to stderr
 * and skipped without aborting. Partial configs are discarded on failure.
 *
 * Returns true on success (including file-not-found with defaults applied).
 * Returns false only on fatal errors (malloc failure). On false return,
 * *config is freed and left in an indeterminate state — call
 * config_set_defaults() before using it again.
 */
bool config_load(EwConfig *config);

/*
 * config_free — free heap-allocated members of *config.
 *
 * Does not free the EwConfig struct itself (it lives inside Server on the
 * stack). Safe to call on a zero-initialized config (no-op).
 */
void config_free(EwConfig *config);

#endif /* CONFIG_H */
