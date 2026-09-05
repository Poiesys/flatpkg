// run and import build a jail and write the store, so they are root's.
#include <assert.h>
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

#include "flatpkg.h"
#define FPKG_MODULE "run"
#include "fpkglog.h"

static fp_logger_t log = {.scope = "run", .desc = ""};

static const char* current_home(void) {
	const char* h = cli_getenv("HOME");
	if (h && *h) return h;
	struct passwd* pw = getpwuid(getuid());
	return pw ? pw->pw_dir : "/";
}

// Interrupt handling for `run`: the handler only records the signal (no
// SA_RESTART, so the parent's waitpid returns EINTR); teardown then re-raises
// it.
static volatile sig_atomic_t fpkg_got_signal = 0;

static void on_run_signal(int sig) {
	fpkg_got_signal = sig;
	fpkg_client_mark_gone();
}

static void deploy_arch_branch(const char* app_files_dir, char* arch,
			       size_t archlen, char* branch, size_t branchlen) {
	assert(app_files_dir != NULL);
	assert(arch != NULL);
	assert(archlen > 0);
	assert(branch != NULL);
	assert(branchlen > 0);
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s", app_files_dir);

	char* p = strstr(tmp, "/active/files");
	if (!p) return;
	*p = '\0';
	p = strrchr(tmp, '/');
	if (!p) return;
	snprintf(branch, branchlen, "%s", p + 1);
	*p = '\0';
	p = strrchr(tmp, '/');
	if (!p) return;
	snprintf(arch, archlen, "%s", p + 1);
}

static void check_deploy_ref(const sandbox_t* sb, const run_opts_t* opts) {
	assert(sb != NULL);
	assert(opts != NULL);
	if (!opts->branch && !opts->arch) return;
	char arch[64] = "", branch[64] = "";
	deploy_arch_branch(sb->app_files_dir, arch, sizeof(arch), branch,
			   sizeof(branch));
	if (opts->branch && branch[0] && strcmp(opts->branch, branch) != 0) {
		fpkg_warnf("--branch=%s does not match the resolved deploy "
			   "branch '%s'; continuing with the active deploy",
			   opts->branch, branch);
	}
	if (opts->arch && arch[0] && strcmp(opts->arch, arch) != 0) {
		fpkg_warnf("--arch=%s does not match the resolved deploy "
			   "arch '%s'; continuing with the active deploy",
			   opts->arch, arch);
	}
}

int cmd_run(const char* app_id, char* const extra_argv[],
	    const run_opts_t* opts) {
	assert(app_id != NULL);
	run_opts_t empty;
	if (!opts) {
		memset(&empty, 0, sizeof(empty));
		opts = &empty;
	}

	if (geteuid() != 0) {
		fp_logger_errorf(
			&log, "'run' needs root (jail_set/mount privileges) - "
			      "invoke via mdo/doas or install setuid-root; see "
			      "README.md");
		return 1;
	}

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_run_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	fpkg_got_signal = 0;

	sandbox_t sb;
	fp_metadata_t meta;
	char run_id[256];
	if (ostree_ref_id(app_id, run_id, sizeof(run_id)) == 0) app_id = run_id;
	if (store_resolve(app_id, &sb, &meta) != 0) {
		fpkg_errorf("'%s' is not installed; use "
			    "'flatpkg install %s' first",
			    app_id, app_id);
		return 1;
	}

	if (opts->command) {
		free(meta.command);
		meta.command = strdup(opts->command);
	}

	check_deploy_ref(&sb, opts);

	// Persistent overrides on top of the app's own [Context], before the
	// plan is computed; the command line still wins.
	overrides_apply(&meta, sb.app_id);

	sandbox_plan_t plan;
	if (permissions_plan(&meta, current_home(), &plan) != 0) {
		fp_metadata_free(&meta);
		free(sb.app_id);
		free(sb.app_files_dir);
		free(sb.runtime_dir);
		return 1;
	}

	// The app id is the left column's scope from here on, so every line of
	// the launch can say whose it is.
	const char* dot = strrchr(app_id, '.');
	fp_set_prefix(dot ? dot + 1 : app_id);

	permissions_apply_overrides(&plan, opts);

	int rc = 1;
	log = fp_logger_init("sandbox", "");
	fp_logger_beginf(&log, "assembling sandbox root");
	if (sandbox_build_root(&sb, &plan, &meta) != 0) {
		fp_logger_endf(&log, 0, "sandbox root build failed");
		goto out;
	}
	fp_logger_endf(&log, 1, "sandbox root ready");

	log = fp_logger_init("jail", "");
	fp_logger_beginf(&log, "creating jail");
	if (sandbox_create_jail(&sb, &meta, &plan) != 0) {
		fp_logger_endf(&log, 0, "jail creation failed");
		goto out;
	}
	fp_logger_endf(&log, 1, "jail created");

	dbus_start_proxies(&sb, &plan);

	instance_register(&sb, &meta, &plan);

	log = fp_logger_init("exec", "");
	{
		fp_logger_infof(&log, "executing %s in jail", app_id);
	}
	rc = sandbox_exec(&sb, &meta, &plan, extra_argv);

out:
	fp_set_prefix("");
	instance_unregister(&sb);
	sandbox_teardown(&sb);
	permissions_free_plan(&plan);
	fp_metadata_free(&meta);
	free(sb.app_id);
	free(sb.app_files_dir);
	free(sb.runtime_dir);
	if (fpkg_got_signal != 0) {
		int sig = (int)fpkg_got_signal;
		signal(sig, SIG_DFL);
		raise(sig);
		return 128 + sig;
	}
	return rc;
}

int cmd_import(const char* path) {
	assert(path != NULL);
	char** imported = NULL;
	size_t n = 0;
	if (store_import(path, &imported, &n) != 0) return 1;
	for (size_t i = 0; i < n; i++) {
		fp_logger_okf(&log, "imported %s", imported[i]);
		history_append("import", imported[i], NULL, NULL);
		world_add(imported[i]);
		free(imported[i]);
	}
	free(imported);
	return 0;
}
