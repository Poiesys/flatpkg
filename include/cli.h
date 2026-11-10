#ifndef FPKG_CLI_H
#define FPKG_CLI_H

#include <stdbool.h>
#include <stddef.h>

#include "flatpkg.h"

// cli.c is the one place that reads argv or an environment variable; a module
// receives a cli_req_t and never sees a token or a name.  Help, parsing and the
// value checks are all generated from cli.c's tables, so they cannot drift.

typedef struct {
	int cmd; // index of the command word in argv, -1 when there is none
	bool unknown_opt; // `cmd` is an unrecognised option, not a command
	bool help;	  // --help or -h before the command
	bool version;
	bool verbose;
	bool ostree_verbose;
	bool profile;
	bool simulate;
	int color; // -1 unset, 0 off, 1 on
	bool gl_drivers; // --gl-drivers: print the drivers and exit
	const char* logfmt;
	const char* installation;
} cli_globals_t;

typedef struct cli_cmd cli_cmd_t;

// Everything a command line says, decided.  Strings point into argv.
typedef struct {
	cli_globals_t g;
	const cli_cmd_t* cmd;
	const char* name; // the command as typed (`remove` for uninstall)
	const char** ops; // operands, in order
	size_t nops;
	char** owned_ops; // the operands cli.c spelled itself (see install)
	size_t nowned_ops;
	char* const* rest; // the application's own arguments, or NULL

	bool assume_yes;
	bool rdepends;
	bool runtime;
	bool prune;
	bool available;
	const char* commit;
	const char* from;
	const char* columns;

	bool reinstall;
	bool appstream;
	const char* remote;

	// info and remote-info: the machine-readable selectors.
	bool show_ref, show_origin, show_commit, show_location, show_metadata;
	bool show_size, show_permissions, show_extensions, show_parent;
	bool show_runtime, show_sdk;
	const char* arch; // --arch, when the command is not `run`

	// remote-modify: the changes, each NULL or unset when not asked for.
	const char* url;
	const char* gpg_import;
	const char* branch_name;
	const char* title;
	const char* comment;
	const char* description;
	const char* homepage;
	const char* icon;
	int gpg_verify; // -1 unset, 0 off, 1 on

	run_opts_t run;	    // run: the per-launch grants
	fp_metadata_t over; // override: the changes asked for
	bool over_user, over_show, over_reset;
} cli_req_t;

// What cli_parse() returns when the command is to run; any other value is
// the exit status, the problem or the page having been shown already.
#define CLI_PROCEED (-1)

// The command a request names; the enum is what the typed client and
// dispatch_local() switch on.
typedef enum {
	CMD_NONE = 0,
	CMD_INSTALL,
	CMD_IMPORT,
	CMD_UNINSTALL,
	CMD_UPDATE,
	CMD_UPGRADE,
	CMD_REPAIR,
	CMD_EXPORTS,
	CMD_ROLLBACK,
	CMD_LIST,
	CMD_INFO,
	CMD_SEARCH,
	CMD_LIST_COMMITS,
	CMD_HISTORY,
	CMD_RUN,
	CMD_PS,
	CMD_ENTER,
	CMD_KILL,
	CMD_OVERRIDE,
	CMD_REMOTE_ADD,
	CMD_REMOTE_LIST,
	CMD_REMOTE_DELETE,
	CMD_REMOTE_INFO,
	CMD_REMOTE_MODIFY,
	CMD_COMPLETE,
	CMD_HELP
} cli_cmd_id_t;

// Read argv and the environment into *req.
int cli_parse(int argc, char* const* argv, cli_req_t* req);
void cli_req_free(cli_req_t* req);
cli_cmd_id_t cli_req_cmd(const cli_req_t* req);

// The program entry points that are not commands: `flatpkg portal` and
// `flatpkg session-helper`, the two per-user D-Bus services.  cli.c owns
// argv, so it matches the word and keeps the program name for the portal.
typedef enum {
	CLI_SERVICE_NONE = 0,
	CLI_SERVICE_PORTAL,
	CLI_SERVICE_SESSION_HELPER,
} cli_service_t;

cli_service_t cli_service_entry(int argc, char* const* argv);
const char* cli_argv0(void);

// cmd_* handlers.  cmd_run is the daemon's, the rest are the CLI's.
int cmd_override(const cli_req_t* r);
int cmd_info(const cli_req_t* r);
int cmd_run(const char* app_id, char* const extra_argv[], const run_opts_t* ro);

// Whether a command runs in the unprivileged CLI without the daemon.
bool fpkg_cmd_is_local(const char* cmd);
// Whether THIS request is local: mostly the command, but `override` depends on
// it - the caller's overrides are a file in their home, the system ones belong
// to the store and need the daemon.
bool cli_req_is_local(const cli_req_t* r);

// The application id of a `run` command line, read quietly: the client
// pre-creates ~/.var/app/<id> before the daemon sees the request.  NULL when
// there is none.
const char* cli_run_app_id(int argc, char* const* argv);

// The process environment, read in one place (cli.c).
char** cli_environ(void);

// One command spelling per line, for the shell completions.
void cli_complete_commands(void);
// The generated half of the completions: option spellings and vocabularies.
void cli_complete_words(const char* cmd);
void cli_complete_values(const char* opt);
void cli_complete_operands(const char* cmd);
// The generated option sections of man/flatpkg.8.
void cli_man_options(const char* which);

#endif
