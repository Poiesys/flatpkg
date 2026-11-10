#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include "flatpkg.h"
#include "cli.h"
#include "protocol.h"
#define FPKG_MODULE "cli"
#include "fpkglog.h"

static fp_logger_t log = {.scope = "run", .desc = ""};

// A property/value table: `info`'s permission section is keyed pairs.
enum { KV_COLS = 2, KV_ROWS_MAX = 64 };

static void kv_row(const char*** rows, const char* (*cells)[KV_COLS],
		   size_t* m, const char* key, const char* val) {
	assert(rows != NULL);
	assert(cells != NULL);
	assert(m != NULL);
	if (*m >= KV_ROWS_MAX) return;
	cells[*m][0] = key;
	cells[*m][1] = val;
	rows[*m] = (const char**)cells[*m];
	(*m)++;
}

// <store>/app/<id>/<arch>/<branch>/<commit>/files is the deploy layout, so the
// report reads the parts off the path it already has.
static void deploy_parts(const char* files_dir, char* deploy, size_t dlen,
			 char* arch, size_t alen, char* branch, size_t blen,
			 char* commit, size_t clen) {
	assert(files_dir != NULL);
	char buf[PATH_MAX];
	// files_dir may be the `active` symlink: the parts are the real deploy's.
	if (realpath(files_dir, buf) == NULL)
		snprintf(buf, sizeof(buf), "%s", files_dir);
	size_t n = strlen(buf);
	if (n > 6 && strcmp(buf + n - 6, "/files") == 0) buf[n - 6] = '\0';
	snprintf(deploy, dlen, "%s", buf);
	const char* leaf = strrchr(buf, '/');
	snprintf(commit, clen, "%s", leaf != NULL ? leaf + 1 : "");
	if (leaf != NULL) ((char*)leaf)[0] = '\0';
	leaf = strrchr(buf, '/');
	snprintf(branch, blen, "%s", leaf != NULL ? leaf + 1 : "");
	if (leaf != NULL) ((char*)leaf)[0] = '\0';
	leaf = strrchr(buf, '/');
	snprintf(arch, alen, "%s", leaf != NULL ? leaf + 1 : "");
}

// The remote a deploy came from, as `remote-delete` and `info --show-origin`
// spell it.  The file sits in the branch directory, above the commit.
static char* read_origin(const char* deploy) {
	char path[PATH_MAX], buf[256] = "";
	char branch_dir[PATH_MAX];

	snprintf(branch_dir, sizeof(branch_dir), "%s", deploy);
	char* slash = strrchr(branch_dir, '/');
	if (slash == NULL) return NULL;
	*slash = '\0';
	snprintf(path, sizeof(path), "%s/.origin", branch_dir);
	FILE* f = fopen(path, "r");
	if (f == NULL) return NULL;
	if (fgets(buf, sizeof(buf), f) == NULL) buf[0] = '\0';
	fclose(f);
	buf[strcspn(buf, " \t\r\n")] = '\0';
	return buf[0] != '\0' ? xstrdup(buf) : NULL;
}

static void aligned_line(size_t width, const char* label, const char* value) {
	assert(label != NULL);
	size_t l = strlen(label);
	size_t pad = width > l ? width - l : 0;

	fp_outl("%*s%s %s", (int)pad, "", label, value != NULL ? value : "-");
}

