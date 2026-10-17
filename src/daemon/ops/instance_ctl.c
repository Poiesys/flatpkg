#include <assert.h>
#include <dirent.h>
#include <jail.h>
#include <pwd.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/jail.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <gio/gio.h>
#include <glib.h>

#include <errno.h>
#include <fcntl.h>

#define FLATPAK_EMULATED_VERSION "1.16.1"
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "flatpkg.h"
#include "cli.h"
#define FPKG_MODULE "instance"
#include "fpkglog.h"

static fp_logger_t log = {.scope = "instance", .desc = ""};


static fpkg_error_t write_file(const char* path, const char* content,
			       mode_t mode) {
	assert(path != NULL);
	assert(content != NULL);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0) return fpkg_error_from_errno(errno);
	size_t len = strlen(content);
	ssize_t w = write(fd, content, len);
	int e = errno;
	close(fd);
	if (w != (ssize_t)len) return e ? fpkg_error_from_errno(e) : FPKG_EIO;
	return FPKG_OK;
}

static void kf_append(char* buf, size_t cap, size_t* pos, const char* fmt,
		      ...) {
	assert(buf != NULL);
	assert(pos != NULL);
	assert(fmt != NULL);
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
	va_end(ap);
	if (n > 0 && (size_t)n < cap - *pos) *pos += (size_t)n;
	else
		*pos = cap;
}

