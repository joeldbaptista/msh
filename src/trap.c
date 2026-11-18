/*
 * msh - a POSIX shell.
 *
 * Signal dispositions and trap actions (sections 2.11 and 2.12).
 *
 * A signal can arrive between any two machine instructions, so the
 * handler does the least it can: it records which signal arrived and
 * sets one flag. The main loop and the evaluator call dotrap() at
 * points where running a command is safe, and dotrap() runs the
 * action there.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "exec.h"
#include "jobs.h"
#include "msh.h"
#include "trap.h"
#include "tree.h"
#include "vars.h"

#ifndef NSIG
#define NSIG 64
#endif

/*
 * The signal names of the `kill' and `trap' pages. The table is the
 * whole mapping in both directions, so no code spells a signal name.
 */
static const struct {
	const char *name;
	int sig;
} signames[] = {
    {"HUP", SIGHUP},
    {"INT", SIGINT},
    {"QUIT", SIGQUIT},
    {"ILL", SIGILL},
    {"TRAP", SIGTRAP},
    {"ABRT", SIGABRT},
    {"FPE", SIGFPE},
    {"KILL", SIGKILL},
    {"BUS", SIGBUS},
    {"SEGV", SIGSEGV},
    {"SYS", SIGSYS},
    {"PIPE", SIGPIPE},
    {"ALRM", SIGALRM},
    {"TERM", SIGTERM},
    {"URG", SIGURG},
    {"STOP", SIGSTOP},
    {"TSTP", SIGTSTP},
    {"CONT", SIGCONT},
    {"CHLD", SIGCHLD},
    {"TTIN", SIGTTIN},
    {"TTOU", SIGTTOU},
    {"XCPU", SIGXCPU},
    {"XFSZ", SIGXFSZ},
    {"VTALRM", SIGVTALRM},
    {"PROF", SIGPROF},
    {"USR1", SIGUSR1},
    {"USR2", SIGUSR2},
};

volatile sig_atomic_t pendingsigs;
volatile sig_atomic_t gotsigchld;

static char *trapaction[NSIG]; /* NULL: default; "": ignore */
static volatile sig_atomic_t gotsig[NSIG];
static char sigignored[NSIG]; /* ignored when the shell started */
static char *exittrap;

int
signame2num(const char *s)
{
	size_t i;
	char *end;
	long n;

	if (strcasecmp(s, "EXIT") == 0)
		return 0;
	if (s[0] >= '0' && s[0] <= '9') {
		n = strtol(s, &end, 10);
		if (*end || n < 0 || n >= NSIG)
			return -1;
		return (int)n;
	}
	if (strncasecmp(s, "SIG", 3) == 0)
		s += 3;
	for (i = 0; i < LEN(signames); i++) {
		if (strcasecmp(s, signames[i].name) == 0)
			return signames[i].sig;
	}
	return -1;
}

const char *
signum2name(int sig)
{
	size_t i;

	if (sig == 0)
		return "EXIT";
	for (i = 0; i < LEN(signames); i++) {
		if (signames[i].sig == sig)
			return signames[i].name;
	}
	return NULL;
}

int
pendingsigsset(void)
{
	return pendingsigs != 0;
}

static void
onsig(int sig)
{
	if (sig < 0 || sig >= NSIG)
		return;
	if (sig == SIGCHLD) {
		gotsigchld = 1;
		if (!trapaction[SIGCHLD])
			return;
	}
	gotsig[sig] = 1;
	pendingsigs = 1;
	if (sig == SIGINT && intsuppressed())
		setintpending();
}

static void
install(int sig, void (*fn)(int))
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fn;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0; /* no SA_RESTART: a read must return EINTR */
	sigaction(sig, &sa, NULL);
}

void
ignoresig(int sig)
{
	install(sig, SIG_IGN);
}

/*
 * The disposition that one signal should have now. Section 2.11 keeps
 * a signal that was ignored when the shell started ignored, and that
 * rule outranks everything but an explicit trap.
 */
void
setsignal(int sig)
{
	if (sig <= 0 || sig >= NSIG)
		return;
	if (trapaction[sig]) {
		if (*trapaction[sig] == '\0')
			install(sig, SIG_IGN);
		else
			install(sig, onsig);
		return;
	}
	if (sigignored[sig]) {
		install(sig, SIG_IGN);
		return;
	}
	if (iflag && (sig == SIGINT || sig == SIGQUIT)) {
		/*
		 * An interactive shell must survive the interrupt that
		 * stops the command it is running, so it catches these
		 * two rather than dying of them.
		 */
		install(sig, sig == SIGINT ? onsig : SIG_IGN);
		return;
	}
	if (iflag && sig == SIGTERM) {
		install(sig, SIG_IGN);
		return;
	}
	install(sig, SIG_DFL);
}

