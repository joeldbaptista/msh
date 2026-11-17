/*
 * msh - a POSIX shell.
 *
 * Pattern matching (section 2.13) and pathname expansion.
 *
 * The pattern is an EStr, so the matcher can tell a `*' that came from
 * the source text from a `*' that quoting made literal: the quoting
 * flag beside the byte says which. That is the whole reason the
 * matcher takes an EStr instead of a string.
 *
 * The matcher backtracks on `*'. A pattern is short, so the simple
 * algorithm is the right one.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "expand.h"
#include "msh.h"
#include "tree.h"

#define GLOBMAX 4096

/*
 * The character classes of a bracket expression. Each one is a name
 * and the function that tests it, so the matcher needs no switch.
 */
static const struct {
	const char *name;
	int (*fn)(int);
} classes[] = {
    {"alnum", isalnum},
    {"alpha", isalpha},
    {"blank", isblank},
    {"cntrl", iscntrl},
    {"digit", isdigit},
    {"graph", isgraph},
    {"lower", islower},
    {"print", isprint},
    {"punct", ispunct},
    {"space", isspace},
    {"upper", isupper},
    {"xdigit", isxdigit},
};

static int domatch(EStr *p, int pi, int pe, const char *s, int sl, int si);

/*
 * The end of a `[: ... :]', `[. ... .]' or `[= ... =]' item, or -1
 * when the item is not closed.
 */
static int
skipclass(EStr *p, int i, int pe)
{
	int k, delim;

	delim = p->s[i + 1];
	for (k = i + 2; k + 1 < pe; k++) {
		if (p->s[k] == delim && p->s[k + 1] == ']')
			return k + 2;
	}
	return -1;
}

static int
inclass(EStr *p, int i, int end, int c)
{
	size_t k;
	int len;

	len = end - 2 - (i + 2);
	for (k = 0; k < LEN(classes); k++) {
		if ((int)strlen(classes[k].name) != len)
			continue;
		if (strncmp(classes[k].name, p->s + i + 2, (size_t)len) != 0)
			continue;
		return classes[k].fn(c) != 0;
	}
	return 0;
}

/*
 * One bracket expression. The result is 1 on a match, 0 on no match,
 * and -1 when the text is not a bracket expression at all, in which
 * case the `[' stands for itself.
 */
static int
bracket(EStr *p, int pi, int pe, int c, int *next)
{
	int i, j, neg, found, lo, hi, end;

	i = pi + 1;
	neg = 0;
	if (i < pe && !p->q[i] && (p->s[i] == '!' || p->s[i] == '^')) {
		neg = 1;
		i++;
	}
	/* Find the closing bracket first, so an unclosed `[' stays literal. */
	j = i;
	if (j < pe && p->s[j] == ']')
		j++; /* a `]' in the first position is literal */
	while (j < pe && !(p->s[j] == ']' && !p->q[j])) {
		if (p->s[j] == '[' && j + 1 < pe &&
		    (p->s[j + 1] == ':' || p->s[j + 1] == '.' ||
		     p->s[j + 1] == '=')) {
			end = skipclass(p, j, pe);
			if (end < 0)
				return -1;
			j = end;
			continue;
		}
		j++;
	}
	if (j >= pe)
		return -1;
	*next = j + 1;
	found = 0;
	if (i < pe && p->s[i] == ']' && i < j) {
		if (c == ']')
			found = 1;
		i++;
	}
	for (; i < j; i++) {
		if (p->s[i] == '[' && i + 1 < j &&
		    (p->s[i + 1] == ':' || p->s[i + 1] == '.' ||
		     p->s[i + 1] == '=')) {
			end = skipclass(p, i, pe);
			if (p->s[i + 1] == ':') {
				if (inclass(p, i, end, c))
					found = 1;
			} else if (end - 2 - (i + 2) == 1 &&
			           p->s[i + 2] == c) {
				found = 1; /* [.x.] and [=x=] of one byte */
			}
			i = end - 1;
			continue;
		}
		lo = (unsigned char)p->s[i];
		if (i + 2 < j && p->s[i + 1] == '-' && !p->q[i + 1]) {
			hi = (unsigned char)p->s[i + 2];
			if (c >= lo && c <= hi)
				found = 1;
			i += 2;
			continue;
		}
		if (c == lo)
			found = 1;
	}
	return neg ? !found : found;
}

