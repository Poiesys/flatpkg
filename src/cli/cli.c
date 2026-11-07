#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cli.h"
#include "flatpkg.h"
#include "fplog_int.h"
#define FPKG_MODULE "cli"
#include "fpkglog.h"
#include "translations.h"

extern char** environ;

typedef enum {
	OPT_NONE = 0,
	OPT_HELP,
	OPT_VERSION,
	OPT_VERBOSE,
	OPT_OSTREE_VERBOSE,
	OPT_PROFILE,
	OPT_NO_COLOR,
	OPT_FORCE_COLOR, // environment only: FORCE_COLOR has no flag
	OPT_SIMULATE,
	OPT_LOGFMT,
	OPT_INSTALLATION,
	OPT_USER,
	OPT_SYSTEM,
	OPT_YES,
	OPT_NONINTERACTIVE,
	OPT_RUNTIME,
	OPT_COMMIT,
	OPT_FROM,
	OPT_NO_PULL,
	OPT_NO_DEPLOY,
	OPT_REINSTALL,
	OPT_OR_UPDATE,
	OPT_NO_STATIC_DELTAS,
	OPT_NO_DEPS,
	OPT_NO_RELATED,
	OPT_INCLUDE_SDK,
	OPT_INCLUDE_DEBUG,
	OPT_SUBPATH,
	OPT_SIDELOAD_REPO,
	OPT_RDEPENDS,
	OPT_PRUNE,
	OPT_AVAILABLE,
	OPT_IF_NOT_EXISTS,
	OPT_COLUMNS,
	OPT_APPSTREAM,
	OPT_REMOTE,
	OPT_SOCKET,
	OPT_NOSOCKET,
	OPT_SHARE,
	OPT_UNSHARE,
	OPT_DEVICE,
	OPT_NODEVICE,
	OPT_FILESYSTEM,
	OPT_NOFILESYSTEM,
	OPT_ENV,
	OPT_COMMAND,
	OPT_BRANCH,
	OPT_ARCH,
	OPT_DEVEL,
	OPT_CLEARENV,
	OPT_DIE_WITH_PARENT,
	OPT_FILE_FORWARDING,
	OPT_ALLOW,
	OPT_DISALLOW,
	OPT_PERSIST,
	OPT_UNSET_ENV,
	OPT_TALK_NAME,
	OPT_OWN_NAME,
	OPT_NO_TALK_NAME,
	OPT_SYSTEM_TALK_NAME,
	OPT_SYSTEM_OWN_NAME,
	OPT_SYSTEM_NO_TALK_NAME,
	OPT_A11Y_OWN_NAME,
	OPT_SHOW,
	OPT_RESET
} cli_opt_id_t;

#define OPTF_CHECK (1u << 0) // the value must be one of `choices`
#define OPTF_NEG (1u << 1)   // ... or `!` and one of them
#define OPTF_LIST (1u << 2)  // a comma-separated list of them

typedef struct {
	cli_opt_id_t id;
	const char* lname;  // "--socket", or NULL for a short-only option
	const char* alt;    // a second long spelling ("--dry-run"), or NULL
	char sname;	    // 'y' for -y, or 0
	const char* arg;    // value placeholder, NULL for a flag
	const char* help;   // one line; NULL keeps the option out of the help
	const char* unimpl; // refused with this reason; never in the help
	const char* const*
		choices; // the vocabulary, shown and (OPTF_CHECK) enforced
	const char* const*
		values; // completion vocabulary for a value checked elsewhere
	unsigned flags;
} cli_opt_t;

#define FLAG(id, l, s, h) {id, l, NULL, s, NULL, h, NULL, NULL, NULL, 0}
#define VALUE(id, l, s, a, h) {id, l, NULL, s, a, h, NULL, NULL, NULL, 0}
#define VALUE_SET(id, l, s, a, h, set) {id, l, NULL, s, a, h, NULL, set, NULL, 0}
#define CHOICE(id, l, a, h, set, fl) {id, l, NULL, 0, a, h, NULL, set, NULL, fl}
#define REFUSED(id, l, a, why) {id, l, NULL, 0, a, NULL, why, NULL, NULL, 0}
#define NO_OPT {OPT_NONE, NULL, NULL, 0, NULL, NULL, NULL, NULL, NULL, 0}
#define STUB "Accepted for flatpak compatibility; has no effect"

static const char* const sockets_set[] = {"x11",
					  "wayland",
					  "fallback-x11",
					  "pulseaudio",
					  "session-bus",
					  "system-bus",
					  "ssh-auth",
					  "pcsc",
					  "cups",
					  "gpg-agent",
					  "inherit-wayland-socket",
					  NULL};
static const char* const devices_set[] = {"all", "dri", "kvm", "input",
					  "usb", "shm", "snd", NULL};
static const char* const share_set[] = {"network", "ipc", NULL};
static const char* const unshare_set[] = {"network", "ipc", "process", "all",
					  NULL};
static const char* const columns_set[] = {"instance", "pid",  "application",
					  "jail",     "root", NULL};

static const cli_opt_t global_opts[] = {
	FLAG(OPT_HELP, "--help", 'h',
	     "Show help; after a command, that command's page"),
	FLAG(OPT_VERSION, "--version", 0, "Print the version and exit"),
	FLAG(OPT_VERBOSE, "--verbose", 'v', "Show debug output"),
	FLAG(OPT_OSTREE_VERBOSE, "--ostree-verbose", 0,
	     "Also pass through the output of the ostree tool"),
	FLAG(OPT_PROFILE, "--profile", 0,
	     "Print how long each phase took, then a table ranked by the time "
	     "spent in each phase alone"),
	FLAG(OPT_NO_COLOR, "--no-color", 0, "Disable colour output"),
	{OPT_SIMULATE, "--simulate", "--dry-run", 'n', NULL,
	 "Print the changeset without applying it", NULL, NULL, NULL, 0},
	VALUE(OPT_LOGFMT, NULL, 'l', "FILE", "Append logfmt records to FILE"),
	VALUE(OPT_INSTALLATION, "--installation", 'u', "NAME",
	      "Select the installation; flatpkg has one, so only 'default' "
	      "resolves"),
	FLAG(OPT_USER, "--user", 0, STUB),
	FLAG(OPT_SYSTEM, "--system", 0, STUB),
	NO_OPT};

// Commands that still cannot honour a dry run: they have no plan to show,
// so -n would either do the real thing or say nothing.
static const char* const no_simulate[] = {
	"rollback", "import", "remote-add", "remote-delete", "override", NULL};

static const cli_opt_t no_opts[] = {NO_OPT};

static const cli_opt_t install_opts[] = {
	FLAG(OPT_RUNTIME, "--runtime", 0, "Treat each REF as a runtime"),
	FLAG(OPT_YES, "--assumeyes", 'y', "Do not ask for confirmation"),
	FLAG(OPT_NONINTERACTIVE, "--noninteractive", 0, "Same as --assumeyes"),
	VALUE(OPT_COMMIT, "--commit", 0, "HASH", "Install this commit"),
	VALUE(OPT_FROM, "--from", 0, "FILE",
	      "Install a .flatpak bundle instead of pulling from a remote; "
	      "takes no REF"),
	FLAG(OPT_NO_PULL, "--no-pull", 0, STUB),
	FLAG(OPT_NO_DEPLOY, "--no-deploy", 0, STUB),
	FLAG(OPT_REINSTALL, "--reinstall", 0,
	     "Reinstall the application even if it is already installed"),
	FLAG(OPT_OR_UPDATE, "--or-update", 0, STUB),
	REFUSED(OPT_NO_STATIC_DELTAS, "--no-static-deltas", NULL,
		"deltas are taken from the remote's configuration"),
	REFUSED(OPT_NO_DEPS, "--no-deps", NULL,
		"dependencies are always resolved from the ref's runtime"),
	REFUSED(OPT_NO_RELATED, "--no-related", NULL,
		"related apps of a ref are always installed together"),
	REFUSED(OPT_INCLUDE_SDK, "--include-sdk", NULL,
		"an app's runtime is installed as declared by its ref"),
	REFUSED(OPT_INCLUDE_DEBUG, "--include-debug", NULL,
		"debug extensions are resolved from the ref's [Extension] "
		"declarations"),
	REFUSED(OPT_SUBPATH, "--subpath", "PATH",
		"flatpkg installs whole refs; Locale and Debug extensions are "
		"resolved from the ref's [Extension] declarations"),
	REFUSED(OPT_SIDELOAD_REPO, "--sideload-repo", "PATH",
		"add the repo file as a remote with `flatpkg remote-add "
		"--from=FILE NAME`, then install from it"),
	NO_OPT};

static const cli_opt_t uninstall_opts[] = {
	FLAG(OPT_YES, "--assumeyes", 'y', "Do not ask for confirmation"),
	FLAG(OPT_NONINTERACTIVE, "--noninteractive", 0, "Same as --assumeyes"),
	FLAG(OPT_RDEPENDS, "--rdepends", 'r',
	     "Also remove every installed ref that depends on the ones given"),
	NO_OPT};

