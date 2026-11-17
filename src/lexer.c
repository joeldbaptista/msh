/*
 * msh - a POSIX shell.
 *
 * The lexer. It turns characters into the tokens of section 2.3, and
 * it decides nothing else: whether a word is a reserved word, a name
 * or an assignment is a question that section 2.10 makes depend on the
 * parser state, so the parser answers it.
 *
 * A word token keeps its quoting characters, unmodified (rule 4).
 * Expansion and quote removal happen in Phase 5, over that same text,
 * so the lexer only has to find where each word ends. Finding the end
 * of `$(...)', `${...}', `$((...))' and a backquote needs a scan that
 * follows nesting and quoting (rule 5), and scansub() does that.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "builtin.h"
#include "msh.h"
#include "tree.h"

#define MAXHEREDOC 32
#define MAXALIAS 16

/*
 * The operators of section 2.10.2, longest first within one initial
 * character, because rule 2 of section 2.3 extends an operator for as
 * long as the next character can belong to one.
 */
static const struct {
	const char *s;
	int tok;
} ops[] = {
    {"<<-", TDLESSDASH},
    {"<<", TDLESS},
    {"<>", TLESSGREAT},
    {"<&", TLESSAND},
    {"<", TLESS},
    {">>", TDGREAT},
    {">&", TGREATAND},
    {">|", TCLOBBER},
    {">", TGREAT},
    {"&&", TANDAND},
    {"&", TAND},
    {"||", TOROR},
    {"|", TPIPE},
    {";;", TDSEMI},
    {";", TSEMI},
    {"(", TLPAREN},
    {")", TRPAREN},
};

const char *const tokname[NTOKENS] = {
    "end of file", "newline", "word", "io number", "\";\"", "\"&\"",
    "\"|\"", "\"&&\"", "\"||\"", "\";;\"", "\"(\"", "\")\"", "\"<\"",
    "\">\"", "\"<<\"", "\">>\"", "\"<&\"", "\">&\"", "\"<>\"", "\"<<-\"",
    "\">|\""};

static const struct {
	const char *s;
	int rw;
} reswords[] = {
    {"if", RWIF},
    {"then", RWTHEN},
    {"else", RWELSE},
    {"elif", RWELIF},
    {"fi", RWFI},
    {"do", RWDO},
    {"done", RWDONE},
    {"case", RWCASE},
    {"esac", RWESAC},
    {"while", RWWHILE},
    {"until", RWUNTIL},
    {"for", RWFOR},
    {"{", RWLBRACE},
    {"}", RWRBRACE},
    {"!", RWBANG},
    {"in", RWIN},
};

static Arena *la;
static Token tok;
static int pushedback;
static NRedir *hdocq[MAXHEREDOC];
static int nhdocq;
static Alias *alstack[MAXALIAS];
static int alblank[MAXALIAS]; /* that alias value ended in a blank */
static int nalias;
static int aliasblank; /* the alias just finished ended in a blank */

static void readheredocs(void);
static int tracetoken(int type);

int
reswordof(const char *s)
{
	size_t i;

	for (i = 0; i < LEN(reswords); i++) {
		if (strcmp(s, reswords[i].s) == 0)
			return reswords[i].rw;
	}
	return RWNONE;
}

void
lexinit(Arena *a)
{
	la = a;
	pushedback = 0;
	nhdocq = 0;
}

void
lexreset(void)
{
	pushedback = 0;
	nhdocq = 0;
	while (nalias > 0) {
		alstack[--nalias]->inuse = 0;
		popinput();
	}
	aliasblank = 0;
}

Token *
lasttoken(void)
{
	return &tok;
}

void
pushtoken(void)
{
	pushedback = 1;
}

int
lexheredocspending(void)
{
	return nhdocq;
}

void
heredocexpect(NRedir *r)
{
	if (nhdocq >= MAXHEREDOC)
		synerror("too many here-documents");
	hdocq[nhdocq++] = r;
}

static int
isblank_(int c)
{
	return c == ' ' || c == '\t';
}

static int
isopchar(int c)
{
	return c == '|' || c == '&' || c == ';' || c == '<' || c == '>' ||
	       c == '(' || c == ')';
}