static void kf_append_exts(char* buf, size_t cap, size_t* pos, const char* key,
			   const fp_ext_group_t* groups, size_t ngroups,
			   const char* prefix) {
	assert(buf != NULL);
	assert(pos != NULL);
	assert(key != NULL);
	char joined[4096] = "";
	size_t j = 0;
	size_t plen = prefix ? strlen(prefix) : 0;
	for (size_t i = 0; i < ngroups; i++) {
		if (!groups[i].id) continue;
		char d[PATH_MAX];
		snprintf(d, sizeof(d), "%s/extension/%s", store_root(),
			 groups[i].id);
		struct stat st;
		if (stat(d, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
		char one[512];
		snprintf(one, sizeof(one), "%s=%s", groups[i].id, d);
		size_t onel = strlen(one);
		if (j + onel + 1 < sizeof(joined)) {
			if (j) joined[j++] = ';';
			memcpy(joined + j, one, onel);
			j += onel;
			joined[j] = '\0';
		}
	}
	if (prefix) {
		char extdir[PATH_MAX];
		snprintf(extdir, sizeof(extdir), "%s/extension", store_root());
		DIR* d = opendir(extdir);
		if (d) {
			struct dirent* de;
			while ((de = readdir(d))) {
				if (de->d_name[0] == '.') continue;
				if (strncmp(de->d_name, prefix, plen) != 0)
					continue;
				char adir[PATH_MAX], ref[512];
				snprintf(adir, sizeof(adir), "%s/%s", extdir,
					 de->d_name);
				DIR* a = opendir(adir);
				if (!a) continue;
				struct dirent* ad;
				int got = 0;
				while ((ad = readdir(a))) {
					if (ad->d_name[0] == '.') continue;
					snprintf(ref, sizeof(ref), "%s/%s",
						 adir, ad->d_name);
					got = 1;
					break;
				}
				closedir(a);
				if (!got) continue;
				char one[512];
				snprintf(one, sizeof(one), "%s=%s", de->d_name,
					 ref);
				size_t onel = strlen(one);
				if (j + onel + 1 < sizeof(joined)) {
					if (j) joined[j++] = ';';
					memcpy(joined + j, one, onel);
					j += onel;
					joined[j] = '\0';
				}
			}
			closedir(d);
		}
	}
	if (joined[0]) kf_append(buf, cap, pos, "%s=%s\n", key, joined);
}

static void kf_append_strlist(char* buf, size_t cap, size_t* pos,
			      const char* key, const strlist_t* sl) {
	assert(buf != NULL);
	assert(pos != NULL);
	assert(key != NULL);
	if (!sl->count) return;
	kf_append(buf, cap, pos, "%s=", key);
	for (size_t i = 0; i < sl->count; i++) {
		if (i) kf_append(buf, cap, pos, ";");
		kf_append(buf, cap, pos, "%s", sl->items[i]);
	}
	kf_append(buf, cap, pos, "\n");
}

static void kf_append_policy(char* buf, size_t cap, size_t* pos,
			     const char* group, const strlist_t* sl) {
	assert(buf != NULL);
	assert(pos != NULL);
	assert(group != NULL);
	assert(sl != NULL);
	if (!sl->count) return;
	kf_append(buf, cap, pos, "[%s]\n", group);
	for (size_t i = 0; i < sl->count; i++)
		kf_append(buf, cap, pos, "%s\n", sl->items[i]);
	kf_append(buf, cap, pos, "\n");
}

static void extract_arch_branch(const char* app_files_dir, char* arch,
				size_t archlen, char* branch,
				size_t branchlen) {
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

static int write_run_environ(const char* path, const char* snapshot,
			     size_t len) {
	assert(path != NULL);
	assert(snapshot != NULL || len == 0);
	(void)unlink(path);
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0400);
	if (fd < 0) return -1;
	ssize_t w = write(fd, snapshot, len);
	close(fd);
	return (w == (ssize_t)len) ? 0 : -1;
}

static int random_uint32(uint32_t* out) {
	assert(out != NULL);
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0) return -1;
	uint32_t v = 0;
	ssize_t r = read(fd, &v, sizeof(v));
	close(fd);
	if (r != (ssize_t)sizeof(v)) return -1;
	*out = v;
	return 0;
}

static bool is_ostree_checksum(const char* s) {
	if (s == NULL || strlen(s) != 64) return false;
	for (int i = 0; i < 64; i++) {
		char c = s[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	}
	return true;
}

static void commit_set(char* out, size_t out_len, const char* v) {
	assert(out != NULL);
	assert(out_len > 0);
	if (is_ostree_checksum(v) && strlen(v) < out_len)
		snprintf(out, out_len, "%s", v);
}

static void deploy_commit(const char* files_dir, char* out, size_t out_len) {
	assert(out != NULL);
	assert(out_len > 0);
	out[0] = '\0';
	if (files_dir == NULL || files_dir[0] == '\0') return;

	char dir[PATH_MAX];
	if ((size_t)snprintf(dir, sizeof(dir), "%s", files_dir) >= sizeof(dir))
		return;
	char* slash = strrchr(dir, '/');
	if (slash == NULL) return;
	*slash = '\0';
	const char* leaf = strrchr(dir, '/');
	leaf = (leaf != NULL) ? leaf + 1 : dir;

	if (is_ostree_checksum(leaf)) {
		commit_set(out, out_len, leaf);
		return;
	}

	char p[PATH_MAX];
	if (strcmp(leaf, "active") == 0) {
		char link[PATH_MAX];
		ssize_t n = readlink(dir, link, sizeof(link) - 1);
		if (n > 0) {
			link[n] = '\0';
			commit_set(out, out_len, link);
			return;
		}
	}

	if ((size_t)snprintf(p, sizeof(p), "%s/deploy", dir) >= sizeof(p))
		return;
	int fd = open(p, O_RDONLY);
	if (fd < 0) return;
	char buf[512];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) return;
	buf[n] = '\0';
	char* commit = strchr(buf, '\0');
	if (commit == NULL) return;
	commit++;
	char* end = strchr(commit, '\0');
	if (end != NULL) *end = '\0';
	commit_set(out, out_len, commit);
}

void instance_write_bwrapinfo(const char* inst_dir, const char* container_path,
			      long pid) {
	assert(pid >= 0);
	if (inst_dir == NULL || inst_dir[0] == '\0') return;

	char bi_path[PATH_MAX];
	if ((size_t)snprintf(bi_path, sizeof(bi_path), "%s/bwrapinfo.json",
			     inst_dir) >= sizeof(bi_path))
		return;

	char esc[PATH_MAX * 2];
	size_t j = 0;
	for (const char* s = (container_path != NULL) ? container_path : "";
	     *s != '\0' && j + 3 < sizeof(esc); s++) {
		if (*s == '"' || *s == '\\') {
			if (j + 3 >= sizeof(esc)) break;
			esc[j++] = '\\';
		}
		esc[j++] = *s;
	}
	esc[j] = '\0';

	char bi[sizeof(esc) + 256];
	snprintf(bi, sizeof(bi),
		 "{\n"
		 "  \"container-path\": \"%s\",\n"
		 "  \"pid\": %ld,\n"
		 "  \"extra-args\": [],\n"
		 "  \"run-environ\": \"run-environ\"\n"
		 "}\n",
		 esc, pid);

	if (write_file(bi_path, bi, 0644) != 0)
		fpkg_warnf("write %s: %s", bi_path, strerror(errno));
}

static void rmdir_contents(const char* dir);

int instance_register(sandbox_t* sb, const fp_metadata_t* meta,
		      const sandbox_plan_t* plan) {
	// Never fatal: on error, clean up and return 0. NULLs are refused
	// rather than asserted, so a NULL here cannot abort the daemon worker.
	if (sb == NULL || meta == NULL || plan == NULL) {
		fpkg_errorf("instance_register: NULL argument");
		return 0;
	}
	sb->instance_lock_fd = -1;
	if (sb->instance_id || sb->instance_dir) {
		fpkg_warnf("instance already registered (%s), skipping",
			   sb->instance_id ? sb->instance_id : "?");
		return 0;
	}

	uint32_t rnd = 0;
	if (random_uint32(&rnd) != 0)
		rnd = (uint32_t)((uintptr_t)getpid() * 2654435761u ^
				 (uint32_t)time(NULL));
	char id[16];
	snprintf(id, sizeof(id), "%u", rnd);

	char inst_base[PATH_MAX];
	if (sandbox_ensure_runtime_dir(plan) != 0)
		fpkg_warnf("cannot provide a usable %s", plan->xdg_runtime_dir);
	snprintf(inst_base, sizeof(inst_base), "%s/.flatpak",
		 plan->xdg_runtime_dir);
	if (mkdir_p(inst_base, 0755) != 0) {
		fpkg_warnf("cannot create %s: %s", inst_base, strerror(errno));
		return 0;
	}
	char inst_dir[PATH_MAX], priv_dir[PATH_MAX];
	int tries;
	for (tries = 0; tries < 1000; tries++) {
		if (tries > 0) rnd = rnd * 1103515245u + 12345u;
		snprintf(id, sizeof(id), "%u", rnd);
		snprintf(inst_dir, sizeof(inst_dir), "%s/%s", inst_base, id);
		if (mkdir(inst_dir, 0755) == 0) break;
		if (errno != EEXIST) {
			fpkg_warnf("mkdir %s: %s", inst_dir, strerror(errno));
			return 0;
		}
	}
	if (tries == 1000) {
		fpkg_warnf("could not allocate a unique instance id");
		return 0;
	}

	snprintf(priv_dir, sizeof(priv_dir), "%s-private", inst_dir);
	if (mkdir(priv_dir, 0700) != 0) {
		fpkg_warnf("mkdir %s: %s", priv_dir, strerror(errno));
		rmdir(inst_dir);
		return 0;
	}

	// .ref lock held for the whole run. O_CLOEXEC so the app does not
	// inherit the fd; the parent's copy keeps the lock alive.
	char ref_path[PATH_MAX];
	snprintf(ref_path, sizeof(ref_path), "%s/.ref", inst_dir);
	int lock_fd =
		open(ref_path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (lock_fd < 0) {
		fpkg_warnf("open %s: %s", ref_path, strerror(errno));
		rmdir(priv_dir);
		rmdir(inst_dir);
		return 0;
	}
	if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
		fpkg_warnf("flock %s: %s", ref_path, strerror(errno));
		close(lock_fd);
		rmdir(priv_dir);
		rmdir(inst_dir);
		return 0;
	}
	char ref_content[512];
	snprintf(ref_content, sizeof(ref_content), "%s\n", meta->app_id);
	ssize_t wr = write(lock_fd, ref_content, strlen(ref_content));
	if (wr < 0) {
	}

	char runtime_id[256] = "";
	{
		const char* rf = meta->runtime_ref ? meta->runtime_ref : "";
		const char* slash = strchr(rf, '/');
		snprintf(runtime_id, sizeof(runtime_id), "%.*s",
			 (int)(slash ? (size_t)(slash - rf) : strlen(rf)), rf);
	}
	char branch[64] = "", arch[64] = "";
	extract_arch_branch(sb->app_files_dir, arch, sizeof(arch), branch,
			    sizeof(branch));
	if (!branch[0] && meta->branch)
		snprintf(branch, sizeof(branch), "%s", meta->branch);
	if (!arch[0] && meta->runtime_ref) {
		const char* a = strchr(meta->runtime_ref, '/');
		if (a) {
			a++;
			const char* e = strchr(a, '/');
			snprintf(arch, sizeof(arch), "%.*s",
				 (int)(e ? (size_t)(e - a) : strlen(a)), a);
		}
	}
	char info[8192];
	size_t pos = 0;
	char rr[320] = "";
	if (meta->runtime_ref && *meta->runtime_ref) {
		if (strncmp(meta->runtime_ref, "runtime/", 8) == 0 ||
		    strncmp(meta->runtime_ref, "app/", 4) == 0)
			snprintf(rr, sizeof(rr), "%s", meta->runtime_ref);
		else
			snprintf(rr, sizeof(rr), "runtime/%s",
				 meta->runtime_ref);
	}
	kf_append(info, sizeof(info), &pos,
		  "[Application]\nname=%s\nruntime=%s\n\n", meta->app_id, rr);
	kf_append(info, sizeof(info), &pos,
		  "[Runtime]\nname=%s\nruntime=%s\n\n", runtime_id, rr);
	kf_append(info, sizeof(info), &pos,
		  "[Instance]\nflatpak-version=%s\ninstance-id=%s\npath=%s\n"
		  "instance-path=%s\n"
		  "app-path=%s\nruntime-path=%s\nbranch=%s\narch=%s\n"
		  "jid=%d\nuid=%u\ngid=%u\n",
		  FLATPAK_EMULATED_VERSION, id,
		  sb->jailroot ? sb->jailroot : "",
		  sb->jailroot ? sb->jailroot : "",
		  sb->app_files_dir ? sb->app_files_dir : "",
		  sb->runtime_dir ? sb->runtime_dir : "", branch, arch, sb->jid,
		  (unsigned)plan->uid, (unsigned)plan->gid);
	{
		char ac[80] = "", rc[80] = "";
		deploy_commit(sb->app_files_dir, ac, sizeof(ac));
		deploy_commit(sb->runtime_dir, rc, sizeof(rc));
		if (ac[0] && rc[0])
			kf_append(info, sizeof(info), &pos,
				  "app-commit=%s\nruntime-commit=%s\n", ac, rc);
		else if (ac[0])
			kf_append(info, sizeof(info), &pos, "app-commit=%s\n",
				  ac);
		else if (rc[0])
			kf_append(info, sizeof(info), &pos,
				  "runtime-commit=%s\n", rc);
	}
	kf_append(info, sizeof(info), &pos, "devel=%s\n",
		  plan->devel ? "true" : "false");
	kf_append(info, sizeof(info), &pos,
		  "session-bus-proxy=true\n"
		  "system-bus-proxy=true\nsandbox=true\n");
	kf_append_exts(info, sizeof(info), &pos, "app-extensions",
		       meta->ext_groups, meta->next_groups, NULL);
	kf_append_exts(info, sizeof(info), &pos, "runtime-extensions", NULL, 0,
		       runtime_id);
	if (meta->shared.count || meta->sockets.count || meta->devices.count ||
	    meta->filesystems.count) {
		kf_append(info, sizeof(info), &pos, "[Context]\n");
		kf_append_strlist(info, sizeof(info), &pos, "shared",
				  &meta->shared);
		kf_append_strlist(info, sizeof(info), &pos, "sockets",
				  &meta->sockets);
		kf_append_strlist(info, sizeof(info), &pos, "devices",
				  &meta->devices);
		kf_append_strlist(info, sizeof(info), &pos, "filesystems",
				  &meta->filesystems);
		kf_append(info, sizeof(info), &pos, "\n");
	}
	if (meta->env_pairs.count > 0) {
		kf_append(info, sizeof(info), &pos, "[Environment]\n");
		for (size_t i = 0; i < meta->env_pairs.count; i++)
			kf_append(info, sizeof(info), &pos, "%s\n",
				  meta->env_pairs.items[i]);
		kf_append(info, sizeof(info), &pos, "\n");
	}
	kf_append_policy(info, sizeof(info), &pos, "Session Bus Policy",
			 &meta->session_bus_policy);
	kf_append_policy(info, sizeof(info), &pos, "System Bus Policy",
			 &meta->system_bus_policy);
	kf_append_policy(info, sizeof(info), &pos, "A11y Bus Policy",
			 &meta->a11y_bus_policy);

	char info_path[PATH_MAX] = "", runenv_path[PATH_MAX] = "";
	snprintf(info_path, sizeof(info_path), "%s/info", inst_dir);
	if (write_file(info_path, info, 0644) != 0) {
		fpkg_warnf("write %s: %s", info_path, strerror(errno));
		goto fail;
	}
	instance_write_bwrapinfo(inst_dir, sb->jailroot ? sb->jailroot : "", 0);
	snprintf(runenv_path, sizeof(runenv_path), "%s/run-environ", priv_dir);

	{
		const char* nonce = NULL;
		for (size_t i = 0; i < plan->env_overrides.count; i++) {
			if (strncmp(plan->env_overrides.items[i],
				    "FLATPKG_SPAWN_NONCE=", 20) == 0) {
				nonce = plan->env_overrides.items[i];
				break;
			}
		}
		if (nonce) {
			char nonce_path[PATH_MAX];
			snprintf(nonce_path, sizeof(nonce_path),
				 "%s/spawn-nonce", inst_dir);
			if (write_file(nonce_path, nonce, 0644) != 0)
				fpkg_warnf("write %s: %s", nonce_path,
					   strerror(errno));
		}
	}

	if (sb->jailroot) {
		char mir[PATH_MAX], p[PATH_MAX];
		snprintf(mir, sizeof(mir), "%s/run/flatpak/.flatpak/%s",
			 sb->jailroot, id);
		if (mkdir_p(mir, 0755) == 0) {
			snprintf(p, sizeof(p), "%s/info", mir);
			if (write_file(p, info, 0644) != 0)
				fpkg_warnf("write %s: %s", p, strerror(errno));
			snprintf(p, sizeof(p), "%s/.ref", mir);
			if (write_file(p, ref_content, 0644) != 0)
				fpkg_warnf("write %s: %s", p, strerror(errno));
		} else {
			fpkg_warnf("mkdir %s: %s", mir, strerror(errno));
		}
		snprintf(p, sizeof(p), "%s/.flatpak-info", sb->jailroot);
		if (write_file(p, info, 0644) != 0)
			fpkg_warnf("write %s: %s", p, strerror(errno));
		snprintf(p, sizeof(p), "%s/run/.flatpak-info", sb->jailroot);
		if (write_file(p, info, 0644) != 0)
			fpkg_warnf("write %s: %s", p, strerror(errno));
	}

	chmod(inst_base, 0755);
	chown(inst_base, plan->uid, plan->gid);
	chown(inst_dir, plan->uid, plan->gid);
	chown(priv_dir, plan->uid, plan->gid);
	chown(info_path, plan->uid, plan->gid);
	chown(ref_path, plan->uid, plan->gid);
	{
		char nonce_path[PATH_MAX];
		snprintf(nonce_path, sizeof(nonce_path), "%s/spawn-nonce",
			 inst_dir);
		if (access(nonce_path, F_OK) == 0)
			chown(nonce_path, plan->uid, plan->gid);
	}

	sb->instance_id = xstrdup(id);
	sb->instance_dir = xstrdup(inst_dir);
	sb->instance_lock_fd = lock_fd;
	return 0;

fail:
	close(lock_fd);
	unlink(runenv_path);
	rmdir(priv_dir);
	rmdir_contents(inst_dir);
	return 0;
}

static void rmdir_contents(const char* dir) {
	assert(dir != NULL);
	DIR* d = opendir(dir);
	if (d) {
		struct dirent* e;
		while ((e = readdir(d)) != NULL) {
			if (strcmp(e->d_name, ".") == 0 ||
			    strcmp(e->d_name, "..") == 0)
				continue;
			char p[PATH_MAX];
			snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
			unlink(p);
		}
		closedir(d);
	}
	rmdir(dir);
}

static int snapshot_put(const char*** kvp, size_t* n, size_t* cap,
			const char* kv) {
	assert(kvp != NULL);
	assert(n != NULL);
	assert(cap != NULL);
	assert(kv != NULL);
	size_t kl = strcspn(kv, "=");
	const char** g;

	for (size_t i = 0; i < *n; i++) {
		if (strncmp((*kvp)[i], kv, kl) == 0 && (*kvp)[i][kl] == '=')
			return 0;
	}
	if (*n == *cap) {
		g = realloc(*kvp, *cap * 2 * sizeof(*g));
		if (g == NULL) return -1;
		*kvp = g;
		*cap *= 2;
	}
	(*kvp)[(*n)++] = kv;
	return 0;
}

static int snapshot_env(char** out, size_t* outlen, const fp_metadata_t* meta,
			const sandbox_plan_t* plan) {
	assert(out != NULL);
	assert(outlen != NULL);
	size_t cap = 64, n = 0, total = 1;
	const char** kv = calloc(cap, sizeof(*kv));
	char *buf, *p;

	if (kv == NULL) return -1;
	for (char** e = cli_environ(); e && *e; e++)
		if (snapshot_put(&kv, &n, &cap, *e) != 0) goto fail;
	for (size_t i = 0; meta != NULL && i < meta->env_pairs.count; i++)
		if (snapshot_put(&kv, &n, &cap, meta->env_pairs.items[i]) != 0)
			goto fail;
	for (size_t i = 0; plan != NULL && i < plan->env_overrides.count; i++)
		if (snapshot_put(&kv, &n, &cap, plan->env_overrides.items[i]) !=
		    0)
			goto fail;
	for (size_t i = 0; i < n; i++) total += strlen(kv[i]) + 1;
	buf = malloc(total);
	if (buf == NULL) goto fail;
	p = buf;
	for (size_t i = 0; i < n; i++) {
		size_t l = strlen(kv[i]) + 1;
		memcpy(p, kv[i], l);
		p += l;
	}
	// `total` started at 1 to leave room for this: an empty entry ends the
	// list.  Without it the last byte of the file was whatever the heap
	// held.
	*p = '\0';
	free(kv);
	*out = buf;
	*outlen = total;
	return 0;
fail:
	free(kv);
	return -1;
}

void instance_save_run_environ(const sandbox_t* sb, const fp_metadata_t* meta,
			       const sandbox_plan_t* plan) {
	char path[PATH_MAX];
	char* snapshot = NULL;
	size_t snaplen = 0;

	if (sb == NULL || sb->instance_dir == NULL) return;
	snprintf(path, sizeof(path), "%s-private/run-environ",
		 sb->instance_dir);
	if (snapshot_env(&snapshot, &snaplen, meta, plan) != 0) return;
	if (write_run_environ(path, snapshot, snaplen) != 0) {
		fpkg_warnf("cannot record the app's environment at %s: %s",
			   path, strerror(errno));
		free(snapshot);
		return;
	}
	if (plan != NULL && plan->uid != 0)
		(void)chown(path, (uid_t)plan->uid, (gid_t)plan->gid);
	free(snapshot);
}

void instance_unregister(sandbox_t* sb) {
	if (sb == NULL) return;
	if (sb->instance_lock_fd >= 0) {
		close(sb->instance_lock_fd);
		sb->instance_lock_fd = -1;
	}
	if (sb->instance_dir) {
		char p[PATH_MAX];
		rmdir_contents(sb->instance_dir);

		snprintf(p, sizeof(p), "%s-private/run-environ",
			 sb->instance_dir);
		unlink(p);
		snprintf(p, sizeof(p), "%s-private", sb->instance_dir);
		rmdir(p);

		free(sb->instance_dir);
		sb->instance_dir = NULL;
	}
	free(sb->instance_id);
	sb->instance_id = NULL;
}

static int resolve_jid(const char* arg, char* info_path, size_t info_len) {
	assert(arg != NULL);
	// info_path is optional: cmd_kill passes NULL/0, cmd_enter passes a
	// buffer. info_len is only meaningful when info_path is non-NULL.
	assert(info_path == NULL || info_len > 0);

	if (info_path) info_path[0] = '\0';

	bool numeric = arg[0] != '\0';
	for (const char* c = arg; *c; c++) {
		if (*c < '0' || *c > '9') {
			numeric = false;
			break;
		}
	}
	if (numeric) {
		char dir[PATH_MAX];
		snprintf(dir, sizeof(dir), "%s/%s", instances_dir(), arg);
		struct stat st;
		if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
			char infof[PATH_MAX], buf[64];
			snprintf(infof, sizeof(infof), "%s/info", dir);
			if (kf_get(infof, "Instance", "jid", buf,
				   sizeof(buf)) != 0) {
				fpkg_errorf("instance %s has no info "
					    "keyfile",
					    arg);
				return -1;
			}
			int jid = atoi(buf);
			if (jid <= 0) {
				fpkg_errorf("instance %s has no valid "
					    "jail id in its info keyfile",
					    arg);
				return -1;
			}
			if (info_path)
				snprintf(info_path, info_len, "%s", infof);
			return jid;
		}
	}

	char* end = NULL;
	long jid = strtol(arg, &end, 10);
	if (end == arg || *end != '\0' || jid <= 0) {
		fpkg_errorf("'%s' is neither a running instance id nor a "
			    "jail id",
			    arg);
		return -1;
	}
	return (int)jid;
}

