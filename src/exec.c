/*
 * msh - a POSIX shell.
 *
 * The command search of section 2.9.1.1 and the handover to execve().
 *
 * The search order is fixed by the specification: a special built-in,
 * then a function, then a regular built-in, then the directories of
 * PATH in order. A hash table remembers where a PATH search ended, so
 * a repeated command costs no directory reads; assigning to PATH
 * empties the table, because the old answers may no longer hold.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "builtin.h"
#include "exec.h"
#include "expand.h"
#include "jobs.h"
#include "msh.h"
#include "trap.h"
#include "tree.h"
#include "vars.h"

#define NHASH 61
#define DEFPATH "/usr/bin:/bin"

typedef struct hentry HEntry;
struct hentry {
	HEntry *next;
	char *cmd;
	char *path;
};

static HEntry *htab[NHASH];

static unsigned
hash(const char *s)
{
	unsigned h;

	h = 5381;
	for (; *s; s++)
		h = h * 33 + (unsigned char)*s;
	return h % NHASH;
}

int
ispathname(const char *s)
{
	return strchr(s, '/') != NULL;
}

const char *
defpath(void)
{
	return DEFPATH;
}

/*
 * One directory of a PATH-like list. An empty entry means the current
 * directory (section 8.3).
 */
char *
padvance(const char **path, const char *name)
{
	const char *p, *start;
	SBuf b;

	p = *path;
	if (!p)
		return NULL;
	start = p;
	while (*p && *p != ':')
		p++;
	sbinit(&b, stk);
	if (p == start)
		sbputc(&b, '.');
	else
		sbputn(&b, start, (int)(p - start));
	sbputc(&b, '/');
	sbputs(&b, name);
	*path = *p == ':' ? p + 1 : NULL;
	return sbdone(&b);
}

void
pathchanged(void)
{
	clearhash(0);
}

void
clearhash(int verbose)
{
	HEntry *e, *next;
	int i;

	(void)verbose;
	for (i = 0; i < NHASH; i++) {
		for (e = htab[i]; e; e = next) {
			next = e->next;
			free(e->cmd);
			free(e->path);
			free(e);
		}
		htab[i] = NULL;
	}
}

static HEntry *
hfind(const char *cmd)
{
	HEntry *e;

	for (e = htab[hash(cmd)]; e; e = e->next) {
		if (strcmp(e->cmd, cmd) == 0)
			return e;
	}
	return NULL;
}

static void
hadd(const char *cmd, const char *path)
{
	HEntry *e;
	unsigned h;

	e = hfind(cmd);
	if (e) {
		free(e->path);
		e->path = xstrdup(path);
		return;
	}
	h = hash(cmd);
	e = xmalloc(sizeof(*e));
	e->cmd = xstrdup(cmd);
	e->path = xstrdup(path);
	e->next = htab[h];
	htab[h] = e;
}

void
listhash(void)
{
	HEntry *e;
	int i;

	for (i = 0; i < NHASH; i++) {
		for (e = htab[i]; e; e = e->next)
			printf("%s\t%s\n", e->cmd, e->path);
	}
}

/*
 * Whether a candidate is a file that the shell may run. A directory is
 * not, and neither is a file without execute permission, and the two
 * cases give different exit statuses (section 2.8.2).
 */
static int
executable(const char *path, int *why)
{
	struct stat st;

	if (stat(path, &st) < 0) {
		*why = CNOTFOUND;
		return 0;
	}
	if (!S_ISREG(st.st_mode)) {
		*why = CNOTEXEC;
		return 0;
	}
	if (access(path, X_OK) < 0) {
		*why = CNOTEXEC;
		return 0;
	}
	return 1;
}

void
findcommand(const char *name, CmdEntry *e, int usehash, int usefunc)
{
	const char *path;
	HEntry *h;
	Func *f;
	char *cand;
	int idx, why;

	e->idx = -1;
	e->path = NULL;
	e->fn = NULL;
	if (ispathname(name)) {
		e->type = executable(name, &why) ? CPATH : why;
		e->path = arenastrdup(stk, name);
		return;
	}
	idx = findbuiltin(name);
	if (idx >= 0 && (builtins[idx].flags & BSPECIAL)) {
		e->type = CSPECIAL;
		e->idx = idx;
		return;
	}
	if (usefunc) {
		f = lookupfunc(name);
		if (f) {
			e->type = CFUNC;
			e->fn = f;
			return;
		}
	}
	if (idx >= 0) {
		e->type = CBUILTIN;
		e->idx = idx;
		return;
	}
	if (usehash) {
		h = hfind(name);
		if (h && executable(h->path, &why)) {
			e->type = CPATH;
			e->path = arenastrdup(stk, h->path);
			return;
		}
	}
	path = getvar("PATH");
	if (!path)
		path = defpath();
	why = CNOTFOUND;
	while ((cand = padvance(&path, name)) != NULL) {
		if (executable(cand, &why)) {
			e->type = CPATH;
			e->path = cand;
			if (usehash)
				hadd(name, cand);
			return;
		}
		if (why == CNOTEXEC)
			e->path = cand;
	}
	e->type = why;
}

void
hashcommand(const char *name)
{
	CmdEntry e;

	findcommand(name, &e, 1, 0);
	if (e.type != CPATH)
		warn("%s: not found", name);
}

/*
 * Hand the process over to another program. A file that the kernel
 * refuses to start because it has no recognised format is a shell
 * script (section 2.9.1.1), and this shell runs it itself rather than
 * calling another one.
 */
void
execcommand(const char *path, char **argv)
{
	char **env;
	char **params;
	int fd, i, n;

	env = environment();
	execve(path, argv, env);
	if (errno == ENOEXEC) {
		fd = open(path, O_RDONLY);
		if (fd < 0)
			error("%s: %s", path, strerror(errno));
		for (n = 0; argv[n]; n++)
			;
		params = xmalloc(sizeof(*params) * (size_t)n);
		for (i = 1; i < n; i++)
			params[i - 1] = xstrdup(argv[i]);
		params[n - 1] = NULL;
		arg0 = xstrdup(path);
		setparams(params);
		clearalltraps();
		pushfile(fd, path);
		exitshell(cmdloop(0));
	}
	if (errno == ENOENT || errno == ENOTDIR) {
		warn("%s: not found", path);
		exitstatus = 127;
	} else {
		warn("%s: %s", path, strerror(errno));
		exitstatus = 126;
	}
	flushall();
	_exit(exitstatus);
	/* NOTREACHED */
}

/*
 * The last step of a child process: find the command and run it. The
 * status that the shell reports when the search fails is 127 for `not
 * found' and 126 for `found but not executable'.
 */
void
shellexec(char **argv, const char *path, int idx)
{
	(void)idx;
	if (path)
		execcommand(path, argv);
	warn("%s: not found", argv[0]);
	flushall();
	_exit(127);
	/* NOTREACHED */
}