int cmd_info(const cli_req_t* r) {
	assert(r != NULL);
	const char* app_id = r->nops > 0 ? r->ops[0] : "";
	sandbox_t sb;
	fp_metadata_t meta;
	if (store_resolve(app_id, &sb, &meta) != 0) return 1;

	// Report the permissions the app actually runs with, not the
	// metadata alone, so an override is never hidden from `info`.
	overrides_apply(&meta, sb.app_id);

	char deploy[PATH_MAX], arch[64], branch[64], commit[128];
	deploy_parts(sb.app_files_dir, deploy, sizeof(deploy), arch,
		     sizeof(arch), branch, sizeof(branch), commit,
		     sizeof(commit));
	char* origin = read_origin(deploy);
	char ref[512];
	snprintf(ref, sizeof(ref), "app/%s/%s/%s", sb.app_id, arch, branch);

	bool machine = r->show_ref || r->show_origin || r->show_commit ||
		       r->show_location || r->show_metadata ||
		       r->show_permissions;
	if (machine) {
		// flatpak puts the selected values on one line, then the metadata
		// and the permissions as their own text.
		char line[4096];
		size_t off = 0;
		bool first = true;
		const char* values[8];
		size_t nv = 0;

		if (r->show_ref) values[nv++] = ref;
		if (r->show_origin) values[nv++] = origin != NULL ? origin : "-";
		if (r->show_commit) values[nv++] = commit;
		if (r->show_location) values[nv++] = deploy;
		if (r->show_runtime)
			values[nv++] = meta.runtime_ref != NULL
					       ? meta.runtime_ref
					       : "-";
		if (r->show_sdk)
			values[nv++] = meta.sdk_ref != NULL ? meta.sdk_ref
							    : "-";
		line[0] = '\0';
		for (size_t i = 0; i < nv && off < sizeof(line); i++) {
			off += (size_t)snprintf(line + off, sizeof(line) - off,
						"%s%s", first ? "" : " ",
						values[i]);
			first = false;
		}
		if (nv > 0) fp_outl("%s", line);

		if (r->show_metadata) {
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "%s/metadata", deploy);
			int fd = open(path, O_RDONLY);

			if (fd >= 0) {
				char chunk[4096];
				ssize_t got;

				while ((got = read(fd, chunk, sizeof(chunk))) > 0)
					fp_out_raw(chunk, (size_t)got);
				close(fd);
			}
		}
		if (r->show_permissions) {
			char* text = overrides_to_text(&meta);

			if (text != NULL) {
				fp_out_raw(text, strlen(text));
				free(text);
			}
		}
		free(origin);
		fp_metadata_free(&meta);
		free(sb.app_id);
		free(sb.app_files_dir);
		free(sb.runtime_dir);
		return 0;
	}

	static const char* labels[] = {"ID:",		"Ref:",	     "Arch:",
				       "Branch:",	"Version:",  "Origin:",
				       "Installation:", "Runtime:",  "Sdk:",
				       "Commit:"};
	size_t width = 0;
	for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
		size_t l = strlen(labels[i]);

		if (l > width) width = l;
	}

	aligned_line(width, "ID:", sb.app_id);
	aligned_line(width, "Ref:", ref);
	aligned_line(width, "Arch:", arch);
	aligned_line(width, "Branch:", branch);
	if (meta.version != NULL && *meta.version != '\0')
		aligned_line(width, "Version:", meta.version);
	aligned_line(width, "Origin:", origin);
	aligned_line(width, "Installation:", "default");
	if (meta.runtime_ref != NULL && *meta.runtime_ref != '\0')
		aligned_line(width, "Runtime:", meta.runtime_ref);
	if (meta.sdk_ref != NULL && *meta.sdk_ref != '\0')
		aligned_line(width, "Sdk:", meta.sdk_ref);
	aligned_line(width, "Commit:", commit);

	fp_out("\n");

	const char* headers2[] = {"Permission", "Grant"};
	const char* cells[KV_ROWS_MAX][KV_COLS];
	const char** rows[KV_ROWS_MAX];
	size_t m = 0;
	for (size_t i = 0; i < meta.shared.count; i++)
		kv_row(rows, cells, &m, "shared", meta.shared.items[i]);
	for (size_t i = 0; i < meta.sockets.count; i++)
		kv_row(rows, cells, &m, "sockets", meta.sockets.items[i]);
	for (size_t i = 0; i < meta.devices.count; i++)
		kv_row(rows, cells, &m, "devices", meta.devices.items[i]);
	for (size_t i = 0; i < meta.filesystems.count; i++)
		kv_row(rows, cells, &m, "filesystems",
		       meta.filesystems.items[i]);
	fp_print_table(headers2, FP_TABLE_COLS(headers2),
		       (const char* const* const*)rows, m, FP_TABLE_OUTPUT);

	free(origin);
	fp_metadata_free(&meta);
	free(sb.app_id);
	free(sb.app_files_dir);
	free(sb.runtime_dir);
	return 0;
}

