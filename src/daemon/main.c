// `system-helper` is a daemon argument, so the D-Bus service exists even
// when no flatpkg CLI is installed.
#include <string.h>

#include "flatpkg.h"

int main(int argc, char* argv[]) {
	if (argc == 2 && strcmp(argv[1], "system-helper") == 0)
		return system_helper_main(argc, argv);
	return daemon_main();
}
