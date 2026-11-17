/*
 * msh - a POSIX shell.
 *
 * Error reporting and the unwind to the main loop.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "jobs.h"
#include "msh.h"
#include "trap.h"
#include "tree.h"

struct jmploc *handler;
int exception;
int exitstatus;
const char *argv0 = "msh";

static int suppressint;
static int intpending;

void
intoff(void)
{
	suppressint++;
}

void
inton(void)
{
	if (--suppressint == 0 && intpending) {
		intpending = 0;
		raise_interrupt();
	}
}

void
setintpending(void)
{
	intpending = 1;
}

int
intsuppressed(void)
{
	return suppressint;
}

static void
vreport(const char *fmt, va_list ap)
{
	fflush(stdout);
	fprintf(stderr, "%s: ", argv0);
	if (iflag == 0 && inputname())
		fprintf(stderr, "%s: line %d: ", inputname(), inputlineno());
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	fflush(stderr);
}

void
warn(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vreport(fmt, ap);
	va_end(ap);
}

/*
 * An error abandons the current command. The transient arena is not
 * released here, because the main loop releases it to its own mark
 * after the longjmp.
 */
void
error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vreport(fmt, ap);
	va_end(ap);
	exitstatus = 2;
	errexit();
	/* NOTREACHED */
}

void
errexit(void)
{
	if (!handler)
		exitshell(exitstatus ? exitstatus : 2);
	exception = EXERROR;
	longjmp(handler->loc, 1);
	/* NOTREACHED */
}

/*
 * Leave the shell. The EXIT trap runs first, and it runs in a subshell
 * too, because a subshell is a shell environment and may have set one
 * of its own; section 2.12 has already cleared the trap that the
 * parent set, so there is no danger of running it twice.
 *
 * The status that the trap action leaves behind does not become the
 * status of the shell. Only an `exit' inside the action changes it,
 * and that arrives here again as a fresh call.
 */
void
exitshell(int status)
{
	struct jmploc h;

	exitstatus = status;
	if (setjmphandler(&h) == 0)
		runexittrap();
	popjmphandler(&h);
	flushall();
	_exit(status & 0xff);
	/* NOTREACHED */
}

void
flushall(void)
{
	fflush(stdout);
	fflush(stderr);
}
