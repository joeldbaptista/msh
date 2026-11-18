/*
 * msh - a POSIX shell.
 *
 * The input reader. One interface serves a file descriptor, a string
 * and a terminal. Sources nest, because `eval' and `.' push a source
 * and pop it at its end of input. End of input does not pop the
 * source, so the parser of an `eval' string sees the end of that
 * string and not the text below it.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "expand.h"
#include "msh.h"
#include "trap.h"
#include "tree.h"
#include "vars.h"

#define IBUFSZ 4096
#define NUNGOT 4

typedef struct insrc InSrc;
struct insrc {
	InSrc *prev;
	int type;
	int fd;
	char *buf;  /* INFD: the read buffer; INSTR: the whole string */
	int len;    /* bytes in buf */
	int pos;    /* the next byte to hand out */
	int lno;    /* the line number of the next byte */
	char *name; /* for error messages */
	int eof;
	int istty;
	char ungot[NUNGOT];
	int nungot;
};

static InSrc *top;
static int whichprompt = 1;

static InSrc *
newsrc(void)
{
	InSrc *s;

	s = xmalloc(sizeof(*s));
	memset(s, 0, sizeof(*s));
	s->prev = top;
	s->lno = 1;
	return s;
}

void
pushfile(int fd, const char *name)
{
	InSrc *s;

	s = newsrc();
	s->type = INFD;
	s->fd = fd;
	s->buf = xmalloc(IBUFSZ);
	s->name = name ? xstrdup(name) : NULL;
	s->istty = isatty(fd);
	top = s;
}

void
pushstring(const char *str, const char *name)
{
	InSrc *s;

	s = newsrc();
	s->type = INSTR;
	s->buf = xstrdup(str);
	s->len = (int)strlen(str);
	s->name = name ? xstrdup(name) : NULL;
	if (s->prev)
		s->lno = s->prev->lno;
	top = s;
}

void
popinput(void)
{
	InSrc *s;

	if (!top)
		return;
	s = top;
	top = s->prev;
	if (s->type == INFD && s->fd > 2)
		close(s->fd);
	free(s->buf);
	free(s->name);
	free(s);
}

const char *
inputname(void)
{
	InSrc *s;

	for (s = top; s; s = s->prev) {
		if (s->name)
			return s->name;
	}
	return NULL;
}

int
inputlineno(void)
{
	return top ? top->lno : 0;
}

void
setprompt(int which)
{
	whichprompt = which;
}

/*
 * The prompt goes to standard error, and parameter expansion applies
 * to it (the `sh' page, PS1).
 */
static void
showprompt(void)
{
	const char *v;
	char *p;

	if (!iflag || !top || !top->istty || whichprompt == 0)
		return;
	v = getvar(whichprompt == 1 ? "PS1" : "PS2");
	if (!v)
		v = whichprompt == 1 ? "$ " : "> ";
	p = expandprompt(v);
	fflush(stdout);
	fputs(p, stderr);
	fflush(stderr);
}

static int
fillbuf(void)
{
	int n;

	if (top->eof)
		return -1;
	showprompt();
	for (;;) {
		n = (int)read(top->fd, top->buf, IBUFSZ);
		if (n >= 0)
			break;
		if (errno == EINTR) {
			if (pendingsigsset())
				return -1;
			continue;
		}
		return -1;
	}
	if (n == 0) {
		top->eof = 1;
		return -1;
	}
	top->len = n;
	top->pos = 0;
	if (vflag && write(2, top->buf, (size_t)n) < 0) {
		; /* a failed echo of the input is not worth a message */
	}
	return 0;
}

int
pgetc(void)
{
	int c;

	if (!top)
		return -1;
	if (top->nungot > 0) {
		c = (unsigned char)top->ungot[--top->nungot];
		if (c == '\n')
			top->lno++;
		return c;
	}
	if (top->pos >= top->len) {
		if (top->type == INSTR)
			return -1;
		if (fillbuf() < 0)
			return -1;
	}
	c = (unsigned char)top->buf[top->pos++];
	if (c == '\n')
		top->lno++;
	return c;
}

void
pungetc(int c)
{
	if (c < 0 || !top)
		return;
	if (c == '\n' && top->lno > 1)
		top->lno--;
	if (top->nungot < NUNGOT)
		top->ungot[top->nungot++] = (char)c;
}

int
atinputeof(void)
{
	if (!top)
		return 1;
	if (top->nungot > 0)
		return 0;
	if (top->pos < top->len)
		return 0;
	return top->type == INSTR || top->eof;
}

/*
 * After an error, the rest of the line that the shell had already read
 * is not part of any command, so discard it. Only the rest of the
 * line: the lines after it are still input, and an interactive shell
 * goes on to read them.
 */
void
flushinput(void)
{
	int c;

	if (!top)
		return;
	top->nungot = 0;
	while (top->pos < top->len) {
		c = (unsigned char)top->buf[top->pos++];
		if (c == '\n')
			break;
	}
}

int
inputdepth(void)
{
	InSrc *s;
	int n;

	n = 0;
	for (s = top; s; s = s->prev)
		n++;
	return n;
}

void
popinputto(int depth)
{
	while (inputdepth() > depth)
		popinput();
}

void
resetinput(void)
{
	whichprompt = 1;
	if (top)
		top->nungot = 0;
}

/*
 * With `set -o ignoreeof' an end of file at the terminal is not the
 * end of the session, so the source has to forget that it saw one.
 */
void
clearinputeof(void)
{
	if (top)
		top->eof = 0;
}

/*
 * The `read' built-in reads standard input. When the script the shell
 * is running *is* standard input, the bytes the shell has buffered but
 * not used belong to that same stream, so give them back to the file
 * and drop the buffer. When the script is any other file, there is
 * nothing to give back: seeking then would move an offset that a
 * parent shell shares, and corrupt the script it is reading.
 */
void
syncinput(void)
{
	int n;

	if (!top || top->type != INFD || top->fd != 0)
		return;
	n = top->len - top->pos + top->nungot;
	if (n <= 0)
		return;
	if (lseek(top->fd, (off_t)-n, SEEK_CUR) < 0)
		return;
	top->pos = top->len;
	top->nungot = 0;
}
