/*
 * msh - a POSIX shell.
 *
 * Word expansion, in the order of section 2.6: tilde expansion,
 * parameter expansion, command substitution and arithmetic expansion
 * first, all in one left-to-right pass; then field splitting; then
 * pathname expansion; and quote removal last.
 *
 * The representation of a quoted character is the design decision that
 * the rest of the module depends on. Expansion writes into an EStr,
 * which holds the bytes of the result and, beside them, one flag per
 * byte that says whether that byte came from inside quotes. A quoting
 * character itself is never stored. So quote removal is just reading
 * the bytes out, field splitting and pathname expansion skip a byte
 * whose flag is set, and nothing needs an in-band escape byte.
 *
 * A byte of 0 in the result is a hard field break, which only $@ and
 * "$@" produce. A word can hold no other 0 byte, so the marker needs
 * no escape either.
 */
#include <ctype.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "expand.h"
#include "msh.h"
#include "tree.h"
#include "vars.h"

/*
 * The state of one pass over a word. `dq' says whether the scan is
 * inside double quotes, and the other two fields answer the one
 * question that field splitting cannot answer by itself: whether a
 * word that expanded to nothing should still give one empty field.
 * `"" ' gives one, and `"$@"' with no positional parameters gives
 * none, so the pass records which of the two happened.
 */
typedef struct exp Exp;
struct exp {
	int flags;
	int dq;
	int gotquote;  /* a quoting construct justifies an empty field */
	int sawatzero; /* $@ expanded to no parameters */
};

static void expandto(const char *p, Exp *x, EStr *e);

void
flinit(FieldList *l)
{
	l->head = NULL;
	l->tail = &l->head;
	l->n = 0;
}

void
fladd(FieldList *l, char *s)
{
	Field *f;

	f = arenaalloc(stk, sizeof(*f));
	f->next = NULL;
	f->text = s;
	*l->tail = f;
	l->tail = &f->next;
	l->n++;
}

char **
flargv(FieldList *l)
{
	char **v;
	Field *f;
	int i;

	v = arenaalloc(stk, sizeof(*v) * (size_t)(l->n + 1));
	i = 0;
	for (f = l->head; f; f = f->next)
		v[i++] = f->text;
	v[i] = NULL;
	return v;
}

/* The bytes of one range, with the quoting flags dropped. */
char *
esrange(EStr *e, int from, int to)
{
	char *p;
	int i, j;

	p = arenaalloc(stk, (size_t)(to - from) + 1);
	j = 0;
	for (i = from; i < to; i++) {
		if (e->s[i] == '\0')
			continue;
		p[j++] = e->s[i];
	}
	p[j] = '\0';
	return p;
}

/*
 * Find the character that closes a nested construct. `p' is the first
 * character of the contents, and the result points at the closing
 * character. The text came from the lexer, which already balanced it,
 * so a failure here means the text did not come from the lexer.
 */
static const char *
skipsub(const char *p, int endc)
{
	int depth, openc;

	openc = endc == ')' ? '(' : -1;
	depth = 1;
	for (; *p; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			continue;
		}
		if (*p == '\'' && endc != '`') {
			for (p++; *p && *p != '\''; p++)
				;
			if (!*p)
				break;
			continue;
		}
		if (*p == '"') {
			for (p++; *p && *p != '"'; p++) {
				if (*p == '\\' && p[1])
					p++;
			}
			if (!*p)
				break;
			continue;
		}
		if (*p == '$' && (p[1] == '(' || p[1] == '{')) {
			p = skipsub(p + 2, p[1] == '(' ? ')' : '}');
			if (!*p)
				break;
			continue;
		}
		if (*p == '$' && p[1] == '\'') {
			for (p += 2; *p && *p != '\''; p++) {
				if (*p == '\\' && p[1])
					p++;
			}
			if (!*p)
				break;
			continue;
		}
		if (*p == '`' && endc != '`') {
			p = skipsub(p + 1, '`');
			if (!*p)
				break;
			continue;
		}
		if (*p == openc) {
			depth++;
			continue;
		}
		if (*p == endc && --depth == 0)
			return p;
	}
	error("unterminated expansion");
	/* NOTREACHED */
	return p;
}

