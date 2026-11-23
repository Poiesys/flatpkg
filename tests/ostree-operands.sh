#!/bin/sh
# ostree-operands.sh - every ostree argv puts its operands after `--`.
#
# ostree parses options after its positionals: `ostree remote add ... NAME
# --not-an-option` answers "Unknown option".  So a value a caller influenced -
# a ref, a remote name, a URL, a commit - must not reach the argv as a bare
# positional, or a leading dash makes it an option root acts on.  The grammar
# checks are the first defence; this is the second, and it holds the sites that
# still cannot take the separator.
#
# `ostree remote add|delete|refs` reject `--` itself (verified against the
# binary: the command prints its usage), so those three are exempted by name
# and must validate their operand with remote_name_valid()/remote_url_ok().
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

command -v python3 >/dev/null 2>&1 ||
	{ echo "SKIP: no python3 in PATH"; exit 0; }

T=$(mktemp -d /tmp/.fpkg-ostreeops.XXXXXX)
trap 'rm -rf "$T"' EXIT

cat > "$T/scan.py" <<'PY'
import glob
import os
import re
import sys

# Operands this tree hands ostree as bare positional words.  A name added here
# is a value that comes from a request, a config file or a remote's summary.
BARE = {
    "refstr", "remote", "rem", "origin", "ref", "commit", "name", "url",
    "cur_url", "newref", "cref", "refname", "branch", "key", "value",
    "cfgkey", "bundle_path", "child_repo", "jail_deploy", "tmpdir",
    "out->remote", "job->remote", "job->refstr", "prefixes[i]",
}
# `ostree remote add|delete|refs` print usage when given `--`; their operands
# are validated by the caller instead.
NO_SEPARATOR = {("remote", "add"), ("remote", "delete"), ("remote", "refs")}
VALIDATORS = ("remote_name_valid(", "remote_url_ok(")

def functions_of(text):
    """Map a 1-based line to (function name, function body)."""
    starts = []
    for i, line in enumerate(text.split("\n"), 1):
        m = re.match(r"^(?:static\s+)?[A-Za-z_][\w \t*]*\b([A-Za-z_]\w*)\s*\(",
                     line)
        if m:
            starts.append((i, m.group(1)))
    return starts

def enclosing(starts, line, text):
    name = None
    begin = 0
    for i, fn in starts:
        if i <= line:
            name, begin = fn, i
        else:
            break
    body = "\n".join(text.split("\n")[begin - 1:line + 20])
    return name, body

def scan(path):
    text = open(path, errors="replace").read()
    lines = text.split("\n")
    starts = functions_of(text)
    findings = []
    for i, line in enumerate(lines):
        stripped = line.strip()
        if '"ostree"' not in line or stripped.startswith(("//", "*", "/*")):
            continue
        block = []
        for j in range(i, min(i + 14, len(lines))):
            block.append(lines[j])
            if "NULL" in lines[j]:
                break
        joined = " ".join(block)
        words = re.findall(r'"([^"]*)"', joined)
        sub = next((w for w in words if w != "ostree" and not w.startswith("-")), "")
        sub2 = ""
        if sub in ("remote", "config", "static-delta", "refs"):
            rest = [w for w in words if w != "ostree"]
            try:
                idx = rest.index(sub)
                sub2 = next((w for w in rest[idx + 1:] if not w.startswith("-")), "")
            except ValueError:
                pass
        if '"--"' in joined:
            # An option after the separator is a positional to ostree: this is
            # how `pull --disable-fsync -- remote ref --repo=...` broke, with
            # the repo argument read as a third ref.
            tail = joined.split('"--"', 1)[1]
            late = [w for w in re.findall(r'"([^"]*)"', tail)
                    if w.startswith("--") and w != "--"]
            if late:
                findings.append((path, i + 1, sub, sub2,
                                 "option %s after --" % ",".join(late)))
            continue
        # Look for the operands outside the option literals: `--print-metadata-
        # key=` is not the variable `key`, and `"remote"` is not the operand.
        bare = re.sub(r'"[^"]*"', " ", joined)
        # A lookahead, not \b: "prefixes[i]" ends in a non-word character, so
        # \b after the "]"-", boundary would never match it.
        present = sorted(
            v for v in BARE
            if re.search(re.escape(v) + r"(?![A-Za-z0-9_])", bare))
        if not present:
            continue
        if (sub, sub2) in NO_SEPARATOR or (sub, "") in NO_SEPARATOR:
            _fn, body = enclosing(starts, i + 1, text)
            if any(v in body for v in VALIDATORS):
                continue
            findings.append((path, i + 1, sub, sub2,
                             "exempt site does not validate its operand"))
            continue
        findings.append((path, i + 1, sub, sub2,
                         "operand %s without --" % ",".join(present)))
    return findings

paths = sys.argv[1:] or sorted(glob.glob("src/**/*.c", recursive=True))
bad = 0
for p in paths:
    for path, line, sub, sub2, why in scan(p):
        print("FAIL: %s:%d: ostree %s %s: %s" % (path, line, sub, sub2, why))
        bad += 1
sys.exit(1 if bad else 0)
PY

if ! python3 "$T/scan.py" >"$T/out" 2>"$T/err"; then
	cat "$T/out"
	head -4 "$T/err" | sed 's/^/       /'
	exit 1
fi

# The scan must be able to fail: take a copy with one separator removed.
cp -R "$ROOT/src" "$T/src"
sed -i '' 's/"--delete", repo_arg, "--",/"--delete", repo_arg,/' \
	"$T/src/daemon/ostree_store.c"
if cmp -s "$ROOT/src/daemon/ostree_store.c" \
	"$T/src/daemon/ostree_store.c"; then
	echo "FAIL: the operand plant was not applied"
	exit 1
fi
if (cd "$T/src" && python3 "$T/scan.py" daemon/ostree_store.c) >/dev/null 2>&1; then
	echo "FAIL: the scan passes with a separator removed"
	exit 1
fi

echo "PASS: every ostree argv separates its operands, and the three"
echo "      subcommands that reject -- are the only ones without it, each"
echo "      behind a caller-side validator."