/*
 * Quote removal over a token that the lexer kept verbatim. The
 * delimiter of a here-document and the name in a function definition
 * both need it, and neither may expand.
 */
char *
rmquotes(const char *s, Arena *a, int *quoted)
{
	SBuf b;
	int q;

	sbinit(&b, a);
	q = 0;
	while (*s) {
		if (*s == '\'') {
			q = 1;
			for (s++; *s && *s != '\''; s++)
				sbputc(&b, *s);
			if (*s)
				s++;
		} else if (*s == '"') {
			q = 1;
			for (s++; *s && *s != '"'; s++) {
				if (*s == '\\' && s[1])
					s++;
				sbputc(&b, *s);
			}
			if (*s)
				s++;
		} else if (*s == '\\' && s[1]) {
			q = 1;
			sbputc(&b, s[1]);
			s += 2;
		} else {
			sbputc(&b, *s++);
		}
	}
	if (quoted)
		*quoted = q;
	return sbdone(&b);
}

/*
 * Scan one nested construct to its end and copy it verbatim, opening
 * character included. `endc' is the character that closes it, and the
 * scan follows quoting and further nesting inside.
 */
static void
scansub(SBuf *b, int endc)
{
	int c, depth, startc;

	/*
	 * Only `(' nests by itself. A bare `{' is not special inside
	 * `${...}', and the `${' case below handles a real nesting.
	 */
	startc = endc == ')' ? '(' : -1;
	depth = 1;
	for (;;) {
		c = pgetc();
		if (c < 0)
			synerror("unexpected end of file");
		if (c == '\\') {
			sbputc(b, c);
			c = pgetc();
			if (c < 0)
				synerror("unexpected end of file");
			sbputc(b, c);
			continue;
		}
		if (c == '\'' && endc != '`') {
			sbputc(b, c);
			while ((c = pgetc()) >= 0 && c != '\'')
				sbputc(b, c);
			if (c < 0)
				synerror("unterminated quoted string");
			sbputc(b, c);
			continue;
		}
		if (c == '"') {
			sbputc(b, c);
			for (;;) {
				c = pgetc();
				if (c < 0)
					synerror("unterminated quoted string");
				if (c == '\\') {
					sbputc(b, c);
					c = pgetc();
					if (c < 0)
						synerror("unexpected end of "
						         "file");
					sbputc(b, c);
					continue;
				}
				if (c == '"')
					break;
				if (c == '`') {
					sbputc(b, c);
					scansub(b, '`');
					continue;
				}
				if (c == '$') {
					sbputc(b, c);
					c = pgetc();
					if (c == '(') {
						sbputc(b, c);
						scansub(b, ')');
					} else if (c == '{') {
						sbputc(b, c);
						scansub(b, '}');
					} else {
						pungetc(c);
					}
					continue;
				}
				sbputc(b, c);
			}
			sbputc(b, c);
			continue;
		}
		if (c == '`' && endc != '`') {
			sbputc(b, c);
			scansub(b, '`');
			continue;
		}
		if (c == '$') {
			sbputc(b, c);
			c = pgetc();
			if (c == '(') {
				sbputc(b, c);
				scansub(b, ')');
			} else if (c == '{') {
				sbputc(b, c);
				scansub(b, '}');
			} else {
				pungetc(c);
			}
			continue;
		}
		if (c == startc)
			depth++;
		if (c == endc && --depth == 0) {
			sbputc(b, c);
			return;
		}
		sbputc(b, c);
	}
}

/*
 * One word. Quoting characters stay in the text, so the expander can
 * tell a quoted character from a bare one.
 */