static int
isnamechar(int c, int first)
{
	if (c >= 'a' && c <= 'z')
		return 1;
	if (c >= 'A' && c <= 'Z')
		return 1;
	if (c == '_')
		return 1;
	if (!first && c >= '0' && c <= '9')
		return 1;
	return 0;
}

static int
isspecialparam(int c)
{
	return c == '@' || c == '*' || c == '#' || c == '?' || c == '-' ||
	       c == '$' || c == '!';
}

static const char *
ifsvalue(void)
{
	const char *p;

	p = getvar("IFS");
	return p ? p : " \t\n";
}

static int
hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return c - 'A' + 10;
}

/* One code point as UTF-8, which is what \u and \U ask for. */
static void
pututf8(EStr *e, unsigned long n)
{
	if (n < 0x80) {
		esputc(e, (int)n, 1);
		return;
	}
	if (n < 0x800) {
		esputc(e, (int)(0xc0 | (n >> 6)), 1);
		esputc(e, (int)(0x80 | (n & 0x3f)), 1);
		return;
	}
	if (n < 0x10000) {
		esputc(e, (int)(0xe0 | (n >> 12)), 1);
		esputc(e, (int)(0x80 | ((n >> 6) & 0x3f)), 1);
		esputc(e, (int)(0x80 | (n & 0x3f)), 1);
		return;
	}
	esputc(e, (int)(0xf0 | (n >> 18)), 1);
	esputc(e, (int)(0x80 | ((n >> 12) & 0x3f)), 1);
	esputc(e, (int)(0x80 | ((n >> 6) & 0x3f)), 1);
	esputc(e, (int)(0x80 | (n & 0x3f)), 1);
}

static char *
numstr(long n)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%ld", n);
	return arenastrdup(stk, buf);
}

/*
 * The value of one parameter as a plain string. The positional
 * parameters taken together join with the first character of IFS, as
 * section 2.5.2 requires of $*.
 */
static char *
paramvalue(const char *name, int len, int *isset)
{
	SBuf b;
	const char *v, *ifs;
	int i, n;

	*isset = 1;
	if (len == 1 && (name[0] == '@' || name[0] == '*')) {
		ifs = ifsvalue();
		sbinit(&b, stk);
		for (i = 1; i <= shparam.nparam; i++) {
			if (i > 1 && *ifs)
				sbputc(&b, ifs[0]);
			sbputs(&b, shparam.p[i - 1]);
		}
		return sbdone(&b);
	}
	if (len == 1) {
		switch (name[0]) {
		case '#':
			return numstr(shparam.nparam);
		case '?':
			return numstr(exitstatus);
		case '$':
			return numstr(shlvlpid);
		case '!':
			return numstr(lastbgpid);
		case '-':
			sbinit(&b, stk);
			for (i = 0; i < NOPTS; i++) {
				if (opts[i].val && opts[i].letter)
					sbputc(&b, opts[i].letter);
			}
			return sbdone(&b);
		}
	}
	if (name[0] >= '0' && name[0] <= '9') {
		n = 0;
		for (i = 0; i < len; i++)
			n = n * 10 + (name[i] - '0');
		if (n == 0)
			return arg0;
		if (n > shparam.nparam) {
			*isset = 0;
			return (char *)"";
		}
		return shparam.p[n - 1];
	}
	v = getvar(arenastrndup(stk, name, (size_t)len));
	if (!v) {
		*isset = 0;
		return (char *)"";
	}
	return (char *)v;
}

/*
 * Append the value of one parameter. $@ and "$@" put a hard field
 * break between parameters, so each one stays a separate field
 * whatever its contents and whatever IFS holds.
 */
