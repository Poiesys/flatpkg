#include "flatpkg.h"
#include "cli.h"
#define FPKG_MODULE "client"
#include "fpkglog.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static void noop_handler(int sig) { (void)sig; }

static struct termios g_saved_term;
static bool g_raw_active = false;

// Modes an application can turn on and leave on: after the app exits, the
// terminal must type and paste normally again.
static const char g_term_reset[] =
	"\033(B"				     // ASCII charset
	"\033[?25h\033[?12h"			     // cursor on, blinking
	"\033[0m"				     // default SGR
	"\033[?2004l"				     // bracketed paste off
	"\033[?9l"				     // X10 mouse off
	"\033[?1000l\033[?1002l\033[?1003l"	     // mouse reporting off
	"\033[?1005l\033[?1006l\033[?1015l\033[?1016l" // mouse encodings off
	"\033[?1004l"				     // focus reporting off
	"\033[<u\033[<u\033[<u\033[<u"		     // pop kitty keyboard modes
	"\033[=0u"				     // ...and clear its flags
	"\033[>4;0m"				     // modifyOtherKeys off
	"\033[?1l\033>"				     // normal cursor keys/keypad
	"\033[?66l"				     // numeric keypad off
	"\033[?8h\033[?7h"			     // autorepeat and autowrap on
	"\033[?5l"				     // reverse video off
	"\033[?2026l\033[?2048l"		     // sync output, in-band resize off
	"\033[4l";				     // insert mode off

static void reset_terminal(void) {
	int fd = -1;
	if (isatty(STDERR_FILENO)) fd = STDERR_FILENO;
	else if (isatty(STDOUT_FILENO)) fd = STDOUT_FILENO;
	else if (isatty(STDIN_FILENO)) fd = STDIN_FILENO;
	if (fd < 0) return;
	(void)!write(fd, g_term_reset, sizeof(g_term_reset) - 1);
}

static void restore_term(void) {
	if (g_raw_active) {
		// TCSANOW, not TCSAFLUSH: the app's last log line may still be in
		// the terminal's output queue, and TCSAFLUSH throws that away.
		(void)tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_term);
		g_raw_active = false;
	}
}

static void on_term_signal(int sig) {
	restore_term();
	reset_terminal();
	signal(sig, SIG_DFL);
	raise(sig);
}

void client_enter_raw_stdin(void) {
	if (!isatty(STDIN_FILENO)) return;
	struct termios t;
	if (tcgetattr(STDIN_FILENO, &t) != 0) return;
	g_saved_term = t;
	// The shell may have left the line discipline raw while running us;
	// give it back the settings a terminal prompt needs.
	g_saved_term.c_iflag |= ICRNL | IXON;
	g_saved_term.c_iflag &= ~(INLCR | IGNCR | ISTRIP);
	g_saved_term.c_lflag |= ICANON | ECHO | ECHOE | ECHOKE | ECHOCTL |
				ISIG | IEXTEN;
	g_saved_term.c_oflag |= OPOST | ONLCR;
	t.c_lflag &= ~(ICANON | ECHO | IEXTEN);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0) return;
	g_raw_active = true;
	atexit(restore_term);
	(void)signal(SIGINT, on_term_signal);
	(void)signal(SIGTERM, on_term_signal);
	(void)signal(SIGHUP, on_term_signal);
	(void)signal(SIGQUIT, on_term_signal);
}

// Called when a run/enter request is over: put the terminal back before the
// app's modes are left for the shell to inherit.
void client_leave_raw_stdin(void) {
	restore_term();
	reset_terminal();
}

void ignore_sigpipe(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = noop_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGPIPE, &sa, NULL);
}

// Say why the daemon cannot be used, and return the exit status for it.  The
// log line is cut at the terminal width, so reason, socket and hint are
// separate short lines; a long path on the end is what gets cut off.
int daemon_unreachable(int err) {
	switch (err) {
	case ENOENT:
		fpkg_errorf("flatpkgd is not running");
		fpkg_infof("no control socket at %s", FPKG_SOCKET);
		fpkg_infof("start it with: service flatpkgd start");
		break;
	case ECONNREFUSED:
		fpkg_errorf("flatpkgd is not accepting connections");
		fpkg_infof("stale socket (nothing listens): %s", FPKG_SOCKET);
		fpkg_infof("start it with: service flatpkgd start");
		break;
	case EACCES:
	case EPERM:
		fpkg_errorf("not permitted to use the flatpkgd socket");
		fpkg_infof("socket: %s", FPKG_SOCKET);
		break;
	default:
		fpkg_errorf("cannot talk to flatpkgd: %s", strerror(err));
		fpkg_infof("socket: %s", FPKG_SOCKET);
		break;
	}
	return FPKG_EXIT_UNAVAILABLE;
}