static void
readword(SBuf *b, int *quoted)
{
	int c;

	*quoted = 0;
	for (;;) {
		c = pgetc();
		if (c < 0)
			break;
		if (c == '\\') {
			c = pgetc();
			if (c < 0) {
				sbputc(b, '\\');
				break;
			}
			if (c == '\n')
				continue; /* line continuation, 2.2.1 */
			*quoted = 1;
			sbputc(b, '\\');
			sbputc(b, c);
			continue;
		}
		if (c == '\'') {
			*quoted = 1;
			sbputc(b, c);
			/* no line continuation inside single quotes */
			while ((c = pgetc()) >= 0 && c != '\'')
				sbputc(b, c);
			if (c < 0)
				synerror("unterminated quoted string");
			sbputc(b, c);
			continue;
		}
		if (c == '"') {
			*quoted = 1;
			sbputc(b, c);
			for (;;) {
				c = pgetc();
				if (c < 0)
					synerror("unterminated quoted "
					         "string");
				if (c == '\\') {
					c = pgetc();
					if (c < 0)
						synerror("unterminated quoted "
						         "string");
					if (c == '\n')
						continue;
					sbputc(b, '\\');
					sbputc(b, c);
					continue;
				}
				if (c == '"')
					break;
				if (c == '`') {
					sbputc(b, c);
					scansub(b, '`');
					continue;
				}
				if (c == '$') {
					sbputc(b, c);
					c = pgetc();
					if (c == '(') {
						sbputc(b, c);
						scansub(b, ')');
					} else if (c == '{') {
						sbputc(b, c);
						scansub(b, '}');
					} else {
						pungetc(c);
					}
					continue;
				}
				sbputc(b, c);
			}
			sbputc(b, c);
			continue;
		}
		if (c == '`') {
			sbputc(b, c);
			scansub(b, '`');
			continue;
		}
		if (c == '$') {
			sbputc(b, c);
			c = pgetc();
			if (c == '(') {
				sbputc(b, c);
				scansub(b, ')');
			} else if (c == '{') {
				sbputc(b, c);
				scansub(b, '}');
			} else if (c == '\'') {
				/*
				 * $'...' of Issue 8. A backslash escapes
				 * the quote inside it, so the scan cannot
				 * stop at the first one.
				 */
				*quoted = 1;
				sbputc(b, c);
				for (;;) {
					c = pgetc();
					if (c < 0)
						synerror("unterminated "
						         "quoted string");
					sbputc(b, c);
					if (c == '\\') {
						c = pgetc();
						if (c < 0)
							synerror(
							    "unterminated "
							    "quoted string");
						sbputc(b, c);
						continue;
					}
					if (c == '\'')
						break;
				}
			} else {
				pungetc(c);
			}
			continue;
		}
		if (isblank_(c) || c == '\n' || isopchar(c)) {
			pungetc(c);
			break;
		}
		sbputc(b, c);
	}
}

static int
readoperator(int c)
{
	char buf[4];
	int n, i, found;

	buf[0] = (char)c;
	n = 1;
	while (n < 3) {
		c = pgetc();
		if (c < 0)
			break;
		buf[n] = (char)c;
		found = 0;
		for (i = 0; (size_t)i < LEN(ops); i++) {
			if (strncmp(ops[i].s, buf, (size_t)n + 1) == 0) {
				found = 1;
				break;
			}
		}
		if (!found) {
			pungetc(c);
			break;
		}
		n++;
	}
	buf[n] = '\0';
	for (i = 0; (size_t)i < LEN(ops); i++) {
		if (strcmp(ops[i].s, buf) == 0)
			return ops[i].tok;
	}
	/*
	 * Unreachable: every prefix that the loop above accepted is
	 * itself an operator, so the exact match always succeeds.
	 */
	synerror("unknown operator");
	/* NOTREACHED */
	return TEOF;
}

/*
 * Alias substitution (section 2.3.1). The value becomes input, and the
 * alias is marked while its value is being read, so an alias that
 * names itself expands once. A value that ends in a blank makes the
 * next word a candidate too.
 */
static int
substalias(const char *name)
{
	Alias *a;
	size_t n;

	a = lookupalias(name);
	if (!a || a->inuse || nalias >= MAXALIAS)
		return 0;
	n = strlen(a->val);
	a->inuse = 1;
	alblank[nalias] = n > 0 && isblank_(a->val[n - 1]);
	alstack[nalias++] = a;
	pushstring(a->val, NULL);
	return 1;
}

static int
popalias(void)
{
	if (nalias == 0)
		return 0;
	popinput();
	nalias--;
	alstack[nalias]->inuse = 0;
	aliasblank = alblank[nalias];
	return 1;
}

