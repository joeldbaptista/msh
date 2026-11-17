/*
 * msh - a POSIX shell.
 *
 * The built-ins that work on jobs, traps and resource limits: `jobs',
 * `wait', `fg', `bg', `trap' and `ulimit'.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
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
bltin_jobs(int argc, char **argv)
{
	BOpt o;
	Job *j;
	int *ids;
	int c, i, n, mode;

	mode = 0;
	boptinit(&o, argc, argv);
	while ((c = bopt(&o, "lp")) != -1) {
		if (c == 'l' || c == 'p') {
			mode = c;
			continue;
		}
		warn("jobs: %c: bad option", o.opt);
		return 2;
	}
	n = argc - o.ind;
	if (n <= 0) {
		showjobs(mode, NULL, 0);
		return 0;
	}
	ids = arenaalloc(stk, sizeof(*ids) * (size_t)n);
	for (i = 0; i < n; i++) {
		j = getjob(argv[o.ind + i]);
		if (!j) {
			warn("jobs: %s: no such job", argv[o.ind + i]);
			return 1;
		}
		ids[i] = jobidof(j);
	}
	showjobs(mode, ids, n);
	return 0;
}

int
bltin_wait(int argc, char **argv)
{
	Job *j;
	long pid;
	char *end;
	int i, status;

	if (argc == 1) {
		dowaitall();
		return 0;
	}
	status = 0;
	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '%') {
			j = getjob(argv[i]);
			if (!j) {
				warn("wait: %s: no such job", argv[i]);
				status = 127;
				continue;
			}
			status = dowaitjob(j);
			continue;
		}
		pid = strtol(argv[i], &end, 10);
		if (*argv[i] == '\0' || *end != '\0') {
			warn("wait: %s: bad process id", argv[i]);
			status = 2;
			continue;
		}
		status = dowaitpid((pid_t)pid);
	}
	return status;
}

static int
resume(int argc, char **argv, int fg)
{
	Job *j;
	int i, status;

	if (!mflag) {
		warn("%s: job control is not enabled", argv[0]);
		return 1;
	}
	if (argc == 1) {
		j = getjob(NULL);
		if (!j) {
			warn("%s: no current job", argv[0]);
			return 1;
		}
		return startjob(j, fg);
	}
	status = 0;
	for (i = 1; i < argc; i++) {
		j = getjob(argv[i]);
		if (!j) {
			warn("%s: %s: no such job", argv[0], argv[i]);
			status = 1;
			continue;
		}
		status = startjob(j, fg);
	}
	return status;
}

int
bltin_fg(int argc, char **argv)
{
	return resume(argc, argv, 1);
}

int
bltin_bg(int argc, char **argv)
{
	return resume(argc, argv, 0);
}

static int
allnumeric(const char *s)
{
	if (!*s)
		return 0;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return 0;
	}
	return 1;
}

/*
 * `trap'. The first operand is the action, unless it is `-' or an
 * unsigned decimal integer, in which case every operand is a
 * condition and the action goes back to the default (the `trap' page).
 */
int
bltin_trap(int argc, char **argv)
{
	const char *action;
	int i, status;

	i = 1;
	if (i < argc && strcmp(argv[i], "--") == 0)
		i++;
	if (i >= argc) {
		listtraps();
		return 0;
	}
	if (strcmp(argv[i], "-") == 0) {
		action = NULL;
		i++;
	} else if (allnumeric(argv[i])) {
		action = NULL;
	} else {
		action = argv[i];
		i++;
	}
	if (i >= argc) {
		warn("trap: a condition is required");
		return 2;
	}
	status = 0;
	for (; i < argc; i++) {
		if (settrap(argv[i], action) < 0)
			status = 1;
	}
	return status;
}

/*
 * The resource limits, as a table: the option letter, the name for
 * `-a', the resource, and the unit that the shell reports in. So
 * adding a limit means adding a row.
 */
static const struct {
	char letter;
	const char *name;
	int res;
	long unit;
} limits[] = {
    {'c', "core file size (blocks)", RLIMIT_CORE, 512},
    {'d', "data seg size (kbytes)", RLIMIT_DATA, 1024},
    {'f', "file size (blocks)", RLIMIT_FSIZE, 512},
#ifdef RLIMIT_NOFILE
    {'n', "open files", RLIMIT_NOFILE, 1},
#endif
    {'s', "stack size (kbytes)", RLIMIT_STACK, 1024},
    {'t', "cpu time (seconds)", RLIMIT_CPU, 1},
#ifdef RLIMIT_AS
    {'v', "virtual memory (kbytes)", RLIMIT_AS, 1024},
#endif
#ifdef RLIMIT_NPROC
    {'u', "max user processes", RLIMIT_NPROC, 1},
#endif
};

static void
showlimit(int i, int hard, int withname)
{
	struct rlimit rl;
	rlim_t v;

	if (getrlimit(limits[i].res, &rl) < 0)
		return;
	v = hard ? rl.rlim_max : rl.rlim_cur;
	if (withname)
		printf("-%c: %-28s ", limits[i].letter, limits[i].name);
	if (v == RLIM_INFINITY)
		printf("unlimited\n");
	else
		printf("%ld\n", (long)(v / (rlim_t)limits[i].unit));
}

int
bltin_ulimit(int argc, char **argv)
{
	struct rlimit rl;
	BOpt o;
	size_t k;
	int c, hard, soft, which, all;
	long n;
	char *end;

	hard = 0;
	soft = 0;
	all = 0;
	which = -1;
	boptinit(&o, argc, argv);
	while ((c = bopt(&o, "HSacdfnstuv")) != -1) {
		if (c == 'H') {
			hard = 1;
			continue;
		}
		if (c == 'S') {
			soft = 1;
			continue;
		}
		if (c == 'a') {
			all = 1;
			continue;
		}
		if (c == '?') {
			warn("ulimit: %c: bad option", o.opt);
			return 2;
		}
		for (k = 0; k < LEN(limits); k++) {
			if (limits[k].letter == c)
				which = (int)k;
		}
	}
	if (all) {
		for (k = 0; k < LEN(limits); k++)
			showlimit((int)k, hard, 1);
		return 0;
	}
	if (which < 0) {
		for (k = 0; k < LEN(limits); k++) {
			if (limits[k].letter == 'f')
				which = (int)k;
		}
	}
	if (o.ind >= argc) {
		showlimit(which, hard && !soft, 0);
		return 0;
	}
	if (getrlimit(limits[which].res, &rl) < 0) {
		warn("ulimit: %s", strerror(errno));
		return 1;
	}
	if (strcmp(argv[o.ind], "unlimited") == 0) {
		n = -1;
	} else {
		n = strtol(argv[o.ind], &end, 10);
		if (*argv[o.ind] == '\0' || *end != '\0' || n < 0) {
			warn("ulimit: %s: bad limit", argv[o.ind]);
			return 1;
		}
	}
	if (!hard && !soft)
		hard = soft = 1;
	if (soft)
		rl.rlim_cur = n < 0 ? RLIM_INFINITY
		                    : (rlim_t)n * (rlim_t)limits[which].unit;
	if (hard)
		rl.rlim_max = n < 0 ? RLIM_INFINITY
		                    : (rlim_t)n * (rlim_t)limits[which].unit;
	if (setrlimit(limits[which].res, &rl) < 0) {
		warn("ulimit: %s", strerror(errno));
		return 1;
	}
	return 0;
}
