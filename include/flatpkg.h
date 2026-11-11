#ifndef FLATPKG_H
#define FLATPKG_H

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "fplog.h"

#define FLATPKG_VERSION "0.1.0"

// A compile-time setting; nothing at run time can move it for root.
#ifndef FLATPKG_STORE_DIR
#define FLATPKG_STORE_DIR "/var/db/flatpkg"
#endif
#define FLATPKG_RUN_DIR "/var/run/flatpkg"
#define FLATPKG_JAIL_NAME_PREFIX "flatpkg-"
// The jail's `meta` holds "<this>=<uid>", read back by enter and kill.
#define FLATPKG_JAIL_OWNER_KEY "flatpkg-uid"
#define FLATPKG_RULESET_BASE 1000
#define FLATPKG_RULESET_MIN 1100
#define FLATPKG_RULESET_MAX 1999
#define FLATPKG_MAX_LIST 256

#define FLATPKG_KEEP_DEPLOYS 3

// The owner worker sets this from SIGUSR1; long operations poll it.  The
// definition is shared, so the CLI's copy is simply always false.
extern volatile sig_atomic_t fpkg_cancel_flag;

// True after the broker asked this worker to stop (FP_REQ_CANCEL).
int fpkg_cancel_requested(void);

// pkg-style errors; FPKG_OK is 0 so existing !=0 checks keep working.
typedef enum {
	FPKG_OK = 0,
	FPKG_FATAL = -1,
	FPKG_ERR = -2,
	FPKG_ENOENT = -3,
	FPKG_EINVAL = -5,
	FPKG_ENOMEM = -6,
	FPKG_EACCES = -7,
	FPKG_EIO = -8,
} fpkg_error_t;

// Small shared helpers, once copied into most modules.
char* xstrdup(const char* s);
char* xasprintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Strip whitespace in place; returns the new start.  Header-inline because a
// parser that only needs this should not have to link the file helpers.
static inline char* trim(char* s) {
	assert(s != NULL);
	char* p = s;
	while (*p && isspace((unsigned char)*p)) p++;
	if (p != s) memmove(s, p, strlen(p) + 1);
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
	return s;
}
fpkg_error_t mkdir_p(const char* path, mode_t mode);
// Copy a regular file, creating `to`; errno is set on failure.
fpkg_error_t copy_file(const char* from, const char* to, mode_t mode);
// Append formatted text to a bounded buffer, refusing rather than truncating.
// snprintf's return is the length it *wanted*, so adding it back into the next
// size argument underflows once the buffer is full; this never does.
bool fpkg_buf_append(char* buf, size_t len, size_t* off, const char* fmt,
		     ...) __attribute__((format(printf, 4, 5)));
// chown without following a symlink.  For a path the caller can swap.
fpkg_error_t chown_nofollow(const char* path, uid_t uid, gid_t gid);
// Replace `path` with `content`.
fpkg_error_t write_file(const char* path, const char* content, mode_t mode);
// Remove a tree; symlinks are not followed and other mounts are not crossed.
int rm_rf(const char* path);
// Fork/exec a program with no shell; fds < 0 inherit.  Returns its exit
// status, or -1 when it could not be run.
int run_program(char* const argv[], int in_fd, int out_fd, int err_fd);

static inline fpkg_error_t fpkg_error_from_errno(int e) {
	switch (e) {
	case 0:
		return FPKG_OK;
	case ENOENT:
		return FPKG_ENOENT;
	case EINVAL:
		return FPKG_EINVAL;
	case ENOMEM:
		return FPKG_ENOMEM;
	case EACCES:
	case EPERM:
		return FPKG_EACCES;
	case EIO:
		return FPKG_EIO;
	default:
		return FPKG_ERR;
	}
}

typedef struct {
	char* buf;
	size_t len, cap;
} fpkg_sbuf_t;

static inline fpkg_error_t fpkg_sbuf_ensure(fpkg_sbuf_t* sb, size_t need) {
	if (need <= sb->cap) return FPKG_OK;
	size_t ncap = sb->cap ? sb->cap * 2 : 256;
	while (ncap < need) ncap *= 2;
	char* p = realloc(sb->buf, ncap);
	if (!p) return FPKG_ENOMEM;
	sb->buf = p;
	sb->cap = ncap;
	return FPKG_OK;
}

