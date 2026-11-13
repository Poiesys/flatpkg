#ifndef FPKG_PROTOCOL_H
#define FPKG_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/nv.h>

// Wire format: one nvlist per frame, sent with nvlist_send(2).  `kind`, `type`
// and `id` are fields, the message's own fields are named values, and
// descriptors are named values in the same nvlist.  Decoding runs as root on
// caller-chosen bytes.
//
// An unknown event type cannot be skipped, only refused, so a new type or a
// new framing is a new version and the mismatch fails at HELLO, not
// mid-operation.
#define FP_PROTO_VERSION 10u
// The whole packed frame, including libnv's own header.
#define FP_PROTO_MAX_PAYLOAD (1u << 20)
#define FP_PROTO_MAX_STR (64u << 10)
#define FP_PROTO_MAX_LIST 4096u
#define FP_PROTO_MAX_TOPIC 128u
#define FP_PROTO_MAX_COLS 16u

typedef enum {
	FP_P_OK = 0,
	FP_P_ERR_MAGIC = -1,	 // not a flatpkg frame
	FP_P_ERR_VERSION = -2,	 // a version this build does not speak
	FP_P_ERR_KIND = -3,	 // unknown frame kind, or the wrong one here
	FP_P_ERR_TOO_BIG = -4,	 // payload or field over its limit
	FP_P_ERR_TRUNCATED = -5, // the frame ends inside a field
	FP_P_ERR_MALFORMED = -6, // unknown type, trailing bytes, bad value
	FP_P_ERR_NOMEM = -7,
	FP_P_ERR_IO = -8,
	FP_P_EOF = -9, // clean end of stream before a frame
} fp_proto_err_t;

typedef enum {
	FP_FRAME_REQUEST = 1,
	FP_FRAME_ACK = 2,
	FP_FRAME_EVENT = 3,
} fp_frame_kind_t;

// Primitives, not policy: the client solves, so upgrade is absent on purpose.
typedef enum {
	FP_REQ_HELLO = 1,	   // first frame: {version, client name}
	FP_REQ_SUBSCRIBE = 2,	   // {topic}
	FP_REQ_UNSUBSCRIBE = 3,	   // {topic}
	FP_REQ_CANCEL = 4,	   // {op_id}
	FP_REQ_ROLLBACK = 8,	   // {ref, commit}
	FP_REQ_RUN = 10,	   // {app_id, flags, args, run_opts}
	FP_REQ_ENTER = 11,	   // {instance, command}
	FP_REQ_KILL = 12,	   // {instance}
	FP_REQ_IMPORT = 13,	   // {path}
	FP_REQ_REMOTE_ADD = 14,	   // {name, url, from}
	FP_REQ_REMOTE_DELETE = 15, // {name}
	FP_REQ_OVERRIDE = 16,	   // {app_id, flags, changes}
	FP_REQ_ENV = 17,	   // {NAME=value, ...}: the caller's session
	FP_REQ_STDIO = 19, // {count}: carries `count` descriptors (SCM_RIGHTS)
	FP_REQ_INSTALL_BUNDLE =
		20, // {flags}: carries the bundle, open, as one fd
	FP_REQ_EXPORTS = 21, // {}: rebuild the exported desktop/binary farm
	FP_REQ_APPLY = 22,   // {flags, remote, world, rows}: a solved changeset
	FP_REQ_PREFETCH = 23, // {remote, refs}: fetch metadata for the caller
	// {flags, name, data}: a file in the caller's own state namespace.
	// Both path components are single names, so another client's file is
	// unreachable.
	FP_REQ_CLIENT_STATE = 24,
	FP_REQ_CLIENT_DECLARE = 25, // {names}: the files this client owns
	FP_REQ_APPSTREAM = 26, // {remote, arch}: refresh appstream; empty is all
	FP_REQ_REMOTE_MODIFY = 27, // {name, changes}: a remote's settings
} fp_req_type_t;

// FP_REQ_CLIENT_STATE flags.  Without WRITE the request is a read.
#define FP_CLIENT_STATE_WRITE 0x1u

#define FP_APPLY_MAX 256u
// The change types, in flatpkg.h's order; the wire keeps its own names.
enum {
	FP_APPLY_INSTALL = 0,
	FP_APPLY_UPGRADE,
	FP_APPLY_DOWNGRADE,
	FP_APPLY_REMOVE,
	FP_APPLY_REINSTALL,
	FP_APPLY_KEEP,
	FP_APPLY_NOOP,
};
// One row of a solved changeset; the daemon only executes rows in order.
typedef struct {
	uint32_t type; // FP_CHANGE_*
	char* ref;
	char* old_version;
	char* new_version;
} fp_change_row_t;