static void
putparam(EStr *e, const char *name, int len, Exp *x)
{
	char *v;
	int i, isset;

	if (len == 1 && name[0] == '@') {
		if (shparam.nparam == 0) {
			x->sawatzero = 1;
			return;
		}
		for (i = 1; i <= shparam.nparam; i++) {
			if (i > 1)
				esbreak(e, x->dq);
			esputs(e, shparam.p[i - 1], x->dq);
		}
		return;
	}
	v = paramvalue(name, len, &isset);
	if (!isset && uflag)
		error("%.*s: parameter not set", len, name);
	esputs(e, v, x->dq);
}

static int
paramisset(const char *name, int len, int colon)
{
	char *v;
	int isset;

	if (len == 1 && (name[0] == '@' || name[0] == '*'))
		return shparam.nparam > 0;
	v = paramvalue(name, len, &isset);
	if (!isset)
		return 0;
	return colon ? *v != '\0' : 1;
}

/*
 * Expand a piece of a word that is itself a word: the `word' part of
 * `${x:-word}' and its relatives. The quoting state carries into it.
 */
static void
expandpiece(const char *s, int len, Exp *x, EStr *e)
{
	Exp y;

	y.flags = (x->flags & ~(EXTILDE | EXASSIGN)) | EXBRACE;
	y.dq = x->dq;
	y.gotquote = 0;
	y.sawatzero = 0;
	expandto(arenastrndup(stk, s, (size_t)len), &y, e);
	if (y.gotquote)
		x->gotquote = 1;
	if (y.sawatzero)
		x->sawatzero = 1;
}

static char *
expandbytes(const char *s, int len, int dq)
{
	EStr e;
	Exp y;

	esinit(&e);
	y.flags = EXBRACE;
	y.dq = dq;
	y.gotquote = 0;
	y.sawatzero = 0;
	expandto(arenastrndup(stk, s, (size_t)len), &y, &e);
	return esrange(&e, 0, e.len);
}

/*
 * Remove the shortest or the longest prefix or suffix that matches a
 * pattern (section 2.6.2). The search tries every candidate length,
 * which is simple and is fast enough, because a word is short.
 */
static char *
trimparam(const char *val, EStr *pat, int fromend, int longest)
{
	int n, i;

	n = (int)strlen(val);
	if (fromend) {
		if (longest) {
			for (i = 0; i <= n; i++) {
				if (patmatchlen(pat, 0, pat->len, val + i,
				                n - i))
					return arenastrndup(stk, val,
					                    (size_t)i);
			}
		} else {
			for (i = n; i >= 0; i--) {
				if (patmatchlen(pat, 0, pat->len, val + i,
				                n - i))
					return arenastrndup(stk, val,
					                    (size_t)i);
			}
		}
	} else if (longest) {
		for (i = n; i >= 0; i--) {
			if (patmatchlen(pat, 0, pat->len, val, i))
				return arenastrdup(stk, val + i);
		}
	} else {
		for (i = 0; i <= n; i++) {
			if (patmatchlen(pat, 0, pat->len, val, i))
				return arenastrdup(stk, val + i);
		}
	}
	return arenastrdup(stk, val);
}

/*
 * One `${...}' construct. `p' points at the first character of the
 * contents and `end' at the closing brace.
 */
