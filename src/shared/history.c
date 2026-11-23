#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "flatpkg.h"
#define FPKG_MODULE "history"
#include "fpkglog.h"

static void history_path(char* buf, size_t len) {
	assert(buf != NULL);
	assert(len > 0);
	snprintf(buf, len, "%s/history", store_root());
}

// A field of the history record: the separators cannot appear inside one.
static void history_field(char* out, size_t n, const char* in,
			  const char* dash) {
	size_t k = 0;

	if (out == NULL || n == 0) return;
	if (in == NULL || *in == '\0') in = dash;
	for (const char* p = in; *p != '\0' && k + 1 < n; p++)
		out[k++] = (*p == '\t' || *p == '\n' || *p == '\r') ? '_' : *p;
	out[k] = '\0';
}

int history_append(const char* action, const char* ref, const char* old_version,
		   const char* new_version) {
	char path[PATH_MAX];
	history_path(path, sizeof(path));
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s", store_root());
	(void)mkdir(dir, 0755);
	FILE* f = fopen(path, "a");
	if (!f) {
		fpkg_warnf("cannot write history: %s", strerror(errno));
		return -1;
	}
	// The version strings come out of app metadata, and this is a
	// tab-separated record: a tab or newline in one would forge a field or a
	// whole line in `flatpkg history`.
	time_t now = time(NULL);
	char oldv[256], newv[256];

	history_field(oldv, sizeof(oldv), old_version, "-");
	history_field(newv, sizeof(newv), new_version, "-");
	fprintf(f, "%ld\t%s\t%s\t%s\t%s\n", (long)now, action ? action : "?",
		ref ? ref : "?", oldv, newv);
	fclose(f);
	return 0;
}

// The returned pointers point into the line buffer, so the caller does not
// free them.
static int history_parse_line(char* line, time_t* ts, char** action, char** ref,
			      char** old_v, char** new_v) {
	assert(line != NULL);
	assert(ts != NULL);
	assert(action != NULL);
	assert(ref != NULL);
	assert(old_v != NULL);
	assert(new_v != NULL);
	*ts = 0;
	*action = *ref = *old_v = *new_v = NULL;
	char* p = line;
	char* fields[5];
	int nf = 0;
	while (nf < 5 && p) {
		fields[nf++] = p;
		char* tab = strchr(p, '\t');
		if (tab) {
			*tab = '\0';
			p = tab + 1;
		} else {
			p = NULL;
		}
	}
	if (nf < 5) return -1;
	*ts = (time_t)strtoul(fields[0], NULL, 10);
	*action = fields[1];
	*ref = fields[2];
	*old_v = fields[3];
	*new_v = fields[4];
	return 0;
}

int cmd_history(void) {
	char path[PATH_MAX];
	history_path(path, sizeof(path));
	FILE* f = fopen(path, "r");
	if (!f) {
		fpkg_infof("no history yet");
		return 0;
	}
	char line[1024];
	int n = 0;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		time_t ts;
		char *action, *ref, *old_v, *new_v;
		if (history_parse_line(line, &ts, &action, &ref, &old_v,
				       &new_v) != 0)
			continue;
		char tbuf[32];
		struct tm tm;
		localtime_r(&ts, &tm);
		strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", &tm);
		char ver[256] = "";
		if (strcmp(old_v, "-") != 0 && strcmp(new_v, "-") != 0)
			snprintf(ver, sizeof(ver), " %s -> %s", old_v, new_v);
		else if (strcmp(new_v, "-") != 0)
			snprintf(ver, sizeof(ver), " %s", new_v);
		else if (strcmp(old_v, "-") != 0)
			snprintf(ver, sizeof(ver), " %s", old_v);
		fpkg_infof("%s  %s  %s%s", tbuf, action, ref, ver);
		n++;
	}
	fclose(f);
	if (n == 0) fpkg_infof("history is empty");
	return 0;
}
