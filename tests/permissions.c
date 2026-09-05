/* permissions.c - harness for the [Context] -> sandbox plan translation.
 * The plan is built in memory only: no jail, mount, root, exec or launch, so
 * this reaches the half of sandbox.c that no other guard can test. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "flatpkg.h"
/* fplog's own header, for the ONE call into it this harness makes
 * (`fp_set_terminal_size()`, below): an implicit declaration would compile
 * against a signature that is not the real one. */
#include "fplog.h"

/* Answered here because this harness links only the modules that turn a
 * declaration into a plan: `get_string()` is the translation fallback and
 * `cli_getenv()` is cli.c's environment reader.  Without them the link stops. */
const char *
get_string(const char *msgid)
{
	return msgid;
}

const char *
cli_getenv(const char *name)
{
	return getenv(name);
}

/* `opt_push()` is main.c's and main.c is not linked here, so the three lines
 * it does are written out: linking a module whose main() is not what this
 * file tests would be worse than one duplication. */
static void
opt_push(strlist_t *l, const char *item)
{
	char *copy = strdup(item);
	if (copy == NULL)
		exit(1);
	l->items = realloc(l->items, (l->count + 1) * sizeof(char *));
	if (l->items == NULL)
		exit(1);
	l->items[l->count++] = copy;
}

static void
opt_list_free(strlist_t *l)
{
	for (size_t i = 0; i < l->count; i++)
		free(l->items[i]);
	free(l->items);
	l->items = NULL;
	l->count = 0;
}

/* The home every plan is built with.  Named because the two
 * `permissions_plan()` calls, the expected `jail_path` of a resolved xdg dir
 * and `plan_init()` all have to agree on it. */
#define PERM_HOME "/tmp/fpkg-perm-home"

static int failures;
static int n_cases;

/* The child's stderr, captured.  A warning is a diagnostic, so the
 * harness reads it the way a user would: off the terminal. */
static char captured[8192];

/* How many warnings the captured stderr holds.  Not a grep for a word in the
 * message: fplog truncates it to the terminal width, so a control that greps for
 * a removable word cannot fail - the level badge survives the cut. */
static int
warning_lines(void)
{
        int n = 0;
        for (const char *p = captured; (p = strstr(p, "WARN")) != NULL; p++)
                n++;
        return n;
}

static void
ok(const char *what, int pass, const char *detail)
{
	n_cases++;
	printf("  %-56s %s\n", what, pass ? "ok" : "FAIL");
	if (!pass) {
		failures++;
		if (detail != NULL && *detail != '\0')
			printf("      %s\n", detail);
	}
}

/* Run `fn(arg)` in a child whose stderr is a pipe and keep what it wrote.
 * One helper for both subjects: a warning from the `run(1)` override path is
 * no easier to capture than one from the metadata path. */
static void
capture(void (*fn)(void *), void *arg)
{
	int pfd[2];
	pid_t pid;
	int st = 0;
	ssize_t got = 0, r;

	captured[0] = '\0';
	if (pipe(pfd) != 0)
		return;
	pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return;
	}
	if (pid == 0) {
		close(pfd[0]);
		dup2(pfd[1], STDERR_FILENO);
		if (pfd[1] > STDERR_FILENO)
			close(pfd[1]);
		/* A pipe falls back to 80 columns and truncates, which is why most
		 * cases match a prefix.  Widening it lets a case name the ENTRY:
		 * `xdg-bogus` and `xdg-bogus2` are one line at 80 columns. */
		fp_set_terminal_size(24, 200);
		fn(arg);
		_exit(0);
	}
	close(pfd[1]);
	while (got < (ssize_t)sizeof(captured) - 1 &&
	    (r = read(pfd[0], captured + got, sizeof(captured) - 1 - got)) > 0)
		got += r;
	captured[got] = '\0';
	close(pfd[0]);
	waitpid(pid, &st, 0);
}