static inline fpkg_error_t fpkg_sbuf_append(fpkg_sbuf_t* sb, const char* s,
					    size_t n) {
	if (fpkg_sbuf_ensure(sb, sb->len + n + 1) != FPKG_OK)
		return FPKG_ENOMEM;
	memcpy(sb->buf + sb->len, s, n);
	sb->len += n;
	sb->buf[sb->len] = '\0';
	return FPKG_OK;
}

static inline void fpkg_sbuf_free(fpkg_sbuf_t* sb) {
	free(sb->buf);
	sb->buf = NULL;
	sb->len = sb->cap = 0;
}

typedef struct {
	char** items;
	size_t count;
} strlist_t;

typedef struct {
	char* id;
	char* version;
	strlist_t versions; // versions= (semicolon list, one ref per branch)
	char* directory;
	char* subdir_suffix;
	bool subdirectories;
	bool needs_tmpfs;
	char* add_ld_path;
	char* download_if;
	char* enable_if;
	strlist_t merge_dirs;
	bool autodelete;
	bool no_autodownload;
} fp_ext_group_t;

bool ext_id_matches_declared(const char* id, const char* branch,
			     const fp_ext_group_t* g);

bool remote_name_valid(const char* name);

// One "[ExtensionOf <parent>]" group; priority= orders extension handling.
typedef struct {
	char* parent;
	int priority;
	char* ref;
} fp_extof_t;

typedef struct {
	char* app_id;
	char* version;
	char* runtime_ref;
	char* sdk_ref;
	char* command;
	char* branch;

	strlist_t required_flatpak;
	strlist_t tags;

	strlist_t shared;
	strlist_t sockets;
	strlist_t devices;
	strlist_t filesystems;
	strlist_t persistent;
	strlist_t features;

	strlist_t env_pairs;

	// [Context] unset-environment=: names to unset in the child after
	// [Environment] is applied, so it overrides both sources.
	strlist_t unset_environment;

	strlist_t session_bus_talk;
	strlist_t session_bus_own;

	// [Session|System|A11y Bus Policy]: "NAME=policy", none/see/talk/own.
	strlist_t session_bus_policy;
	strlist_t system_bus_policy;
	strlist_t a11y_bus_policy;

	fp_ext_group_t* ext_groups;
	size_t next_groups;
	fp_extof_t* extof_groups;
	size_t nextof_groups;
} fp_metadata_t;

int fp_metadata_parse(const char* path, fp_metadata_t* out);
int fp_metadata_parse_any(const char* path, fp_metadata_t* out);
void fp_metadata_free(fp_metadata_t* m);

typedef enum {
	FS_GRANT_RO,
	FS_GRANT_RW,
	FS_GRANT_CREATE,
	FS_GRANT_HIDE,
} fs_grant_mode_t;

typedef struct {
	char* host_path;
	char* jail_path;
	fs_grant_mode_t mode;
	bool persistent;
} fs_grant_t;

typedef struct {
	fs_grant_t* grants;
	size_t ngrants;

	bool want_network;
	bool want_ipc;

	bool dev_dri, dev_kvm, dev_input, dev_usb, dev_shm, dev_snd, dev_all;

	bool sock_x11, sock_fallback_x11, sock_wayland, sock_inherit_wayland,
		sock_pulseaudio, sock_session_bus, sock_system_bus,
		sock_ssh_auth, sock_pcsc, sock_cups, sock_gpg_agent;

	char* user_name;
	char* group_name;
	char* real_name;
	uid_t uid;
	gid_t gid;
	char* home_dir;
	char* xdg_runtime_dir;
	char* app_data_dir;

	bool devel;
	bool multiarch, bluetooth, canbus, per_app_dev_shm;

	strlist_t env_overrides;
	bool spawned_via_portal;
	bool clearenv;

	bool die_with_parent;

	strlist_t session_bus_policy;
	strlist_t system_bus_policy;
	strlist_t a11y_bus_policy;
} sandbox_plan_t;

int permissions_plan(const fp_metadata_t* meta, const char* home_dir,
		     sandbox_plan_t* plan);
void permissions_free_plan(sandbox_plan_t* plan);

