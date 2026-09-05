# bash completion for flatpkg (and the flatpak -> flatpkg symlink).
#
# Installed by `make install` as
#   share/bash-completion/completions/flatpkg
#   share/bash-completion/completions/flatpak
# Commands, options and option values all come from `flatpkg complete`, which
# reads the tables in src/cli/cli.c; nothing here is a second list.

_flatpkg() {
	local cur cmd i kind vals

	cur="${COMP_WORDS[COMP_CWORD]}"

	# The subcommand is the first non-option word after the program name.
	cmd=""
	for ((i = 1; i < COMP_CWORD; i++)); do
		case "${COMP_WORDS[i]}" in
		-*) ;;
		*)
			cmd="${COMP_WORDS[i]}"
			break
			;;
		esac
	done

	if [ -z "$cmd" ]; then
		COMPREPLY=($(compgen -W "$(flatpkg complete commands 2>/dev/null)" -- "$cur"))
		return 0
	fi

	# A value option's vocabulary is the parser's own.
	case "$cur" in
	--*=*)
		local opt="${cur%%=*}"
		vals=$(flatpkg complete values -- "$opt" 2>/dev/null)
		if [ -n "$vals" ]; then
			COMPREPLY=($(compgen -W "$vals" -P "$opt=" -- "${cur#*=}"))
			return 0
		fi
		;;
	esac

	# Once run's app id is present, everything else belongs to the app.
	if [ "$cmd" = run ] && [[ "$cur" != -* ]]; then
		local npos=0
		for ((i = 2; i < COMP_CWORD; i++)); do
			case "${COMP_WORDS[i]}" in -*) ;; *) npos=$((npos + 1)) ;; esac
		done
		if [ "$npos" -ge 1 ]; then
			COMPREPLY=($(compgen -f -- "$cur"))
			return 0
		fi
	fi

	kind=$(flatpkg complete operands "$cmd" 2>/dev/null)
	case "$kind" in
	files)
		COMPREPLY=($(compgen -f -- "$cur"))
		return 0
		;;
	esac

	local words
	words="$(flatpkg complete words "$cmd" 2>/dev/null)"
	if [ -n "$kind" ]; then
		words="$words $(flatpkg complete "$kind" 2>/dev/null)"
	fi
	COMPREPLY=($(compgen -W "$words" -- "$cur"))
	return 0
}

complete -F _flatpkg flatpkg flatpak