static void
bracedparam(const char *p, const char *end, Exp *x, EStr *e)
{
	EStr pat;
	const char *name, *word, *op;
	char *v;
	int namelen, colon, isset, longest;

	if (*p == '#' && p + 1 != end) {
		/*
		 * ${#parameter}: the length of the value. Section 2.6.2
		 * leaves ${#@} and ${#*} unspecified, and this shell
		 * gives the length of the joined value for both.
		 */
		name = p + 1;
		namelen = (int)(end - name);
		v = paramvalue(name, namelen, &isset);
		if (!isset && uflag)
			error("%.*s: parameter not set", namelen, name);
		esputs(e, numstr((long)strlen(v)), x->dq);
		return;
	}
	name = p;
	if (*p >= '0' && *p <= '9') {
		while (p < end && *p >= '0' && *p <= '9')
			p++;
	} else if (isspecialparam(*p)) {
		p++;
	} else if (isnamechar(*p, 1)) {
		while (p < end && isnamechar(*p, 0))
			p++;
	} else {
		error("${%.*s}: bad substitution", (int)(end - p), p);
	}
	namelen = (int)(p - name);
	if (p == end) {
		putparam(e, name, namelen, x);
		return;
	}
	op = p;
	colon = 0;
	if (*p == ':' && p + 1 < end &&
	    (p[1] == '-' || p[1] == '=' || p[1] == '?' || p[1] == '+')) {
		colon = 1;
		p++;
		op = p;
	}
	word = p + 1;
	switch (*op) {
	case '-':
		if (paramisset(name, namelen, colon))
			putparam(e, name, namelen, x);
		else
			expandpiece(word, (int)(end - word), x, e);
		return;
	case '+':
		if (paramisset(name, namelen, colon))
			expandpiece(word, (int)(end - word), x, e);
		return;
	case '=':
		if (paramisset(name, namelen, colon)) {
			putparam(e, name, namelen, x);
			return;
		}
		if (!isnamechar(name[0], 1))
			error("cannot assign to %.*s", namelen, name);
		v = expandbytes(word, (int)(end - word), x->dq);
		setvar(arenastrndup(stk, name, (size_t)namelen), v, 0);
		esputs(e, v, x->dq);
		return;
	case '?':
		if (paramisset(name, namelen, colon)) {
			putparam(e, name, namelen, x);
			return;
		}
		if (end > word)
			error("%.*s: %s", namelen, name,
			      expandbytes(word, (int)(end - word), x->dq));
		error("%.*s: parameter not set", namelen, name);
		/* NOTREACHED */
		return;
	case '#': /* FALLTHROUGH */
	case '%':
		longest = op[1] == *op;
		word = op + (longest ? 2 : 1);
		esinit(&pat);
		{
			Exp y;

			y.flags = 0;
			y.dq = 0;
			y.gotquote = 0;
			y.sawatzero = 0;
			expandto(arenastrndup(stk, word,
			                      (size_t)(end - word)),
			         &y, &pat);
		}
		v = paramvalue(name, namelen, &isset);
		if (!isset && uflag)
			error("%.*s: parameter not set", namelen, name);
		esputs(e, trimparam(v, &pat, *op == '%', longest), x->dq);
		return;
	}
	error("${%.*s}: bad substitution", (int)(end - name), name);
	/* NOTREACHED */
}

/*
 * One escape of a `$'...'' string. `p' points at the backslash, and
 * the result points at the last character the escape used. The set is
 * the one that Issue 8 gives for this quoting form.
 */
static const char *
dollarescape(const char *p, EStr *e)
{
	static const char named[] = "abefnrtv\\'\"?";
	static const char value[] = "\a\b\033\f\n\r\t\v\\'\"?";
	const char *q;
	unsigned long n;
	int i;

	q = strchr(named, p[1]);
	if (q && p[1]) {
		esputc(e, value[q - named], 1);
		return p + 1;
	}
	if (p[1] == 'E') {
		esputc(e, '\033', 1);
		return p + 1;
	}
	if (p[1] >= '0' && p[1] <= '7') {
		n = 0;
		for (i = 1; i <= 3 && p[i] >= '0' && p[i] <= '7'; i++)
			n = n * 8 + (unsigned long)(p[i] - '0');
		esputc(e, (int)(n & 0xff), 1);
		return p + i - 1;
	}
	if (p[1] == 'x' && isxdigit((unsigned char)p[2])) {
		n = 0;
		for (i = 2; i <= 3 && isxdigit((unsigned char)p[i]); i++)
			n = n * 16 + (unsigned long)hexval(p[i]);
		esputc(e, (int)(n & 0xff), 1);
		return p + i - 1;
	}
	if ((p[1] == 'u' || p[1] == 'U') && isxdigit((unsigned char)p[2])) {
		int max;

		max = p[1] == 'u' ? 4 : 8;
		n = 0;
		for (i = 2; i < 2 + max && isxdigit((unsigned char)p[i]); i++)
			n = n * 16 + (unsigned long)hexval(p[i]);
		pututf8(e, n);
		return p + i - 1;
	}
	if (p[1] == 'c' && p[2]) {
		esputc(e, (unsigned char)p[2] & 0x1f, 1);
		return p + 2;
	}
	esputc(e, '\\', 1);
	return p;
}