int permissions_ensure_data_dirs(const char* app_data_dir);

typedef struct {
	strlist_t sockets;

	strlist_t devices;

	strlist_t shares;

	strlist_t fs_add;
	strlist_t fs_remove;

	char* command;

	strlist_t envs;

	bool devel, clearenv;

	char *branch, *arch;

	bool die_with_parent;

	bool file_forwarding;
} run_opts_t;

void permissions_apply_overrides(sandbox_plan_t* plan, const run_opts_t* opts);

void evaluate_conditionals_in_lists(const fp_metadata_t* meta);

typedef struct {
	char* id;
	char* installed_id;
	char* target;
	char* directory;
	char* subdir_suffix;
	char* files_path;
	bool needs_tmpfs;
	char* add_ld_path;
	strlist_t merge_dirs;
	int priority;
	bool from_app;
} fp_extension_t;

typedef struct {
	char* fstype;
	char* from;
	char* to;
} sb_mount_t;

typedef struct {
	char* app_id;
	char* runtime_dir;
	char* app_files_dir;
	char* jailroot;
	int jid;
	int devfs_ruleset;
	sb_mount_t* mounts;
	size_t nmounts, mounts_cap;

	int bus_proxy_pid;
	int bus_sync_fd;
	bool session_bus_proxied;
	bool system_bus_proxied;
	bool a11y_bus_proxied;
	char* session_bus_sock;
	char* system_bus_sock;
	char* a11y_bus_sock;
	char* bus_id;
	char* bus_proxy_dir;

	// PulseAudio discovery runs on the host before jail_attach(); the result
	// is cached because those paths are unreachable afterwards.
	bool pulse_bound;
	bool pulse_remote;

	// Other discovery results cached for the same reason: ssh_auth_bound,
	// cups_server, pcsc_socket.
	bool ssh_auth_bound;
	char* cups_server;
	char* pcsc_socket;
	bool xauth_written;

	char* p11kit_socket_name;

	fp_extension_t* extensions;
	size_t nextensions;
	char* ld_library_path;

	bool use_ld_so_cache;
	char* ldconfig_path;

	char* egl_vendor_files;
	char* vulkan_icd_files;
	char* vulkan_layer_paths;

	char* instance_id;
	char* instance_dir;
	int instance_lock_fd;
	// The instance's directory in the jail root, held open from registration
	// to teardown: the pid file is written after the app owns the tree, and a
	// descriptor cannot be renamed out from under the open.
	int instance_mirror_fd;

	// Held from mkdtemp() to teardown(): a root with no jail but a held lock
	// is a launch in progress, not a leak.
	int root_lock_fd;

	char wayland_display[64];
} sandbox_t;

char* fp_wayland_display_name(const char* xdg_runtime_dir);

// Ensure $XDG_RUNTIME_DIR exists and belongs to the caller (0700); every
// root-side mkdir under it calls this first.
int sandbox_ensure_runtime_dir(const sandbox_plan_t* plan);

int sandbox_build_root(sandbox_t* sb, const sandbox_plan_t* plan,
		       const fp_metadata_t* meta);
// Hand the per-launch tree to the app, after every root write into it.
void sandbox_own_tree(sandbox_t* sb, const sandbox_plan_t* plan);
int sandbox_create_jail(sandbox_t* sb, const fp_metadata_t* meta,
			const sandbox_plan_t* plan);
// Relay between the caller's terminal and a pty master until `pid` exits.
// in_fd/out_fd are the caller's keys and the app's output (-1: none), and
// size_fd the terminal the window size is copied from.
int sandbox_pty_relay(int master, int in_fd, int out_fd, int size_fd, pid_t pid,
		      bool kill_on_eof, int* status);
// The client connection went away (the worker's SIGHUP): stop reading the
// caller's terminal and end the app.
void fpkg_client_mark_gone(void);
bool fpkg_client_gone(void);
// FLATPKG_APP_TTY as we write it: the pty slave, then which stdio descriptors
// are that terminal ("7" is all three, "7:0,2" stdin and stderr).
bool sandbox_app_tty(int* fd, unsigned* mask);
// Close the parent's copy of the slave once the child has its own.
void sandbox_release_app_tty(void);
int sandbox_exec(sandbox_t* sb, const fp_metadata_t* meta,
		 const sandbox_plan_t* plan, char* const extra_argv[]);
