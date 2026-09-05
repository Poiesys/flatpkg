#!/bin/sh
# capturelib.sh - the ONE way this suite takes a screenshot.  Sourced, never
# run.  The display is resolved, never assumed, and grim asks the compositor:
# an X11 grab of a rootless Xwayland succeeds and is entirely black.

# API: capture_resolve_display, capture_display, capture_colours,
# capture_change_fraction, capture_new_colours, capture_settle, capture_crop,
# capture_mean_luma, capture_differs.

# The procs whose environment a compositor is expected to have published.
# Tried in order; the first that answers a real socket stops the search.
CAPTURE_PROCS=${CAPTURE_PROCS:-"Hyprland sway weston hyprland KWin_wayland river wayfire labwc"}

# A python that can read a PNG, or the empty string.  Without it the
# pixel answers degrade to "unknown", which must not read as a pass.
_capture_python() {
	if [ -n "${CAPTURE_PYTHON:-}" ]; then
		printf '%s' "$CAPTURE_PYTHON"
		return 0
	fi
	for c in python3 python; do
		if command -v "$c" >/dev/null 2>&1; then
			printf '%s' "$c"
			return 0
		fi
	done
	printf ''
}

# Resolve the runtime dir from the compositor's environment, not ours: ours is
# the daemon's or the test user's and the compositor may run elsewhere.
_capture_proc_env() {
	_pid=$(pgrep -u "${CAPTURE_UID:-$(id -u)}" -x "$1" 2>/dev/null | head -n 1)
	[ -n "$_pid" ] || return 1
	[ -r "/proc/$_pid/environ" ] || return 1
	tr '\0' '\n' < "/proc/$_pid/environ" 2>/dev/null | grep -E "^(XDG_RUNTIME_DIR|WAYLAND_DISPLAY|DISPLAY)="
}