/*
 * A `$'...'' string: the escapes are processed, and every byte of the
 * result is quoted, because the whole construct is a quoting form.
 */
static const char *
dollarquote(const char *p, EStr *e, Exp *x)
{
	x->gotquote = 1;
	for (; *p && *p != '\''; p++) {
		if (*p == '\\' && p[1]) {
			p = dollarescape(p, e);
			continue;
		}
		esputc(e, *p, 1);
	}
	return *p ? p : p - 1;
}

/*
 * Everything that a `$' can start. `p' points at the `$', and the
 * result points at the last character that the construct used.
 */
static const char *
dollar(const char *p, Exp *x, EStr *e)
{
	const char *q, *end;
	char *sub;

	if (p[1] == '{') {
		end = skipsub(p + 2, '}');
		bracedparam(p + 2, end, x, e);
		return end;
	}
	/*
	 * `$'...'' is a quoting construct, so it is recognised only
	 * where quoting can begin: inside double quotes the `$' is an
	 * ordinary character.
	 */
	if (p[1] == '\'' && !x->dq)
		return dollarquote(p + 2, e, x);
	if (p[1] == '(') {
		if (p[2] == '(') {
			q = skipsub(p + 3, ')');
			if (q[1] == ')') {
				sub = expandbytes(p + 3, (int)(q - p - 3), 0);
				esputs(e, numstr(arith(sub)), x->dq);
				return q + 1;
			}
		}
		q = skipsub(p + 2, ')');
		sub = cmdsubst(p + 2, (int)(q - p - 2), 0);
		esputs(e, sub, x->dq);
		return q;
	}
	if (isnamechar(p[1], 1)) {
		for (q = p + 1; isnamechar(*q, 0); q++)
			;
		putparam(e, p + 1, (int)(q - p - 1), x);
		return q - 1;
	}
	if ((p[1] >= '0' && p[1] <= '9') || isspecialparam(p[1])) {
		putparam(e, p + 1, 1, x);
		return p + 1;
	}
	esputc(e, '$', x->dq);
	return p;
}

/*
 * Tilde expansion. The result is the last character consumed, or NULL
 * when the construct is not a tilde prefix after all, in which case
 * the `~' is an ordinary character.
 */
static const char *
tilde(const char *p, EStr *e)
{
	struct passwd *pw;
	const char *q, *home;
	char *name;

	for (q = p + 1; *q && *q != '/' && *q != ':'; q++) {
		if (*q == '\'' || *q == '"' || *q == '\\' || *q == '$' ||
		    *q == '`')
			return NULL; /* quoting defeats tilde expansion */
	}
	if (q == p + 1) {
		home = getvar("HOME");
		if (!home)
			return NULL;
		esputs(e, home, 1);
		return q - 1;
	}
	name = arenastrndup(stk, p + 1, (size_t)(q - p - 1));
	pw = getpwnam(name);
	if (!pw)
		return NULL;
	esputs(e, pw->pw_dir, 1);
	return q - 1;
}

/*
 * The one left-to-right pass over a word. Double quotes toggle `dq'
 * rather than opening a nested loop, so every construct is handled in
 * one place and the quoted and unquoted cases cannot drift apart.
 */