void sandbox_teardown(sandbox_t* sb);
// Remove jail roots whose jail no longer exists (a worker that died early).
void sandbox_gc_leaks(void);
void install_devfs_base_rulesets(void);
bool sb_is_mounted(sandbox_t* sb, const char* path);
int m_nullfs(sandbox_t* sb, const char* from, const char* to, bool ro);

int sandbox_ensure_writable_dir(sandbox_t* sb, const char* path);

int fp_extensions_apply(const fp_metadata_t* app_meta, sandbox_t* sb);
void fp_extensions_free(sandbox_t* sb);

const char* dbus_proxy_binary(void);
bool dbus_proxy_is_path(const char* path);

bool flatpkg_kernel_module_loaded(const char* name);

// The modules flatpkg's mounts need, NULL-terminated.
extern const char* const flatpkg_host_modules[];

// Write the "these are missing, here is how to load them" message; true when one is absent.
bool flatpkg_host_modules_message(char* out, size_t n);

// Emit the multi-line message one log line at a time.
void flatpkg_host_modules_emit(const char* msg);

// The one way to read the environment: names are registered in cli.c, and a
// bare getenv() elsewhere does not compile.
const char* cli_getenv(const char* name);

typedef enum {
	FP_CALLER_NO = 0,
	FP_CALLER_JAIL,
	FP_CALLER_PROXY,
	FP_CALLER_UNREADABLE
} fp_caller_t;

fp_caller_t flatpkg_caller_check(pid_t pid, int* jid_out, char** path_out);
bool dbus_system_bus_proxied(const sandbox_plan_t* plan);
int dbus_start_proxies(sandbox_t* sb, const sandbox_plan_t* plan);
void dbus_stop_proxies(sandbox_t* sb);

const char* store_root(void);
int store_import(const char* src_root, char*** imported_out, size_t* n_out);
int store_list(char*** ids_out, size_t* n_out);
void store_pointer_path(char* out, size_t outlen, const char* id);
int store_pointer_export(const char* id, char* export_dir, size_t edirlen,
			 char* arch, size_t archlen, char* branch,
			 size_t branchlen);
typedef struct {
	char* id;
	char* kind;
} store_list_entry_t;

int store_list_all(store_list_entry_t*** entries_out, size_t* n_out);
void store_list_all_free(store_list_entry_t** entries, size_t n);
int store_resolve(const char* app_id, sandbox_t* sb_out,
		  fp_metadata_t* meta_out);
void parse_app_ref(const char* ref, char* id, size_t idlen, char* arch,
		   size_t archlen, char* branch, size_t branchlen);
int store_update_exports(void);
int cmd_exports(void);
int cmd_list(void);

int cmd_run(const char* app_id, char* const extra_argv[], const run_opts_t* ro);
int cmd_import(const char* path);
int cmd_complete(const char* what, const char* arg);

// Instance registry helpers shared by the read and control sides.
bool dir_has_live_instance(const char* base);
int instance_alive(const char* dir, int jid);
char** instance_load_run_environ(const char* inst_dir);
const char* instances_dir(void);
// The instance base the portal and the session helper read identity from: the
// uid's own runtime directory, never a path from an activation environment.
const char* session_instances_dir(void);
// The app id the instance in `jid` recorded, or NULL for a jail no flatpkg
// launch claims.
char* instance_app_for_jid(int jid);
int jail_exists(int jid);
int kf_get(const char* path, const char* group, const char* key, char* out,
	   size_t outlen);

int store_remove_tree(const char* path, const char* label,
		      const char* done_msg);
int find_active_dir(const char* base, const char* component, const char* id,
		    char* out, size_t outlen);

// The system helper's command map: a D-Bus method name to an absolute argv,
// with %-placeholders filled from fpkg_helper_args_t below.  0 means the
// request is unconfigured and the caller refuses it.
#define FPKG_HELPER_MAX_ARGS 32
typedef struct {
	const char* ref;
	const char* installation;
	const char* bundle;
	const char* remote;
	const char* origin;
	const char* arch;
	const char* repo;
	const char* subpaths;
	const char* previous;
	const char* config;
	const char* url;
	const char* key;
	const char* value;
	const char* summary;
	const char* summary_sig;
	const char* src_dir;
	unsigned flags;
} fpkg_helper_args_t;
int fpkg_helper_command(const char* request, const fpkg_helper_args_t* args,
			char*** argv, size_t* argc);