// Only the SYSTEM scope travels: the caller's own overrides are files in
// their own home.  RESET deletes the override instead of changing it.
#define FP_OVERRIDE_RESET 1u
#define FP_OVERRIDE_MAX 65536u

// The bundle travels as an open descriptor: as root the daemon must not open
// a name the caller chose.
#define FP_BUNDLE_FDS 1

// The most descriptors one request carries, and the highest target number a
// forwarded descriptor may name: outside it would land on a worker descriptor.
#define FP_MAX_FDS 64

// FP_REQ_STDIO carries the caller's stdin, stdout and stderr.  Terminals are
// never handed over: a root-run jail on the caller's tty is the TIOCSTI hole.
#define FP_STDIO_FDS 3

#define FP_RUN_DEVEL 0x1u
#define FP_RUN_CLEARENV 0x2u
#define FP_RUN_DIE_WITH_PARENT 0x4u
#define FP_RUN_FILE_FORWARDING 0x8u

typedef struct {
	uint32_t type;
	union {
		struct {
			uint32_t version;
			char* client;
		} hello;
		struct {
			char* topic;
		} sub; // SUBSCRIBE and UNSUBSCRIBE
		struct {
			uint64_t op_id;
		} cancel;
		struct {
			char* ref;
			char* commit;
		} rollback;
		struct {
			char* app_id;
			char** args;
			size_t nargs;
			// Per-launch grants (run --socket=... and so on).  The
			// daemon applies them to a plan it computes itself; the
			// client never sends a finished plan.
			char* command;
			char* branch;
			char* arch;
			uint32_t flags; // FP_RUN_*
			char** sockets;
			size_t nsockets;
			char** devices;
			size_t ndevices;
			char** shares;
			size_t nshares;
			char** fs_add;
			size_t nfs_add;
			char** fs_remove;
			size_t nfs_remove;
			char** envs;
			size_t nenvs;
			// The numbers to put the RUN frame's descriptors
			// on ("9" or "3,9"): an fd number is caller state
			// the worker cannot infer.  Empty when none.
			char* forward;
		} run;
		struct {
			char* instance;
			char** command;
			size_t ncommand;
		} enter;
		struct {
			char* instance;
		} kill;
		struct {
			char* path;
		} import;
		struct {
			char* name;
			char* url;
			char* from;
			// A repo file the CALLER read (a path or file:// word
			// is cleared from url/from when this is set): the worker
			// never opens a caller-named file as root.
			char* data;
		} remote_add;
		struct {
			char* name;
		} remote_delete;
		struct {
			char* name;
			// `key=value` lines: url, branch, title, comment,
			// description, homepage, icon, gpg-verify.  The daemon
			// validates every key.
			char* changes;
			// The keyring's text, read by the CALLER and carried here:
			// the worker must never open a path the caller named.  Empty
			// when the modification does not import a key.
			char* gpg_data;
		} remote_modify;
		struct {
			char* remote; // "" means every remote
			char* arch;   // "" means the host architecture
		} appstream;
		struct {
			char* app_id;	// "" is the global override
			uint32_t flags; // FP_OVERRIDE_*
			// The changes as flatpak's override-file text.  The
			// daemon parses it and writes the file from what that
			// yields; it never stores the caller's text.
			char* changes;
		} override;
		struct {
			// NAME=value.  The daemon keeps only the names on its
			// allowlist and drops the rest; identity (FLATPKG_APP_*)
			// is never taken from here.
			char** vars;
			size_t nvars;
		} env;
		struct {
			uint32_t count; // FP_STDIO_FDS
		} stdio;
		struct {
			uint32_t flags; // 0
		} bundle;
		struct {
			char* remote;
			char** refs;
			size_t nrefs;
		} prefetch;
		struct {
			uint32_t flags;
			char* remote;
			fp_change_row_t* rows;
			size_t nrows;
		} apply;
		struct {
			uint32_t flags;
			char* name;
			char* data;
		} client_state;
		struct {
			char** names;
			size_t nnames;
		} client_declare;
	} u;
} fp_req_t;

typedef enum {
	FP_ACK_OK = 1,	// {op_id}: 0 when the request starts no operation
	FP_ACK_ERR = 2, // {code, message}
} fp_ack_type_t;

typedef enum {
	FP_E_DENIED = 1,
	FP_E_INVALID = 2,
	FP_E_BUSY = 3,
	FP_E_UNSUPPORTED = 4,
	FP_E_INTERNAL = 5,
	FP_E_NOT_FOUND = 6,
} fp_ack_code_t;

typedef struct {
	uint32_t type;
	uint64_t op_id; // FP_ACK_OK
	uint32_t code;	// FP_ACK_ERR
	char* message;	// FP_ACK_ERR
} fp_ack_t;