static const cli_opt_t update_opts[] = {
	FLAG(OPT_APPSTREAM, "--appstream", 0,
	     "Refresh the appstream branch and its icons"),
	VALUE(OPT_REMOTE, "--remote", 0, "NAME",
	      "Limit --appstream to this remote"),
	VALUE(OPT_ARCH, "--arch", 0, "ARCH",
	      "Limit --appstream to this architecture"),
	FLAG(OPT_YES, "--assumeyes", 'y', "Do not ask for confirmation"),
	FLAG(OPT_NONINTERACTIVE, "--noninteractive", 0, "Same as --assumeyes"),
	NO_OPT};

static const cli_opt_t upgrade_opts[] = {
	FLAG(OPT_PRUNE, "--prune", 0,
	     "Also remove deploys nothing in the world file needs any more"),
	FLAG(OPT_AVAILABLE, "--available", 0,
	     "Accepted for apk parity; upgrade already moves to the latest "
	     "available version"),
	FLAG(OPT_YES, "--assumeyes", 'y', "Do not ask for confirmation"),
	FLAG(OPT_NONINTERACTIVE, "--noninteractive", 0, "Same as --assumeyes"),
	NO_OPT};

static const cli_opt_t repair_opts[] = {NO_OPT};

static const cli_opt_t ps_opts[] = {
	CHOICE(OPT_COLUMNS, "--columns", "LIST",
	       "Show only these columns, comma separated, and print nothing "
	       "when no instance is running",
	       columns_set, OPTF_CHECK | OPTF_LIST),
	NO_OPT};

static const cli_opt_t remote_add_opts[] = {
	VALUE(OPT_FROM, "--from", 0, "URL",
	      "Read the remote from this .flatpakrepo file or URL"),
	FLAG(OPT_IF_NOT_EXISTS, "--if-not-exists", 0, STUB), NO_OPT};

static const cli_opt_t run_opts[] = {
	CHOICE(OPT_SOCKET, "--socket", "NAME", "Expose a socket", sockets_set,
	       OPTF_CHECK | OPTF_NEG),
	CHOICE(OPT_DEVICE, "--device", "NAME", "Expose a device", devices_set,
	       OPTF_CHECK | OPTF_NEG),
	VALUE_SET(OPT_SHARE, "--share", 0, "NAME", "Share with the host",
		  share_set),
	VALUE_SET(OPT_UNSHARE, "--unshare", 0, "NAME",
		  "Unshare from the host; process is already the case, every "
		  "sandbox is its own jail",
		  unshare_set),
	VALUE(OPT_FILESYSTEM, "--filesystem", 0, "WHERE",
	      "Expose a filesystem: host, home, xdg-*, ~/path or /path, with a "
	      ":ro or :create suffix; a leading ! takes one away"),
	VALUE(OPT_COMMAND, "--command", 0, "CMD",
	      "Run CMD instead of the application's command"),
	VALUE(OPT_ENV, "--env", 0, "VAR=VALUE", "Set an environment variable"),
	FLAG(OPT_DEVEL, "--devel", 0,
	     "Use the developer environment (devel exports)"),
	VALUE(OPT_BRANCH, "--branch", 0, "BRANCH",
	      "Expect this branch; warns when the active deploy is another"),
	VALUE(OPT_ARCH, "--arch", 0, "ARCH",
	      "Expect this architecture; warns when the active deploy is "
	      "another"),
	FLAG(OPT_CLEARENV, "--clearenv", 0, "Drop the host environment"),
	FLAG(OPT_FILE_FORWARDING, "--file-forwarding", 0,
	     "Forward @@ and @@u file arguments through the portal; always on, "
	     "accepted because exported .desktop files pass it"),
	FLAG(OPT_DIE_WITH_PARENT, "--die-with-parent", 0,
	     "Kill the application when flatpkg exits"),
	NO_OPT};

static const cli_opt_t override_opts[] = {
	VALUE_SET(OPT_SHARE, "--share", 0, "NAME", "Share with the host",
		  share_set),
	VALUE_SET(OPT_UNSHARE, "--unshare", 0, "NAME",
		  "Unshare from the host", unshare_set),
	CHOICE(OPT_SOCKET, "--socket", "NAME", "Expose a socket", sockets_set,
	       0),
	CHOICE(OPT_NOSOCKET, "--nosocket", "NAME", "Do not expose a socket",
	       sockets_set, 0),
	CHOICE(OPT_DEVICE, "--device", "NAME", "Expose a device", devices_set,
	       0),
	CHOICE(OPT_NODEVICE, "--nodevice", "NAME", "Do not expose a device",
	       devices_set, 0),
	VALUE(OPT_ALLOW, "--allow", 0, "NAME", "Allow a feature"),
	VALUE(OPT_DISALLOW, "--disallow", 0, "NAME", "Disallow a feature"),
	VALUE(OPT_FILESYSTEM, "--filesystem", 0, "WHERE",
	      "Expose a filesystem: host, home, xdg-*, ~/path or /path, with a "
	      ":ro or :create suffix"),
	VALUE(OPT_NOFILESYSTEM, "--nofilesystem", 0, "WHERE",
	      "Do not expose a filesystem"),
	VALUE(OPT_PERSIST, "--persist", 0, "PATH",
	      "Keep PATH under the application's data directory"),
	VALUE(OPT_ENV, "--env", 0, "VAR=VALUE", "Set an environment variable"),
	VALUE(OPT_UNSET_ENV, "--unset-env", 0, "VAR",
	      "Unset an environment variable"),
	VALUE(OPT_TALK_NAME, "--talk-name", 0, "NAME",
	      "Allow talking to a session bus name"),
	VALUE(OPT_OWN_NAME, "--own-name", 0, "NAME",
	      "Allow owning a session bus name"),
	VALUE(OPT_NO_TALK_NAME, "--no-talk-name", 0, "NAME",
	      "Do not allow talking to a session bus name"),
	VALUE(OPT_SYSTEM_TALK_NAME, "--system-talk-name", 0, "NAME",
	      "Allow talking to a system bus name"),
	VALUE(OPT_SYSTEM_OWN_NAME, "--system-own-name", 0, "NAME",
	      "Allow owning a system bus name"),
	VALUE(OPT_SYSTEM_NO_TALK_NAME, "--system-no-talk-name", 0, "NAME",
	      "Do not allow talking to a system bus name"),
	VALUE(OPT_A11Y_OWN_NAME, "--a11y-own-name", 0, "NAME",
	      "Allow owning an accessibility bus name"),
	FLAG(OPT_SHOW, "--show", 0, "Show the current overrides"),
	FLAG(OPT_RESET, "--reset", 0, "Remove the overrides"),
	FLAG(OPT_USER, "--user", 0,
	     "Use the user's override file, "
	     "~/.local/share/flatpak/overrides/APP "
	     "(the default)"),
	FLAG(OPT_SYSTEM, "--system", 0,
	     "Write the override under the store instead"),
	NO_OPT};

typedef enum {
	ENV_SET,   // any non-empty value
	ENV_BOOL,  // starts with 1, t, T, y or Y
	ENV_VALUE, // the value itself matters
	ENV_EQ	   // exactly `eq`
} env_kind_t;

typedef struct {
	const char* name;
	env_kind_t kind;
	const char* eq;
	cli_opt_id_t opt; // the option this spelling sets, or OPT_NONE
	const char* help; // NULL keeps it out of the help
} cli_env_t;