static int
domatch(EStr *p, int pi, int pe, const char *s, int sl, int si)
{
	int next, r;

	while (pi < pe) {
		if (!p->q[pi] && p->s[pi] == '*') {
			while (pi < pe && !p->q[pi] && p->s[pi] == '*')
				pi++;
			if (pi == pe)
				return 1;
			for (; si <= sl; si++) {
				if (domatch(p, pi, pe, s, sl, si))
					return 1;
			}
			return 0;
		}
		if (si >= sl)
			return 0;
		if (!p->q[pi] && p->s[pi] == '?') {
			pi++;
			si++;
			continue;
		}
		if (!p->q[pi] && p->s[pi] == '[') {
			r = bracket(p, pi, pe, (unsigned char)s[si], &next);
			if (r >= 0) {
				if (!r)
					return 0;
				pi = next;
				si++;
				continue;
			}
		}
		if (p->s[pi] != s[si])
			return 0;
		pi++;
		si++;
	}
	return si == sl;
}

int
patmatchlen(EStr *pat, int from, int to, const char *s, int slen)
{
	return domatch(pat, from, to, s, slen, 0);
}

int
patmatchrange(EStr *pat, int from, int to, const char *str)
{
	return domatch(pat, from, to, str, (int)strlen(str), 0);
}

int
patmatch(EStr *pat, const char *str)
{
	return patmatchrange(pat, 0, pat->len, str);
}

static int
hasmagic(EStr *e, int from, int to)
{
	int i;

	for (i = from; i < to; i++) {
		if (e->q[i])
			continue;
		if (e->s[i] == '*' || e->s[i] == '?' || e->s[i] == '[')
			return 1;
	}
	return 0;
}

/*
 * One directory level of pathname expansion. `path' holds the prefix
 * that the walk has fixed so far, and the walk is depth first, so one
 * buffer serves every level.
 */
static void
globwalk(EStr *e, int pi, int pe, char *path, int plen, FieldList *out)
{
	DIR *d;
	struct dirent *de;
	struct stat st;
	int cend, i, dot, n, len;

	while (pi < pe && e->s[pi] == '/') {
		if (plen + 1 >= GLOBMAX)
			return;
		path[plen++] = '/';
		pi++;
	}
	path[plen] = '\0';
	if (pi >= pe) {
		if (plen > 0 && stat(path, &st) == 0)
			fladd(out, arenastrdup(stk, path));
		return;
	}
	cend = pi;
	while (cend < pe && e->s[cend] != '/')
		cend++;
	if (!hasmagic(e, pi, cend)) {
		for (i = pi; i < cend; i++) {
			if (plen + 1 >= GLOBMAX)
				return;
			if (e->s[i] != '\0')
				path[plen++] = e->s[i];
		}
		path[plen] = '\0';
		if (cend >= pe) {
			if (stat(path, &st) == 0)
				fladd(out, arenastrdup(stk, path));
			return;
		}
		globwalk(e, cend, pe, path, plen, out);
		return;
	}
	d = opendir(plen ? path : ".");
	if (!d)
		return;
	/*
	 * Section 2.13.3: a leading period matches only a period that
	 * the pattern gives explicitly.
	 */
	dot = e->s[pi] == '.';
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.' && !dot)
			continue;
		if (!patmatchrange(e, pi, cend, de->d_name))
			continue;
		len = (int)strlen(de->d_name);
		if (plen + len + 1 >= GLOBMAX)
			continue;
		memcpy(path + plen, de->d_name, (size_t)len);
		n = plen + len;
		path[n] = '\0';
		if (cend >= pe)
			fladd(out, arenastrdup(stk, path));
		else
			globwalk(e, cend, pe, path, n, out);
	}
	closedir(d);
	path[plen] = '\0';
}

static int
fieldcmp(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/*
 * Pathname expansion of one field. When the pattern matches nothing,
 * section 2.6.6 leaves the field unchanged.
 */
void
globfields(EStr *e, int from, int to, FieldList *l)
{
	FieldList tmp;
	Field *f;
	char **v;
	char *path;
	int i;

	if (!hasmagic(e, from, to)) {
		fladd(l, esrange(e, from, to));
		return;
	}
	flinit(&tmp);
	path = arenaalloc(stk, GLOBMAX);
	globwalk(e, from, to, path, 0, &tmp);
	if (tmp.n == 0) {
		fladd(l, esrange(e, from, to));
		return;
	}
	v = arenaalloc(stk, sizeof(*v) * (size_t)tmp.n);
	i = 0;
	for (f = tmp.head; f; f = f->next)
		v[i++] = f->text;
	qsort(v, (size_t)tmp.n, sizeof(*v), fieldcmp);
	for (i = 0; i < tmp.n; i++)
		fladd(l, v[i]);
}
