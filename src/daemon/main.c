// `system-helper` is a daemon argument, so the D-Bus service exists even
// when no flatpkg CLI is installed.
#include <string.h>

#include "flatpkg.h"

int main(int argc, char* argv[]) {
	if (argc == 2 && strcmp(argv[1], FLATPKG_SYSTEM_HELPER_ARG) == 0)
		return system_helper_main(argc, argv);
	if (argc == 2 && strcmp(argv[1], "--user") == 0)
		return daemon_main(true);
	return daemon_main(false);
}