static const cli_env_t env_vars[] = {
	{"FLATPKG_VERBOSE", ENV_BOOL, NULL, OPT_VERBOSE, "Same as --verbose"},
	{"FLATPKG_LOGFMT", ENV_VALUE, NULL, OPT_LOGFMT, "Same as -l"},
	{"FLATPKG_PROFILE", ENV_BOOL, NULL, OPT_PROFILE, "Same as --profile"},
	{"FLATPKG_SIMULATE", ENV_SET, NULL, OPT_SIMULATE, "Same as --simulate"},
	{"NO_COLOR", ENV_SET, NULL, OPT_NO_COLOR, "Same as --no-color"},
	{"FORCE_COLOR", ENV_SET, NULL, OPT_FORCE_COLOR,
	 "Colour even when the output is not a terminal"},
	// Last matching row wins; a command-line flag beats all of them.
	{"TERM", ENV_EQ, "dumb", OPT_NO_COLOR, NULL},
	{"FLATPKG_STORE", ENV_VALUE, NULL, OPT_NONE,
	 "Use this directory as the store instead of /var/db/flatpkg"},
	{"FLATPKG_DEBUG_KEEP", ENV_VALUE, NULL, OPT_NONE,
	 "Keep the jail and its mounts after a run"},
	{"FLATPKG_NO_SHIMS", ENV_VALUE, NULL, OPT_NONE,
	 "Do not preload the Linux compatibility shims into the application"},
	{"FLATPKG_RLIMIT_SHIM_TRACE", ENV_VALUE, NULL, OPT_NONE,
	 "Trace what the rlimit shim synthesises; FLATPKG_DEVT_SHIM_TRACE, "
	 "FLATPKG_MEMFD_SHIM_TRACE and FLATPKG_SYNCOBJ_SHIM_TRACE do the same "
	 "for their shims"},
	{"FLATPKG_OSTREE_VERBOSE", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"FLATPKG_FILECHOOSER_TIMEOUT", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"FLATPAK_DBUSPROXY", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"FLATPAK_GL_DRIVERS", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"FLATPKG_INTERACTIVE", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"FLATPKG_FORWARD_FDS", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"FLATPKG_APP_UID", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"FLATPKG_APP_GID", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"FLATPKG_APP_USER", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"FLATPKG_APP_HOME", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"FLATPKG_APP_TTY", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"FLATPKG_APP_MASTER", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"HOME", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"PATH", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"LD_PRELOAD", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"TZDIR", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"XDG_RUNTIME_DIR", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"XDG_CURRENT_DESKTOP", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"DISPLAY", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"WAYLAND_DISPLAY", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"DBUS_SESSION_BUS_ADDRESS", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"DBUS_SYSTEM_BUS_ADDRESS", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"AT_SPI_BUS_ADDRESS", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"LC_ALL", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"LC_MESSAGES", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"LANG", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"SSH_AUTH_SOCK", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"PULSE_SERVER", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"PULSE_RUNTIME_PATH", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"PULSE_CLIENTCONFIG", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"CUPS_SERVER", ENV_VALUE, NULL, OPT_NONE, NULL},
	{"PCSCLITE_CSOCK_NAME", ENV_VALUE, NULL, OPT_NONE, NULL},
};

#define N_ENV (sizeof(env_vars) / sizeof(env_vars[0]))

const char* cli_getenv(const char* name) {
	assert(name != NULL);
	return getenv(name);
}

char** cli_environ(void) { return environ; }

// The two per-user D-Bus services are program entry points, not commands:
// the word after `flatpkg` selects one, and this is the only file that reads
// it.  The program name is kept because the portal re-execs flatpkg.
static char g_argv0[PATH_MAX];

const char* cli_argv0(void) { return g_argv0; }

cli_service_t cli_service_entry(int argc, char* const* argv) {
	static const struct {
		const char* name;
		cli_service_t svc;
	} subs[] = {
		{"portal", CLI_SERVICE_PORTAL},
		{"session-helper", CLI_SERVICE_SESSION_HELPER},
	};

	if (argv == NULL || argc < 1) return CLI_SERVICE_NONE;
	snprintf(g_argv0, sizeof(g_argv0), "%s", argv[0] ? argv[0] : "");
	if (argc != 2) return CLI_SERVICE_NONE;
	for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++)
		if (strcmp(argv[1], subs[i].name) == 0) return subs[i].svc;
	return CLI_SERVICE_NONE;
}

#define G_INSTALL "Application installation and removal"
#define G_MAINT "System maintenance"
#define G_QUERY "Querying application information"
#define G_RUN "Running applications"
#define G_REMOTE "Remote management"
#define G_MISC "Miscellaneous"

static const char* const groups[] = {G_INSTALL, G_MAINT, G_QUERY, G_RUN,
				     G_REMOTE,	G_MISC,	 NULL};

#define CLI_CMD_MUTATING (1u << 0) // the daemon arms abort handling
#define CLI_CMD_TAIL                                                           \
	(1u << 1)		  // app's arguments follow the first operand
#define CLI_CMD_LITERAL (1u << 2) // a leading '-' is the user's text
#define CLI_CMD_HIDDEN (1u << 3)  // not in the help
#define CLI_CMD_LOCAL                                                          \
	(1u << 4) // runs in the unprivileged CLI, no daemon needed

struct cli_cmd {
	cli_cmd_id_t id;
	const char* name;
	const char* alias; // a second spelling, or NULL
	const char* group;
	const char* summary;
	const char* args;      // operands as written in a Usage line, or NULL
	const char* desc;      // paragraphs, separated by '\n'; or NULL
	const cli_opt_t* opts; // own options, terminated by OPT_NONE
	int min_args;
	int max_args; // -1: unbounded
	unsigned flags;
};

#define MUT CLI_CMD_MUTATING

static const cli_cmd_t commands[] = {
	{CMD_INSTALL, "install", NULL, G_INSTALL,
	 "Add an application or runtime to WORLD and commit changes", "REF...",
	 "A REF is an application or runtime id, optionally written as "
	 "remote:id//branch. Dependencies, runtimes and extensions are "
	 "resolved from the ref's own metadata.\n"
	 "With --from, install a .flatpak bundle instead.",
	 install_opts, 0, 32, MUT},
	{CMD_IMPORT, "import", NULL, G_INSTALL,
	 "Register a deploy root's applications in the store and add them to "
	 "WORLD",
	 "PATH",
	 "Scan PATH, a deploy root laid out as app/ and runtime/, and "
	 "register what it holds in the store. An imported application is "
	 "kept as it is and never upgraded from a remote.",
	 no_opts, 1, 1, 0},
	{CMD_UNINSTALL, "uninstall", "remove", G_INSTALL,
	 "Remove an application or runtime from WORLD and commit changes",
	 "REF...",
	 "Uninstall each REF and drop it from the world file, asking first "
	 "unless -y is given. A location a running sandbox has mounted is "
	 "never removed.",
	 uninstall_opts, 1, 32, MUT},

	{CMD_UPDATE, "update", NULL, G_MAINT,
	 "Update installed applications from their remotes", "[REF]",
	 "Update REF, or every installed application when none is given. The "
	 "changeset is shown and confirmed before it is applied; the last "
	 "three deploy commits are kept for rollback.",
	 update_opts, 0, 1, MUT},
	{CMD_UPGRADE, "upgrade", NULL, G_MAINT,
	 "Re-solve WORLD and commit available upgrades", "[REF...]",
	 "Solve the world file against the remotes, then upgrade every ref "
	 "that is out of date, or only the REFs given. The changeset is "
	 "printed and confirmed before it is applied.",
	 upgrade_opts, 0, -1, MUT},
	{CMD_REPAIR, "repair", NULL, G_MAINT,
	 "Verify deploys and report the broken ones", "[REF...]",
	 "Report-only by design: act on the output with `flatpkg install "
	 "--reinstall`. A repair that repairs can destroy a working tree. "
	 "REFs are accepted for flatpak parity; the whole store is always "
	 "checked.",
	 repair_opts, 0, -1, CLI_CMD_LOCAL},
	{CMD_EXPORTS, "exports", NULL, G_MAINT,
	 "Rebuild exported desktop menu entries", NULL,
	 "Rebuild <store>/exports: relative symlinks, and .desktop files "
	 "rewritten to run through flatpkg. install and update do this "
	 "themselves; the world file is not touched.",
	 no_opts, 0, 0, 0},
	{CMD_ROLLBACK, "rollback", NULL, G_MAINT,
	 "Roll a ref back to a deploy kept from an earlier update and commit "
	 "changes",
	 "REF COMMIT",
	 "COMMIT is one of the deploys kept by update; `flatpkg list-commits "
	 "REF` lists them.",
	 no_opts, 2, 2, 0},

	{CMD_LIST, "list", NULL, G_QUERY, "List installed applications", NULL,
	 NULL, no_opts, 0, 0, CLI_CMD_LOCAL},
	{CMD_INFO, "info", NULL, G_QUERY,
	 "Show metadata and effective permissions", "APP-ID",
	 "Show the permissions the application actually runs with, overrides "
	 "included.",
	 no_opts, 1, 1, CLI_CMD_LOCAL},
	{CMD_SEARCH, "search", NULL, G_QUERY,
	 "Search configured remotes for applications", "TEXT",
	 "A TEXT that starts with '-' is searched for as it stands, not read "
	 "as an option.",
	 no_opts, 1, 1, CLI_CMD_LITERAL | CLI_CMD_LOCAL},
	{CMD_LIST_COMMITS, "list-commits", NULL, G_QUERY,
	 "List deploys kept for rollback", "REF", NULL, no_opts, 1, 1,
	 CLI_CMD_LOCAL},
	{CMD_HISTORY, "history", NULL, G_QUERY,
	 "Show the install, upgrade and uninstall log", NULL, NULL, no_opts, 0,
	 0, CLI_CMD_LOCAL},

	{CMD_RUN, "run", NULL, G_RUN,
	 "Build a jail from the runtime and run the application",
	 "APP-ID [ARGUMENTS...]",
	 "Build a jail around the application and run it; with the daemon up "
	 "it runs as the calling user. Options go before APP-ID. Everything "
	 "after APP-ID, or after --, is the application's own.\n"
	 "These are the per-launch grants. The rest of an application's "
	 "permissions are persistent: set them once with `flatpkg override`.",
	 run_opts, 1, 1, CLI_CMD_TAIL},
	{CMD_PS, "ps", NULL, G_RUN, "List running instances", NULL, NULL,
	 ps_opts, 0, 0, CLI_CMD_LOCAL},
	{CMD_ENTER, "enter", NULL, G_RUN, "Run a command in a running instance",
	 "INSTANCE [COMMAND...]",
	 "INSTANCE is an instance id or jail id from `flatpkg ps`. The "
	 "command runs with the environment the application was started "
	 "with. A jail flatpkg did not create is refused.",
	 no_opts, 1, 1, CLI_CMD_TAIL},
	{CMD_KILL, "kill", NULL, G_RUN, "Terminate a running instance",
	 "INSTANCE",
	 "INSTANCE is an instance id or jail id from `flatpkg ps`. A jail "
	 "flatpkg did not create is refused.",
	 no_opts, 1, 1, 0},
	{CMD_OVERRIDE, "override", NULL, G_RUN,
	 "Change an application's permissions persistently", "[APP-ID]",
	 "Write the change to flatpak's own override file, the one Flatseal "
	 "reads and writes. The name `global` applies to every application.",
	 override_opts, 0, 1, CLI_CMD_LOCAL},

	{CMD_REMOTE_ADD, "remote-add", NULL, G_REMOTE,
	 "Add a remote repository", "NAME [URL]",
	 "URL is the repository, or give --from a .flatpakrepo.",
	 remote_add_opts, 1, 2, 0},
	{CMD_REMOTE_LIST, "remote-list", "remotes", G_REMOTE,
	 "List configured remotes", NULL, NULL, no_opts, 0, 0, CLI_CMD_LOCAL},
	{CMD_REMOTE_DELETE, "remote-delete", NULL, G_REMOTE,
	 "Remove a remote repository", "NAME", NULL, no_opts, 1, 1, 0},

	{CMD_HELP, "help", NULL, G_MISC, "Show help for flatpkg or a command",
	 "[COMMAND]",
	 "With no COMMAND, list the commands, the global options and the "
	 "environment. `flatpkg COMMAND --help` shows the same page as "
	 "`flatpkg help COMMAND`.",
	 no_opts, 0, 1, 0},
	{CMD_COMPLETE, "complete", NULL, G_MISC, "List completion candidates",
	 "KIND [ARG]",
	 "KIND is apps, refs, runtimes, world, remotes, instances or "
	 "commands; words, values or man generate the option half of the "
	 "completions and the man page and are what the shell scripts call.",
	 no_opts, 1, 2, CLI_CMD_HIDDEN | CLI_CMD_LOCAL},
	{CMD_NONE, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, 0}};