static void appdata_trim(char* s) {
	assert(s != NULL);
	char* p = s;
	while (*p && isspace((unsigned char)*p)) p++;
	if (p != s) memmove(s, p, strlen(p) + 1);
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static void appdata_name_version(const char* app_files_dir, const char* id,
				 char* name, size_t name_len, char* version,
				 size_t ver_len) {
	assert(app_files_dir != NULL);
	assert(id != NULL);
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/share/metainfo/%s.metainfo.xml",
		 app_files_dir, id);
	FILE* f = fopen(path, "r");
	if (!f) {
		snprintf(path, sizeof(path), "%s/share/appdata/%s.appdata.xml",
			 app_files_dir, id);
		f = fopen(path, "r");
	}
	if (!f) return;
	char line[1024];
	int got_name = 0;
	while (fgets(line, sizeof(line), f)) {
		if (!got_name) {
			char* n = strstr(line, "<name>");
			if (n) {
				n += 6;
				char* e = strstr(n, "</name>");
				if (e) {
					*e = '\0';
					appdata_trim(n);
					if (*n)
						snprintf(name, name_len, "%s",
							 n);
					got_name = 1;
				}
			}
		}
		if (!*version) {
			char* r = strstr(line, "<release");
			if (r) {
				char* v = strstr(r, "version=\"");
				if (v) {
					v += 9;
					char* e = strchr(v, '"');
					if (e) {
						*e = '\0';
						snprintf(version, ver_len, "%s",
							 v);
					}
				}
			}
		}
	}
	fclose(f);
}

