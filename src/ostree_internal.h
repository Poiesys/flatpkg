#ifndef FLATPKG_OSTREE_INTERNAL_H
#define FLATPKG_OSTREE_INTERNAL_H

#include <assert.h>
#include <dlfcn.h>
#include <ftw.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <utime.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <zlib.h>


#include "flatpkg.h"
#define FPKG_MODULE "ostree"
#include "fpkglog.h"

// Shared only by the ostree units; not part of the public API.

typedef struct {
	char repo[PATH_MAX];
	char keys[PATH_MAX];
	char store[PATH_MAX];
} tool_ctx_t;

typedef struct {
	char* out;
	char* err;
	int rc;
} tool_res_t;

typedef void (*tool_line_cb)(const char* line, void* arg);

typedef struct {
	char buf[4096];
	size_t len;
} line_buf_t;

typedef struct {
	fp_bar_t* pb;
	uint64_t target;
} pull_bar_t;

typedef struct {
	char* remote;
	char* refstr;
	char* label;
	int rc;
} pull_job_t;

typedef struct {
	pull_job_t* jobs;
	size_t n;
	size_t next;
	pthread_mutex_t mu;
	// Overall-bar tick: called once per finished job (with the pool's
	// lock held) as "pulled: k/n". NULL when no transaction drives it.
	flatpkg_txn_t* txn;
	size_t pulled;
} pull_pool_t;

typedef struct {
	char* remote;
	char** refs;
	size_t n;
	bool failed;
	bool warned;
	time_t at;
} remote_refs_t;

typedef struct {
	char* ref;
	char* id;
	char* arch;
	char* branch;
	bool runtime;
} install_match_t;

typedef struct {
	fpkg_ref_t ref;
	char* remote;
	flatpkg_dep_kind_t kind;
	const char* dep_type;
	bool is_runtime;
	bool is_ext;
	fp_metadata_t meta;
} install_req_t;

typedef struct {
	char* ref;
	char* text;
} plan_meta_t;

typedef struct {
	char name[256];
	time_t mtime;
} commit_dir_t;

extern bool g_fetch_quiet;

char* slurp_file(const char* path);
void store_path(char* buf, size_t len, const char* fmt, ...);
const char* host_arch(void);
int atomic_symlink(const char* target, const char* link_path);
int ref_parse(const char* s, fpkg_ref_t* out);
void ref_full(const fpkg_ref_t* r, const char* type, char* out,
		     size_t len);
char* remote_setting(const char* remote, const char* key);
char** list_remotes(size_t* n_out);
char* first_remote(void);
void tool_res_init(tool_res_t* res);
void tool_res_free(tool_res_t* res);
void tool_ctx_init(tool_ctx_t* tc);
size_t buf_append(char** buf, size_t* cap, const char* data,
			 size_t len);
int ostree_run_cb(char* const argv[], tool_res_t* res, tool_line_cb cb,
			 void* arg, void (*tick)(void* tick_arg),
			 void* tick_arg);
int ostree_run(char* const argv[], tool_res_t* res);
void log_tool_stderr(const tool_res_t* res);
void spinner_tick_cb(void* arg);
int ensure_repo(void);
int parallel_pull(pull_job_t* jobs, size_t n, flatpkg_txn_t* txn);
void pull_job_init(pull_job_t* job, const char* remote,
			  const char* refstr);
void pull_jobs_free(pull_job_t* jobs, size_t n);
void flatpak_ref_header(const char* refstr, char* out, size_t outlen);
void prefetched_add(const char* refstr);
int tool_pull(const char* remote, const char* refstr);
int tool_pull_metadata(const char* remote, const char* refstr);
int tool_rev_parse(const tool_ctx_t* tc, const char* refstr,
			  char* commit_out, size_t len);
int tool_rev_probe(const tool_ctx_t* tc, const char* refstr,
			  char* commit_out, size_t len);
int deploy_checkout(const tool_ctx_t* tc, const char* type,
			   const fpkg_ref_t* ref, const char* commit,
			   char* deploy_out, size_t outlen);
int make_active(const char* deploy_host_path);
void write_origin(const char* deploy_host_path, const char* remote);
bool deploy_exists(const char* type, const char* id);
bool deploy_branch_dir(const char* deploy, char* out, size_t outlen);
int install_one(const fpkg_ref_t* ref, const char* pull_type,
		       const char* deploy_type, const char* remote);
bool remote_local_path(const char* remote, char* out, size_t len);
char** fetch_remote_refs(const char* remote, size_t* n_out,
				fp_spin_t* sp);
remote_refs_t* remote_refs_cached(const char* remote,
					 const char* label);
void install_extensions(const char* remote, const fpkg_ref_t* rt,
			       char** srefs, size_t nsrefs);
int ostree_install_extension_ref(const char* ref);
char* commit_xa_metadata(const char* commit);
char* cat_commit_metadata(const char* commit);
char* summary_ref_metadata_text(const char* remote,
				       const char* refstr);
int find_deploy_dir(const char* type, const fpkg_ref_t* r, char* out,
			   size_t len);
bool remote_name_valid(const char* name);
bool is_commit_dir_name(const char* name);
int collect_commit_dirs(const char* branch_dir, bool newest_first,
			       commit_dir_t** out);
int update_deploy(const char* ref);
void write_deploy_data(const char* branch_dir, const char* commit,
			      const char* origin, char** subpaths,
			      size_t nsubpaths);

#endif /* FLATPKG_OSTREE_INTERNAL_H */