static const cli_cmd_t* cli_find(const char* name) {
	assert(name != NULL);
	for (const cli_cmd_t* c = commands; c->name; c++)
		if (strcmp(name, c->name) == 0 ||
		    (c->alias && strcmp(name, c->alias) == 0))
			return c;
	return NULL;
}

cli_cmd_id_t cli_req_cmd(const cli_req_t* req) {
	assert(req != NULL);
	return req->cmd != NULL ? req->cmd->id : CMD_NONE;
}

bool fpkg_cmd_is_local(const char* cmd) {
	assert(cmd != NULL);
	const cli_cmd_t* c = cli_find(cmd);
	return c != NULL && (c->flags & CLI_CMD_LOCAL);
}

bool cli_req_is_local(const cli_req_t* r) {
	assert(r != NULL);
	if (r->name == NULL || !fpkg_cmd_is_local(r->name)) return false;
	// Writing or resetting a system override is the store's business.
	// Showing one is a read of a file the group can read.
	if (cli_req_cmd(r) == CMD_OVERRIDE && !r->over_user && !r->over_show)
		return false;
	return true;
}

static bool refuses_simulate(const char* typed) {
	assert(typed != NULL);
	for (const char* const* n = no_simulate; *n; n++)
		if (strcmp(typed, *n) == 0) return true;
	return false;
}

void cli_complete_commands(void) {
	for (const cli_cmd_t* c = commands; c->name; c++) {
		fp_outl("%s", c->name);
		if (c->alias) fp_outl("%s", c->alias);
	}
}

static const cli_opt_t* const opt_tables[] = {
	global_opts,  install_opts, uninstall_opts, update_opts,
	upgrade_opts, repair_opts,	 ps_opts,	remote_add_opts,
	run_opts,     override_opts, NULL};

static void choices_join(const cli_opt_t* o, char* out, size_t n);
static const char* const* opt_values(const cli_opt_t* o);
static bool opt_visible(const cli_opt_t* o);

static const cli_opt_t* find_opt(const char* name) {
	for (size_t t = 0; opt_tables[t] != NULL; t++)
		for (const cli_opt_t* o = opt_tables[t]; o->id != OPT_NONE; o++) {
			if (o->lname != NULL && strcmp(o->lname, name) == 0)
				return o;
			if (o->alt != NULL && strcmp(o->alt, name) == 0)
				return o;
			if (name[0] == '-' && name[1] != '\0' && name[2] == '\0' &&
			    o->sname == name[1])
				return o;
		}
	return NULL;
}

static void opt_words(const cli_opt_t* tab) {
	for (const cli_opt_t* o = tab; o->id != OPT_NONE; o++) {
		if (!opt_visible(o)) continue;
		if (o->sname != 0) fp_outl("-%c", o->sname);
		if (o->lname != NULL) fp_outl("%s%s", o->lname, o->arg ? "=" : "");
		if (o->alt != NULL) fp_outl("%s%s", o->alt, o->arg ? "=" : "");
	}
}

void cli_complete_words(const char* cmd) {
	const cli_cmd_t* c = cmd != NULL ? cli_find(cmd) : NULL;
	if (cmd != NULL && c == NULL) return;
	if (c != NULL) opt_words(c->opts);
	opt_words(global_opts);
}

void cli_complete_operands(const char* cmd) {
	const cli_cmd_t* c = cmd != NULL ? cli_find(cmd) : NULL;
	if (c == NULL) return;
	switch (c->id) {
	case CMD_INSTALL:
	case CMD_UPDATE:
	case CMD_UPGRADE:
	case CMD_ROLLBACK:
	case CMD_INFO:
	case CMD_SEARCH:
	case CMD_LIST_COMMITS:
	case CMD_OVERRIDE:
		fp_outl("refs");
		break;
	case CMD_UNINSTALL:
		fp_outl("world");
		break;
	case CMD_RUN:
		fp_outl("apps");
		break;
	case CMD_ENTER:
	case CMD_KILL:
		fp_outl("instances");
		break;
	case CMD_REMOTE_DELETE:
		fp_outl("remotes");
		break;
	case CMD_IMPORT:
	case CMD_REMOTE_ADD:
		fp_outl("files");
		break;
	default:
		break;
	}
}

void cli_complete_values(const char* opt) {
	const cli_opt_t* o = find_opt(opt);
	if (o == NULL) return;
	const char* const* set = o->choices != NULL ? o->choices : o->values;
	if (set == NULL) return;
	for (const char* const* v = set; *v != NULL; v++) fp_outl("%s", *v);
}

static void man_opt_head(const cli_opt_t* o) {
	char buf[256];
	size_t n = 0;
	buf[0] = '\0';
	if (o->sname != 0)
		n += (size_t)snprintf(buf + n, sizeof(buf) - n, ".Fl -%c",
				      o->sname);
	if (o->lname != NULL)
		n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s%s",
				      n ? " , Fl " : ".Fl ", o->lname + 1);
	if (o->alt != NULL)
		n += (size_t)snprintf(buf + n, sizeof(buf) - n, " , Fl %s",
				      o->alt + 1);
	if (o->arg != NULL)
		n += (size_t)snprintf(buf + n, sizeof(buf) - n,
				      o->lname != NULL ? " Ns = Ns Ar %s"
						       : " Ar %s",
				      o->arg);
	fp_outl("%s", buf);
}

void cli_man_options(const char* which) {
	const cli_opt_t* tab;
	bool list_only = false;
	if (strcmp(which, "global") == 0) {
		tab = global_opts;
	} else if (strcmp(which, "run") == 0) {
		tab = run_opts;
	} else if (strcmp(which, "override") == 0) {
		tab = override_opts;
		list_only = true;
	} else {
		fpkg_errorf("unknown man section '%s'", which);
		return;
	}

	bool first = true;
	for (const cli_opt_t* o = tab; o->id != OPT_NONE; o++) {
		if (!opt_visible(o)) continue;
		if (list_only) {
			if (o->lname != NULL)
				fp_outl(".Fl %s%s ,", o->lname + 1,
					o->arg != NULL ? "=" : "");
			continue;
		}
		if (!first) fp_outl(".Pp");
		first = false;
		man_opt_head(o);
		if (opt_values(o) != NULL) {
			char valid[256];
			choices_join(o, valid, sizeof(valid));
			fp_outl("%s: %s", get_string(o->help), valid);
		} else {
			fp_outl("%s", get_string(o->help));
		}
	}
}

static int cli_fail(const cli_cmd_t* cmd, const char* fmt, ...)
	__attribute__((format(printf, 2, 3)));