capture_resolve_display() {
	_crt=""
	_cds=""

	# grim is the capture, so a box without it has no capture - say so
	# instead of falling back to something that lies.
	if [ -n "${GRIM_BIN:-}" ]; then
		:
	else
		GRIM_BIN=$(command -v grim 2>/dev/null) || {
			GRIM_BIN=""
			return 1
		}
	fi

	for p in $CAPTURE_PROCS; do
		_env=$(_capture_proc_env "$p") || continue
		[ -n "$_env" ] || continue
		_rt=$(printf '%s\n' "$_env" | sed -n 's/^XDG_RUNTIME_DIR=//p' | head -n 1)
		_wd=$(printf '%s\n' "$_env" | sed -n 's/^WAYLAND_DISPLAY=//p' | head -n 1)

		# The compositor need not publish the display name.  Derive it:
		# the socket whose name is `wayland-<digits>` and nothing more.
		if [ -z "$_wd" ] && [ -n "$_rt" ] && [ -d "$_rt" ]; then
			_wd=$(ls "$_rt" 2>/dev/null | sed -n 's/^wayland-\([0-9][0-9]*\)$/wayland-\1/p' | head -n 1)
		fi
		if [ -n "$_rt" ] && [ -n "$_wd" ] && [ -S "$_rt/$_wd" ]; then
			_crt=$_rt
			_cds=$_wd
			break
		fi
	done

	# The compositor's environment is not always readable (Hyprland here is a
	# Linux ELF, and a Linuxulator process exposes no /proc/<pid>/environ), so
	# resolve the SOCKET instead: a guard that stopped here would skip forever.
	if [ -z "$_crt" ]; then
		for _d in "${XDG_RUNTIME_DIR:-/nonexistent}" \
				  "/var/run/user/$(id -u)" \
				  /var/run/xdg/* ; do
			[ -d "$_d" ] || continue
			_w2=$(ls "$_d" 2>/dev/null | \
				sed -n 's/^wayland-\([0-9][0-9]*\)$/wayland-\1/p' | head -n 1)
			if [ -n "$_w2" ] && [ -S "$_d/$_w2" ]; then
				_crt=$_d
				_cds=$_w2
				break
			fi
		done
	fi

	[ -n "$_crt" ] && [ -n "$_cds" ] || return 1
	CAPTURE_RUNTIME_DIR=$_crt
	CAPTURE_DISPLAY=$_cds
	export CAPTURE_RUNTIME_DIR CAPTURE_DISPLAY GRIM_BIN
	return 0
}

capture_display() {
	_out=${1:?capture_display: no output file}
	[ -n "${CAPTURE_RUNTIME_DIR:-}" ] && [ -n "${CAPTURE_DISPLAY:-}" ] || return 1
	rm -f "$_out"
	# Remove the destination first and check it after: a file grim cannot
	# create is a failed capture, not an empty one.  No `-o` - that takes an
	# output name, not the socket name - so grim takes the whole output.
	XDG_RUNTIME_DIR="$CAPTURE_RUNTIME_DIR" \
		WAYLAND_DISPLAY="$CAPTURE_DISPLAY" \
		"$GRIM_BIN" "$_out" >/dev/null 2>&1 || return 1
	[ -s "$_out" ] || return 1
	return 0
}

# Every pixel answer here is a measurement the caller must be able to
# distrust: `0` means "could not measure" as well as "nothing", so each one
# returns 0 and says nothing, and the caller floors the value it got.
_capture_py() {
	[ -n "$(_capture_python)" ] || return 1
	$(_capture_python) - "$@"
}

capture_colours() {
	_png=${1:?capture_colours: no png}
	[ -s "$_png" ] || { printf '0'; return 0; }
	_capture_py "$_png" <<-'EOF' 2>/dev/null || printf '0'
import sys
try:
    from PIL import Image
    im = Image.open(sys.argv[1]).convert("RGB")
    # Every pixel, no stride: a colour-rich desktop paints a window's new
    # colours in runs shorter than any stride, and a stride that skips them
    # reads a rendered window as adding nothing.
    print(len(set(im.getdata())))
except Exception:
    sys.exit(1)
EOF
}

capture_change_fraction() {
	_before=${1:?capture_change_fraction: no before}
	_after=${2:?capture_change_fraction: no after}
	[ -s "$_before" ] && [ -s "$_after" ] || { printf '0'; return 0; }
	_capture_py "$_before" "$_after" <<-'EOF' 2>/dev/null || printf '0'
import sys
try:
    from PIL import Image, ImageChops
    a = Image.open(sys.argv[1]).convert("RGB")
    b = Image.open(sys.argv[2]).convert("RGB")
    if a.size != b.size:
        sys.exit(1)
    d = ImageChops.difference(a, b)
    # A pixel counts as changed when its channels sum to more than 24: the
    # session's own churn is sub-pixel, an app's window is not.
    n = sum(1 for p in d.getdata() if p[0] + p[1] + p[2] > 24)
    print(int(round(n * 100.0 / (a.size[0] * a.size[1]))))
except Exception:
    sys.exit(1)
EOF
}

capture_new_colours() {
	_before=${1:?capture_new_colours: no before}
	_after=${2:?capture_new_colours: no after}
	[ -s "$_before" ] && [ -s "$_after" ] || { printf '0'; return 0; }
	_capture_py "$_before" "$_after" <<-'EOF' 2>/dev/null || printf '0'
import sys
try:
    from PIL import Image
    a = Image.open(sys.argv[1]).convert("RGB")
    b = Image.open(sys.argv[2]).convert("RGB")
    print(len(set(b.getdata()[::7]) - set(a.getdata()[::7])))
except Exception:
    sys.exit(1)
EOF
}

capture_mean_luma() {
	_png=${1:?capture_mean_luma: no png}
	[ -s "$_png" ] || { printf '0'; return 0; }
	_capture_py "$_png" <<-'EOF' 2>/dev/null || printf '0'
import sys
try:
    from PIL import Image, ImageStat
    print(int(ImageStat.Stat(Image.open(sys.argv[1]).convert("L")).mean[0]))
except Exception:
    sys.exit(1)
EOF
}

capture_crop() {
	_in=${1:?capture_crop: no input}
	_out=${2:?capture_crop: no output}
	_x=${3:?capture_crop: no x}
	_y=${4:?capture_crop: no y}
	_w=${5:?capture_crop: no width}
	_h=${6:?capture_crop: no height}
	_capture_py "$_in" "$_out" "$_x" "$_y" "$_w" "$_h" <<-'EOF' || return 1
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
x, y, w, h = (int(v) for v in sys.argv[3:7])
im.crop((x, y, x + w, y + h)).save(sys.argv[2])
EOF
}

# Telemetry, not a verdict: the caller reports its own threshold.  The answer
# is how long after the first frame the screen stopped changing.
capture_settle() {
	_before=${1:?capture_settle: no before}
	_after=${2:?capture_settle: no after}
	_poll=${3:-1}
	_limit=${4:-60}
	_i=0
	while [ "$_i" -lt "$_limit" ]; do
		_i=$((_i + _poll))
		sleep "$_poll"
		if ! capture_display "$_after.settle" 2>/dev/null; then
			continue
		fi
		# A settled frame is one the next capture cannot tell from the
		# last: the app has finished laying itself out.
		_changed=$(capture_change_fraction "$_after" "$_after.settle")
		if [ "${_changed:-0}" -eq 0 ] 2>/dev/null; then
			mv "$_after.settle" "$_after" 2>/dev/null || true
			printf '%s' "$_i"
			return 0
		fi
		mv "$_after.settle" "$_after" 2>/dev/null || true
	done
	printf '%s' "-"
	return 1
}

# Deliberately weak, deliberately kept: "did the screen change" is satisfied
# by the desktop on its own, so it is suitable for "the launch did not hang",
# never for "the app drew".
capture_differs() {
	_before=${1:?capture_differs: no before}
	_after=${2:?capture_differs: no after}
	if [ "${_before:-}" != "${_after:-}" ] && [ -s "$_before" ] && [ -s "$_after" ]; then
		return 0
	fi
	return 1
}