int
readtoken(int flags)
{
	SBuf b;
	int c, q, alias;

	if (pushedback) {
		pushedback = 0;
		return tok.type; /* already traced when first read */
	}
again:
	for (;;) {
		c = pgetc();
		if (c < 0) {
			if (popalias())
				continue;
			break;
		}
		if (isblank_(c))
			continue;
		if (c == '\\') {
			q = pgetc();
			if (q == '\n')
				continue;
			pungetc(q);
			pungetc(c);
			break;
		}
		if (c == '#') { /* rule 9: discard the comment */
			while ((c = pgetc()) >= 0 && c != '\n')
				;
			if (c < 0)
				continue;
			pungetc(c);
			continue;
		}
		pungetc(c);
		break;
	}
	tok.lno = inputlineno();
	tok.text = NULL;
	tok.quoted = 0;
	c = pgetc();
	if (c < 0) {
		tok.type = TEOF;
		return tracetoken(tok.type);
	}
	if (c == '\n') {
		tok.type = TNL;
		if (nhdocq)
			readheredocs();
		return tracetoken(tok.type);
	}
	if (isopchar(c)) {
		tok.type = readoperator(c);
		return tracetoken(tok.type);
	}
	pungetc(c);
	sbinit(&b, la);
	readword(&b, &q);
	tok.text = sbdone(&b);
	tok.quoted = q;
	if (b.len == 0) {
		/* nothing consumable here; avoid a loop on a stray byte */
		c = pgetc();
		if (c < 0) {
			tok.type = TEOF;
			return tracetoken(tok.type);
		}
		tok.type = TWORD;
		sbinit(&b, la);
		sbputc(&b, c);
		tok.text = sbdone(&b);
		return tracetoken(tok.type);
	}
	alias = (flags & LEXCMD) || aliasblank;
	aliasblank = 0;
	if (alias && !q && substalias(tok.text))
		goto again;
	/* section 2.10.1: digits before `<' or `>' are an IO_NUMBER */
	if (!q) {
		for (c = 0; b.s[c]; c++) {
			if (b.s[c] < '0' || b.s[c] > '9')
				break;
		}
		if (b.s[c] == '\0') {
			c = pgetc();
			pungetc(c);
			if (c == '<' || c == '>') {
				tok.type = TIONUM;
				return tracetoken(tok.type);
			}
		}
	}
	tok.type = TWORD;
	return tracetoken(tok.type);
}

/*
 * The token stream, for `-T'. Every return of readtoken() passes
 * through here, so the trace cannot miss a token.
 */
static int
tracetoken(int type)
{
	if (!debugtokens)
		return type;
	fflush(stdout);
	fprintf(stderr, "token %-12s", tokname[type]);
	if (tok.text)
		fprintf(stderr, " <%s>%s", tok.text,
		        tok.quoted ? " quoted" : "");
	fprintf(stderr, " line %d\n", tok.lno);
	fflush(stderr);
	return type;
}

/*
 * The body of a here-document starts after the newline that ends the
 * line holding the operator (section 2.7.4). Several operators on one
 * line give several bodies, in the order of the operators.
 */
static void
readonehere(NRedir *r)
{
	SBuf body, line;
	const char *delim;
	int c, q, strip;

	strip = r->hstrip;
	delim = rmquotes(r->fname, la, &q);
	r->hquoted = q;
	sbinit(&body, la);
	for (;;) {
		sbinit(&line, la);
		if (strip) {
			while ((c = pgetc()) == '\t')
				;
			if (c >= 0)
				pungetc(c);
		}
		for (;;) {
			c = pgetc();
			if (c < 0 || c == '\n')
				break;
			sbputc(&line, c);
		}
		sbdone(&line);
		if (strcmp(line.s, delim) == 0)
			break;
		if (c < 0 && line.len == 0) {
			warn("here-document delimited by end of file "
			     "(wanted `%s')",
			     delim);
			break;
		}
		sbputn(&body, line.s, line.len);
		sbputc(&body, '\n');
		if (c < 0) {
			warn("here-document delimited by end of file "
			     "(wanted `%s')",
			     delim);
			break;
		}
	}
	r->hdoc = sbdone(&body);
}

static void
readheredocs(void)
{
	int i, n;

	n = nhdocq;
	nhdocq = 0;
	for (i = 0; i < n; i++)
		readonehere(hdocq[i]);
}