static void cli_help_main(bool to_stderr);
static void cli_help_cmd(const cli_cmd_t* cmd, bool to_stderr);

enum { CLI_POS = -1, CLI_ERR = -2, CLI_HELPED = -3 };

typedef struct {
	const cli_cmd_t* cmd;
	int argc;
	char* const* argv;
	int i;		 // next token
	const char* val; // value of the option just returned, or the operand
	bool quiet;	 // say nothing: the client and daemon read argv too
	bool done;
	bool literal; // past `--`: every token is an operand
	int npos;
	const char* extra; // first operand past the command's maximum
} cli_it_t;

static bool spells(const cli_opt_t* o, const char* a, bool* has_eq,
		   const char** eqv) {
	*has_eq = false;
	*eqv = NULL;
	if (a[1] == '-') {
		size_t n = strcspn(a, "=");
		const char* names[2] = {o->lname, o->alt};
		for (int k = 0; k < 2; k++) {
			if (names[k] == NULL) continue;
			if (strlen(names[k]) != n ||
			    strncmp(a, names[k], n) != 0)
				continue;
			*has_eq = a[n] == '=';
			if (*has_eq) *eqv = a + n + 1;
			return true;
		}
		return false;
	}
	return o->sname != 0 && a[1] == o->sname && a[2] == '\0';
}

static const cli_opt_t* match(const cli_opt_t* tab, const char* a, bool* has_eq,
			      const char** eqv) {
	for (const cli_opt_t* o = tab; o->id != OPT_NONE; o++)
		if (spells(o, a, has_eq, eqv)) return o;
	return NULL;
}

static const char* shown(const cli_opt_t* o, char* buf) {
	if (o->lname) return o->lname;
	buf[0] = '-';
	buf[1] = o->sname;
	buf[2] = '\0';
	return buf;
}

static bool is_help(const char* a) {
	return strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0;
}

static const char* const* opt_values(const cli_opt_t* o) {
	return o->choices != NULL ? o->choices : o->values;
}

static void choices_join(const cli_opt_t* o, char* out, size_t n) {
	size_t len = 0;
	out[0] = '\0';
	for (const char* const* c = opt_values(o); c != NULL && *c; c++)
		len += (size_t)snprintf(out + len, len < n ? n - len : 0,
					"%s%s", len ? ", " : "", *c);
}

static bool value_ok(const cli_opt_t* o, const char* v, char* bad,
		     size_t badn) {
	if (!(o->flags & OPTF_CHECK) || o->choices == NULL) return true;
	const char* p = v;
	for (;;) {
		size_t tlen =
			(o->flags & OPTF_LIST) ? strcspn(p, ",") : strlen(p);
		const char* item = p;
		size_t ilen = tlen;
		if ((o->flags & OPTF_NEG) && ilen > 0 && *item == '!') {
			item++;
			ilen--;
		}
		bool ok = false;
		for (const char* const* c = o->choices; *c; c++)
			if (strlen(*c) == ilen && strncmp(*c, item, ilen) == 0)
				ok = true;
		if (!ok) {
			snprintf(bad, badn, "%.*s", (int)tlen, p);
			return false;
		}
		p += tlen;
		if (*p != ',') return true;
		p++;
	}
}

static void cli_it_init(cli_it_t* it, const cli_cmd_t* cmd, int argc,
			char* const* argv, int first, bool quiet) {
	assert(it != NULL);
	assert(cmd != NULL);
	memset(it, 0, sizeof(*it));
	it->cmd = cmd;
	it->argc = argc;
	it->argv = argv;
	it->i = first;
	it->quiet = quiet;
}

// An option id (> 0), CLI_POS with it->val set, 0 at the end, CLI_ERR once
// the problem has been shown, or CLI_HELPED once --help has been answered.
static int cli_it_next(cli_it_t* it) {
	assert(it != NULL);
	const cli_cmd_t* c = it->cmd;

	if (it->done || it->i >= it->argc) {
		it->done = true;
		return 0;
	}
	const char* a = it->argv[it->i];
	if (!it->literal && strcmp(a, "--") == 0) {
		it->i++;
		it->literal = true;
		return cli_it_next(it);
	}

	bool dashed = !it->literal && a[0] == '-' && a[1] != '\0';
	if (dashed && (c->flags & CLI_CMD_LITERAL) && !is_help(a))
		dashed = false;
	if (!dashed) {
		it->i++;
		it->val = a;
		it->npos++;
		if (c->max_args >= 0 && it->npos > c->max_args &&
		    it->extra == NULL)
			it->extra = a;
		if (c->flags & CLI_CMD_TAIL) {
			// The rest is the application's; a `--` right after
			// the operand is the separator, not an argument.
			it->done = true;
			if (it->i < it->argc &&
			    strcmp(it->argv[it->i], "--") == 0)
				it->i++;
		}
		return CLI_POS;
	}

	bool has_eq;
	const char* eqv;
	char sb[3];
	const cli_opt_t* o = match(c->opts, a, &has_eq, &eqv);
	if (o == NULL) {
		o = match(global_opts, a, &has_eq, &eqv);
		// --version is a request of its own, not an option of a
		// command.
		if (o != NULL && o->id == OPT_VERSION) o = NULL;
	}
	it->done = true;
	if (o == NULL) {
		if (!it->quiet)
			cli_fail(c, "unknown option '%s' for '%s'", a, c->name);
		return CLI_ERR;
	}
	it->i++;
	if (o->unimpl != NULL) {
		if (!it->quiet)
			cli_fail(c, "%s is not implemented: %s", shown(o, sb),
				 o->unimpl);
		return CLI_ERR;
	}
	if (o->arg == NULL && has_eq) {
		if (!it->quiet)
			cli_fail(c, "option '%s' takes no argument",
				 shown(o, sb));
		return CLI_ERR;
	}
	it->val = NULL;
	if (o->id == OPT_HELP) {
		if (!it->quiet) cli_help_cmd(c, false);
		return CLI_HELPED;
	}
	if (o->arg != NULL) {
		if (has_eq) it->val = eqv;
		else if (it->i < it->argc)
			it->val = it->argv[it->i++];
		else {
			if (!it->quiet)
				cli_fail(c, "option '%s' requires an argument",
					 shown(o, sb));
			return CLI_ERR;
		}
		char bad[128], valid[256];
		if (!value_ok(o, it->val, bad, sizeof(bad))) {
			if (!it->quiet) {
				choices_join(o, valid, sizeof(valid));
				cli_fail(c, "invalid %s '%s' (valid: %s)",
					 o->lname + 2, bad, valid);
			}
			return CLI_ERR;
		}
	}
	it->done = false;
	return o->id;
}

static char* const* cli_it_rest(const cli_it_t* it) {
	return it->i < it->argc ? it->argv + it->i : NULL;
}

static int cli_it_finish(cli_it_t* it) {
	const cli_cmd_t* c = it->cmd;
	if (it->npos < c->min_args)
		return cli_fail(c, "missing argument for '%s'", c->name);
	if (it->extra != NULL && c->max_args == 0)
		return cli_fail(c, "'%s' takes no arguments, but got '%s'",
				c->name, it->extra);
	if (it->extra != NULL)
		return cli_fail(
			c, "too many arguments for '%s', first extra: '%s'",
			c->name, it->extra);
	return 0;
}

static void apply_global(cli_globals_t* g, cli_opt_id_t id, const char* val) {
	switch (id) {
	case OPT_VERBOSE:
		g->verbose = true;
		break;
	case OPT_OSTREE_VERBOSE:
		g->ostree_verbose = true;
		break;
	case OPT_PROFILE:
		g->profile = true;
		break;
	case OPT_NO_COLOR:
		g->color = 0;
		break;
	case OPT_SIMULATE:
		g->simulate = true;
		break;
	case OPT_LOGFMT:
		g->logfmt = val;
		break;
	case OPT_INSTALLATION:
		g->installation = val;
		break;
	case OPT_USER:
	case OPT_SYSTEM:
		g->installation = "default";
		break;
	default:
		break;
	}
}

static void cli_apply_env(cli_globals_t* g) {
	int env_color = -1;
	for (size_t k = 0; k < N_ENV; k++) {
		const cli_env_t* e = &env_vars[k];
		const char* v = getenv(e->name);
		if (e->opt == OPT_NONE || v == NULL || *v == '\0') continue;
		if (e->kind == ENV_BOOL && strchr("1tTyY", *v) == NULL)
			continue;
		if (e->kind == ENV_EQ && strcmp(v, e->eq) != 0) continue;
		switch (e->opt) {
		case OPT_NO_COLOR:
			env_color = 0;
			break;
		case OPT_FORCE_COLOR:
			env_color = 1;
			break;
		case OPT_LOGFMT:
			if (g->logfmt == NULL) g->logfmt = v;
			break;
		default:
			apply_global(g, e->opt, v);
			break;
		}
	}
	if (g->color < 0) g->color = env_color;
}

