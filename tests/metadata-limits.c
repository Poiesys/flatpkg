// metadata-limits.c - the metadata parser's limits, driven through the real
// fp_metadata_parse(): a value longer than the line buffer must not have its
// tail parsed as a fresh key=value pair, a group name too long for the buffer
// must not be truncated into another group, and the environment list is
// capped.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flatpkg.h"

// Answered here because this harness links only the parser and the logging it
// warns through: these are cli.c's and the translation layer's, not the
// parser's.
const char* get_string(const char* msgid) { return msgid; }

const char* cli_getenv(const char* name) { (void)name; return NULL; }

static int bad;

static void note(const char* what) {
	printf("FAIL: %s\n", what);
	bad = 1;
}

static int write_temp(const char* body, size_t len) {
	static int seq;
	char path[64];
	FILE* f;

	snprintf(path, sizeof(path), "/tmp/.fpkg-meta-limits.%d", seq++);
	f = fopen(path, "w");
	if (f == NULL) return -1;
	fwrite(body, 1, len, f);
	fclose(f);
	return atoi(path + strlen("/tmp/.fpkg-meta-limits."));
}

static char* temp_path(int id) {
	static char path[64];

	snprintf(path, sizeof(path), "/tmp/.fpkg-meta-limits.%d", id);
	return path;
}

// A line of ~5 KiB: its first bufferful is a truncated KEY=..., and the rest
// used to come back from the next fgets() as INJECTED=yes.
static void long_line_case(void) {
	size_t longlen = 4900;
	size_t len = longlen + 200;
	char* body = calloc(1, len);
	size_t n = 0;

	n += (size_t)snprintf(body + n, len - n, "[Application]\nname=Demo\ncommand=demo\n");
	n += (size_t)snprintf(body + n, len - n, "[Environment]\nKEY=");
	memset(body + n, 'x', longlen);
	n += longlen;
	n += (size_t)snprintf(body + n, len - n, "INJECTED=yes\nAFTER=ok\n");

	int id = write_temp(body, n);
	free(body);
	if (id < 0) {
		note("could not write the long-line case");
		return;
	}
	fp_metadata_t m;
	if (fp_metadata_parse(temp_path(id), &m) != 0) {
		note("the long-line case did not parse");
		remove(temp_path(id));
		return;
	}
	if (m.env_pairs.count != 1) {
		printf("FAIL: the long line produced %zu environment entries, "
		       "wanted 1\n",
		       m.env_pairs.count);
		bad = 1;
	}
	for (size_t i = 0; i < m.env_pairs.count; i++)
		if (strncmp(m.env_pairs.items[i], "INJECTED=", 9) == 0) {
			printf("FAIL: the tail of a long line became '%s'\n",
			       m.env_pairs.items[i]);
			bad = 1;
		}
	fp_metadata_free(&m);
	remove(temp_path(id));
}

// Two [Extension ...] groups sharing their first 95 characters: truncation
// merged them into one.
static void long_group_case(void) {
	// Longer than the group buffer (127): truncating these into one buffer
	// merged two extensions into one entry.
	char prefix[130];
	char body[512];
	size_t n = 0;

	memset(prefix, 'g', sizeof(prefix));
	prefix[sizeof(prefix) - 1] = '\0';
	n += (size_t)snprintf(body + n, sizeof(body) - n,
			      "[Application]\nname=Demo\ncommand=demo\n");
	n += (size_t)snprintf(body + n, sizeof(body) - n,
			      "[Extension %sAAA]\nversion=1\n", prefix);
	n += (size_t)snprintf(body + n, sizeof(body) - n,
			      "[Extension %sBBB]\nversion=2\n", prefix);

	int id = write_temp(body, n);
	if (id < 0) {
		note("could not write the long-group case");
		return;
	}
	fp_metadata_t m;
	if (fp_metadata_parse(temp_path(id), &m) != 0) {
		note("the long-group case did not parse");
		remove(temp_path(id));
		return;
	}
	if (m.next_groups != 0) {
		printf("FAIL: two over-long group names produced %zu group(s), "
		       "wanted 0\n",
		       m.next_groups);
		bad = 1;
	}
	fp_metadata_free(&m);
	remove(temp_path(id));
}

// The environment list is capped; the file is a remote's.
static void env_cap_case(void) {
	size_t len = 4096 * 64;
	char* body = calloc(1, len);
	size_t n = 0;

	n += (size_t)snprintf(body + n, len - n, "[Application]\nname=Demo\ncommand=demo\n");
	n += (size_t)snprintf(body + n, len - n, "[Environment]\n");
	for (int i = 0; i < 4000; i++)
		n += (size_t)snprintf(body + n, len - n, "E%d=v\n", i);

	int id = write_temp(body, n);
	free(body);
	if (id < 0) {
		note("could not write the environment case");
		return;
	}
	fp_metadata_t m;
	if (fp_metadata_parse(temp_path(id), &m) != 0) {
		note("the environment case did not parse");
		remove(temp_path(id));
		return;
	}
	if (m.env_pairs.count != 1024) {
		printf("FAIL: %zu environment entries, wanted the 1024 cap\n",
		       m.env_pairs.count);
		bad = 1;
	}
	fp_metadata_free(&m);
	remove(temp_path(id));
}

int main(void) {
	long_line_case();
	long_group_case();
	env_cap_case();
	if (bad) return 1;
	printf("ok   a long line, a long group name and an endless environment "
	       "list are all bounded\n");
	return 0;
}
