/*
 * msh - a POSIX shell.
 *
 * Arenas, growable buffers and the expanded string.
 */
#include <stdlib.h>
#include <string.h>

#include "msh.h"

#define BLOCKMIN 4096
#define ALIGN sizeof(long double)

Arena *stk;

static ABlock *
newblock(size_t n, ABlock *next)
{
	ABlock *b;
	size_t cap;

	cap = BLOCKMIN;
	while (cap < n)
		cap *= 2;
	b = xmalloc(sizeof(*b));
	b->mem = xmalloc(cap);
	b->cap = cap;
	b->used = 0;
	b->next = next;
	return b;
}

void *
xmalloc(size_t n)
{
	void *p;

	p = malloc(n ? n : 1);
	if (!p)
		error("out of memory");
	return p;
}

void *
xrealloc(void *p, size_t n)
{
	void *q;

	q = realloc(p, n ? n : 1);
	if (!q)
		error("out of memory");
	return q;
}

char *
xstrdup(const char *s)
{
	size_t n;
	char *p;

	n = strlen(s) + 1;
	p = xmalloc(n);
	memcpy(p, s, n);
	return p;
}

Arena *
arenanew(void)
{
	Arena *a;

	a = xmalloc(sizeof(*a));
	a->cur = NULL;
	a->refs = 1;
	return a;
}

/*
 * A new block always goes at the head of the chain, so a mark can free
 * every block that the arena gained after it. A large request wastes
 * the tail of the block below it, which is cheap enough.
 */
void *
arenaalloc(Arena *a, size_t n)
{
	char *p;

	n = (n + (ALIGN - 1)) & ~(ALIGN - 1);
	if (!a->cur || a->cur->cap - a->cur->used < n)
		a->cur = newblock(n, a->cur);
	p = a->cur->mem + a->cur->used;
	a->cur->used += n;
	return p;
}

char *
arenastrdup(Arena *a, const char *s)
{
	return arenastrndup(a, s, strlen(s));
}

char *
arenastrndup(Arena *a, const char *s, size_t n)
{
	char *p;

	p = arenaalloc(a, n + 1);
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

void
arenamark(Arena *a, AMark *m)
{
	m->blk = a->cur;
	m->used = a->cur ? a->cur->used : 0;
}

void
arenarelease(Arena *a, AMark *m)
{
	ABlock *b;

	while (a->cur != m->blk) {
		b = a->cur;
		a->cur = b->next;
		free(b->mem);
		free(b);
	}
	if (a->cur)
		a->cur->used = m->used;
}

void
arenaretain(Arena *a)
{
	a->refs++;
}

/*
 * A function definition keeps the arena of the command that defined
 * it, so the arena of a complete command outlives the command when the
 * command defined a function.
 */
void
arenadrop(Arena *a)
{
	AMark m;

	if (!a || --a->refs > 0)
		return;
	m.blk = NULL;
	m.used = 0;
	arenarelease(a, &m);
	free(a);
}

void
sbinit(SBuf *b, Arena *a)
{
	b->a = a;
	b->s = NULL;
	b->len = 0;
	b->cap = 0;
}

static void
sbgrow(SBuf *b, int n)
{
	char *s;
	int cap;

	cap = b->cap ? b->cap * 2 : 32;
	while (cap < b->len + n)
		cap *= 2;
	s = arenaalloc(b->a, (size_t)cap);
	if (b->len)
		memcpy(s, b->s, (size_t)b->len);
	b->s = s;
	b->cap = cap;
}

void
sbputc(SBuf *b, int c)
{
	if (b->len + 2 > b->cap)
		sbgrow(b, 2);
	b->s[b->len++] = (char)c;
}

void
sbputn(SBuf *b, const char *s, int n)
{
	if (b->len + n + 1 > b->cap)
		sbgrow(b, n + 1);
	memcpy(b->s + b->len, s, (size_t)n);
	b->len += n;
}

void
sbputs(SBuf *b, const char *s)
{
	sbputn(b, s, (int)strlen(s));
}

char *
sbdone(SBuf *b)
{
	if (b->len + 1 > b->cap)
		sbgrow(b, 1);
	b->s[b->len] = '\0';
	return b->s;
}

/*
 * The expanded string lives in the transient arena, so a longjmp out
 * of expansion leaks nothing.
 */
void
esinit(EStr *e)
{
	e->s = NULL;
	e->q = NULL;
	e->len = 0;
	e->cap = 0;
}

static void
esgrow(EStr *e, int n)
{
	char *s, *q;
	int cap;

	cap = e->cap ? e->cap * 2 : 32;
	while (cap < e->len + n)
		cap *= 2;
	s = arenaalloc(stk, (size_t)cap);
	q = arenaalloc(stk, (size_t)cap);
	if (e->len) {
		memcpy(s, e->s, (size_t)e->len);
		memcpy(q, e->q, (size_t)e->len);
	}
	e->s = s;
	e->q = q;
	e->cap = cap;
}

void
esputc(EStr *e, int c, int quoted)
{
	if (e->len + 2 > e->cap)
		esgrow(e, 2);
	e->s[e->len] = (char)c;
	e->q[e->len] = (char)(quoted != 0);
	e->len++;
}

void
esputn(EStr *e, const char *s, int n, int quoted)
{
	if (e->len + n + 1 > e->cap)
		esgrow(e, n + 1);
	memcpy(e->s + e->len, s, (size_t)n);
	memset(e->q + e->len, quoted != 0, (size_t)n);
	e->len += n;
}

void
esputs(EStr *e, const char *s, int quoted)
{
	esputn(e, s, (int)strlen(s), quoted);
}

void
esbreak(EStr *e, int quoted)
{
	esputc(e, '\0', quoted);
}

/*
 * Quote removal. The bytes come out and the flags stay behind, and a
 * field break becomes nothing, because a caller that joins fields has
 * already inserted its own separator.
 */
char *
escstr(EStr *e)
{
	char *p;
	int i, j;

	p = arenaalloc(stk, (size_t)e->len + 1);
	j = 0;
	for (i = 0; i < e->len; i++) {
		if (e->s[i] == '\0')
			continue;
		p[j++] = e->s[i];
	}
	p[j] = '\0';
	return p;
}