// Walk argv like a command line: globals before the command apply, the terminal
// ones stop the walk, and after it only non-terminal ones do, never past the app's operands.
static int cli_scan(int argc, char* const* argv, int first, cli_globals_t* g,
		    bool report) {
	assert(argv != NULL);
	assert(g != NULL);
	memset(g, 0, sizeof(*g));
	g->cmd = -1;
	g->color = -1;

	for (int j = first; j < argc; j++) {
		const char* a = argv[j];
		bool has_eq;
		const char* eqv;
		char sb[3];

		if (strcmp(a, "--") == 0) return 0;
		if (a[0] != '-' || a[1] == '\0') {
			g->cmd = j;
			break;
		}
		const cli_opt_t* o = match(global_opts, a, &has_eq, &eqv);
		if (o == NULL) {
			// An option before any command is the command word as
			// far as the client and the daemon are concerned.
			g->cmd = j;
			g->unknown_opt = true;
			if (!report) return 0;
			return cli_fail(NULL, "unknown option '%s'", a);
		}
		const char* val = NULL;
		if (o->arg == NULL && has_eq) {
			if (!report) return -1;
			return cli_fail(NULL, "option '%s' takes no argument",
					shown(o, sb));
		}
		if (o->arg != NULL) {
			if (has_eq) val = eqv;
			else if (j + 1 < argc)
				val = argv[++j];
			else {
				if (!report) return -1;
				return cli_fail(NULL,
						"option '%s' requires an "
						"argument",
						shown(o, sb));
			}
		}
		// Before the command --help and --version are the whole
		// request: `flatpkg --version list` prints the version.
		if (o->id == OPT_HELP) {
			g->help = true;
			return 0;
		}
		if (o->id == OPT_VERSION) {
			g->version = true;
			return 0;
		}
		apply_global(g, o->id, val);
	}
	if (g->cmd < 0) return 0;

	const cli_cmd_t* c = cli_find(argv[g->cmd]);
	if (c == NULL) return 0;
	cli_it_t it;
	cli_it_init(&it, c, argc, argv, g->cmd + 1, true);
	int r;
	while ((r = cli_it_next(&it)) != 0) {
		if (r == CLI_ERR || r == CLI_HELPED) break;
		if (r > 0) apply_global(g, (cli_opt_id_t)r, it.val);
	}
	return 0;
}

const char* cli_run_app_id(int argc, char* const* argv) {
	assert(argv != NULL);
	if (argc < 3 || strcmp(argv[1], "run") != 0) return NULL;
	cli_it_t it;
	cli_it_init(&it, cli_find("run"), argc, argv, 2, true);
	int o;
	while ((o = cli_it_next(&it)) != 0) {
		if (o == CLI_ERR || o == CLI_HELPED) return NULL;
		if (o == CLI_POS) return it.val;
	}
	return NULL;
}


static void push(strlist_t* l, const char* item) {
	char** items = realloc(l->items, (l->count + 1) * sizeof(char*));
	if (items == NULL || (items[l->count] = strdup(item)) == NULL) {
		fpkg_errorf("out of memory");
		exit(1);
	}
	l->items = items;
	l->count++;
}

static void set_str(char** dst, const char* v) {
	free(*dst);
	if ((*dst = strdup(v)) == NULL) {
		fpkg_errorf("out of memory");
		exit(1);
	}
}

static void ops_add(cli_req_t* r, const char* v) {
	const char** ops = realloc((void*)r->ops, (r->nops + 1) * sizeof(*ops));
	if (ops == NULL) {
		fpkg_errorf("out of memory");
		exit(1);
	}
	r->ops = ops;
	r->ops[r->nops++] = v;
}

// --share / --unshare keep flatpak's vocabulary; `process` is inherent (every
// sandbox is a jail) and `all` expands to the two that can be switched off.
static int run_share(cli_req_t* r, const char* v, bool unshare) {
	bool neg = v[0] == '!';
	const char* n = neg ? v + 1 : v;
	const char* flag = unshare ? "--unshare" : "--share";

	if (strcmp(n, "network") != 0 && strcmp(n, "ipc") != 0 &&
	    strcmp(n, "process") != 0 && strcmp(n, "all") != 0)
		return cli_fail(r->cmd,
				"invalid %s value '%s' (valid: network, ipc, "
				"process, all)",
				flag, v);
	if (!unshare && (strcmp(n, "process") == 0 || strcmp(n, "all") == 0))
		return cli_fail(r->cmd,
				"--share=%s is not available: every flatpkg "
				"sandbox is its own jail(8) jail, so the host "
				"process namespace is never shared; "
				"--unshare=process is accepted and is already "
				"the case",
				n);
	if (neg && unshare)
		return cli_fail(r->cmd, "invalid --unshare value '%s'", v);
	if (strcmp(n, "all") == 0) {
		push(&r->run.shares, "!network");
		push(&r->run.shares, "!ipc");
	} else if (unshare) {
		char buf[32];
		snprintf(buf, sizeof(buf), "!%s", n);
		push(&r->run.shares, buf);
	} else {
		push(&r->run.shares, v);
	}
	return 0;
}

static int apply_run(cli_req_t* r, int id, const char* v) {
	switch (id) {
	case OPT_SOCKET:
		push(&r->run.sockets, v);
		break;
	case OPT_DEVICE:
		push(&r->run.devices, v);
		break;
	case OPT_SHARE:
		return run_share(r, v, false);
	case OPT_UNSHARE:
		return run_share(r, v, true);
	case OPT_FILESYSTEM:
		push(v[0] == '!' ? &r->run.fs_remove : &r->run.fs_add, v);
		break;
	case OPT_COMMAND:
		set_str(&r->run.command, v);
		break;
	case OPT_ENV:
		if (strchr(v, '=') == NULL)
			return cli_fail(r->cmd, "option --env requires "
						"KEY=VALUE");
		push(&r->run.envs, v);
		break;
	case OPT_BRANCH:
		set_str(&r->run.branch, v);
		break;
	case OPT_ARCH:
		set_str(&r->run.arch, v);
		break;
	case OPT_APPSTREAM:
		r->appstream = true;
		break;
	case OPT_REMOTE:
		r->remote = v;
		break;
	case OPT_DEVEL:
		r->run.devel = true;
		break;
	case OPT_CLEARENV:
		r->run.clearenv = true;
		break;
	case OPT_DIE_WITH_PARENT:
		r->run.die_with_parent = true;
		break;
	case OPT_FILE_FORWARDING:
		r->run.file_forwarding = true;
		break;
	default:
		break;
	}
	return 0;
}

// An override is a metadata keyfile's worth of changes; a leading `!` removes an entry.
static int over_add(cli_req_t* r, strlist_t* l, const char* v, bool neg,
		    const char* policy) {
	char buf[1024];
	if (*v == '\0')
		return cli_fail(r->cmd, "an empty value is not an override");
	if (policy != NULL) snprintf(buf, sizeof(buf), "%s=%s", v, policy);
	else if (neg)
		snprintf(buf, sizeof(buf), "!%s", v);
	else
		snprintf(buf, sizeof(buf), "%s", v);
	push(l, buf);
	return 0;
}

static int apply_override(cli_req_t* r, int id, const char* v) {
	fp_metadata_t* m = &r->over;
	switch (id) {
	case OPT_USER:
		r->over_user = true;
		return 0;
	case OPT_SYSTEM:
		r->over_user = false;
		return 0;
	case OPT_SHOW:
		r->over_show = true;
		return 0;
	case OPT_RESET:
		r->over_reset = true;
		return 0;
	case OPT_SHARE:
		return over_add(r, &m->shared, v, false, NULL);
	case OPT_UNSHARE:
		return over_add(r, &m->shared, v, true, NULL);
	case OPT_SOCKET:
		return over_add(r, &m->sockets, v, false, NULL);
	case OPT_NOSOCKET:
		return over_add(r, &m->sockets, v, true, NULL);
	case OPT_DEVICE:
		return over_add(r, &m->devices, v, false, NULL);
	case OPT_NODEVICE:
		return over_add(r, &m->devices, v, true, NULL);
	case OPT_ALLOW:
		return over_add(r, &m->features, v, false, NULL);
	case OPT_DISALLOW:
		return over_add(r, &m->features, v, true, NULL);
	case OPT_FILESYSTEM:
		return over_add(r, &m->filesystems, v, false, NULL);
	case OPT_NOFILESYSTEM:
		return over_add(r, &m->filesystems, v, true, NULL);
	case OPT_PERSIST:
		return over_add(r, &m->persistent, v, false, NULL);
	case OPT_ENV:
		return over_add(r, &m->env_pairs, v, false, NULL);
	case OPT_UNSET_ENV:
		return over_add(r, &m->unset_environment, v, false, NULL);
	case OPT_TALK_NAME:
		return over_add(r, &m->session_bus_policy, v, false, "talk");
	case OPT_OWN_NAME:
		return over_add(r, &m->session_bus_policy, v, false, "own");
	case OPT_NO_TALK_NAME:
		return over_add(r, &m->session_bus_policy, v, false, "none");
	case OPT_SYSTEM_TALK_NAME:
		return over_add(r, &m->system_bus_policy, v, false, "talk");
	case OPT_SYSTEM_OWN_NAME:
		return over_add(r, &m->system_bus_policy, v, false, "own");
	case OPT_SYSTEM_NO_TALK_NAME:
		return over_add(r, &m->system_bus_policy, v, false, "none");
	case OPT_A11Y_OWN_NAME:
		return over_add(r, &m->a11y_bus_policy, v, false, "own");
	default:
		return 0;
	}
}