int cmd_list(void) {
	store_list_entry_t** entries = NULL;
	size_t n = 0;
	if (store_list_all(&entries, &n) != 0) return 1;
	if (n == 0) {
		fp_logger_infof(&log, "nothing installed");
		store_list_all_free(entries, n);
		return 0;
	}
	const char* headers[] = {"Kind", "Name", "ID", "Version", "Branch",
				 "Remote"};
	enum {
		LIST_SOURCE_MAX = 128,
		LIST_ROWS_MAX = 1024,
		LIST_COLS = FP_TABLE_COLS(headers),
	};
	if (n > LIST_SOURCE_MAX) {
		fpkg_warnf("listing the first %d of %zu entries",
			   LIST_SOURCE_MAX, n);
		n = LIST_SOURCE_MAX;
	}
	char* cells[LIST_ROWS_MAX][LIST_COLS];
	const char** rows[LIST_ROWS_MAX];
	size_t m = 0;
	bool truncated = false;
	for (size_t i = 0; i < n; i++) {
		if (m == LIST_ROWS_MAX) {
			truncated = true;
			break;
		}
		const char* id = entries[i]->id;
		const char* kind = entries[i]->kind;
		char name[128] = "", version[64] = "", branch[64] = "-",
		     remote[64] = "-";
		if (strcmp(kind, "app") == 0) {
			sandbox_t sb;
			fp_metadata_t meta;
			if (store_resolve(id, &sb, &meta) == 0) {
				char dispname[128] = "", dispver[64] = "";
				if (sb.app_files_dir && *sb.app_files_dir)
					appdata_name_version(
						sb.app_files_dir, id, dispname,
						sizeof(dispname), dispver,
						sizeof(dispver));
				snprintf(name, sizeof(name), "%s",
					 dispname[0] ? dispname
						     : ((meta.app_id &&
							 *meta.app_id)
								? meta.app_id
								: id));
				if (dispver[0])
					snprintf(version, sizeof(version), "%s",
						 dispver);
				else if (meta.version && *meta.version)
					snprintf(version, sizeof(version), "%s",
						 meta.version);
				char cur[PATH_MAX], target[PATH_MAX];
				snprintf(cur, sizeof(cur), "%s/app/%s/current",
					 store_root(), id);
				ssize_t tl = readlink(cur, target,
						      sizeof(target) - 1);
				if (tl > 0) {
					target[tl] = '\0';
					const char* slash =
						strrchr(target, '/');
					snprintf(branch, sizeof(branch), "%s",
						 slash ? slash + 1 : target);
				}
				char refsdir[PATH_MAX];
				snprintf(refsdir, sizeof(refsdir),
					 "%s/repo/refs/remotes", store_root());
				DIR* rd = opendir(refsdir);
				if (rd) {
					struct dirent* de;
					while ((de = readdir(rd))) {
						if (de->d_name[0] == '.')
							continue;
						char refp[PATH_MAX];
						snprintf(refp, sizeof(refp),
							 "%s/%s/app/%s",
							 refsdir, de->d_name,
							 id);
						struct stat st;
						if (stat(refp, &st) == 0 &&
						    S_ISDIR(st.st_mode)) {
							snprintf(remote,
								 sizeof(remote),
								 "%s",
								 de->d_name);
							break;
						}
					}
					closedir(rd);
				}
				fp_metadata_free(&meta);
				free(sb.app_id);
				free(sb.app_files_dir);
				free(sb.runtime_dir);
			} else {
				snprintf(name, sizeof(name), "%s", id);
			}
		} else if (strcmp(kind, "runtime") == 0) {
			snprintf(name, sizeof(name), "%s", id);
			char active[PATH_MAX];
			if (find_active_dir(store_root(), "runtime", id, active,
					    sizeof(active)) == 0) {
				char* slash = strrchr(active, '/');
				if (slash && slash > active) {
					*slash = '\0';
					const char* base = strrchr(active, '/');
					base = base ? base + 1 : active;
					snprintf(branch, sizeof(branch), "%s",
						 base);
					snprintf(version, sizeof(version), "%s",
						 base);
					*slash = '/';
				}
			} else {
				snprintf(branch, sizeof(branch), "-");
			}
			char refsdir[PATH_MAX];
			snprintf(refsdir, sizeof(refsdir),
				 "%s/repo/refs/remotes", store_root());
			DIR* rd = opendir(refsdir);
			if (rd) {
				struct dirent* de;
				while ((de = readdir(rd))) {
					if (de->d_name[0] == '.') continue;
					char refp[PATH_MAX];
					snprintf(refp, sizeof(refp),
						 "%s/%s/runtime/%s", refsdir,
						 de->d_name, id);
					struct stat st;
					if (stat(refp, &st) == 0 &&
					    S_ISDIR(st.st_mode)) {
						snprintf(remote, sizeof(remote),
							 "%s", de->d_name);
						break;
					}
				}
				closedir(rd);
			}
		} else {
			snprintf(name, sizeof(name), "%s", id);
			char active[PATH_MAX];
			if (find_active_dir(store_root(), "extension", id,
					    active, sizeof(active)) == 0) {
				char* slash = strrchr(active, '/');
				if (slash && slash > active) {
					*slash = '\0';
					const char* base = strrchr(active, '/');
					base = base ? base + 1 : active;
					snprintf(branch, sizeof(branch), "%s",
						 base);
					snprintf(version, sizeof(version), "%s",
						 base);
					*slash = '/';
				}
			} else {
				snprintf(branch, sizeof(branch), "-");
			}
			char refsdir[PATH_MAX];
			snprintf(refsdir, sizeof(refsdir),
				 "%s/repo/refs/remotes", store_root());
			DIR* rd = opendir(refsdir);
			if (rd) {
				struct dirent* de;
				while ((de = readdir(rd))) {
					if (de->d_name[0] == '.') continue;
					char refp[PATH_MAX];
					snprintf(refp, sizeof(refp),
						 "%s/%s/runtime/%s", refsdir,
						 de->d_name, id);
					struct stat st;
					if (stat(refp, &st) == 0 &&
					    S_ISDIR(st.st_mode)) {
						snprintf(remote, sizeof(remote),
							 "%s", de->d_name);
						break;
					}
				}
				closedir(rd);
			}
		}
		cells[m][0] = strdup(kind);
		cells[m][1] = strdup(name);
		cells[m][2] = strdup(id);
		cells[m][3] = strdup(version);
		cells[m][4] = strdup(branch);
		cells[m][5] = strdup(remote);
		rows[m] = (const char**)cells[m];
		m++;
	}
	fp_print_table(headers, LIST_COLS, (const char* const* const*)rows, m,
		       FP_TABLE_OUTPUT);
	if (truncated)
		fpkg_warnf("more than %d entries; not all are listed",
			   LIST_ROWS_MAX);
	for (size_t i = 0; i < m; i++)
		for (size_t c = 0; c < LIST_COLS; c++) free(cells[i][c]);
	store_list_all_free(entries, n);
	return 0;
}

