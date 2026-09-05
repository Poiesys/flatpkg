# fish completion for flatpkg (and the flatpak -> flatpkg symlink).
#
# Installed by `make install` as share/fish/vendor_completions.d/flatpkg.fish.
# Commands, options and option values come from `flatpkg complete`, which reads
# the tables in src/cli/cli.c.

function __flatpkg_commands
    flatpkg complete commands 2>/dev/null
end

function __flatpkg_operands
    set -l kind (flatpkg complete operands (commandline -opc)[2] 2>/dev/null)
    test -n "$kind"; and flatpkg complete $kind 2>/dev/null
end

function __flatpkg_opt_values
    set -l ct (commandline -ct)
    string match -q -- '--*=*' $ct; or return
    set -l opt (string split -m1 = -- $ct)[1]
    for v in (flatpkg complete values -- $opt 2>/dev/null)
        printf '%s=%s\n' $opt $v
    end
end

for prog in flatpkg flatpak
    complete -c $prog -f -n "not __fish_seen_subcommand_from (__flatpkg_commands)" \
        -a "(__flatpkg_commands)"
    for cmd in (__flatpkg_commands)
        complete -c $prog -f -n "__fish_seen_subcommand_from $cmd" \
            -a "(flatpkg complete words $cmd 2>/dev/null)"
        complete -c $prog -f -n "__fish_seen_subcommand_from $cmd" \
            -a "(__flatpkg_operands)"
    end
    complete -c $prog -f -a "(__flatpkg_opt_values)"
end
