#!/usr/bin/env python3
"""memcheck-scan.py - read valgrind reports and fail on what is not a known
artifact of running valgrind on FreeBSD.

    memcheck-scan.py MIN_REPORTS DIR [DIR...]

Every *.log in the directories is a valgrind report (one per process: the
daemon and each worker it forked).  A finding is a block of one of these
kinds: a conditional jump or a syscall argument that depends on uninitialised
memory, an invalid read/write/free, a mismatched free, overlapping copy
arguments, or a block that is definitely or indirectly lost.  (Still-reachable
and possibly-lost blocks are not asked for.)

A finding is allowed only if it matches a rule in KNOWN below, and each rule
says WHY it is not a flatpkg defect and how that was established.  Anything
else is printed in full and the exit status is 1.  The count of allowed
findings is printed too, so a rule that starts matching far more than it did
is visible.

Fewer than MIN_REPORTS reports is a failure: valgrind that did not run
produces no findings either.
"""
import glob
import re
import sys

FINDING = re.compile(
    r"^(Conditional jump or move depends on uninitialised|"
    r"Use of uninitialised|"
    r"Invalid (read|write|free)|"
    r"Mismatched free|"
    r"Source and destination overlap|"
    r"Syscall param .* points to (uninitialised|unaddressable)|"
    r"[\d,]+ (\(\d+ direct, \d+ indirect\) )?bytes in [\d,]+ blocks are "
    r"(definitely|indirectly) lost)")

# (name, finding-kind regex, regex that must match the block, why)
KNOWN = [
    ("libc-environ",
     re.compile(r"uninitialised"),
     re.compile(r"(?m)^\s+(at|by) 0x[0-9A-F]+: "
                r"(getenv|setenv|unsetenv|putenv|__findenv|execvp|execvpe) "
                r"\((in /lib/libc\.so\.7|vg_replace_strmem\.c:\d+)\)"),
     "valgrind's interception of setenv/getenv on FreeBSD: the value is "
     "created by libc's own reallocarray inside setenv and used by libc's "
     "own getenv, with no flatpkg frame between (checked with "
     "--track-origins=yes on 2026-10-30)."),
    ("jail-get-syscall",
     re.compile(r"uninitialised"),
     re.compile(r"jail_getname"),
     "jail_get(2) fills the name buffer in the kernel and valgrind has no "
     "wrapper for it, so the buffer still looks uninitialised; the origin "
     "reported is the stack buffer inside libjail's jail_getname()."),
    ("libnv-unpack-malformed",
     re.compile(r"lost"),
     re.compile(r"(?s)\(in /lib/libnv\.so\.1\)(?:(?!\n\s+by ).)*?"
                r"\n\s+by 0x[0-9A-F]+: unpack_frame \(protocol\.c:"),
     "libnv's unpack of a blob that ends inside a pair or a nested list "
     "leaks the pair (and any child list) it had built before the failure; "
     "the allocations are all inside /lib/libnv.so.1 with no flatpkg frame "
     "below them.  The protocol decoders never pass a buffer whose size "
     "disagrees with its header, so this is only reachable from fuzzed or "
     "hostile bytes; typed_worker closes a connection after "
     "FP_MAX_BAD_FRAMES malformed frames, which bounds one worker's leak. "
     "Reported upstream; the rule is removed when the base library is "
     "fixed."),
    ("child-before-exec",
     re.compile(r"lost"),
     re.compile(r"(?s)\A[^\n]*\n\s+at 0x[0-9A-F]+: calloc [^\n]*\n"
                r"\s+by 0x[0-9A-F]+: (rewrite_forwarded_args|sandbox_exec) "),
     "the forked child allocates its argv and then exec()s: valgrind "
     "(without --trace-children) reports the exec as the process ending, "
     "and the allocation is replaced with the address space, not leaked."),
]


def blocks(text):
    cur = []
    for line in text.split("\n"):
        line = re.sub(r"^==\d+== ?", "", line)
        if line == "":
            if cur:
                yield "\n".join(cur)
            cur = []
        else:
            cur.append(line)
    if cur:
        yield "\n".join(cur)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    min_reports = int(sys.argv[1])
    reports = []
    for d in sys.argv[2:]:
        reports += sorted(glob.glob(d + "/*.log"))
    if len(reports) < min_reports:
        print("FAIL: %d valgrind reports found, at least %d expected: "
              "valgrind did not run as intended" % (len(reports), min_reports))
        sys.exit(1)

    allowed = {name: 0 for name, _, _, _ in KNOWN}
    unexpected = []
    for path in reports:
        text = open(path, errors="replace").read()
        for b in blocks(text):
            first = b.split("\n", 1)[0]
            if not FINDING.match(first):
                continue
            for name, kind, body, _why in KNOWN:
                if kind.search(first) and body.search(b):
                    allowed[name] += 1
                    break
            else:
                unexpected.append((path, b))

    summary = ", ".join("%s %d" % (n, c) for n, c in allowed.items())
    print("scanned %d reports; known artifacts: %s" % (len(reports), summary))
    if unexpected:
        print("FAIL: %d finding(s) that are not known artifacts:" % len(unexpected))
        seen = set()
        for path, b in unexpected:
            key = "\n".join(b.split("\n")[:5])
            if key in seen:
                continue
            seen.add(key)
            print("--- " + path.split("/")[-1])
            print("\n".join("    " + l for l in b.split("\n")[:16]))
        sys.exit(1)


main()