// A client's own state, $store/clients/<client>/<file>; both components are
// single path components, so no client can name another's file.
bool flatpkg_client_name_ok(const char* name);
int flatpkg_client_path(const char* client, const char* file, char* out,
			size_t len);

// Thin wrappers over world_load()/world_save(), so the file has one parser
// and one save path; the id view sees only the dep name.
int world_has(const char* id);
int world_add(const char* id);
int world_seed(void);

int store_deploy_in_use(const char* kind, const char* id);
int store_find_extension(const char* id, const char* arch, const char* branch,
			 char* files_out, size_t outlen);
bool store_extension_any_branch(const char* id, const char* arch);
int store_list_extensions(const char* id_prefix, const char* arch,
			  const char* branch, char*** ids_out, size_t* n_out);

int instance_register(sandbox_t* sb, const fp_metadata_t* meta,
		      const sandbox_plan_t* plan);
void instance_write_bwrapinfo(const char* inst_dir, const char* container_path,
			      long pid);
void instance_save_run_environ(const sandbox_t* sb, const fp_metadata_t* meta,
			       const sandbox_plan_t* plan);
void instance_unregister(sandbox_t* sb);
int instance_uses_path(const char* path);
int cmd_ps(const char* columns);
void instance_complete_list(void);
int cmd_enter(const char* jid, char* const extra_argv[]);
int cmd_kill(const char* jid);

int cmd_remote_add(const char* name, const char* url, const char* from);
int cmd_remote_list(void);
int cmd_remote_delete(const char* name);
// The changes are `key=value` lines from the client: url, branch, title,
// comment, description, homepage, icon, gpg-verify.  `gpg_data` is the
// keyring's text, already read by the caller; it is NULL when no key is
// imported, and the worker never opens a caller-named path.
int cmd_remote_modify(const char* name, const char* changes,
		      const char* gpg_data);
int cmd_remote_info(const char* remote, const char* request, const char* arch,
		    const char* commit, bool runtime, bool show_ref,
		    bool show_commit, bool show_metadata, bool show_parent);
// The GL drivers flatpak would consider active on this host, one per line.
void cmd_gl_drivers(void);
char** ostree_remote_names(size_t* n_out);

typedef enum {
	FLATPKG_DEP_ANY = 0,
	FLATPKG_DEP_EQ = 1,
	FLATPKG_DEP_GT = 2,
	FLATPKG_DEP_LT = 3,
	FLATPKG_DEP_GE = 4,
	FLATPKG_DEP_LE = 5,
	FLATPKG_DEP_PREFIX = 6,
} flatpkg_dep_op_t;

typedef enum {
	FLATPKG_DEP_KIND_APP = 0,
	FLATPKG_DEP_KIND_RUNTIME = 1,
	FLATPKG_DEP_KIND_EXTENSION = 2,
} flatpkg_dep_kind_t;

// The executor a confirmed plan row reaches; `update` plans first.
int update_deploy(const char* ref);
int cmd_search(const char* text);
int cmd_list_commits(const char* ref);
int cmd_rollback(const char* ref, const char* commit);

// Remove one deploy after a confirmed plan; the world save belongs to the commit.
int remove_deploy_ref(const char* ref);
// Install one extension ref by pulling its runtime-kind ref under extension/.
int ostree_install_extension_ref(const char* ref);
// Refs a deployed app or runtime needs that the store lacks; `full` asks the
// remote for the versions and subdirectory groups it alone can enumerate.
char** ostree_missing_deps(const char* ref, bool full, size_t* n_out);

// Branches of an extension group to install now (no .Debug; download-if honoured).
size_t ostree_group_install_branches(const fp_ext_group_t* g, const char* arch,
				     const char* parent_branch,
				     const char** out, size_t max);

typedef struct {
	char* name;
	char* remote;
	char* branch;
	flatpkg_dep_op_t op;
	bool conflict;
	flatpkg_dep_kind_t kind;
} flatpkg_dep_t;