/* The child's half of plan_from(): parse this metadata, build the plan,
 * throw both away.  Only the diagnostics are of interest here. */
static void
plan_child(void *arg)
{
	const char *meta = arg;
	char path[] = "/tmp/.fpkg-perm.XXXXXX";
	int fd = mkstemp(path);

	if (fd < 0)
		_exit(1);
	if (write(fd, meta, strlen(meta)) < 0) {
		close(fd);
		unlink(path);
		_exit(1);
	}
	close(fd);
	fp_metadata_t m;
	sandbox_plan_t p;
	if (fp_metadata_parse(path, &m) == 0) {
		(void)permissions_plan(&m, PERM_HOME, &p);
		permissions_free_plan(&p);
		fp_metadata_free(&m);
	}
	unlink(path);
	_exit(0);
}

/* And the same for the `run(1)` override path. */
struct override_args {
	const run_opts_t *opts;
	sandbox_plan_t   *plan;
};

static void
override_child(void *arg)
{
	struct override_args *a = arg;

	(void)permissions_apply_overrides(a->plan, a->opts);
	_exit(0);
}

/* A hand-built plan has to be in the state `permissions_plan()` leaves it in.
 * `home_dir` is the whole of it: `remove_fs_entry()`'s xdg branch hands it to
 * `resolve_xdg()`, which dereferences it, so a memset plan segfaults. */
static void
plan_init(sandbox_plan_t *plan, const char *home)
{
	memset(plan, 0, sizeof(*plan));
	plan->home_dir = strdup(home);
	if (plan->home_dir == NULL)
		exit(1);
}

/* Write `meta` to a file, build a plan from it in a child whose stderr
 * is a pipe, and return the plan plus what the child warned. */
static int
plan_from(const char *name, const char *meta, sandbox_plan_t *out)
{
	char path[] = "/tmp/.fpkg-perm.XXXXXX";
	int fd;

	fd = mkstemp(path);
	if (fd < 0)
		return -1;
	if (write(fd, meta, strlen(meta)) < 0) {
		close(fd);
		unlink(path);
		return -1;
	}
	close(fd);

	fp_metadata_t m;
	if (fp_metadata_parse(path, &m) != 0) {
		unlink(path);
		return -1;
	}
	int rc = permissions_plan(&m, PERM_HOME, out);
	fp_metadata_free(&m);
	unlink(path);

	/* Second pass, in a child, purely to capture the warnings. */
	capture(plan_child, (void *)meta);
	(void)name;
	return rc;
}

