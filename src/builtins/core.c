/*
 * msh - a POSIX shell.
 *
 * The built-ins that control the shell itself: the no-ops, `exit',
 * `shift', `times', the loop control, `return', `eval', `exec' and
 * `.'.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/times.h>
#include <unistd.h>

#include "../builtin.h"
#include "../exec.h"
#include "../expand.h"
#include "../jobs.h"
#include "../msh.h"
#include "../trap.h"
#include "../tree.h"
#include "../vars.h"

int
bltin_true(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return 0;
}

int
bltin_colon(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return 0;
}

int
bltin_false(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return 1;
}

/*
 * Section 2.14: `exit' with no operand uses the status of the last
 * command, so the shell cannot simply report zero.
 */
int
bltin_exit(int argc, char **argv)
{
	int status;

	status = exitstatus;
	if (argc > 1)
		status = (int)bnumber(argv[1], "exit");
	exitshell(status);
	/* NOTREACHED */
	return 0;
}

int
bltin_shift(int argc, char **argv)
{
	long n;
	int i;

	n = 1;
	if (argc > 1)
		n = bnumber(argv[1], "shift");
	if (n < 0 || n > shparam.nparam)
		error("shift: can shift only %d", shparam.nparam);
	for (i = 0; i < n; i++)
		free(shparam.p[i]);
	for (i = 0; i + n <= shparam.nparam; i++)
		shparam.p[i] = shparam.p[i + n];
	shparam.nparam -= (int)n;
	return 0;
}

int
bltin_times(int argc, char **argv)
{
	struct tms t;
	long hz;

	(void)argc;
	(void)argv;
	hz = sysconf(_SC_CLK_TCK);
	if (hz <= 0)
		hz = 100;
	if (times(&t) == (clock_t)-1) {
		warn("times: %s", strerror(errno));
		return 1;
	}
	printf("%ldm%fs %ldm%fs\n", (long)(t.tms_utime / hz / 60),
	       (double)(t.tms_utime % (hz * 60)) / (double)hz,
	       (long)(t.tms_stime / hz / 60),
	       (double)(t.tms_stime % (hz * 60)) / (double)hz);
	printf("%ldm%fs %ldm%fs\n", (long)(t.tms_cutime / hz / 60),
	       (double)(t.tms_cutime % (hz * 60)) / (double)hz,
	       (long)(t.tms_cstime / hz / 60),
	       (double)(t.tms_cstime % (hz * 60)) / (double)hz);
	return 0;
}

static int
loopcontrol(int argc, char **argv, int what)
{
	long n;

	n = 1;
	if (argc > 1)
		n = bnumber(argv[1], argv[0]);
	if (n < 1)
		error("%s: %s: bad count", argv[0], argv[1]);
	if (loopnest == 0)
		return 0;
	if (n > loopnest)
		n = loopnest;
	evalskip = what;
	skipcount = (int)n;
	return 0;
}

int
bltin_break(int argc, char **argv)
{
	return loopcontrol(argc, argv, SKIPBREAK);
}

int
bltin_continue(int argc, char **argv)
{
	return loopcontrol(argc, argv, SKIPCONT);
}

/*
 * `return' leaves a function or a dot script. Outside both, section
 * 2.14 leaves the behaviour unspecified; this shell treats it as the
 * end of the file that is running, which is what a dot script does.
 */
int
bltin_return(int argc, char **argv)
{
	int status;

	status = exitstatus;
	if (argc > 1)
		status = (int)bnumber(argv[1], "return");
	evalskip = SKIPRETURN;
	skipcount = 1;
	exitstatus = status;
	return status;
}

int
bltin_eval(int argc, char **argv)
{
	SBuf b;
	int i;

	if (argc < 2)
		return 0;
	sbinit(&b, stk);
	for (i = 1; i < argc; i++) {
		if (i > 1)
			sbputc(&b, ' ');
		sbputs(&b, argv[i]);
	}
	exitstatus = 0;
	evalstring(sbdone(&b), NULL);
	return exitstatus;
}

/*
 * `exec' replaces the shell. With no command word it only keeps the
 * redirections that the command carried, and the evaluator has already
 * made those permanent.
 */
int
bltin_exec(int argc, char **argv)
{
	CmdEntry e;

	if (argc < 2)
		return 0;
	findcommand(argv[1], &e, 1, 0);
	if (e.type == CBUILTIN || e.type == CSPECIAL) {
		/*
		 * A built-in is not a file, so there is nothing to
		 * exec. Running it is the useful reading, and section
		 * 2.14 leaves the case unspecified.
		 */
		return evalbuiltin(e.idx, argc - 1, argv + 1);
	}
	if (e.type != CPATH) {
		warn("%s: %s", argv[1],
		     e.type == CNOTFOUND ? "not found" : "cannot execute");
		if (!iflag)
			exitshell(e.type == CNOTFOUND ? 127 : 126);
		return e.type == CNOTFOUND ? 127 : 126;
	}
	defaultsigs();
	flushall();
	execcommand(e.path, argv + 1);
	/* NOTREACHED */
	return 127;
}

/*
 * `.' runs a file in the current environment. The file is looked up in
 * PATH when its name holds no slash (section 2.14).
 */
int
bltin_dot(int argc, char **argv)
{
	const char *path;
	char *cand, *found;
	int fd, status, depth;

	if (argc < 2)
		error(".: a file name is required");
	found = NULL;
	if (ispathname(argv[1])) {
		found = argv[1];
	} else {
		path = getvar("PATH");
		if (!path)
			path = defpath();
		while ((cand = padvance(&path, argv[1])) != NULL) {
			if (access(cand, R_OK) == 0) {
				found = cand;
				break;
			}
		}
		if (!found)
			found = argv[1];
	}
	fd = open(found, O_RDONLY);
	if (fd < 0)
		error("%s: %s", found, strerror(errno));
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	depth = inputdepth();
	pushfile(fd, found);
	status = cmdloop(0);
	popinputto(depth);
	if (evalskip == SKIPRETURN) {
		evalskip = SKIPNONE;
		skipcount = 0;
		status = exitstatus;
	}
	return status;
}
