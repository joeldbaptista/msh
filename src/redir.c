/*
 * msh - a POSIX shell.
 *
 * Redirections (section 2.7).
 *
 * A redirection that applies to a built-in or to a compound command
 * runs inside the shell process, so the old file descriptors have to
 * come back afterwards. Each redirect() call that asks for it records
 * the descriptors it displaced, and popredir() puts them back in the
 * reverse order. The copies live at descriptor 10 and above and are
 * close-on-exec, so no child ever sees them.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "exec.h"
#include "expand.h"
#include "msh.h"
#include "tree.h"
#include "vars.h"

#define SAVEBASE 10
#define HEREMAX 4096

/*
 * What openredir() found. A negative result is not a descriptor: the
 * redirection either closes its target or duplicates another
 * descriptor, and those two cases carry no new open file.
 */
#define RCLOSE (-2)
#define RDUPFROM(fd) (-3 - (fd))
#define RDUPFD(res) (-3 - (res))

static RedirSave *savestack;

/*
 * A copy of one descriptor, out of the way of the script and marked
 * close-on-exec.
 */
int
savefd(int fd)
{
	int n;

	n = fcntl(fd, F_DUPFD, SAVEBASE);
	if (n < 0)
		return -1;
	fcntl(n, F_SETFD, FD_CLOEXEC);
	return n;
}

static void
movefd(int from, int to)
{
	if (from == to)
		return;
	if (dup2(from, to) < 0)
		error("cannot duplicate file descriptor: %s",
		      strerror(errno));
	close(from);
}

/*
 * The body of a here-document. A short body goes through a pipe,
 * which needs no file system; a long one would fill the pipe before
 * the reader starts, so it goes into a temporary file that is unlinked
 * at once and therefore needs no cleanup.
 */
int
openhere(NRedir *r)
{
	const char *tmpdir;
	char *body, *name;
	SBuf b;
	int pip[2], fd, len;

	body = expandhere(r->hdoc ? r->hdoc : "", r->hquoted);
	len = (int)strlen(body);
	if (len <= HEREMAX) {
		if (pipe(pip) < 0)
			error("cannot create pipe: %s", strerror(errno));
		if (write(pip[1], body, (size_t)len) != len) {
			close(pip[0]);
			close(pip[1]);
			error("cannot write here-document");
		}
		close(pip[1]);
		return pip[0];
	}
	tmpdir = getvar("TMPDIR");
	if (!tmpdir || !*tmpdir)
		tmpdir = "/tmp";
	sbinit(&b, stk);
	sbputs(&b, tmpdir);
	sbputs(&b, "/msh.XXXXXX");
	name = sbdone(&b);
	fd = mkstemp(name);
	if (fd < 0)
		error("cannot create a temporary file: %s", strerror(errno));
	unlink(name);
	if (write(fd, body, (size_t)len) != len) {
		close(fd);
		error("cannot write here-document");
	}
	if (lseek(fd, (off_t)0, SEEK_SET) < 0) {
		close(fd);
		error("cannot rewind here-document");
	}
	return fd;
}