typedef struct {
	flatpkg_dep_t* items;
	size_t count, cap;
} flatpkg_dep_array_t;

// A missing worldfile is an empty world; one that cannot be read is an error,
// because solving against an empty world would prune.
int world_load(flatpkg_dep_array_t* out);
char* world_to_text(const flatpkg_dep_array_t* world);
// Atomic save (tmp + rename), with the format header.
int world_save(const flatpkg_dep_array_t* world);
void world_free(flatpkg_dep_array_t* world);
int world_dep_add(flatpkg_dep_array_t* world, const flatpkg_dep_t* dep);
int world_dep_del(flatpkg_dep_array_t* world, const char* name);
bool world_dep_has(const flatpkg_dep_array_t* world, const char* name);

int world_dep_parse(const char* line, flatpkg_dep_t* out);

char* world_dep_format(const flatpkg_dep_t* dep);

#define FLATPKG_SOLVERF_UPGRADE (1u << 0)
#define FLATPKG_SOLVERF_PRUNE (1u << 1)
#define FLATPKG_SOLVERF_AVAILABLE (1u << 2)
#define FLATPKG_SOLVERF_SIMULATE (1u << 3)
#define FLATPKG_SOLVERF_REPAIR (1u << 4)

typedef enum {
	FP_CHANGE_INSTALL,
	FP_CHANGE_UPGRADE,
	FP_CHANGE_DOWNGRADE,
	FP_CHANGE_REMOVE,
	FP_CHANGE_REINSTALL,
	FP_CHANGE_KEEP,
	// An explicit request with no work: KEEP means leave this installed ref
	// alone and is skipped; NOOP means the caller asked and there is nothing
	// to do, and the table must say so.
	FP_CHANGE_NOOP,
} flatpkg_change_type_t;

typedef struct {
	flatpkg_change_type_t type;
	char* ref;
	char* old_version;
	char* new_version;
} flatpkg_change_t;

typedef struct {
	flatpkg_change_t* items;
	size_t count, cap;
} flatpkg_changeset_t;

// Changeset, new world, remote.  The CLI sends it as FP_REQ_APPLY; the daemon
// only executes the rows.
typedef struct {
	flatpkg_changeset_t cs;
	flatpkg_dep_array_t world;
	char* remote;
	bool absent;	 // a requested ref was not installed
	bool had_work;	 // the plan removes something (not only NOOP rows)
} fpkg_plan_t;

int ostree_plan_install(const char** requests, const char* const* commits,
			const flatpkg_dep_kind_t* kinds, size_t n,
			bool reinstall, fpkg_plan_t* out);
void fpkg_plan_free(fpkg_plan_t* plan);
int ostree_plan_remove(const char** refs, size_t nrefs, bool rdepends,
		       fpkg_plan_t* out);
int ostree_plan_upgrade(unsigned flags, const char* const* refs,
			size_t nrefs, fpkg_plan_t* out);
// Metadata fetched by the daemon for this plan, keyed by full ref.
void ostree_plan_meta_reset(void);
int ostree_plan_meta_add(const char* ref, const char* metadata);

// An "overall" bar pinned under the log lines; transactions nest, and only the
// outermost draws anything.
typedef struct {
	fp_bar_t bar;
	size_t total;
	size_t done;
	size_t pull_total;
	bool pinned;
	bool nested;
	bool started;
	bool pulling;
	int64_t start_ns;
} flatpkg_txn_t;

void flatpkg_txn_begin(flatpkg_txn_t* t, size_t total);
// Announce the next operation; this also completes the previous one.
void flatpkg_txn_op(flatpkg_txn_t* t, const char* verb, const char* ref);
// The prefetch phase pulls in parallel, so pull_begin hands it the bar
// ("pulled: k/n") and pull_end leaves "Installed: x/y" for the checkout.
void flatpkg_txn_pull_begin(flatpkg_txn_t* t, size_t n);
void flatpkg_txn_pull_progress(flatpkg_txn_t* t, size_t n);
void flatpkg_txn_pull_end(flatpkg_txn_t* t);
void flatpkg_txn_end(flatpkg_txn_t* t, int rc);
// One lock for anything that changes the world or the store, recursive so a
// plan nested in a plan does not wait on itself.  The world is read after
// taking it, so no plan races another command.
void flatpkg_lock(void);
void flatpkg_unlock(void);
// True while any transaction runs: executors stay quiet because the plan said it all.
bool flatpkg_txn_active(void);