void
trapsinit(void)
{
	struct sigaction sa;
	size_t i;
	int sig;

	for (i = 0; i < LEN(signames); i++) {
		sig = signames[i].sig;
		if (sig <= 0 || sig >= NSIG)
			continue;
		if (sigaction(sig, NULL, &sa) == 0 &&
		    sa.sa_handler == SIG_IGN)
			sigignored[sig] = 1;
	}
	setsignal(SIGINT);
	setsignal(SIGQUIT);
	setsignal(SIGTERM);
}

/*
 * Every disposition back to the default, as a shell that replaces
 * itself with a script needs.
 */
void
clearalltraps(void)
{
	int sig;

	free(exittrap);
	exittrap = NULL;
	for (sig = 1; sig < NSIG; sig++) {
		if (!trapaction[sig])
			continue;
		free(trapaction[sig]);
		trapaction[sig] = NULL;
		install(sig, sigignored[sig] ? SIG_IGN : SIG_DFL);
	}
	pendingsigs = 0;
}

/*
 * Section 2.12: a subshell resets every trap that the parent caught to
 * its default, and keeps the ones the parent ignored.
 */
void
clearcaughttraps(void)
{
	int sig;

	free(exittrap);
	exittrap = NULL;
	for (sig = 1; sig < NSIG; sig++) {
		gotsig[sig] = 0;
		if (!trapaction[sig] || *trapaction[sig] == '\0')
			continue;
		free(trapaction[sig]);
		trapaction[sig] = NULL;
		install(sig, sigignored[sig] ? SIG_IGN : SIG_DFL);
	}
	pendingsigs = 0;
}

void
defaultsigs(void)
{
	int sig;

	for (sig = 1; sig < NSIG; sig++) {
		if (sigignored[sig])
			continue;
		if (trapaction[sig] && *trapaction[sig] == '\0')
			continue; /* an ignored signal stays ignored */
		install(sig, SIG_DFL);
	}
}

int
settrap(const char *signame, const char *action)
{
	int sig;

	sig = signame2num(signame);
	if (sig < 0) {
		warn("%s: bad signal specification", signame);
		return -1;
	}
	if (sig == 0) {
		free(exittrap);
		exittrap = action ? xstrdup(action) : NULL;
		return 0;
	}
	free(trapaction[sig]);
	trapaction[sig] = action ? xstrdup(action) : NULL;
	setsignal(sig);
	return 0;
}

void
listtraps(void)
{
	int sig;
	const char *name;

	if (exittrap)
		printf("trap -- %s EXIT\n", singlequote(exittrap, stk));
	for (sig = 1; sig < NSIG; sig++) {
		if (!trapaction[sig])
			continue;
		name = signum2name(sig);
		if (!name)
			continue;
		printf("trap -- %s %s\n",
		       singlequote(trapaction[sig], stk), name);
	}
}

/*
 * Run the actions of the signals that arrived. The action is ordinary
 * shell input, so a trap can do anything a command can, and `$?' keeps
 * the value it had before the trap ran.
 */
void
dotrap(void)
{
	int sig, savestatus, saveskip, savecount;

	if (!pendingsigs)
		return;
	pendingsigs = 0;
	savestatus = exitstatus;
	saveskip = evalskip;
	savecount = skipcount;
	for (sig = 1; sig < NSIG; sig++) {
		if (!gotsig[sig])
			continue;
		gotsig[sig] = 0;
		if (!trapaction[sig] || !*trapaction[sig]) {
			/*
			 * An interrupt with no trap abandons the command
			 * that an interactive shell is running.
			 */
			if (sig == SIGINT && iflag) {
				exitstatus = 128 + SIGINT;
				raise_interrupt();
			}
			continue;
		}
		evalskip = SKIPNONE;
		skipcount = 0;
		exitstatus = savestatus;
		evalstring(trapaction[sig], NULL);
	}
	if (evalskip == SKIPNONE) {
		evalskip = saveskip;
		skipcount = savecount;
	}
	exitstatus = savestatus;
}

void
runexittrap(void)
{
	char *action;

	if (!exittrap)
		return;
	action = exittrap;
	exittrap = NULL; /* so a nested exit does not run it twice */
	evalskip = SKIPNONE;
	evalstring(action, NULL);
	free(action);
}