static int
openredir(NRedir *r)
{
	struct stat st;
	char *fname;
	int fd, flags;

	if (r->type == RHERE)
		return openhere(r);
	fname = expandone(r->fname, EXTILDE);
	switch (r->type) {
	case RIN:
		fd = open(fname, O_RDONLY);
		break;
	case ROUT:
		/*
		 * Section 2.7.2: with `set -C' the shell refuses to
		 * truncate an existing regular file, and `>|' asks for
		 * the truncation anyway.
		 */
		if (Cflag) {
			fd = open(fname, O_WRONLY | O_CREAT | O_EXCL, 0666);
			if (fd < 0 && errno == EEXIST) {
				if (stat(fname, &st) == 0 &&
				    !S_ISREG(st.st_mode))
					fd = open(fname, O_WRONLY, 0666);
				else
					error("%s: cannot overwrite existing "
					      "file",
					      fname);
			}
			break;
		}
		fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		break;
	case RCLOBBER:
		fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		break;
	case RAPPEND:
		fd = open(fname, O_WRONLY | O_CREAT | O_APPEND, 0666);
		break;
	case RINOUT:
		fd = open(fname, O_RDWR | O_CREAT, 0666);
		break;
	case RDUPIN: /* FALLTHROUGH */
	case RDUPOUT:
		if (strcmp(fname, "-") == 0)
			return RCLOSE;
		flags = 0;
		for (fd = 0; fname[fd]; fd++) {
			if (fname[fd] < '0' || fname[fd] > '9')
				flags = 1;
		}
		if (flags || !*fname)
			error("%s: bad file descriptor", fname);
		fd = (int)strtol(fname, NULL, 10);
		if (fcntl(fd, F_GETFD) < 0)
			error("%d: bad file descriptor", fd);
		return RDUPFROM(fd);
	default:
		error("unknown redirection");
		/* NOTREACHED */
		return -1;
	}
	if (fd < 0)
		error("%s: %s", fname, strerror(errno));
	return fd;
}

/*
 * Apply a list of redirections. With RSAVE the displaced descriptors
 * are remembered, so popredir() can restore them; without it the
 * caller is a child process that will never need them again.
 */
void
redirect(NRedir *r, int flags)
{
	RedirSave *sv;
	NRedir *p;
	int n, i, fd, res;

	n = 0;
	for (p = r; p; p = p->next)
		n++;
	sv = NULL;
	if (flags & RSAVE) {
		/*
		 * These records outlive the transient arena, because an
		 * error unwinds to the main loop, which releases that
		 * arena before it restores the descriptors.
		 */
		sv = xmalloc(sizeof(*sv));
		sv->n = n;
		sv->from = xmalloc(sizeof(*sv->from) * (size_t)(n ? n : 1));
		sv->to = xmalloc(sizeof(*sv->to) * (size_t)(n ? n : 1));
		sv->prev = savestack;
		for (i = 0; i < n; i++) {
			sv->from[i] = -1;
			sv->to[i] = -1;
		}
		savestack = sv;
	}
	i = 0;
	for (p = r; p; p = p->next, i++) {
		res = openredir(p);
		if (sv) {
			sv->from[i] = p->fd;
			if (fcntl(p->fd, F_GETFD) < 0)
				sv->to[i] = -1;
			else
				sv->to[i] = savefd(p->fd);
		}
		if (res == RCLOSE) {
			close(p->fd);
			continue;
		}
		if (res <= RDUPFROM(0)) {
			fd = RDUPFD(res);
			if (dup2(fd, p->fd) < 0)
				error("%d: %s", fd, strerror(errno));
			continue;
		}
		movefd(res, p->fd);
	}
}

void
popredir(void)
{
	RedirSave *sv;
	int i;

	sv = savestack;
	if (!sv)
		return;
	savestack = sv->prev;
	for (i = sv->n - 1; i >= 0; i--) {
		if (sv->from[i] < 0)
			continue;
		if (sv->to[i] < 0) {
			close(sv->from[i]);
			continue;
		}
		dup2(sv->to[i], sv->from[i]);
		close(sv->to[i]);
	}
	free(sv->from);
	free(sv->to);
	free(sv);
}

/*
 * `exec' with redirections and no command word keeps them, so the
 * saved descriptors are dropped instead of restored (section 2.14).
 */
void
dropredir(void)
{
	RedirSave *sv;
	int i;

	sv = savestack;
	if (!sv)
		return;
	savestack = sv->prev;
	for (i = 0; i < sv->n; i++) {
		if (sv->to[i] >= 0)
			close(sv->to[i]);
	}
	free(sv->from);
	free(sv->to);
	free(sv);
}

/*
 * After an error unwinds to the main loop, every saved descriptor is
 * restored, because the commands that displaced them are gone.
 */
void
clearredir(void)
{
	while (savestack)
		popredir();
}