int
main(void)
{
	sandbox_plan_t plan;
	char why[9000];

	/* 1. A KNOWN entry sets the flag and warns about nothing.  This is
	 * the control: without it, "warns on an unknown entry" is also
	 * satisfied by a build that warns on everything. */
	memset(&plan, 0, sizeof(plan));
	(void)plan_from("known",
	    /* Repeated keys, NOT `;`-separated: the parser takes a whole
	     * `;`-separated line as one value. */
	    "[Application]\nname=Demo\ncommand=demo\n"
	    "[Context]\nsockets=x11\nsockets=pulseaudio\nshared=network\n",
	    &plan);
	ok("a known sockets= sets its flag", plan.sock_x11 && plan.sock_pulseaudio, why);
	ok("a known shared= sets its flag", plan.want_network, why);
	snprintf(why, sizeof(why), "warnings: %s", captured);
	/* Count, not grep: the near-miss warning says "looks like a typo" and
	 * never "unknown", so grepping one word proves nothing. */
	ok("known entries warn about nothing", warning_lines() == 0, why);
	permissions_free_plan(&plan);

	/* 2. An UNKNOWN entry warns, naming the list and the entry.  One case
	 * per list: four separate `else` branches, so one warning proves
	 * nothing about the other three. */
	struct {
		const char *list;
		const char *entry;
		const char *ctx;
	} lists[] = {
		{ "shared",    "persisted",   "shared=persisted" },
		{ "devices",   "persisted",   "devices=persisted" },
		{ "sockets",   "pipewire",    "sockets=pipewire" },
		{ "features",  "persisted",   "features=persisted" },
	};
	for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
		char meta[512];
		snprintf(meta, sizeof(meta),
		    "[Application]\nname=Demo\ncommand=demo\n[Context]\n%s\n",
		    lists[i].ctx);
		memset(&plan, 0, sizeof(plan));
		(void)plan_from(lists[i].list, meta, &plan);
		snprintf(why, sizeof(why), "warnings: %s", captured);
		char want[128];
		snprintf(want, sizeof(want), "unknown %s entry '%s'",
		    lists[i].list, lists[i].entry);
		char what[128];
		snprintf(what, sizeof(what), "an unknown %s= warns, naming the entry",
		    lists[i].list);
		ok(what, strstr(captured, want) != NULL, why);
		permissions_free_plan(&plan);
	}

	/* 3. The two warnings stay distinguishable: an unknown `filesystem=`
	 * location has its own message.  It must be RELATIVE to be unknown - an
	 * absolute path is a legal bind, so there is nothing to warn about. */
	memset(&plan, 0, sizeof(plan));
	(void)plan_from("bad fs",
	    "[Application]\nname=Demo\ncommand=demo\n"
	    "[Context]\nfilesystems=persisted\n", &plan);
	snprintf(why, sizeof(why), "warnings: %s", captured);
	ok("an unknown filesystem= keeps its own, different message",
	    strstr(captured, "unknown filesystem location 'persisted'") != NULL,
	    why);
	permissions_free_plan(&plan);

	/* 3b. ... and the control: an absolute location is NOT unknown, so it
	 * binds a grant and stays silent. */
	memset(&plan, 0, sizeof(plan));
	(void)plan_from("good fs",
	    "[Application]\nname=Demo\ncommand=demo\n"
	    "[Context]\nfilesystems=/opt/data\n", &plan);
	snprintf(why, sizeof(why), "warnings: %s", captured);
	size_t bound = 0;
	for (size_t i = 0; i < plan.ngrants; i++)
		if (strcmp(plan.grants[i].jail_path, "/opt/data") == 0)
			bound++;
	ok("an absolute filesystem= binds its grant and warns about nothing",
	    bound == 1 && strstr(captured, "unknown") == NULL, why);
	permissions_free_plan(&plan);

	/* 4. A conditional that holds is consumed, not warned about:
	 * `if:NAME:COND` is rewritten to NAME before the loops see it. */
	memset(&plan, 0, sizeof(plan));
	(void)plan_from("conditional",
	    "[Application]\nname=Demo\ncommand=demo\n"
	    "[Context]\nsockets=if:wayland:true\n", &plan);
	snprintf(why, sizeof(why), "warnings: %s", captured);
	ok("a satisfied if: conditional is consumed, not warned about",
	    warning_lines() == 0, why);
	ok("a satisfied if: conditional still sets its flag",
	    plan.sock_wayland, why);
	permissions_free_plan(&plan);

	/* 5. An if: conditional that FAILS is dropped, and dropping it
	 * silently is right - the upstream spelling asked for it only
	 * when the condition held. */
	memset(&plan, 0, sizeof(plan));
	(void)plan_from("conditional false",
	    "[Application]\nname=Demo\ncommand=demo\n"
	    "[Context]\nsockets=if:wayland:has-nothing-here\n", &plan);
	snprintf(why, sizeof(why), "warnings: %s", captured);
	ok("an unsatisfied if: conditional does not set its flag",
	    !plan.sock_wayland, why);
	ok("an unsatisfied if: conditional warns about nothing",
	    warning_lines() == 0, why);
	permissions_free_plan(&plan);

	/* 6. A MISSPELLED KEY is the same dropped declaration one level up:
	 * `sokets=` is not `sockets=`, so the list is empty.  Match `key 'sokets'`
	 * and not the sentence: fplog truncates past "looks like a typo". */
	memset(&plan, 0, sizeof(plan));
	(void)plan_from("misspelled key",
	    "[Application]\nname=Demo\ncommand=demo\n"
	    "[Context]\nsokets=x11;wayland;\n", &plan);
	snprintf(why, sizeof(why), "warnings: %s", captured);
	ok("a misspelled key warns, naming the key",
	    warning_lines() == 1 && strstr(captured, "key 'sokets'") != NULL,
	    why);
	ok("... and the list it stood for really is empty",
	    !plan.sock_x11 && !plan.sock_wayland, why);
	permissions_free_plan(&plan);

	/* 7. A TRANSPOSITION is the commonest typo and a different branch of
	 * the distance function: `shraed` is 2 edits from `shared` under plain
	 * Levenshtein and 1 under the alignment variant. */
	memset(&plan, 0, sizeof(plan));
	(void)plan_from("transposed key",
	    "[Application]\nname=Demo\ncommand=demo\n"
	    "[Context]\nshraed=network;ipc;\n", &plan);
	snprintf(why, sizeof(why), "warnings: %s", captured);
	ok("a transposed key warns too",
	    warning_lines() == 1 && strstr(captured, "key 'shraed'") != NULL,
	    why);
	ok("... and shared= is empty, not half-set",
	    !plan.want_network, why);
	permissions_free_plan(&plan);

	/* 8. A key flatpkg genuinely does not implement, in a section it does
	 * know, must stay SILENT: the rule is a near miss, not "unknown". */
	memset(&plan, 0, sizeof(plan));
	(void)plan_from("unimplemented key",
	    "[Application]\nname=Demo\ncommand=demo\n"
	    "[Extension org.example.Demo.Locale]\n"
	    "locale-subset=cs;de;\ndirectory=locale\n", &plan);
	snprintf(why, sizeof(why), "warnings: %s", captured);
	ok("a key flatpkg does not implement stays silent",
	    warning_lines() == 0, why);
	permissions_free_plan(&plan);

	/* 9. An OPEN-ENDED section has no vocabulary and no typo to catch:
	 * every key in `[Environment]` is a variable name and every key in a
	 * Bus Policy section is a dbus name. */
	memset(&plan, 0, sizeof(plan));
	(void)plan_from("open-ended sections",
	    "[Application]\nname=Demo\ncommand=demo\n"
	    "[Context]\nsockets=x11\n"
	    "[Environment]\nMESA_GL_VERSION=radeonsi\n"
	    "[Session Bus Policy]\norg.freedesktop.Notifications=own\n",
	    &plan);
	snprintf(why, sizeof(why), "warnings: %s", captured);
	ok("an open-ended section is never reported",
	    warning_lines() == 0, why);
	permissions_free_plan(&plan);

	/* 10. The NEGATED filesystem token: `filesystems=!xdg-bogus` fell out of
	 * the branch with no `else`, so the app kept the directory the author wrote
	 * a `!` to take away and flatpkg said nothing. */
	memset(&plan, 0, sizeof(plan));
	(void)plan_from("negated unknown xdg",
	    "[Application]\nname=Demo\ncommand=demo\n"
	    "[Context]\nfilesystems=!xdg-bogus\n", &plan);
	snprintf(why, sizeof(why), "warnings: %s", captured);
	ok("a negated xdg name flatpkg does not know warns, naming it",
	    warning_lines() == 1 &&
	    strstr(captured, "unsupported or disabled xdg dir 'xdg-bogus'")
	        != NULL,
	    why);
	ok("... and grants nothing, so the hide did not happen silently",
	    plan.ngrants == 0, why);
	permissions_free_plan(&plan);

	/* 10b. The control: a KNOWN negated xdg dir hides the grant and says
	 * nothing.  It is at `$HOME/Downloads` because the harness pins `$HOME` to
	 * a path with no `user-dirs.dirs`, which is the resolver's fallback. */
	memset(&plan, 0, sizeof(plan));
	(void)plan_from("negated known xdg",
	    "[Application]\nname=Demo\ncommand=demo\n"
	    "[Context]\nfilesystems=xdg-download\nfilesystems=!xdg-download\n",
	    &plan);
	snprintf(why, sizeof(why), "warnings: %s", captured);
	int hidden = 0;
	for (size_t i = 0; i < plan.ngrants; i++)
		if (plan.grants[i].mode == FS_GRANT_HIDE &&
		    strcmp(plan.grants[i].jail_path,
		        "/tmp/fpkg-perm-home/Downloads") == 0)
			hidden++;
	ok("a known negated xdg dir warns about nothing",
	    warning_lines() == 0, why);
	ok("... and really hides the directory it names", hidden == 1, why);
	permissions_free_plan(&plan);

	/* 10c. The other way `resolve_xdg()` says no: `xdg-run` with no
	 * subpath.  A different reason in the resolver, the same silent outcome
	 * for the author. */
	memset(&plan, 0, sizeof(plan));
	(void)plan_from("xdg-run without a subpath",
	    "[Application]\nname=Demo\ncommand=demo\n"
	    "[Context]\nfilesystems=!xdg-run\n", &plan);
	snprintf(why, sizeof(why), "warnings: %s", captured);
	ok("a negated xdg-run with no subpath warns too",
	    warning_lines() == 1, why);
	permissions_free_plan(&plan);

	/* 11. The BUS POLICY vocabulary.  A malformed entry is dropped silently and
	 * a policy naming flatpkg's OWN bus names said nothing - the dangerous
	 * shape: `talk` on `org.freedesktop.Flatpak` is `flatpak-spawn --host`. */
	{
		struct {
			const char *meta;
			const char *want;      /* NULL = must stay quiet */
			const char *what;
		} cases[] = {
			{ "[Session Bus Policy]\norg.gnome.Settings=see\n",
			  NULL, "a well-known see= is quiet" },
			{ "[Session Bus Policy]\norg.gnome.Settings=own\n",
			  NULL, "a well-known own= is quiet" },
			{ "[Session Bus Policy]\ntalk=org.gnome.Settings\n",
			  "is wrong; write",
			  "the legacy talk= shape warns: it grants nothing" },
			{ "[Session Bus Policy]\norg.gnome.Settings=listen\n",
			  "unknown policy 'listen'",
			  "an unknown policy warns, naming it" },
			{ "[Session Bus Policy]\norg.freedesktop.Flatpak=talk\n",
			  "on flatpkg's own session helper",
			  "talk on the session helper warns, in words" },
			{ "[Session Bus Policy]\norg.freedesktop.portal.Flatpak=own\n",
			  "on flatpkg's own portal",
			  "own on the portal warns, in words" },
			{ "[Session Bus Policy]\norg.freedesktop.Flatpak=see\n",
			  NULL, "see on the session helper is quiet" },
		};
		for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
			char meta[512];
			snprintf(meta, sizeof(meta),
			    "[Application]\nname=Demo\ncommand=demo\n%s",
			    cases[c].meta);
			memset(&plan, 0, sizeof(plan));
			(void)plan_from("bus policy", meta, &plan);
			snprintf(why, sizeof(why), "warnings: %s", captured);
			if (cases[c].want == NULL)
				ok(cases[c].what, warning_lines() == 0, why);
			else
				ok(cases[c].what,
				    warning_lines() == 1 &&
				    strstr(captured, cases[c].want) != NULL, why);
			permissions_free_plan(&plan);
		}
	}

	/* 7. The `run(1)` OVERRIDE path.  `option-values.sh` proves the
	 * vocabulary agrees - but a vocabulary that matches in both places can
	 * still be applied wrongly, so these assert the PLAN, not the names. */
	{
		run_opts_t o;

		/* The two entries `--unshare=all` EXPANDS into: `all` never reaches
		 * this loop, because `set_share()` rewrites it in main.c, which is not
		 * linked here. */
		memset(&o, 0, sizeof(o));
		opt_push(&o.shares, "!network");
		opt_push(&o.shares, "!ipc");
		memset(&plan, 0, sizeof(plan));
		plan.want_network = true;
		plan.want_ipc = true;
		permissions_apply_overrides(&plan, &o);
		snprintf(why, sizeof(why),
		    "after the expansion of --unshare=all: network=%d ipc=%d",
		    plan.want_network, plan.want_ipc);
		ok("the expansion of --unshare=all clears every granted share",
		    !plan.want_network && !plan.want_ipc, why);
		opt_list_free(&o.shares);
		permissions_free_plan(&plan);

		/* --device=snd: the value option-values.sh found honoured by
		 * the loop and refused by the parser. */
		memset(&o, 0, sizeof(o));
		opt_push(&o.devices, "snd");
		memset(&plan, 0, sizeof(plan));
		permissions_apply_overrides(&plan, &o);
		ok("--device=snd sets the flag sandbox.c reads",
		    plan.dev_snd, why);
		opt_list_free(&o.devices);
		permissions_free_plan(&plan);

		/* The negated form is a REMOVAL, and it is the direction a
		 * user reaches for when isolating an app - so it is the one
		 * whose silence would be most expensive. */
		memset(&o, 0, sizeof(o));
		opt_push(&o.sockets, "!x11");
		memset(&plan, 0, sizeof(plan));
		plan.sock_x11 = true;
		permissions_apply_overrides(&plan, &o);
		ok("--socket=!x11 turns the socket off again",
		    !plan.sock_x11, why);
		opt_list_free(&o.sockets);
		permissions_free_plan(&plan);

		/* `--filesystem=!xdg-bogus` is the same typo on the CLI's own removal
		 * path.  The override path runs in the parent, where a diagnostic is gone,
		 * so `capture()` is what makes it audible. */
		memset(&o, 0, sizeof(o));
		opt_push(&o.fs_remove, "!xdg-bogus");
		plan_init(&plan, PERM_HOME);
		permissions_apply_overrides(&plan, &o);
		{
			struct override_args a = { &o, &plan };
			capture(override_child, &a);
		}
		snprintf(why, sizeof(why), "warnings: %s", captured);
		ok("--filesystem=!xdg-bogus warns instead of dropping it",
		    warning_lines() == 1 &&
		    strstr(captured,
		        "unsupported or disabled xdg dir 'xdg-bogus'") != NULL,
		    why);
		opt_list_free(&o.fs_remove);
		permissions_free_plan(&plan);

		/* And the control: removing a grant that was never there is the
		 * right no-op, so flatpkg must not warn for `!xdg-download` when
		 * the app never had it.  Only an unknown NAME is a typo. */
		memset(&o, 0, sizeof(o));
		opt_push(&o.fs_remove, "!xdg-download");
		plan_init(&plan, PERM_HOME);
		permissions_apply_overrides(&plan, &o);
		{
			struct override_args a = { &o, &plan };
			capture(override_child, &a);
		}
		snprintf(why, sizeof(why), "warnings: %s", captured);
		ok("--filesystem=!xdg-download of an absent grant is quiet",
		    warning_lines() == 0, why);
		opt_list_free(&o.fs_remove);
		permissions_free_plan(&plan);
	}

	/* The case COUNT is on stdout so a shell guard can floor on it
	 * mechanically: "every case passed" is a claim about cases that might
	 * be zero. */
	printf("  %d case(s), %d failed\n", n_cases, failures);
	return failures > 0 ? 1 : 0;
}