static void
expandto(const char *p, Exp *x, EStr *e)
{
	const char *q;
	char *sub;
	int tildeok, dqstart;

	tildeok = (x->flags & EXTILDE) && !x->dq;
	dqstart = 0;
	for (; *p; p++) {
		if (tildeok && *p == '~' && !x->dq) {
			q = tilde(p, e);
			if (q) {
				p = q;
				tildeok = 0;
				continue;
			}
		}
		tildeok = 0;
		if (*p == '"') {
			if (!x->dq) {
				x->dq = 1;
				dqstart = e->len;
				x->sawatzero = 0;
			} else {
				x->dq = 0;
				if (e->len > dqstart || !x->sawatzero)
					x->gotquote = 1;
			}
			continue;
		}
		if (*p == '\'' && !x->dq && !(x->flags & EXHERE)) {
			for (p++; *p && *p != '\''; p++)
				esputc(e, *p, 1);
			x->gotquote = 1;
			if (!*p)
				break;
			continue;
		}
		if (*p == '\\' && p[1]) {
			if (x->dq || (x->flags & EXHERE)) {
				/*
				 * Section 2.6.2 adds `}' to the characters
				 * that a backslash escapes, but only inside
				 * the word of a `${...}' form, because there
				 * it would otherwise end the expansion.
				 */
				if (p[1] == '$' || p[1] == '`' ||
				    p[1] == '\\' ||
				    (p[1] == '}' && (x->flags & EXBRACE)) ||
				    (p[1] == '"' && !(x->flags & EXHERE))) {
					p++;
					esputc(e, *p, 1);
					continue;
				}
				esputc(e, '\\', 1);
				continue;
			}
			p++;
			esputc(e, *p, 1);
			x->gotquote = 1;
			continue;
		}
		if (*p == '`') {
			q = skipsub(p + 1, '`');
			sub = cmdsubst(p + 1, (int)(q - p - 1), 1);
			esputs(e, sub, x->dq);
			p = q;
			continue;
		}
		if (*p == '$' && p[1]) {
			p = dollar(p, x, e);
			if ((x->flags & EXASSIGN) && !x->dq && p[0] == ':')
				tildeok = 1;
			continue;
		}
		esputc(e, *p, x->dq || (x->flags & EXHERE) != 0);
		if ((x->flags & EXASSIGN) && !x->dq &&
		    (*p == '=' || *p == ':'))
			tildeok = 1;
	}
}

static int
inifs(int c, const char *ifs)
{
	return c != '\0' && strchr(ifs, c) != NULL;
}

static int
isifsws(int c, const char *ifs)
{
	return (c == ' ' || c == '\t' || c == '\n') && inifs(c, ifs);
}

static void
emitfield(EStr *e, int from, int to, int doglob, FieldList *l)
{
	if (doglob)
		globfields(e, from, to, l);
	else
		fladd(l, esrange(e, from, to));
}

/*
 * Field splitting of one segment (section 2.6.5). A run of IFS white
 * space delimits one field and is ignored at both ends of the segment;
 * one IFS character that is not white space delimits one field by
 * itself, and may carry IFS white space on either side.
 *
 * The result says how many fields the segment gave, because an empty
 * segment of a quoted $@ still owes one empty field and the caller is
 * the one that knows.
 */
static int
splitsegment(EStr *e, int from, int to, const char *ifs, int doglob,
             FieldList *l)
{
	int i, start, sawdelim, n;

	n = 0;
	if (*ifs == '\0') {
		if (to > from) {
			emitfield(e, from, to, doglob, l);
			n++;
		}
		return n;
	}
	i = from;
	while (i < to && !e->q[i] && isifsws(e->s[i], ifs))
		i++;
	start = i;
	for (;;) {
		if (i >= to) {
			if (i > start) {
				emitfield(e, start, i, doglob, l);
				n++;
			}
			break;
		}
		if (!e->q[i] && inifs(e->s[i], ifs)) {
			emitfield(e, start, i, doglob, l);
			n++;
			sawdelim = 0;
			while (i < to && !e->q[i] && inifs(e->s[i], ifs)) {
				if (!isifsws(e->s[i], ifs)) {
					if (sawdelim)
						break;
					sawdelim = 1;
				}
				i++;
			}
			start = i;
			if (i >= to)
				break;
			continue;
		}
		i++;
	}
	return n;
}

