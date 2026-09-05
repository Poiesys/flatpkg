// English is one of the locale tables, not a special case: it is the
// fallback when a locale has no table or no entry for a msgid.
static const struct tr_entry tr_en[] = {
	// Group names
	{"Application installation and removal",
	 "Application installation and removal"},
	{"System maintenance", "System maintenance"},
	{"Querying application information",
	 "Querying application information"},
	{"Running applications", "Running applications"},
	{"Remote management", "Remote management"},
	{"Miscellaneous", "Miscellaneous"},

	// Command summaries
	{"Add an application or runtime to WORLD and commit changes",
	 "Add an application or runtime to WORLD and commit changes"},
	{"Register a deploy root's applications in the store and add them to "
	 "WORLD",
	 "Register a deploy root's applications in the store and add them to "
	 "WORLD"},
	{"Remove an application or runtime from WORLD and commit changes",
	 "Remove an application or runtime from WORLD and commit changes"},
	{"Update installed applications from their remotes",
	 "Update installed applications from their remotes"},
	{"Re-solve WORLD and commit available upgrades",
	 "Re-solve WORLD and commit available upgrades"},
	{"Verify deploys and report the broken ones",
	 "Verify deploys and report the broken ones"},
	{"Rebuild exported desktop menu entries",
	 "Rebuild exported desktop menu entries"},
	{"Roll a ref back to a deploy kept from an earlier update and commit "
	 "changes",
	 "Roll a ref back to a deploy kept from an earlier update and commit "
	 "changes"},
	{"List installed applications", "List installed applications"},
	{"Show metadata and effective permissions",
	 "Show metadata and effective permissions"},
	{"Search configured remotes for applications",
	 "Search configured remotes for applications"},
	{"List deploys kept for rollback", "List deploys kept for rollback"},
	{"Show the install, upgrade and uninstall log",
	 "Show the install, upgrade and uninstall log"},
	{"Build a jail from the runtime and run the application",
	 "Build a jail from the runtime and run the application"},
	{"List running instances", "List running instances"},
	{"Run a command in a running instance",
	 "Run a command in a running instance"},
	{"Terminate a running instance", "Terminate a running instance"},
	{"Change an application's permissions persistently",
	 "Change an application's permissions persistently"},
	{"Add a remote repository", "Add a remote repository"},
	{"List configured remotes", "List configured remotes"},
	{"Remove a remote repository", "Remove a remote repository"},
	{"Show help for flatpkg or a command",
	 "Show help for flatpkg or a command"},
	{"Run the system service (root only)",
	 "Run the system service (root only)"},
	{"List completion candidates", "List completion candidates"},

	// Command descriptions
	{"A REF is an application or runtime id, optionally written as "
	 "remote:id//branch. Dependencies, runtimes and extensions are "
	 "resolved from the ref's own metadata.\nWith --from, install a "
	 ".flatpak bundle instead.",
	 "A REF is an application or runtime id, optionally written as "
	 "remote:id//branch. Dependencies, runtimes and extensions are "
	 "resolved from the ref's own metadata.\nWith --from, install a "
	 ".flatpak bundle instead."},
	{"Scan PATH, a deploy root laid out as app/ and runtime/, and register "
	 "what it holds in the store. An imported application is kept as it is "
	 "and never upgraded from a remote.",
	 "Scan PATH, a deploy root laid out as app/ and runtime/, and register "
	 "what it holds in the store. An imported application is kept as it is "
	 "and never upgraded from a remote."},
	{"Uninstall each REF and drop it from the world file, asking first "
	 "unless -y is given. A location a running sandbox has mounted is "
	 "never removed.",
	 "Uninstall each REF and drop it from the world file, asking first "
	 "unless -y is given. A location a running sandbox has mounted is "
	 "never removed."},
	{"Update REF, or every installed application when none is given. The "
	 "last three deploy commits are kept for rollback.",
	 "Update REF, or every installed application when none is given. The "
	 "last three deploy commits are kept for rollback."},
	{"Solve the world file against the remotes, then upgrade every ref "
	 "that is out of date, or only the REFs given. The changeset is "
	 "printed and confirmed before it is applied.",
	 "Solve the world file against the remotes, then upgrade every ref "
	 "that is out of date, or only the REFs given. The changeset is "
	 "printed and confirmed before it is applied."},
	{"Report-only by design: act on the output with `flatpkg install "
	 "--reinstall`. A repair that repairs can destroy a working tree. REFs "
	 "are accepted for flatpak parity; the whole store is always checked.",
	 "Report-only by design: act on the output with `flatpkg install "
	 "--reinstall`. A repair that repairs can destroy a working tree. REFs "
	 "are accepted for flatpak parity; the whole store is always checked."},
	{"Rebuild <store>/exports: relative symlinks, and .desktop files "
	 "rewritten to run through flatpkg. install and update do this "
	 "themselves; the world file is not touched.",
	 "Rebuild <store>/exports: relative symlinks, and .desktop files "
	 "rewritten to run through flatpkg. install and update do this "
	 "themselves; the world file is not touched."},
	{"COMMIT is one of the deploys kept by update; `flatpkg list-commits "
	 "REF` lists them.",
	 "COMMIT is one of the deploys kept by update; `flatpkg list-commits "
	 "REF` lists them."},
	{"Show the permissions the application actually runs with, overrides "
	 "included.",
	 "Show the permissions the application actually runs with, overrides "
	 "included."},
	{"A TEXT that starts with '-' is searched for as it stands, not read "
	 "as an option.",
	 "A TEXT that starts with '-' is searched for as it stands, not read "
	 "as an option."},
	{"Build a jail around the application and run it; with the daemon up "
	 "it runs as the calling user. Options go before APP-ID. Everything "
	 "after APP-ID, or after --, is the application's own.\nThese are the "
	 "per-launch grants. The rest of an application's permissions are "
	 "persistent: set them once with `flatpkg override`.",
	 "Build a jail around the application and run it; with the daemon up "
	 "it runs as the calling user. Options go before APP-ID. Everything "
	 "after APP-ID, or after --, is the application's own.\nThese are the "
	 "per-launch grants. The rest of an application's permissions are "
	 "persistent: set them once with `flatpkg override`."},
	{"INSTANCE is an instance id or jail id from `flatpkg ps`. The command "
	 "runs with the environment the application was started with. A jail "
	 "flatpkg did not create is refused.",
	 "INSTANCE is an instance id or jail id from `flatpkg ps`. The command "
	 "runs with the environment the application was started with. A jail "
	 "flatpkg did not create is refused."},
	{"INSTANCE is an instance id or jail id from `flatpkg ps`. A jail "
	 "flatpkg did not create is refused.",
	 "INSTANCE is an instance id or jail id from `flatpkg ps`. A jail "
	 "flatpkg did not create is refused."},
	{"Write the change to flatpak's own override file, the one Flatseal "
	 "reads and writes. The name `global` applies to every application.",
	 "Write the change to flatpak's own override file, the one Flatseal "
	 "reads and writes. The name `global` applies to every application."},
	{"URL is the repository, or give --from a .flatpakrepo.",
	 "URL is the repository, or give --from a .flatpakrepo."},
	{"With no COMMAND, list the commands, the global options and the "
	 "environment. `flatpkg COMMAND --help` shows the same page as "
	 "`flatpkg help COMMAND`.",
	 "With no COMMAND, list the commands, the global options and the "
	 "environment. `flatpkg COMMAND --help` shows the same page as "
	 "`flatpkg help COMMAND`."},
	{"Serve every command over /var/run/flatpkg/control.sock to members of "
	 "the flatpkg group; the application itself runs as the caller. "
	 "Normally started by the flatpkgd rc.d script. It must be the only "
	 "argument.",
	 "Serve every command over /var/run/flatpkg/control.sock to members of "
	 "the flatpkg group; the application itself runs as the caller. "
	 "Normally started by the flatpkgd rc.d script. It must be the only "
	 "argument."},
	{"KIND is apps, refs, runtimes, world, remotes, instances or commands.",
	 "KIND is apps, refs, runtimes, world, remotes, instances or "
	 "commands."},

	// Option help texts (global)
	{"Show help; after a command, that command's page",
	 "Show help; after a command, that command's page"},
	{"Print the version and exit", "Print the version and exit"},
	{"Show debug output", "Show debug output"},
	{"Also pass through the output of the ostree tool",
	 "Also pass through the output of the ostree tool"},
	{"Print how long each phase took, then a table ranked by the time "
	 "spent in each phase alone",
	 "Print how long each phase took, then a table ranked by the time "
	 "spent in each phase alone"},
	{"Disable colour output", "Disable colour output"},
	{"Print the changeset without applying it; only upgrade and repair "
	 "can, so every other command refuses the flag",
	 "Print the changeset without applying it; only upgrade and repair "
	 "can, so every other command refuses the flag"},
	{"Append logfmt records to FILE", "Append logfmt records to FILE"},
	{"Select the installation; flatpkg has one, so only 'default' resolves",
	 "Select the installation; flatpkg has one, so only 'default' "
	 "resolves"},
	{"Accepted for flatpak compatibility; has no effect",
	 "Accepted for flatpak compatibility; has no effect"},

	// Install options
	{"Treat each REF as a runtime", "Treat each REF as a runtime"},
	{"Do not ask for confirmation", "Do not ask for confirmation"},
	{"Same as --assumeyes", "Same as --assumeyes"},
	{"Install this commit", "Install this commit"},
	{"Install a .flatpak bundle instead of pulling from a remote; takes no "
	 "REF",
	 "Install a .flatpak bundle instead of pulling from a remote; takes no "
	 "REF"},
	{"Reinstall the application even if it is already installed",
	 "Reinstall the application even if it is already installed"},

	// Uninstall options
	{"Also remove every installed ref that depends on the ones given",
	 "Also remove every installed ref that depends on the ones given"},

	// Upgrade options
	{"Print the changeset without applying it",
	 "Print the changeset without applying it"},
	{"Also remove deploys nothing in the world file needs any more",
	 "Also remove deploys nothing in the world file needs any more"},
	{"Accepted for apk parity; upgrade already moves to the latest "
	 "available version",
	 "Accepted for apk parity; upgrade already moves to the latest "
	 "available version"},

	// Repair options
	{"Print what would be done without doing it",
	 "Print what would be done without doing it"},

	// PS options
	{"Show only these columns, comma separated, and print nothing when no "
	 "instance is running",
	 "Show only these columns, comma separated, and print nothing when no "
	 "instance is running"},

	// Remote-add options
	{"Read the remote from this .flatpakrepo file or URL",
	 "Read the remote from this .flatpakrepo file or URL"},

	// Run options
	{"Expose a socket", "Expose a socket"},
	{"Expose a device", "Expose a device"},
	{"Share with the host: network, ipc",
	 "Share with the host: network, ipc"},
	{"Unshare from the host: network, ipc, process, all; process is "
	 "already the case, every sandbox is its own jail",
	 "Unshare from the host: network, ipc, process, all; process is "
	 "already the case, every sandbox is its own jail"},
	{"Expose a filesystem: host, home, xdg-*, ~/path or /path, with a :ro "
	 "or :create suffix; a leading ! takes one away",
	 "Expose a filesystem: host, home, xdg-*, ~/path or /path, with a :ro "
	 "or :create suffix; a leading ! takes one away"},
	{"Run CMD instead of the application's command",
	 "Run CMD instead of the application's command"},
	{"Set an environment variable", "Set an environment variable"},
	{"Use the developer environment (devel exports)",
	 "Use the developer environment (devel exports)"},
	{"Expect this branch; warns when the active deploy is another",
	 "Expect this branch; warns when the active deploy is another"},
	{"Expect this architecture; warns when the active deploy is another",
	 "Expect this architecture; warns when the active deploy is another"},
	{"Drop the host environment", "Drop the host environment"},
	{"Forward @@ and @@u file arguments through the portal; always on, "
	 "accepted because exported .desktop files pass it",
	 "Forward @@ and @@u file arguments through the portal; always on, "
	 "accepted because exported .desktop files pass it"},
	{"Kill the application when flatpkg exits",
	 "Kill the application when flatpkg exits"},

	// Override options
	{"Unshare from the host: network, ipc, process, all",
	 "Unshare from the host: network, ipc, process, all"},
	{"Do not expose a socket", "Do not expose a socket"},
	{"Do not expose a device", "Do not expose a device"},
	{"Allow a feature", "Allow a feature"},
	{"Disallow a feature", "Disallow a feature"},
	{"Expose a filesystem: host, home, xdg-*, ~/path or /path, with a :ro "
	 "or :create suffix",
	 "Expose a filesystem: host, home, xdg-*, ~/path or /path, with a :ro "
	 "or :create suffix"},
	{"Do not expose a filesystem", "Do not expose a filesystem"},
	{"Keep PATH under the application's data directory",
	 "Keep PATH under the application's data directory"},
	{"Unset an environment variable", "Unset an environment variable"},
	{"Allow talking to a session bus name",
	 "Allow talking to a session bus name"},
	{"Allow owning a session bus name", "Allow owning a session bus name"},
	{"Do not allow talking to a session bus name",
	 "Do not allow talking to a session bus name"},
	{"Allow talking to a system bus name",
	 "Allow talking to a system bus name"},
	{"Allow owning a system bus name", "Allow owning a system bus name"},
	{"Do not allow talking to a system bus name",
	 "Do not allow talking to a system bus name"},
	{"Allow owning an accessibility bus name",
	 "Allow owning an accessibility bus name"},
	{"Show the current overrides", "Show the current overrides"},
	{"Remove the overrides", "Remove the overrides"},
	{"Use the user's override file, ~/.local/share/flatpak/overrides/APP "
	 "(the default)",
	 "Use the user's override file, ~/.local/share/flatpak/overrides/APP "
	 "(the default)"},
	{"Write the override under the store instead",
	 "Write the override under the store instead"},

	// Environment variable help
	{"Same as --verbose", "Same as --verbose"},
	{"Same as -l", "Same as -l"},
	{"Same as --profile", "Same as --profile"},
	{"Same as --simulate", "Same as --simulate"},
	{"Same as --no-color", "Same as --no-color"},
	{"Colour even when the output is not a terminal",
	 "Colour even when the output is not a terminal"},
	{"0 runs the command in this process instead of through the daemon "
	 "(root only)",
	 "0 runs the command in this process instead of through the daemon "
	 "(root only)"},
	{"Use this directory as the store instead of /var/db/flatpkg",
	 "Use this directory as the store instead of /var/db/flatpkg"},
	{"Keep the jail and its mounts after a run",
	 "Keep the jail and its mounts after a run"},
	{"Log how long each phase of building the sandbox root takes; start "
	 "here when a launch is slow but not stuck",
	 "Log how long each phase of building the sandbox root takes; start "
	 "here when a launch is slow but not stuck"},
	{"Do not preload the Linux compatibility shims into the application",
	 "Do not preload the Linux compatibility shims into the application"},
	{"Trace what the rlimit shim synthesises; FLATPKG_DEVT_SHIM_TRACE, "
	 "FLATPKG_MEMFD_SHIM_TRACE and FLATPKG_SYNCOBJ_SHIM_TRACE do the same "
	 "for their shims",
	 "Trace what the rlimit shim synthesises; FLATPKG_DEVT_SHIM_TRACE, "
	 "FLATPKG_MEMFD_SHIM_TRACE and FLATPKG_SYNCOBJ_SHIM_TRACE do the same "
	 "for their shims"},

	// Help page text
	{"Usage:", "Usage:"},
	{"Description", "Description"},
	{"Options", "Options"},
	{"Environment", "Environment"},
	{"Global options", "Global options"},
	{"For the environment, see `flatpkg --help`.",
	 "For the environment, see `flatpkg --help`."},
	{"flatpkg says meow", "flatpkg says meow"},
	{"For more information: man 8 flatpkg",
	 "For more information: man 8 flatpkg"},
	{"Also spelled ", "Also spelled "},
	{".\n", ".\n"},

	// Error messages that should be translated
	{"out of memory", "out of memory"},
	{"installation '%s' does not exist (flatpkg has one: 'default')",
	 "installation '%s' does not exist (flatpkg has one: 'default')"},
	{"--simulate is not implemented for `%s`; run it without -n to "
	 "actually %s",
	 "--simulate is not implemented for `%s`; run it without -n to "
	 "actually %s"},
	{"unknown option '%s' for '%s'", "unknown option '%s' for '%s'"},
	{"option '%s' takes no argument", "option '%s' takes no argument"},
	{"option '%s' requires an argument",
	 "option '%s' requires an argument"},
	{"invalid %s '%s' (valid: %s)", "invalid %s '%s' (valid: %s)"},
	{"missing argument for '%s'", "missing argument for '%s'"},
	{"'%s' takes no arguments, but got '%s'",
	 "'%s' takes no arguments, but got '%s'"},
	{"too many arguments for '%s', first extra: '%s'",
	 "too many arguments for '%s', first extra: '%s'"},
	{"unknown command '%s'", "unknown command '%s'"},
	{"command '%s' must be the only argument",
	 "command '%s' must be the only argument"},

	{"Accepted for flatpak compatibility; has no effect",
	 "Accepted for flatpak compatibility; has no effect"},

	// Terminator
	{NULL, NULL},
};