static int apply_opt(cli_req_t* r, int id, const char* v) {
	switch (r->cmd->id) {
	case CMD_RUN:
		return apply_run(r, id, v);
	case CMD_OVERRIDE:
		return apply_override(r, id, v);
	default:
		break;
	}
	switch (id) {
	case OPT_YES:
	case OPT_NONINTERACTIVE:
		r->assume_yes = true;
		break;
	case OPT_RDEPENDS:
		r->rdepends = true;
		break;
	case OPT_RUNTIME:
		r->runtime = true;
		break;
	case OPT_PRUNE:
		r->prune = true;
		break;
	case OPT_AVAILABLE:
		r->available = true;
		break;
	case OPT_REINSTALL:
		r->reinstall = true;
		break;
	case OPT_COMMIT:
		r->commit = v;
		break;
	case OPT_FROM:
		r->from = v;
		break;
	case OPT_COLUMNS:
		r->columns = v;
		break;
	case OPT_APPSTREAM:
		r->appstream = true;
		break;
	case OPT_REMOTE:
		r->remote = v;
		break;
	case OPT_ARCH:
		set_str(&r->run.arch, v);
		break;
	default:
		break; // a global, already applied; or a no-op flag
	}
	return 0;
}

void cli_req_free(cli_req_t* r) {
	assert(r != NULL);
	strlist_t* lists[] = {&r->run.sockets,	 &r->run.devices,
			      &r->run.shares,	 &r->run.fs_add,
			      &r->run.fs_remove, &r->run.envs};
	for (size_t k = 0; k < sizeof(lists) / sizeof(lists[0]); k++) {
		for (size_t i = 0; i < lists[k]->count; i++)
			free(lists[k]->items[i]);
		free(lists[k]->items);
	}
	free(r->run.command);
	free(r->run.branch);
	free(r->run.arch);
	free((void*)r->ops);
	fp_metadata_free(&r->over);
	memset(r, 0, sizeof(*r));
}

// The host runs flatpak under a different name sometimes; flatpkg has one
// installation and accepts the names stock tooling passes for it.
static bool installation_ok(const char* name) {
	return name == NULL || *name == '\0' || strcmp(name, "default") == 0 ||
	       strcmp(name, "user") == 0;
}

int cli_parse(int argc, char* const* argv, cli_req_t* r) {
	assert(argv != NULL);
	assert(r != NULL);
	memset(r, 0, sizeof(*r));
	r->over_user = true;

	if (cli_scan(argc, argv, 1, &r->g, true) != 0) return 1;
	cli_apply_env(&r->g);
	{
		const char* _loc = getenv("LC_ALL");
		if (_loc == NULL || *_loc == '\0') _loc = getenv("LC_MESSAGES");
		if (_loc == NULL || *_loc == '\0') _loc = getenv("LANG");
		translations_set_locale(_loc);
	}
	if (r->g.color >= 0) fp_set_color(r->g.color == 1);
	fp_set_term_dumb(getenv("TERM") != NULL &&
			 strcmp(getenv("TERM"), "dumb") == 0);
	if (r->g.verbose) fp_set_verbose(true);

	if (r->g.version) {
		fp_outl("flatpkg %s", FLATPKG_VERSION);
		return 0;
	}
	if (r->g.help) {
		cli_help_main(false);
		return 0;
	}
	if (r->g.cmd < 0) {
		cli_help_main(true);
		return 1;
	}
	r->name = argv[r->g.cmd];
	r->cmd = cli_find(r->name);
	if (r->cmd == NULL) {
		cli_fail(NULL, "unknown command '%s'", r->name);
		return 1;
	}
	const cli_cmd_t* c = r->cmd;

	if (!installation_ok(r->g.installation)) {
		fpkg_errorf(
			"installation '%s' does not exist (flatpkg has one: "
			"'default')",
			r->g.installation);
		return 1;
	}
	if (r->g.simulate && refuses_simulate(r->name)) {
		fpkg_errorf("--simulate is not implemented for `%s`; run it "
			    "without -n to actually %s",
			    r->name, r->name);
		return 1;
	}
	cli_it_t it;
	cli_it_init(&it, c, argc, argv, r->g.cmd + 1, false);
	int o;
	while ((o = cli_it_next(&it)) != 0) {
		if (o == CLI_HELPED) return 0;
		if (o == CLI_ERR) return 1;
		if (o == CLI_POS) ops_add(r, it.val);
		else if (apply_opt(r, o, it.val) != 0)
			return 1;
	}
	if (cli_it_finish(&it) != 0) return 1;
	if (c->flags & CLI_CMD_TAIL) r->rest = cli_it_rest(&it);

	if (c->id == CMD_HELP) {
		if (r->nops == 0) {
			cli_help_main(false);
			return 0;
		}
		const cli_cmd_t* topic = cli_find(r->ops[0]);
		if (topic == NULL) {
			cli_fail(NULL, "unknown command '%s'", r->ops[0]);
			return 1;
		}
		cli_help_cmd(topic, false);
		return 0;
	}
	return CLI_PROCEED;
}

// Pages are rendered into one buffer and written once - stdout when asked
// for, stderr under an error - so a page never splits between the streams.

typedef struct {
	char* p;
	size_t n, cap;
	bool color;
} buf_t;

static void b_put(buf_t* b, const char* s, size_t n) {
	if (b->n + n + 1 > b->cap) {
		b->cap = (b->n + n + 1) * 2;
		b->p = realloc(b->p, b->cap);
		if (b->p == NULL) exit(1);
	}
	memcpy(b->p + b->n, s, n);
	b->n += n;
	b->p[b->n] = '\0';
}

static void b_str(buf_t* b, const char* s) { b_put(b, s, strlen(s)); }

static void b_pad(buf_t* b, size_t n) {
	while (n-- > 0) b_put(b, " ", 1);
}

static void b_fmt(buf_t* b, const char* fmt, ...)
	__attribute__((format(printf, 2, 3)));