int cmd_complete(const char* what, const char* arg) {
	assert(what != NULL);

	if (strcmp(what, "commands") == 0) {
		cli_complete_commands();
		return 0;
	}

	if (strcmp(what, "words") == 0) {
		cli_complete_words(arg);
		return 0;
	}

	if (strcmp(what, "values") == 0) {
		if (arg == NULL) {
			fpkg_errorf("complete values needs an option name");
			return 1;
		}
		cli_complete_values(arg);
		return 0;
	}

	if (strcmp(what, "operands") == 0) {
		if (arg == NULL) {
			fpkg_errorf("complete operands needs a command");
			return 1;
		}
		cli_complete_operands(arg);
		return 0;
	}

	if (strcmp(what, "man") == 0) {
		if (arg == NULL) {
			fpkg_errorf("complete man needs a section");
			return 1;
		}
		cli_man_options(arg);
		return 0;
	}

	if (strcmp(what, "remotes") == 0) {
		size_t n = 0;
		char** names = ostree_remote_names(&n);
		for (size_t i = 0; i < n; i++) {
			fp_outl("%s", names[i]);
			free(names[i]);
		}
		free(names);
		return 0;
	}

	if (strcmp(what, "instances") == 0) {
		instance_complete_list();
		return 0;
	}

	if (strcmp(what, "world") == 0) {
		flatpkg_dep_array_t world = {0};
		if (world_load(&world) != 0) return 1;
		for (size_t i = 0; i < world.count; i++)
			if (!world.items[i].conflict)
				fp_outl("%s", world.items[i].name);
		world_free(&world);
		return 0;
	}

	bool want_app = strcmp(what, "apps") == 0;
	bool want_runtime = strcmp(what, "runtimes") == 0;
	bool want_refs = strcmp(what, "refs") == 0;
	if (!want_app && !want_runtime && !want_refs) {
		fpkg_errorf("unknown kind '%s' for complete (apps, refs, "
			    "runtimes, world, remotes, instances, commands, "
			    "words, values, operands, man)",
			    what);
		return 1;
	}

	store_list_entry_t** entries = NULL;
	size_t n = 0;
	if (store_list_all(&entries, &n) != 0) return 1;
	for (size_t i = 0; i < n; i++) {
		const char* kind = entries[i]->kind;
		if (want_refs || (want_app && strcmp(kind, "app") == 0) ||
		    (want_runtime && strcmp(kind, "runtime") == 0))
			fp_outl("%s", entries[i]->id);
	}
	store_list_all_free(entries, n);
	return 0;
}

