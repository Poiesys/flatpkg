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

static char* trim(char* s) {
	while (*s && isspace((unsigned char)*s)) s++;
	char* end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1])) end--;
	*end = '\0';
	return s;
}

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
		const char* word = tok;
		if (strcmp(tok, "%r") == 0)
			word = args->ref != NULL ? args->ref : "";
		else if (strcmp(tok, "%i") == 0)
			word = args->installation != NULL ? args->installation
							  : "";
		else if (strcmp(tok, "%b") == 0)
			word = args->bundle != NULL ? args->bundle : "";
		else if (strcmp(tok, "%n") == 0)
			word = args->remote != NULL ? args->remote : "";
		else if (strcmp(tok, "%o") == 0)
			word = args->origin != NULL ? args->origin : "";
		else if (strcmp(tok, "%a") == 0)
			word = args->arch != NULL ? args->arch : "";
		else if (strcmp(tok, "%p") == 0)
			word = args->repo != NULL ? args->repo : "";
		else if (strcmp(tok, "%s") == 0)
			word = args->subpaths != NULL ? args->subpaths : "";
		else if (strcmp(tok, "%P") == 0)
			word = args->previous != NULL ? args->previous : "";
		else if (strcmp(tok, "%c") == 0)
			word = args->config != NULL ? args->config : "";
		else if (strcmp(tok, "%u") == 0)
			word = args->url != NULL ? args->url : "";
		else if (strcmp(tok, "%k") == 0)
			word = args->key != NULL ? args->key : "";
		else if (strcmp(tok, "%v") == 0)
			word = args->value != NULL ? args->value : "";
		else if (strcmp(tok, "%S") == 0)
			word = args->summary != NULL ? args->summary : "";
		else if (strcmp(tok, "%G") == 0)
			word = args->summary_sig != NULL ? args->summary_sig : "";
		else if (strcmp(tok, "%d") == 0)
			word = args->src_dir != NULL ? args->src_dir : "";
		else if (strcmp(tok, "%f") == 0) {
			static char flagbuf[16];
			snprintf(flagbuf, sizeof(flagbuf), "%u", args->flags);
			word = flagbuf;
		}
		argv[n++] = strdup(word);
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