// Topics: "op/<op_id>", "instance/<id>", "store", "remote"; each event carries one.
typedef enum {
	FP_EV_OP_STARTED = 1,  // {op_id, kind, label}
	FP_EV_OP_PROGRESS = 2, // {op_id, stage, current, total}; coalesced
	FP_EV_OP_STATE = 3,    // {op_id, state}
	FP_EV_OP_LOG = 4, // {op_id, level, scope, text}: about the operation
	FP_EV_OP_FINISHED = 5, // {op_id, status, error}: the result
	FP_EV_OP_TABLE = 6,    // {op_id, ncols, nrows, headers, cells}: a table
			       // for the requester to draw
	FP_EV_META = 7,	       // {ref, commit, metadata}: fetched for a plan
	FP_EV_STORE_CHANGED = 8,  // {action, ref}: an installed ref changed
	FP_EV_REMOTE_CHANGED = 9, // {name}: a remote was added or removed
	FP_EV_INSTANCE_STARTED = 10, // {app_id}: a launch began
	FP_EV_INSTANCE_EXITED = 11,  // {app_id}: a launch ended
	FP_EV_CLIENT_STATE = 12,     // {name, data}: the caller's file
} fp_ev_type_t;

typedef struct {
	uint32_t type;
	char* topic;
	uint64_t op_id;
	union {
		struct {
			uint32_t kind;
			char* label;
		} started;
		struct {
			char* stage;
			uint64_t current;
			uint64_t total;
		} progress;
		struct {
			char* state;
		} state;
		struct {
			uint32_t level; // FP_LEVEL_*
			char* scope;	// the subsystem it is about ("ostree",
					// ...)
			char* text;
		} log;
		struct {
			uint32_t status;
			char* error;
		} finished;
		// Data, not a drawing: the daemon has no terminal, so a table
		// goes out as its cells.  headers is ncols strings or NULL;
		// cells is nrows*ncols strings, row by row.
		struct {
			uint32_t ncols;
			uint32_t nrows;
			char** headers;
			char** cells;
		} table;
		struct {
			char* ref;
			char* commit;
			char* metadata;
		} meta;
		struct {
			char* action;
			char* ref;
		} store;
		struct {
			char* name;
		} remote;
		struct {
			char* app_id;
		} instance;
		struct {
			char* name;
			char* data;
		} client_state;
	} u;
} fp_event_t;

// Descriptor names.  A forwarded descriptor is named by the number the app
// will see it on, so the frame says which fd is which.
#define FP_FD_STDIN "stdin"
#define FP_FD_STDOUT "stdout"
#define FP_FD_STDERR "stderr"
#define FP_FD_BUNDLE "bundle"
#define FP_FD_PREFIX "fd/"

// Build one frame.  On success *out is a new nvlist the caller sends and
// destroys; the caller may add descriptor fields to it first.  On error *out
// is NULL.
int fp_req_encode(nvlist_t** out, uint32_t id, const fp_req_t* r);
int fp_ack_encode(nvlist_t** out, uint32_t id, const fp_ack_t* a);
int fp_event_encode(nvlist_t** out, uint32_t seq, const fp_event_t* e);

// Parse one frame.  The kind and the type are checked before anything else;
// a missing or mistyped field is FP_P_ERR_MALFORMED.  id_out may be NULL.
int fp_req_decode(const nvlist_t* nvl, uint32_t* id_out, fp_req_t* out);
int fp_ack_decode(const nvlist_t* nvl, uint32_t* id_out, fp_ack_t* out);
int fp_event_decode(const nvlist_t* nvl, uint32_t* id_out, fp_event_t* out);
void fp_req_free(fp_req_t* r);
void fp_ack_free(fp_ack_t* a);
void fp_event_free(fp_event_t* e);

// A topic is 1..FP_PROTO_MAX_TOPIC characters of [a-z0-9/._-] with no ".."
// and no leading or trailing '/'; it is a lookup key, so check it on decode.
bool fp_topic_valid(const char* s);

// Stream transport.  The receive side peeks libnv's wire header first and
// refuses an over-sized body or a descriptor count over FP_MAX_FDS before
// nvlist_recv() allocates anything.  FP_P_EOF is a clean close between
// frames.
int fp_frame_send(int fd, const nvlist_t* nvl);
int fp_frame_recv(int fd, nvlist_t** out);

// How many descriptors a frame carries.  libnv 1.7 ships the counter but its
// header does not declare it, so the frame walks its own names.
size_t fp_nvl_fd_count(const nvlist_t* nvl);

const char* fp_proto_strerror(int err);

#endif