static void b_fmt(buf_t* b, const char* fmt, ...) {
	char tmp[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	b_str(b, tmp);
}

#define C_HEAD (FP_LEVELS[FP_LVL_OKAY].bg)  // group headings: green bg
#define C_CMD (FP_LEVELS[FP_LVL_INFO].msg)  // command names: cyan
#define C_OPT (FP_LEVELS[FP_LVL_WARN].msg)  // option names: yellow
#define C_ARG (FP_LEVELS[FP_LVL_DEBUG].msg) // descriptions: dimmed blue
#define C_ENV (FP_LEVELS[FP_LVL_WARN].msg)  // env vars: yellow

static void st_bold(buf_t* b, int c) {
	if (b->color) b_fmt(b, FP_SGR_BOLD_FG256, c);
}
static void st_fg(buf_t* b, int c) {
	if (b->color) b_fmt(b, FP_SGR_FG256, c);
}
static void st_dim(buf_t* b, int c) {
	if (b->color) b_fmt(b, FP_SGR_DIM_FG256, c);
}
static void st_off(buf_t* b) {
	if (b->color) b_str(b, FP_SGR_RESET);
}

static void b_heading(buf_t* b, const char* text) {
	st_bold(b, C_HEAD);
	b_str(b, text);
	b_str(b, ":");
	st_off(b);
	b_str(b, "\n");
}

static void b_wrap(buf_t* b, const char* text, size_t indent, size_t col,
		   size_t width) {
	bool line_start = true;
	const char* p = text;
	for (;;) {
		while (*p == ' ') p++;
		const char* w = p;
		while (*p != '\0' && *p != ' ') p++;
		size_t len = (size_t)(p - w);
		if (len == 0) break;
		if (!line_start && col + 1 + len > width) {
			b_str(b, "\n");
			b_pad(b, indent);
			col = indent;
			line_start = true;
		}
		if (!line_start) {
			b_str(b, " ");
			col++;
		}
		b_put(b, w, len);
		col += len;
		line_start = false;
	}
	b_str(b, "\n");
}

// Colour where it can be seen: a terminal, a client with one behind the daemon, or FORCE_COLOR.
static bool help_color(int fd) {
	if (!fp_get_color()) return false;
	const char* fc = cli_getenv("FORCE_COLOR");
	const char* in = cli_getenv("FLATPKG_INTERACTIVE");
	return fp_fd_is_tty(fd) || (fc != NULL && *fc != '\0') ||
	       (in != NULL && *in == '1');
}

static size_t help_width(void) {
	size_t w = fp_term_cols();
	if (w < 60) w = 60;
	if (w > 100) w = 100;
	return w;
}

static void emit(buf_t* b, bool to_stderr) {
	if (to_stderr) fp_err_raw(b->p, b->n);
	else
		fp_out_raw(b->p, b->n);
	free(b->p);
}

static const char* arch_name(void) {
#if defined(__amd64__)
	return "x86_64";
#elif defined(__aarch64__)
	return "arm64";
#elif defined(__i386__)
	return "i386";
#elif defined(__riscv)
	return "riscv64";
#else
	return NULL;
#endif
}

static void opt_spec(const cli_opt_t* o, char* out, size_t n, size_t* ph) {
	size_t len = 0;
	out[0] = '\0';
	if (o->sname != 0)
		len += (size_t)snprintf(out + len, n - len, "-%c", o->sname);
	if (o->lname != NULL) {
		len += (size_t)snprintf(out + len, n - len, "%s%s",
					len ? ", " : "", o->lname);
		if (o->alt != NULL)
			len += (size_t)snprintf(out + len, n - len, ", %s",
						o->alt);
		*ph = len;
		if (o->arg != NULL)
			len += (size_t)snprintf(out + len, n - len, "=%s",
						o->arg);
	} else {
		*ph = len;
		if (o->arg != NULL)
			len += (size_t)snprintf(out + len, n - len, " %s",
						o->arg);
	}
}

static bool opt_visible(const cli_opt_t* o) {
	return o->help != NULL && o->unimpl == NULL;
}

static void b_row(buf_t* b, const char* left, size_t ph, int color, size_t col,
		  const char* right, size_t width) {
	size_t l = strlen(left);
	if (ph > l) ph = l;
	b_str(b, "  ");
	st_fg(b, color);
	b_put(b, left, ph);
	st_off(b);
	st_dim(b, C_ARG);
	b_put(b, left + ph, l - ph);
	st_off(b);
	if (l + 2 + 2 > col) {
		b_str(b, "\n");
		b_pad(b, col);
	} else {
		b_pad(b, col - 2 - l);
	}
	b_wrap(b, right, col, col, width);
}

static size_t opts_column(const cli_opt_t* tab) {
	size_t max = 0, ph;
	char spec[128];
	for (const cli_opt_t* o = tab; o->id != OPT_NONE; o++) {
		if (!opt_visible(o)) continue;
		opt_spec(o, spec, sizeof(spec), &ph);
		if (strlen(spec) > max) max = strlen(spec);
	}
	// Two of indent, the widest spec, two of gap; a spec past 28 puts its
	// description on the next line instead of pushing every row right.
	if (max > 28) max = 28;
	return 2 + max + 2;
}

static void b_opts(buf_t* b, const cli_opt_t* tab, size_t col, size_t width) {
	char spec[128], desc[512], valid[256];
	size_t ph;
	for (const cli_opt_t* o = tab; o->id != OPT_NONE; o++) {
		if (!opt_visible(o)) continue;
		opt_spec(o, spec, sizeof(spec), &ph);
		if (opt_values(o) != NULL) {
			choices_join(o, valid, sizeof(valid));
			snprintf(desc, sizeof(desc), "%s: %s",
				 get_string(o->help), valid);
		} else {
			snprintf(desc, sizeof(desc), "%s", get_string(o->help));
		}
		b_row(b, spec, ph, C_OPT, col, desc, width);
	}
}

static bool has_visible_opts(const cli_cmd_t* c) {
	for (const cli_opt_t* o = c->opts; o->id != OPT_NONE; o++)
		if (opt_visible(o)) return true;
	return false;
}

static void b_usage(buf_t* b, const cli_cmd_t* c) {
	st_bold(b, C_HEAD);
	b_str(b, "Usage:");
	st_off(b);
	b_str(b, " flatpkg");
	if (c == NULL) {
		b_str(b, " [<GLOBAL OPTIONS>...] ");
		st_fg(b, C_CMD);
		b_str(b, "COMMAND");
		st_off(b);
		b_str(b, " [<OPTIONS>...] [<ARGUMENTS>...]\n");
		return;
	}
	b_str(b, " ");
	st_fg(b, C_CMD);
	b_str(b, c->name);
	st_off(b);
	if (has_visible_opts(c)) b_str(b, " [<OPTIONS>...]");
	if (c->args != NULL) {
		b_str(b, " ");
		st_dim(b, C_ARG);
		b_str(b, c->args);
		st_off(b);
	}
	b_str(b, "\n");
}

static void cli_help_main(bool to_stderr) {
	int fd = to_stderr ? STDERR_FILENO : STDOUT_FILENO;
	buf_t b = {NULL, 0, 0, help_color(fd)};
	size_t width = help_width();
	const char* arch = arch_name();

	st_bold(&b, C_HEAD);
	b_str(&b, "flatpkg ");
	b_str(&b, FLATPKG_VERSION);
	st_off(&b);
	st_dim(&b, C_ARG);
	b_str(&b, ", compiled for FreeBSD");
	if (arch != NULL) b_fmt(&b, "/%s", arch);
	b_str(&b, ".");
	st_off(&b);
	b_str(&b, "\n\n");
	b_usage(&b, NULL);
	b_str(&b, "\n");

	size_t name_w = 0;
	for (const cli_cmd_t* c = commands; c->name; c++)
		if (!(c->flags & CLI_CMD_HIDDEN) && strlen(c->name) > name_w)
			name_w = strlen(c->name);
	size_t col = 2 + name_w + 2;

	for (const char* const* g = groups; *g; g++) {
		b_heading(&b, *g);
		for (const cli_cmd_t* c = commands; c->name; c++) {
			if ((c->flags & CLI_CMD_HIDDEN) ||
			    strcmp(c->group, *g) != 0)
				continue;
			b_row(&b, c->name, strlen(c->name), C_CMD, col,
			      get_string(c->summary), width);
		}
		b_str(&b, "\n");
	}

	b_heading(&b, "Global options");
	b_opts(&b, global_opts, opts_column(global_opts), width);
	b_str(&b, "\n");

	size_t ecol = 0;
	for (size_t k = 0; k < N_ENV; k++)
		if (env_vars[k].help != NULL && strlen(env_vars[k].name) > ecol)
			ecol = strlen(env_vars[k].name);
	if (ecol > 28) ecol = 28;
	ecol += 4;
	b_heading(&b, "Environment");
	for (size_t k = 0; k < N_ENV; k++)
		if (env_vars[k].help != NULL)
			b_row(&b, env_vars[k].name, strlen(env_vars[k].name),
			      C_ENV, ecol, get_string(env_vars[k].help), width);
	b_str(&b, "\n");

	b_str(&b, "For more information: man 8 flatpkg\n");
	emit(&b, to_stderr);
}

static void cli_help_cmd(const cli_cmd_t* c, bool to_stderr) {
	assert(c != NULL);
	int fd = to_stderr ? STDERR_FILENO : STDOUT_FILENO;
	buf_t b = {NULL, 0, 0, help_color(fd)};
	size_t width = help_width();

	b_usage(&b, c);
	b_str(&b, "\n");

	b_heading(&b, "Description");
	b_str(&b, "  ");
	b_wrap(&b, get_string(c->summary), 2, 2, width);
	if (c->desc != NULL) {
		const char* p = get_string(c->desc);
		while (*p != '\0') {
			size_t n = strcspn(p, "\n");
			char* para = strndup(p, n);
			if (para == NULL) exit(1);
			b_str(&b, "\n  ");
			b_wrap(&b, para, 2, 2, width);
			free(para);
			p += n;
			if (*p == '\n') p++;
		}
	}
	if (c->alias != NULL) {
		b_str(&b, "\n  Also spelled ");
		st_fg(&b, C_CMD);
		b_str(&b, c->alias);
		st_off(&b);
		b_str(&b, ".\n");
	}

	if (has_visible_opts(c)) {
		b_str(&b, "\n");
		b_heading(&b, "Options");
		b_opts(&b, c->opts, opts_column(c->opts), width);
	}
	b_str(&b, "\n");
	b_heading(&b, "Global options");
	b_opts(&b, global_opts, opts_column(global_opts), width);
	b_str(&b, "\n");
	st_dim(&b, C_ARG);
	b_str(&b, "For the environment, see `flatpkg --help`.\n");
	st_off(&b);
	emit(&b, to_stderr);
}

static void cli_hint(const cli_cmd_t* c) {
	buf_t b = {NULL, 0, 0, help_color(STDERR_FILENO)};
	b_usage(&b, c);
	if (c == NULL)
		b_str(&b, "Try `flatpkg --help` for the list of commands.\n");
	else
		b_fmt(&b, "Try `flatpkg help %s` for more information.\n",
		      c->name);
	emit(&b, true);
}

static int cli_fail(const cli_cmd_t* c, const char* fmt, ...) {
	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	fpkg_errorf("%s", msg);
	cli_hint(c);
	return -1;
}
