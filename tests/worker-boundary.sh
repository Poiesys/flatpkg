#!/bin/sh
# worker-boundary.sh - the three requests that carry descriptors are gated on
# HELLO like every other one, a frame carrying more descriptors than the
# protocol allows is refused from its header before libnv allocates, a client's
# state namespace belongs to the uid that first declared it, and CancelPull is
# an authorized "not supported" instead of an unauthenticated success.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

. "$ROOT/tests/commentlib.sh"

T=$(mktemp -d /tmp/.fpkg-boundary.XXXXXX)
trap 'rm -rf "$T"' EXIT

strip_comments "$ROOT/src/shared/protocol.c" > "$T/protocol.c"
strip_comments "$ROOT/src/daemon/ops/typed_worker.c" > "$T/worker.c"
strip_comments "$ROOT/src/daemon/systemhelper.c" > "$T/helper.c"

carve_function '^static int nv_header_check\(' "$T/protocol.c" > "$T/hdr.inc" ||
	{ echo "FAIL: nv_header_check() not found"; exit 1; }
carve_function '^static void handle\(' "$T/worker.c" > "$T/handle.inc" ||
	{ echo "FAIL: handle() not found"; exit 1; }
carve_function '^void typed_worker\(' "$T/worker.c" > "$T/worker.inc" ||
	{ echo "FAIL: typed_worker() not found"; exit 1; }
carve_function '^static bool client_owner_ok\(' "$T/worker.c" > "$T/owner.inc" ||
	{ echo "FAIL: client_owner_ok() not found"; exit 1; }
carve_function '^static bool client_state_dir\(' "$T/worker.c" > "$T/state.inc" ||
	{ echo "FAIL: client_state_dir() not found"; exit 1; }
carve_function '^static void handle_cancel_pull\(' "$T/helper.c" > "$T/cancel.inc" ||
	{ echo "FAIL: handle_cancel_pull() not found"; exit 1; }
carve_function '^static void handle_unimplemented\(' "$T/helper.c" > "$T/unimpl.inc" ||
	{ echo "FAIL: handle_unimplemented() not found"; exit 1; }
carve_function '^syshelper_method_call\(' "$T/helper.c" > "$T/sys.inc" ||
	{ echo "FAIL: syshelper_method_call() not found"; exit 1; }

bug() {
	echo "FAIL: $1"
	exit 1
}

check() {
	hdr=$1 handle=$2 worker=$3 owner=$4 state=$5 cancel=$6 unimpl=$7 sys=$8

	grep -q 'recv(fd, &h, sizeof(h), MSG_PEEK)' "$T/protocol.c" ||
		bug "the frame header is not peeked before the body"
	grep -q 'descs > FP_MAX_FDS' "$hdr" ||
		bug "a frame may carry more descriptors than the protocol allows"

	grep -q '!c->hello && r->type != FP_REQ_HELLO' "$handle" ||
		bug "handle() no longer gates on HELLO"
	grep -q '!c.hello && (req.type == FP_REQ_STDIO' "$worker" ||
		bug "the descriptor-carrying requests bypass the HELLO gate"
	grep -q 'req.type == FP_REQ_INSTALL_BUNDLE' "$worker" ||
		bug "the gate does not name the bundle request"
	grep -q 'req.type == FP_REQ_RUN' "$worker" ||
		bug "the gate does not name the run request"

	grep -q 'client_owner_ok(c)' "$state" ||
		bug "the client state directory is not bound to its owner"
	grep -q 'FP_CLI_CLIENT' "$owner" ||
		bug "flatpkg's own client name is not the installation's"
	grep -q 'strcmp(buf, want) == 0' "$owner" ||
		bug "a client name is not compared with the uid that took it"

	grep -q 'handle_cancel_pull(' "$sys" ||
		bug "CancelPull is not routed to its own handler"
	grep -q 'authorize(connection, sender, invocation, "CancelPull")' "$cancel" ||
		bug "CancelPull is answered without authorizing the caller"
	grep -q 'G_DBUS_ERROR_NOT_SUPPORTED' "$cancel" ||
		bug "CancelPull answers success it cannot show"
	if grep -q 'cancel-pull' "$unimpl"; then
		bug "the helper map still runs a cancel-pull command"
	fi
}

( check "$T/hdr.inc" "$T/handle.inc" "$T/worker.inc" "$T/owner.inc" \
	"$T/state.inc" "$T/cancel.inc" "$T/unimpl.inc" "$T/sys.inc" )

# --- the plants ----------------------------------------------------------
prove() {
	what=$1
	shift
	if ( "$@" ) >/dev/null 2>&1; then
		echo "FAIL: the guard passes with $what"
		exit 1
	fi
}

sed 's/if (descs > FP_MAX_FDS) return FP_P_ERR_TOO_BIG;/if (0) return FP_P_ERR_TOO_BIG;/' \
	"$T/hdr.inc" > "$T/plant-descs.inc"
if cmp -s "$T/hdr.inc" "$T/plant-descs.inc"; then
	echo "FAIL: the descriptor plant was not applied"
	exit 1
fi
prove "a frame with too many descriptors" \
	check "$T/plant-descs.inc" "$T/handle.inc" "$T/worker.inc" \
	"$T/owner.inc" "$T/state.inc" "$T/cancel.inc" "$T/unimpl.inc" \
	"$T/sys.inc"

sed 's/!c.hello && (req.type == FP_REQ_STDIO/1 \&\& (req.type == FP_REQ_STDIO/' \
	"$T/worker.inc" > "$T/plant-hello.inc"
prove "the descriptor requests ungated" \
	check "$T/hdr.inc" "$T/handle.inc" "$T/plant-hello.inc" \
	"$T/owner.inc" "$T/state.inc" "$T/cancel.inc" "$T/unimpl.inc" \
	"$T/sys.inc"

sed 's/client_owner_ok(c)/1/' "$T/state.inc" > "$T/plant-owner.inc"
prove "an unbound client namespace" \
	check "$T/hdr.inc" "$T/handle.inc" "$T/worker.inc" "$T/owner.inc" \
	"$T/plant-owner.inc" "$T/cancel.inc" "$T/unimpl.inc" "$T/sys.inc"

sed 's/handle_cancel_pull(connection, sender, invocation);/handle_unimplemented(connection, sender, object_path, interface_name, method_name, parameters, invocation, user_data);/' \
	"$T/sys.inc" > "$T/plant-cancel.inc"
prove "CancelPull back on the catch-all path" \
	check "$T/hdr.inc" "$T/handle.inc" "$T/worker.inc" "$T/owner.inc" \
	"$T/state.inc" "$T/cancel.inc" "$T/unimpl.inc" "$T/plant-cancel.inc"

echo "PASS: the descriptor-carrying requests are gated, a frame with too"
echo "      many descriptors is refused from its header, a client name"
echo "      belongs to its uid, and CancelPull is an authorized refusal"
