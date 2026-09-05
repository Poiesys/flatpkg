#!/bin/sh
# tests/commentlib.sh - comment handling, shared.  SOURCED, NEVER RUN.
# strip_comments() lexes C, sh_strip_comments() strips a shell '#', and
# carve_function() carves a function body by brace count.

# Emit the code with comments removed.  A byte scanner, not a tokenizer: it
# tracks strings so a `/*` inside one is not mistaken for a comment.
strip_comments() {
	awk '
	BEGIN { inblock = 0 }
	{
		out = ""; i = 1; n = length($0); q = ""
		while (i <= n) {
			c = substr($0, i, 1)
			if (inblock) {
				e = index(substr($0, i), "*/")
				if (e == 0) { i = n + 1; break }
				i += e + 1; inblock = 0; continue
			}
			if (q != "") {
				out = out c
				if (c == "\\") { out = out substr($0, i + 1, 1); i += 2; continue }
				if (c == q) q = ""
				i++; continue
			}
			if (c == "\"" || c == "\x27") { q = c; out = out c; i++; continue }
			if (c == "/" && substr($0, i + 1, 1) == "/") break
			if (c == "/" && substr($0, i + 1, 1) == "*") { inblock = 1; i += 2; continue }
			out = out c; i++
		}
		print out
	}' "$@"
}

# Shell comment stripper for guard-lint.sh.
sh_strip_comments() {
	awk '
	{
		out = ""; n = length($0); sq = 0; dq = 0
		for (i = 1; i <= n; i++) {
			c = substr($0, i, 1)
			if (sq) { out = out c; if (c == "'"'"'") sq = 0; continue }
			if (dq) {
				if (c == "\\") { out = out c substr($0, i + 1, 1); i++; continue }
				out = out c; if (c == "\"") dq = 0; continue
			}
			if (c == "'"'"'") { sq = 1; out = out c; continue }
			if (c == "\"")     { dq = 1; out = out c; continue }
			if (c == "#")      { break }
			out = out c
		}
		print out
	}'
}



# carve_function() - prints the body from the first SIGNATURE-REGEX match to
# its closing brace, 1 on no match.  Feed it STRIPPED input (braces in comments
# count) and ANCHOR the regex (an unanchored one matches a call site).
carve_function() {
	# The regex goes in through the environment (`awk -v` eats backslashes)
	# and is matched with `~`.  A match only OPENS a carve if the signature
	# that starts there ends in `{`: a `;` first is a prototype or a call.
	FPKG_SIG=$1 awk '
	function first(s,   a, b) {
		a = index(s, "{"); b = index(s, ";")
		if (a == 0 && b == 0) return ""
		if (b == 0 || (a > 0 && a < b)) return "{"
		return ";"
	}
	# `^name` is the definition of `name` (a return type may share its line),
	# and a pattern ending in an identifier character gets the `(` it implies:
	# `^worker` is not `worker_abort(`.
	BEGIN {
		re = ENVIRON["FPKG_SIG"]; re2 = ""
		if (re ~ /[A-Za-z0-9_]$/) re = re "\\("
		if (substr(re, 1, 1) == "^")
			re2 = "^[A-Za-z_][A-Za-z0-9_ \t*]*[ \t*]" substr(re, 2)
	}
	!inb && !cand && ($0 ~ re || (re2 != "" && $0 ~ re2)) {
		cand = 1; buf = ""
	}
	cand {
		buf = buf $0 "\n"
		f = first($0)
		if (f == ";") { cand = 0; buf = "" }
		else if (f == "{") {
			cand = 0; inb = 1; hit = 1
			printf "%s", buf
			o += gsub(/\{/, "{", buf); c += gsub(/\}/, "}", buf)
			if (o > 0 && c >= o) exit
		}
		next
	}
	inb {
		print
		o += gsub(/\{/, "{"); c += gsub(/\}/, "}")
		if (o > 0 && c >= o) exit
	}
	END { if (!hit) exit 1 }' "$2"
}

# srcf NAME - the path of a source file by basename across src/{cli,daemon,
# shared}/.  Needs $ROOT.  Prints the src/ path when the name is nowhere, so a
# missing file still fails at the caller's open.
srcf() {
	for _d in "$ROOT/src" "$ROOT"/src/*/ "$ROOT"/src/*/*/; do
		if [ -f "${_d%/}/$1" ]; then
			printf '%s\n' "${_d%/}/$1"
			return 0
		fi
	done
	printf '%s\n' "$ROOT/src/$1"
}
