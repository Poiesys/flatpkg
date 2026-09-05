#!/bin/sh
# devfs-coverage.sh - every device flag the plan can carry must reach the
# kernel: the devfs ruleset is a table of string literals, so a flag can be
# accepted, parsed and set and still expose nothing.  Both directions checked.
set -e
# shellcheck source=tests/commentlib.sh
. "$(dirname "$0")/commentlib.sh"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SB="$ROOT/src/daemon/sandbox.c"
HDR="$ROOT/include/flatpkg.h"

T=$(mktemp -d /tmp/.fpkg-devfs.XXXXXX)
trap 'rm -rf "$T"' EXIT

fail=0
note() { echo "FAIL: $1"; fail=1; }

# Carve alloc_devfs_ruleset() by brace count; the signature travels in the
# environment because `awk -v` would eat the backslashes out of the pattern.
fn=$(carve_function '^alloc_devfs_ruleset\(' "$SB")
printf '%s\n' "$fn" > "$T/fn"

# A carve that stopped early loses the table and every comparison below
# then passes on an empty set - so its shape is asserted before it is
# used, not after.
case "$fn" in
*alloc_devfs_ruleset*) : ;;
*) echo "FAIL: could not carve alloc_devfs_ruleset() out of src/daemon/sandbox.c"; exit 1 ;;
esac
grep -q '^}$' "$T/fn" ||
	{ echo "FAIL: the carve stopped before the function's closing brace"; exit 1; }
grep -q 'devfs rule -s' "$T/fn" ||
	{ echo "FAIL: the carve has no devfs rule in it - the shape moved"; exit 1; }

# The flags the struct declares, from the struct itself and not from a
# hand-typed list: a list here would be a fifth copy of the vocabulary,
# and the guard exists because copies drift.
sed -n '/bool dev_dri/,/;/p' "$HDR" | grep -o 'dev_[a-z0-9_]*' | sort -u \
	> "$T/flags"
[ -s "$T/flags" ] ||
	{ echo "FAIL: no dev_* flags found in sandbox_plan_t - the field moved"; exit 1; }

# The flags the function READS, paired with the local each fills.  Matching
# the pair rather than the flag alone is what lets the second check work: a
# local named for one flag and fed from another is exactly the drift.
grep -o 'on_[a-z0-9_]* *= *plan->dev_[a-z0-9_]*' "$T/fn" |
	sed 's/ *= *plan->/ /' | sort -u > "$T/pairs"
[ -s "$T/pairs" ] ||
	{ echo "FAIL: alloc_devfs_ruleset() reads no plan->dev_* flag"; exit 1; }
cut -d' ' -f1 "$T/pairs" | sort -u > "$T/read_locals"
cut -d' ' -f2 "$T/pairs" | sort -u > "$T/read_flags"

# The locals the rules TABLE actually switches on.  `{ true, "shm" }` is
# the unconditional entry and contributes no local, which is why the
# pattern asks for a name.
grep -o '{ *on_[a-z0-9_]*,' "$T/fn" | tr -d '{ ,' | sort -u > "$T/used"

[ -s "$T/used" ] ||
	{ echo "FAIL: the rules table has no on_* entry at all"; exit 1; }

# `dev_all` is the one declared carve: it short-circuits to "no restrictions"
# before the table is consulted, so no entry can apply.  Named, not filtered,
# so a second carve has to be written down.
grep -q 'plan->dev_all' "$T/fn" ||
	note "dev_all no longer short-circuits alloc_devfs_ruleset(); the carve in this file needs updating, because dev_all is exempted from the coverage check below"

# Direction 1: a local the table never consults - accepted, parsed, set in the
# plan, exposing nothing.  Compare LOCALS to LOCALS: `dev_dri` against `on_dri`
# cannot match, and the first version reported all six on a correct tree.
silent=$(comm -23 "$T/read_locals" "$T/used" | tr '\n' ' ')
[ -n "$silent" ] && {
	echo "FAIL: the devfs rules read $silent into a local that no table"
	echo "       entry switches on, so the flag behind each is accepted and"
	echo "       exposes nothing: $(grep -E "^($silent) " "$T/pairs" |
		awk '{printf "%s ", $2}')"
	fail=1
}

# Direction 2: a local the table switches on, fed from no flag.  Dead
# code, and the other half of the same one-edit drift.
orphan=$(comm -13 "$T/read_locals" "$T/used" | tr '\n' ' ')
[ -n "$orphan" ] && {
	echo "FAIL: the rules table switches on $orphan, which is fed from no"
	echo "       plan->dev_* flag: dead rules that can never be on"
	fail=1
}

# Direction 3: a local read from a flag that is not one of the declared
# device flags - a typo'd field name that compiles because the struct is
# passed whole.
for f in $(cat "$T/read_flags"); do
	grep -qx "$f" "$T/flags" || {
		echo "FAIL: alloc_devfs_ruleset() reads plan->$f, which is not a"
		echo "       declared dev_* field of sandbox_plan_t"
		fail=1
	}
done

# Every declared flag is either covered or the declared carve.  This is
# the check that fails when someone adds a capability and forgets the
# table, which is the whole reason the file exists.
uncovered=$(comm -23 "$T/flags" "$T/read_flags" | grep -v '^dev_all$' |
	tr '\n' ' ')
[ -n "$uncovered" ] && {
	echo "FAIL: sandbox_plan_t declares $uncovered and the devfs rules never"
	echo "       read it: a --device= value for it would be accepted and"
	echo "       expose nothing"
	fail=1
}

# The floor.  Six flags are declared, one is carved out, so five must be
# read and used.  A count that can only be zero is not a check, and a
# check with no floor passes on an empty extraction.
nf=$(grep -cv '^dev_all$' "$T/flags" || true)
[ "$nf" -ge 5 ] ||
	{ echo "FAIL: only $nf device flag(s) declared, below the floor of 5 -"
	  echo "       either the struct moved or the extraction broke"; fail=1; }

if [ $fail -eq 0 ]; then
	echo "PASS: all $nf device flags reach the devfs rules table, every"
	echo "      on_* entry is fed from a real flag, and dev_all's carve is"
	echo "      still where this file says it is"
fi
exit $fail