// Is this jail one of ours? A bare jail id reaches jail_remove() as root, so
// the test is the jail's name prefix: a jail we did not create cannot carry it.
static bool jail_is_flatpkg_owned(int jid) {
	assert(jid > 0);
	char* name = jail_getname(jid);
	bool ours = false;

	if (name != NULL && strncmp(name, FLATPKG_JAIL_NAME_PREFIX,
				    sizeof(FLATPKG_JAIL_NAME_PREFIX) - 1) == 0)
		ours = true;
	free(name);
	return ours;
}

static int require_flatpkg_jail(int jid) {
	assert(jid > 0);
	if (jail_is_flatpkg_owned(jid)) return 0;
	// EPERM and ENOENT both mean "not ours to act on".
	fpkg_errorf("not a flatpkg instance: no jail with id %d that this "
		    "user may act on, or it is not one flatpkg created",
		    jid, FLATPKG_JAIL_NAME_PREFIX);
	return -1;
}

// The uid sandbox_create_jail() recorded in the jail's `meta` parameter, or -1
// (no parameter, or not our jail). The kernel holds it, so a dir cannot fake it.
static long jail_owner_uid(int jid) {
	assert(jid > 0);
	struct jailparam p[2];
	char jidstr[32];
	snprintf(jidstr, sizeof(jidstr), "%d", jid);
	if (jailparam_init(&p[0], "jid") != 0) return -1;
	if (jailparam_import(&p[0], jidstr) != 0) {
		jailparam_free(&p[0], 1);
		return -1;
	}
	if (jailparam_init(&p[1], "meta") != 0) {
		jailparam_free(&p[0], 1);
		return -1;
	}
	long owner = -1;
	if (jailparam_get(p, 2, 0) >= 0) {
		char* meta = jailparam_export(&p[1]);
		const size_t klen = sizeof(FLATPKG_JAIL_OWNER_KEY "=") - 1;
		if (meta != NULL &&
		    strncmp(meta, FLATPKG_JAIL_OWNER_KEY "=", klen) == 0) {
			char* end = NULL;
			errno = 0;
			unsigned long v = strtoul(meta + klen, &end, 10);
			if (errno == 0 && end != meta + klen && *end == '\0')
				owner = (long)v;
		}
		free(meta);
	}
	jailparam_free(p, 2);
	return owner;
}

