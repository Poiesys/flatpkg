# scratchd.sh - SOURCE this; it is not a guard.  A scratch flatpkgd for a guard,
# with its own COMPILE-TIME control socket and store under SD_TMP, so the live
# daemon and store are never touched.  Sets SD_BIN, SD_STORE, SD_LOG, SD_TMP.

SD_TMP=""
SD_PID=""

scratchd_stop() {
	[ -n "$SD_PID" ] && mdo kill "$SD_PID" >/dev/null 2>&1 || true
	SD_PID=""
	if [ -n "$SD_TMP" ]; then
		mdo rm -rf "$SD_TMP" >/dev/null 2>&1 || rm -rf "$SD_TMP"
		SD_TMP=""
	fi
}

scratchd_start() {
	if ! command -v cc >/dev/null 2>&1 || ! command -v make >/dev/null 2>&1; then
		echo "SKIP: no cc or make in PATH"
		exit 0
	fi
	if ! command -v mdo >/dev/null 2>&1 ||
		! mdo -u root true </dev/null >/dev/null 2>&1; then
		echo "SKIP: mdo cannot give root without a prompt"
		exit 0
	fi
	SD_TMP=$(mktemp -d /tmp/.fpkg-scratchd.XXXXXX)
	chmod 755 "$SD_TMP"
	mkdir -p "$SD_TMP/s"
	SD_STORE="$SD_TMP/store"
	SD_LOG="$SD_TMP/daemon.log"
	mkdir -p "$SD_STORE"
	# The sourcing guard owns ROOT.  $0 is the guard, not this file, so the
	# repository cannot be found from here; refuse rather than build in "/".
	_sd_root=${ROOT:-}
	if [ -z "$_sd_root" ] || [ ! -f "$_sd_root/Makefile" ]; then
		echo "FAIL: scratchd_start needs ROOT set to the repository (got '$_sd_root')"
		exit 1
	fi
	_sd_jobs=$(sysctl -n hw.ncpu 2>/dev/null || echo 2)
	if ! (cd "$_sd_root" && CFLAGS="${CFLAGS:-} $1" make -j"$_sd_jobs" \
		BUILD="$SD_TMP/build" SOCKET_PATH="$SD_TMP/s/control.sock" \
		FLATPKG_STORE_DIR="$SD_STORE" \
		"$SD_TMP/build/flatpkg" "$SD_TMP/build/flatpkgd") \
		> "$SD_TMP/make.log" 2>&1; then
		echo "FAIL: the scratch build did not build:"
		tail -n 15 "$SD_TMP/make.log"
		exit 1
	fi
	SD_BIN="$SD_TMP/build/flatpkg"
	mdo sh -c "$SD_TMP/build/flatpkgd > $SD_LOG 2>&1 & echo \$! > $SD_TMP/pid"
	SD_PID=$(cat "$SD_TMP/pid")
	_sd_n=0
	while [ ! -S "$SD_TMP/s/control.sock" ] && [ "$_sd_n" -lt 100 ]; do
		sleep 0.1
		_sd_n=$((_sd_n + 1))
	done
	if [ ! -S "$SD_TMP/s/control.sock" ]; then
		echo "FAIL: the scratch daemon did not create its socket"
		cat "$SD_LOG"
		exit 1
	fi
}

# Empty the store, keep the directory (the daemon made it, with its group).
scratchd_reset() {
	mdo sh -c "find '$SD_STORE' -mindepth 1 -maxdepth 1 -exec rm -rf {} +"
}