// Apply a confirmed changeset: parallel pull, then one transaction over every row.
int changeset_apply(flatpkg_changeset_t* cs, const char* remote);

void changeset_free(flatpkg_changeset_t* cs);
// Record an explicit request with no work, so the table can name the ref.
void changeset_add_noop(flatpkg_changeset_t* cs, const char* ref);
void changeset_add_reinstall(flatpkg_changeset_t* cs, const char* ref);

bool changeset_has_work(const flatpkg_changeset_t* cs);

int history_append(const char* action, const char* ref, const char* old_version,
		   const char* new_version);
int cmd_history(void);

int cmd_repair(bool simulate);

int daemon_main(void);
int cli_entry(int argc, char* argv[]);
// The control socket: a compile-time setting, never the environment, because
// where the root daemon listens is not its callers' choice.
#ifndef FPKG_SOCKET
#define FPKG_SOCKET "/var/run/flatpkg/control.sock"
#endif

// EX_UNAVAILABLE: distinct from 1 so a script can tell "the daemon is down" from a failure.
#define FPKG_EXIT_UNAVAILABLE 69

// Send argv to the daemon and relay its output.  Never returns a value
// meaning "run it yourself": such a command does not quietly run in-process.
int client_try_typed(int argc, char* argv[]);
void ignore_sigpipe(void);
// Raw mode for a relayed session (the app's pty does line editing and echo),
// restored at exit; nothing when stdin is not a terminal.
void client_enter_raw_stdin(void);
// Put the terminal back after a run/enter, including modes the app enabled:
// bracketed paste, mouse/focus reporting and the keyboard encodings.
void client_leave_raw_stdin(void);
int daemon_unreachable(int err);
int flatpkg_main(int argc, char* argv[]);

int portal_main(void);

#define FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE (1u << 0)
#define FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY (1u << 1)
#define FLATPAK_HELPER_DEPLOY_FLAGS_LOCAL_PULL (1u << 2)
#define FLATPAK_HELPER_DEPLOY_FLAGS_REINSTALL (1u << 3)
#define FLATPAK_HELPER_DEPLOY_FLAGS_NO_INTERACTION (1u << 4)
#define FLATPAK_HELPER_DEPLOY_FLAGS_APP_HINT (1u << 5)
#define FLATPAK_HELPER_DEPLOY_FLAGS_INSTALL_HINT (1u << 6)
#define FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE_PINNED (1u << 7)
#define FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE_PREINSTALLED (1u << 8)
#define FLATPAK_HELPER_DEPLOY_FLAGS_ALLOW_DOWNGRADE (1u << 9)

#define FLATPAK_HELPER_UNINSTALL_FLAGS_KEEP_REF (1u << 0)
#define FLATPAK_HELPER_UNINSTALL_FLAGS_FORCE_REMOVE (1u << 1)
#define FLATPAK_HELPER_UNINSTALL_FLAGS_NO_INTERACTION (1u << 2)

#define FLATPAK_HELPER_CONFIGURE_FLAGS_UNSET (1u << 0)
#define FLATPAK_HELPER_CONFIGURE_FLAGS_NO_INTERACTION (1u << 1)

#define FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_FORCE_REMOVE (1u << 0)
#define FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_NO_INTERACTION (1u << 1)

#define FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_NO_INTERACTION (1u << 0)
#define FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_SUMMARY_IS_INDEX (1u << 1)
#define FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_ALL                                 \
	(FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_NO_INTERACTION |                   \
	 FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_SUMMARY_IS_INDEX)

#define FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_DELETE (1u << 1)
#define FLATPAK_HELPER_INSTALL_BUNDLE_FLAGS_REINSTALL (1u << 1)
#define FLATPAK_HELPER_INSTALL_BUNDLE_FLAGS_NO_INTERACTION (1u << 0)

#define FLATPAK_HELPER_NO_INTERACTION_FLAG (1u << 0)

int ostree_deploy_from_repo(const char* child_repo, const char* ref,
			    const char* origin, char** subpaths,
			    size_t nsubpaths, unsigned flags);