/*
 * Field splitting of a whole word. A break marker ends one parameter
 * of $@ and starts the next, so each segment between markers splits on
 * its own; without that, two parameters could run into one field.
 */
static void
splitfields(EStr *e, int doglob, FieldList *l)
{
	const char *ifs;
	int i, from, quoted;

	ifs = ifsvalue();
	i = 0;
	for (;;) {
		from = i;
		while (i < e->len && e->s[i] != '\0')
			i++;
		quoted = 0;
		if (from > 0 && e->s[from - 1] == '\0')
			quoted = e->q[from - 1];
		else if (i < e->len)
			quoted = e->q[i];
		if (splitsegment(e, from, i, ifs, doglob, l) == 0 && quoted)
			fladd(l, arenastrdup(stk, ""));
		if (i >= e->len)
			break;
		i++;
	}
}

static void
initexp(Exp *x, int flags)
{
	x->flags = flags;
	x->dq = 0;
	x->gotquote = 0;
	x->sawatzero = 0;
}

void
expandword(const char *word, int flags, FieldList *l)
{
	EStr e;
	Exp x;

	esinit(&e);
	initexp(&x, flags);
	expandto(word, &x, &e);
	if (e.len == 0) {
		/*
		 * Nothing came out. A quoting construct still justifies
		 * one empty field, as `""' must give; without one the
		 * word gives no field at all, as `$unset' must.
		 */
		if (x.gotquote)
			fladd(l, arenastrdup(stk, ""));
		return;
	}
	if (flags & EXSPLIT) {
		splitfields(&e, (flags & EXGLOB) && !fflag, l);
		return;
	}
	emitfield(&e, 0, e.len, (flags & EXGLOB) && !fflag, l);
}

/*
 * One word that must give exactly one field: a redirection file name,
 * the word of a `case', the value of an assignment. Section 2.7 and
 * section 2.9.1 leave out field splitting and pathname expansion
 * there, so a field break becomes an ordinary space.
 */
char *
expandone(const char *word, int flags)
{
	EStr e;
	Exp x;
	int i;

	esinit(&e);
	initexp(&x, flags & ~(EXSPLIT | EXGLOB));
	expandto(word, &x, &e);
	for (i = 0; i < e.len; i++) {
		if (e.s[i] == '\0')
			e.s[i] = ' ';
	}
	return esrange(&e, 0, e.len);
}

char *
expandassignvalue(const char *word)
{
	return expandone(word, EXTILDE | EXASSIGN);
}

void
expandpattern(const char *word, EStr *e)
{
	Exp x;

	initexp(&x, 0);
	expandto(word, &x, e);
}

/*
 * A here-document body. When the delimiter was quoted the body is
 * literal. Otherwise parameter expansion, command substitution and
 * arithmetic expansion apply, a backslash escapes `$', a backquote, a
 * backslash and a newline, and nothing else is syntax -- a single or
 * double quote in the body stands for itself.
 */
char *
expandhere(const char *body, int quoted)
{
	EStr e;
	Exp x;

	if (quoted)
		return (char *)body;
	esinit(&e);
	initexp(&x, EXHERE);
	expandto(body, &x, &e);
	return esrange(&e, 0, e.len);
}

char *
expandprompt(const char *s)
{
	struct jmploc h;
	char *p;

	/*
	 * An error while expanding the prompt must not abandon the
	 * command that the user is about to type, so catch it here.
	 */
	if (setjmphandler(&h) == 0)
		p = expandone(s, 0);
	else
		p = (char *)"$ ";
	popjmphandler(&h);
	return p;
}
