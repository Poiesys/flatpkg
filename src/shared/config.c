#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>

#include "flatpkg.h"

#ifndef SYSCONFDIR
#define SYSCONFDIR "/usr/local/etc"
#endif

static int config_path(char* out, size_t len) {
	snprintf(out, len, "%s/helper.conf", store_root());
	struct stat st;
	if (lstat(out, &st) == 0 && S_ISREG(st.st_mode)) return 0;
	snprintf(out, len, "%s/flatpkg/helper.conf", SYSCONFDIR);
	if (lstat(out, &st) == 0 && S_ISREG(st.st_mode)) return 0;
	return -1;
}

static void tokens_free(char** argv, size_t n) {
	for (size_t i = 0; i < n; i++) free(argv[i]);
}

// What a placeholder stands for, or NULL when the character is not one.
static const char* placeholder(const char c, const fpkg_helper_args_t* args) {
	assert(args != NULL);
	switch (c) {
	case 'r': return args->ref;
	case 'i': return args->installation;
	case 'b': return args->bundle;
	case 'n': return args->remote;
	case 'o': return args->origin;
	case 'a': return args->arch;
	case 'p': return args->repo;
	case 's': return args->subpaths;
	case 'P': return args->previous;
	case 'c': return args->config;
	case 'u': return args->url;
	case 'k': return args->key;
	case 'v': return args->value;
	case 'S': return args->summary;
	case 'G': return args->summary_sig;
	case 'd': return args->src_dir;
	default: return NULL;
	}
}

// A placeholder is substituted wherever it appears in a word, not only when
// the word is exactly one: the shipped map spells `--remote=%o`, which used
// to be passed through verbatim and made that entry inert.
static char* expand_word(const char* word, const fpkg_helper_args_t* args) {
	size_t cap = strlen(word) + 64, n = 0;
	char* out = malloc(cap);
	char flag[16];

	if (out == NULL) return NULL;
	for (const char* p = word; *p != '\0';) {
		const char* val = NULL;
		size_t len;

		if (*p == '%' && p[1] != '\0') {
			if (p[1] == 'f') {
				snprintf(flag, sizeof(flag), "%u", args->flags);
				val = flag;
			} else {
				val = placeholder(p[1], args);
			}
			if (val != NULL) {
				len = strlen(val);
				if (n + len + 1 > cap) {
					char* grown;
					while (n + len + 1 > cap) cap *= 2;
					grown = realloc(out, cap);
					if (grown == NULL) {
						free(out);
						return NULL;
					}
					out = grown;
				}
				memcpy(out + n, val, len);
				n += len;
				p += 2;
				continue;
			}
		}
		if (n + 2 > cap) {
			char* grown = realloc(out, cap * 2);
			if (grown == NULL) {
				free(out);
				return NULL;
			}
			out = grown;
			cap *= 2;
		}
		out[n++] = *p++;
	}
	out[n] = '\0';
	return out;
}

int fpkg_helper_command(const char* request, const fpkg_helper_args_t* args,
			char*** argv_out, size_t* argc_out) {
	assert(request != NULL);
	assert(argv_out != NULL);
	assert(argc_out != NULL);
	*argv_out = NULL;
	*argc_out = 0;
	char path[PATH_MAX];
	if (config_path(path, sizeof(path)) != 0) return 0;

	FILE* f = fopen(path, "r");
	if (f == NULL) return 0;
	char line[4096];
	bool found = false;
	char value[4096] = "";
	while (fgets(line, sizeof(line), f)) {
		char* s = trim(line);
		if (*s == '\0' || *s == '#') continue;
		char* eq = strchr(s, '=');
		if (eq == NULL) continue;
		*eq = '\0';
		if (strcmp(trim(s), request) != 0) continue;
		snprintf(value, sizeof(value), "%s", trim(eq + 1));
		found = true;
		break;
	}
	fclose(f);
	if (!found || value[0] == '\0') return 0;

	char* argv[FPKG_HELPER_MAX_ARGS + 1];
	size_t n = 0;
	char* save = NULL;
	for (char* tok = strtok_r(value, " \t\r\n", &save); tok != NULL;
	     tok = strtok_r(NULL, " \t\r\n", &save)) {
		if (n == FPKG_HELPER_MAX_ARGS) {
			tokens_free(argv, n);
			return -1;
		}
		argv[n++] = expand_word(tok, args);
		if (argv[n - 1] == NULL) {
			tokens_free(argv, n);
			return -1;
		}
	}
	if (n == 0 || argv[0][0] != '/') {
		tokens_free(argv, n);
		return -1;
	}
	argv[n] = NULL;
	*argv_out = calloc(n + 1, sizeof(char*));
	if (*argv_out == NULL) {
		tokens_free(argv, n);
		return -1;
	}
	for (size_t i = 0; i <= n; i++) (*argv_out)[i] = argv[i];
	*argc_out = n;
	return 1;
}