void store_mark_changed(void);
bool ostree_remote_exists(const char* name);
int ostree_ref_id(const char* ref, char* out, size_t len);
int ostree_remote_add(const char* name, const char* url, bool gpg_verify,
		      bool gpg_verify_summary);

int ostree_install_bundle(const char* bundle_path, const char* remote,
			  unsigned flags, char** out_ref);
int ostree_bundle_ref(const char* bundle_path, char** out_ref);

int overrides_apply(fp_metadata_t* meta, const char* app_id);

char* overrides_path(bool user, const char* app_id);
int overrides_load(bool user, const char* app_id, fp_metadata_t* out);
void overrides_merge(fp_metadata_t* dst, const fp_metadata_t* src);
int overrides_save(bool user, const char* app_id, const fp_metadata_t* ctx);
int overrides_show(bool user, const char* app_id);
int overrides_reset(bool user, const char* app_id);
char* overrides_to_text(const fp_metadata_t* ctx);
int overrides_from_text(const char* text, const char* dir, fp_metadata_t* out);
int cmd_override_system(const char* app_id, unsigned flags,
			const char* changes);

int ostree_appstream_extract(const char* remote, const char* arch);

int ostree_appstream_deploy(const char* remote, const char* arch);

// Take ownership of repo/tmp/cache/summaries: ostree writes it as whoever
// pulled, and a reader trusts only a root-owned cache file.
void ostree_summary_cache_own(void);

int session_helper_main(void);

int system_helper_main(int argc, char* argv[]);

// One answer to "is this deployed runtime/extension still needed": a runtime
// declares its extensions, so an app-only scan would orphan them.
bool ostree_deploy_referenced(const char* ref);
// Reference to a specific deploy: remote:id/arch//branch
typedef struct {
	char* remote;
	char* id;
	char* arch;
	char* branch;
} fpkg_ref_t;

static inline void ref_free(fpkg_ref_t* r) {
	assert(r != NULL);
	free(r->remote);
	free(r->id);
	free(r->arch);
	free(r->branch);
	memset(r, 0, sizeof(*r));
}

typedef struct {
	fpkg_ref_t ref;
	fp_metadata_t meta;
} solver_incoming_t;

typedef struct {
	char** v;
	size_t n, cap;
} idset_t;

void ostree_closure_reset(void);
// Mark the closure fully built so ostree_deploy_referenced() does not rebuild
// it from scratch and drop incoming-app data the solver added.
void ostree_closure_mark_ready(void);
void closure_follow(idset_t* s, const char* id);
extern idset_t g_closure;

void idset_add(idset_t* s, const char* id);
bool idset_has(const idset_t* s, const char* id);
int ref_parse(const char* s, fpkg_ref_t* out);
// True when `path` is the store root or under it once canonicalized.
bool store_path_contained(const char* path);
// Resolve a partial name for the CLI; returns "[remote:]id[/arch]//branch" or NULL.
char* ostree_resolve_install_request(const char* request, bool runtime_only);
// Fetch one ref's commit and metadata text for a client-side plan.
int ostree_fetch_ref_metadata(const char* remote, const char* request,
			      char** commit_out, char** meta_out);
void closure_follow(idset_t* s, const char* id);

void changeset_show_full(const flatpkg_changeset_t* cs, const char* remote);

int solver_solve(const flatpkg_dep_array_t* world, unsigned flags,
		 flatpkg_changeset_t* cs_out, const solver_incoming_t* incoming,
		 size_t nincoming);

int confirm_proceed(int assume_yes);
void ostree_closure_reset(void);

// --profile or FLATPKG_PROFILE=1: a span prints its duration when it closes,
// and the report ranks labels by self time.  No-op when profiling is off.
#define FPKG_SPAN_LABEL 96

typedef struct {
	char label[FPKG_SPAN_LABEL];
	int64_t t0;
	bool live;
} fpkg_span_t;

bool fpkg_prof_on(void);
void fpkg_prof_enable(void);
fpkg_span_t fpkg_span_begin(const char* fmt, ...)
	__attribute__((format(printf, 1, 2)));
void fpkg_span_end(fpkg_span_t* s);
void fpkg_prof_report(void);

#endif
