// helper-config.c - parse the daemon's D-Bus command map.
#include <stdio.h>
#include <stdlib.h>

#include "flatpkg.h"

const char* store_root(void) { return getenv("FPKG_TEST_STORE"); }

int main(void) {
	char** argv = NULL;
	size_t argc = 0;
	fpkg_helper_args_t args = {
		.ref = "app/org.example.App/x86_64/stable",
		.installation = "default",
		.bundle = "/tmp/example.flatpak",
		.remote = "flathub",
		.origin = "origin",
		.arch = "x86_64",
		.repo = "/tmp/repo",
		.subpaths = "a,b",
		.previous = "old",
		.config = "cfg",
		.url = "https://example.invalid/repo",
		.key = "languages",
		.value = "en",
		.summary = "/tmp/summary",
		.summary_sig = "/tmp/sig",
		.src_dir = "/tmp/src",
		.flags = 7,
	};
	int r = fpkg_helper_command("uninstall", &args, &argv, &argc);
	printf("mapped=%d argc=%zu\n", r, argc);
	for (size_t i = 0; i < argc; i++) printf("arg%zu=%s\n", i, argv[i]);
	for (size_t i = 0; i < argc; i++) free(argv[i]);
	free(argv);
	return r < 0 ? 1 : 0;
}