static uid_t caller_uid(void) {
	const char* s = cli_getenv("FLATPKG_APP_UID");
	if (s == NULL || *s == '\0') return 0;
	char* end = NULL;
	errno = 0;
	unsigned long v = strtoul(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0') return (uid_t)-1;
	return (uid_t)v;
}

// enter and kill act as root, so only for the user the jail was made for (or root).
// A jail with no recorded owner cannot be shown to be theirs, so only root acts.
static int require_jail_owner(int jid) {
	assert(jid > 0);
	uid_t me = caller_uid();
	if (me == 0) return 0;
	long owner = jail_owner_uid(jid);
	if (owner >= 0 && (uid_t)owner == me) return 0;
	if (owner < 0)
		fpkg_errorf(
			"jail %d has no recorded owner (it was made before "
			"owners were recorded, or is not one flatpkg made): "
			"only root may act on it",
			jid);
	else
		fpkg_errorf("jail %d belongs to another user", jid);
	return -1;
}

int cmd_kill(const char* arg) {
	assert(arg != NULL);
	if (geteuid() != 0) {
		fpkg_errorf("'kill' needs root (jail_remove privileges) - "
			    "invoke via mdo/doas");
		return 1;
	}
	int jid = resolve_jid(arg, NULL, 0);
	if (jid <= 0) return 1;
	if (require_flatpkg_jail(jid) != 0) return 1;
	if (require_jail_owner(jid) != 0) return 1;

	if (jail_remove(jid) != 0) {
		fpkg_errorf("jail_remove(%d): %s", jid, strerror(errno));
		return 1;
	}
	fp_logger_okf(&log, "killed jail %d", jid);
	return 0;
}

int cmd_enter(const char* arg, char* const extra_argv[]) {
	assert(arg != NULL);
	if (geteuid() != 0) {
		fpkg_errorf("'enter' needs root (jail_attach privileges) - "
			    "invoke via mdo/doas");
		return 1;
	}
	char info_path[PATH_MAX];
	int jid = resolve_jid(arg, info_path, sizeof(info_path));
	if (jid <= 0) return 1;
	char inst_dir[PATH_MAX] = "";
	if (info_path[0] != '\0') {
		snprintf(inst_dir, sizeof(inst_dir), "%s", info_path);
		char* slash = strrchr(inst_dir, '/');
		if (slash != NULL && slash != inst_dir) *slash = '\0';
		else
			inst_dir[0] = '\0';
	}
	if (require_flatpkg_jail(jid) != 0) return 1;
	if (require_jail_owner(jid) != 0) return 1;

	uid_t uid = 0;
	gid_t gid = 0;
	if (info_path[0]) {
		char buf[64];
		if (kf_get(info_path, "Instance", "uid", buf, sizeof(buf)) == 0)
			uid = (uid_t)strtoul(buf, NULL, 10);
		if (kf_get(info_path, "Instance", "gid", buf, sizeof(buf)) == 0)
			gid = (gid_t)strtoul(buf, NULL, 10);
	} else {
		const char* cu = cli_getenv("FLATPKG_APP_UID");
		const char* cg = cli_getenv("FLATPKG_APP_GID");
		if (cu != NULL && *cu != '\0')
			uid = (uid_t)strtoul(cu, NULL, 10);
		if (cg != NULL && *cg != '\0')
			gid = (gid_t)strtoul(cg, NULL, 10);
	}

	// As in sandbox_exec(): the command must not start while this process still holds
	// a pty slave, or FreeBSD drops queued output. The child waits; the parent drops it.
	int gate[2];
	if (pipe(gate) != 0) {
		fpkg_errorf("pipe: %s", strerror(errno));
		return 1;
	}
	(void)fcntl(gate[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(gate[1], F_SETFD, FD_CLOEXEC);

	pid_t pid = fork();
	if (pid < 0) {
		fpkg_errorf("fork: %s", strerror(errno));
		close(gate[0]);
		close(gate[1]);
		return 1;
	}
	if (pid == 0) {
		close(gate[1]);
		// FLATPKG_APP_TTY (caller on a terminal): the command gets the pty slave as stdio
		// and controlling terminal, never the caller's own, before closefrom().
		int ttyfd = -1;
		unsigned ttymask = 0;
		if (sandbox_app_tty(&ttyfd, &ttymask) && ttyfd >= 0) {
			int ctty = -1;
			for (int i = 0; i < 3; i++)
				if (ttymask & (1u << i)) {
					(void)dup2(ttyfd, i);
					if (ctty < 0) ctty = i;
				}
			if (ctty >= 0) {
				(void)setsid();
				(void)ioctl(ctty, TIOCSCTTY, 0);
			}
		}

		{
			char ch;
			while (read(gate[0], &ch, 1) < 0 && errno == EINTR);
			close(gate[0]);
		}

		if (gid != 0) (void)setgroups(1, &gid);

		closefrom(3);
		if (jail_attach(jid) != 0) {
			dprintf(2,
				"flatpkg enter: jail_attach(%d): "
				"%s\n",
				jid, strerror(errno));
			_exit(127);
		}
		if (chdir("/") != 0) {
			dprintf(2, "flatpkg enter: chdir: %s\n",
				strerror(errno));
			_exit(127);
		}
		if (gid != 0) setgid(gid);
		if (uid != 0) setuid(uid);

		char** recorded = instance_load_run_environ(inst_dir);
		if (recorded != NULL) {
			for (char** e = recorded; *e != NULL; e++)
				if (strchr(*e, '=') != NULL)
					setenv(*e, strchr(*e, '=') + 1, 1);
		} else {
			fpkg_warnf(
				"no run-environ snapshot for this"
				" instance; the shell gets only the sandbox's"
				" own environment");
		}
		g_strfreev(recorded);

		setenv("PATH", "/app/bin:/usr/bin", 1);
		setenv("SHELL", "/bin/sh", 1);
		char xdg[64];
		snprintf(xdg, sizeof(xdg), "/run/user/%u", (unsigned)uid);
		setenv("XDG_RUNTIME_DIR", xdg, 1);
		struct passwd* pw = getpwuid(uid);
		if (pw) setenv("HOME", pw->pw_dir, 1);

		const char* cmd = (extra_argv && extra_argv[0]) ? extra_argv[0]
								: "/bin/sh";
		// argv = [cmd, extra[1..], NULL]; nextra == 0 must not wipe
		// argv[0].
		size_t nargs = 0;
		while (extra_argv && extra_argv[nargs]) nargs++;
		char** argv = calloc(nargs + 2, sizeof(char*));
		if (!argv) _exit(127);
		size_t ai = 0;
		argv[ai++] = (char*)cmd;
		for (size_t i = 1; i < nargs; i++) argv[ai++] = extra_argv[i];
		argv[ai] = NULL;
		umask(0);
		execvp(cmd, argv);
		dprintf(2, "flatpkg enter: execvp %s: %s\n", cmd,
			strerror(errno));
		_exit(127);
	}

	int ttyfd = -1;
	unsigned ttymask = 7;
	(void)sandbox_app_tty(&ttyfd, &ttymask);
	sandbox_release_app_tty();
	close(gate[0]);
	{
		char ch = 'g';
		if (write(gate[1], &ch, 1) != 1)
			fpkg_warnf("could not release the entered command: %s",
				   strerror(errno));
	}
	close(gate[1]);
	int status = 0;
	const char* master_s = cli_getenv("FLATPKG_APP_MASTER");
	if (master_s != NULL && *master_s != '\0') {
		// Carry the caller's keys and window to the pty and its output back until the
		// command ends; the caller going away ends it too.
		const int in_fd = (ttymask & 1u) ? 0 : -1;
		const int out_fd =
			(ttymask & 2u) ? 1 : ((ttymask & 4u) ? 2 : -1);
		const int size_fd = (ttymask & 1u)   ? 0
				    : (ttymask & 2u) ? 1
				    : (ttymask & 4u) ? 2
						     : -1;
		(void)sandbox_pty_relay(atoi(master_s), in_fd, out_fd, size_fd,
					pid, true, &status);
	} else {
		while (waitpid(pid, &status, 0) < 0) {
			if (errno == EINTR) continue;
			fpkg_errorf("waitpid: %s", strerror(errno));
			return 1;
		}
	}
	if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