// Open `path` as the CALLER.  Root creating a caller-named file in a root-owned
// directory (rc.d, say) is how a caller gets root at the next boot, so the
// kernel's own permission checks decide.  Restore root, or the process ends.
static int open_as_caller(const char* path, int flags, mode_t mode, uid_t uid,
			  gid_t gid) {
	assert(path != NULL);
	if (geteuid() != 0 || uid == 0) return open(path, flags, mode);

	gid_t saved[NGROUPS_MAX];
	int nsaved = getgroups(NGROUPS_MAX, saved);
	gid_t egid = getegid();
	if (nsaved < 0) return -1;
	if (setgroups(1, &gid) != 0) return -1;
	if (setegid(gid) != 0 || seteuid(uid) != 0) {
		int e = errno;
		(void)seteuid(0);
		(void)setegid(egid);
		if (setgroups(nsaved, saved) != 0) _exit(70);
		errno = e;
		return -1;
	}
	int fd = open(path, flags, mode);
	int e = errno;
	if (seteuid(0) != 0 || setegid(egid) != 0 ||
	    setgroups(nsaved, saved) != 0)
		_exit(70);
	errno = e;
	return fd;
}

// Open the -l / FLATPKG_LOGFMT path as the caller; it must be a regular file.
static int logfmt_open(const char* path) {
	assert(path != NULL);
	uid_t caller = getuid();
	gid_t cgid = getgid();
	const char* eu = cli_getenv("FLATPKG_APP_UID");
	const char* eg = cli_getenv("FLATPKG_APP_GID");
	if (geteuid() == 0 && eu != NULL && *eu != '\0' && eg != NULL &&
	    *eg != '\0') {
		caller = (uid_t)strtoul(eu, NULL, 10);
		cgid = (gid_t)strtoul(eg, NULL, 10);
	} else if (geteuid() == 0 && eu != NULL && *eu != '\0') {
		// A uid with no gid cannot be dropped to safely.
		errno = EPERM;
		return -1;
	}

	int fd = open_as_caller(
		path, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_NONBLOCK,
		0644, caller, cgid);
	if (fd < 0) return -1;
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		errno = EINVAL;
		return -1;
	}
	return fd;
}

static void client_ensure_app_data_dirs(int argc, char* argv[]) {
	assert(argv != NULL);
	const char* app_id = cli_run_app_id(argc, argv);
	if (app_id == NULL || strchr(app_id, '.') == NULL) return;
	struct passwd* pw = getpwuid(getuid());
	if (!pw || !pw->pw_dir) return;
	char data_dir[PATH_MAX];
	snprintf(data_dir, sizeof(data_dir), "%s/.var/app/%s", pw->pw_dir,
		 app_id);
	if (permissions_ensure_data_dirs(data_dir) != 0)
		fpkg_warnf("cannot pre-create %s (the daemon will retry "
			   "as you): %s",
			   data_dir, strerror(errno));
}

int cmd_override(const cli_req_t* r) {
	const char* app_id = r->nops > 0 ? r->ops[0] : NULL;
	if (r->over_reset)
		return overrides_reset(r->over_user, app_id) == 0 ? 0 : 1;
	if (r->over_show)
		return overrides_show(r->over_user, app_id) == 0 ? 0 : 1;

	fp_metadata_t file;
	overrides_load(r->over_user, app_id, &file);
	overrides_merge(&file, &r->over);
	int rc = overrides_save(r->over_user, app_id, &file) == 0 ? 0 : 1;
	fp_metadata_free(&file);
	return rc;
}

static int dispatch_local(const cli_req_t* r) {
	assert(r != NULL);
	const char** op = r->ops;
	switch (cli_req_cmd(r)) {
	case CMD_LIST:
		return cmd_list();
	case CMD_INFO:
		return cmd_info(r);
	case CMD_REMOTE_INFO:
		return cmd_remote_info(op[0], op[1], r->arch, r->commit,
				       r->runtime, r->show_ref, r->show_commit,
				       r->show_metadata, r->show_parent);
	case CMD_SEARCH:
		return cmd_search(op[0]);
	case CMD_LIST_COMMITS:
		return cmd_list_commits(op[0]);
	case CMD_HISTORY:
		return cmd_history();
	case CMD_PS:
		return cmd_ps(r->columns);
	case CMD_REPAIR:
		return cmd_repair(r->g.simulate);
	case CMD_OVERRIDE:
		return cmd_override(r);
	case CMD_REMOTE_LIST:
		return cmd_remote_list();
	case CMD_COMPLETE:
		return cmd_complete(op[0], r->nops > 1 ? op[1] : NULL);
	default:
		break;
	}
	fpkg_errorf("command '%s' is not local; it must go to flatpkgd",
		    r->name);
	return 1;
}

int flatpkg_main(int argc, char* argv[]) {
	assert(argv != NULL);
	cli_req_t req;

	int rc = cli_parse(argc, argv, &req);
	if (rc != CLI_PROCEED) {
		cli_req_free(&req);
		return rc;
	}
	if (req.g.ostree_verbose) setenv("FLATPKG_OSTREE_VERBOSE", "1", 1);
	if (req.g.profile) fpkg_prof_enable();
	fpkg_debugf("command: %s argc=%d", req.name, argc);

	if (req.g.logfmt != NULL) {
		int fd = logfmt_open(req.g.logfmt);
		if (fd >= 0) fp_set_logfmt_fd(fd);
		else
			fpkg_warnf("cannot open logfmt file %s: %s",
				   req.g.logfmt, strerror(errno));
	}

	// A cmd_* handler signals "error, message already logged" with a
	// negative return; the CLI maps that to the generic exit status 1.
	rc = dispatch_local(&req);
	cli_req_free(&req);
	fpkg_prof_report();
	return rc < 0 ? 1 : rc;
}

int cli_entry(int argc, char* argv[]) {
	// The per-user D-Bus services are the CLI's entry points.  cli.c owns
	// argv, so it is the one that matches the word; this only dispatches.
	// The daemon and the system helper are flatpkgd's, so no root service
	// starts here.
	switch (cli_service_entry(argc, argv)) {
	case CLI_SERVICE_PORTAL:
		return portal_main();
	case CLI_SERVICE_SESSION_HELPER:
		return session_helper_main();
	case CLI_SERVICE_NONE:
		break;
	}

	// What needs no daemon is decided by the real parser: help, --version, a usage
	// error and `complete commands`. The rest asks flatpkgd and fails rather than run in-process.
	{
		client_ensure_app_data_dirs(argc, argv);
		if (argc >= 2) {
			cli_req_t req;
			int prc = cli_parse(argc, argv, &req);
			bool local = false;
			char* logfmt_path = NULL;
			if (prc == CLI_PROCEED && req.cmd != NULL) {
				local = cli_req_is_local(&req);
				if (req.g.logfmt != NULL)
					logfmt_path = strdup(req.g.logfmt);
			}
			cli_req_free(&req);
			if (prc != CLI_PROCEED) {
				free(logfmt_path);
				return prc;
			}
			if (local) free(logfmt_path);
			if (!local) {
				// The records are rendered here, so the caller's client opens the file; the
				// daemon never opens a caller-named path.
				if (logfmt_path != NULL) {
					int lfd = logfmt_open(logfmt_path);
					if (lfd >= 0) fp_set_logfmt_fd(lfd);
					else
						fpkg_warnf("cannot open logfmt "
							   "file %s: %s",
							   logfmt_path,
							   strerror(errno));
					free(logfmt_path);
				}
				// The request is built here and there is no other path, so a refusal to build
				// it is an error, not a retry.
				int trc = client_try_typed(argc, argv);
				if (trc >= 0) return trc;
				fpkg_errorf("this command cannot be sent to "
					    "flatpkgd");
				return 1;
			}
		}
	}

	return flatpkg_main(argc, argv);
}
