/*
 * msh - a POSIX shell.
 *
 * A recursive-descent parser for the grammar of section 2.10. One
 * function stands for one grammar rule, so the shape of the code is
 * the shape of the grammar. The parser returns one syntax tree per
 * complete command, and every word in the tree is unexpanded.
 *
 * Grammar rules 1 to 9 live here and not in the lexer, because they
 * all depend on the position in the grammar: a reserved word counts
 * only where a command may start, an assignment only before the
 * command name, and `in' and `esac' only inside their clause.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msh.h"
#include "tree.h"
#include "vars.h"

static Arena *pa;

static Node *parselist(int nlok);
static Node *parseandor(void);
static Node *parsepipeline(void);
static Node *parsecommand(void);
static Node *parsesimple(char *first);
static Node *parsecompound(int rw);
static NRedir *parseredirs(void);

void
synerror(const char *fmt, ...)
{
	va_list ap;
	char msg[256];

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	lexreset();
	/*
	 * The parser stopped part of the way through a line, and the
	 * rest of that line belongs to no command, so drop it. Only a
	 * syntax error needs this: after any other error the command
	 * had already been read in full.
	 */
	flushinput();
	/*
	 * Section 2.8.1: a syntax error in the shell language makes a
	 * non-interactive shell exit.
	 */
	exitstatus = 2;
	if (!iflag) {
		warn("syntax error: %s", msg);
		exitshell(2);
	}
	error("syntax error: %s", msg);
	/* NOTREACHED */
}

static void NORETURN
synexpect(const char *want)
{
	Token *t;

	t = lasttoken();
	if (t->type == TWORD)
		synerror("unexpected `%s', expected %s", t->text, want);
	synerror("unexpected %s, expected %s", tokname[t->type], want);
	/* NOTREACHED */
}

static Node *
mknode(int type)
{
	Node *n;

	n = arenaalloc(pa, sizeof(*n));
	memset(n, 0, sizeof(*n));
	n->type = type;
	n->lno = lasttoken()->lno;
	return n;
}

static Node *
mkbin(int type, Node *l, Node *r)
{
	Node *n;

	n = mknode(type);
	n->u.bin.left = l;
	n->u.bin.right = r;
	return n;
}

static int
rwof(Token *t)
{
	if (t->type != TWORD)
		return RWNONE;
	return reswordof(t->text);
}

/*
 * A word is an assignment when a valid name is followed by `='. The
 * test runs on the unexpanded text, and that is sound, because a name
 * holds no quoting character.
 */
static int
assignword(const char *s)
{
	const char *p;

	p = s;
	if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
	      *p == '_'))
		return 0;
	for (p++; *p; p++) {
		if (*p == '=')
			return p != s;
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '_'))
			return 0;
	}
	return 0;
}

static int
isredirtok(int t)
{
	switch (t) {
	case TLESS:
	case TGREAT:
	case TDLESS:
	case TDGREAT:
	case TLESSAND:
	case TGREATAND:
	case TLESSGREAT:
	case TDLESSDASH:
	case TCLOBBER:
	case TIONUM:
		return 1;
	}
	return 0;
}

/*
 * Whether the token just read can begin a command. A reserved word
 * that closes a construct cannot, and that is how a compound list
 * knows where it ends.
 */
static int
startscommand(int t)
{
	if (isredirtok(t) || t == TLPAREN)
		return 1;
	if (t != TWORD)
		return 0;
	switch (reswordof(lasttoken()->text)) {
	case RWTHEN:
	case RWELSE:
	case RWELIF:
	case RWFI:
	case RWDO:
	case RWDONE:
	case RWESAC:
	case RWRBRACE:
	case RWIN:
		return 0;
	}
	return 1;
}

static void
skipnl(void)
{
	while (readtoken(0) == TNL)
		;
	pushtoken();
}

static void
expectrw(int rw, const char *what)
{
	readtoken(LEXCMD);
	if (rwof(lasttoken()) != rw)
		synexpect(what);
}

/*
 * list: and_or, separated by `;', `&' or a newline.
 *
 * A compound list begins with a linebreak in the grammar of section
 * 2.10, so `then', `do', `else', `{' and `(' may all be followed by
 * newlines before the first command.
 */
static Node *
parselist(int nlok)
{
	Node *n, *n2;
	int t;

	if (nlok)
		skipnl();
	n = parseandor();
	for (;;) {
		t = readtoken(0);
		if (t == TAND) {
			n2 = mknode(NBACKGND);
			n2->u.un.child = n;
			n = n2;
		} else if (t == TSEMI) {
			/* nothing: a plain separator */
		} else if (t == TNL && nlok) {
			/* nothing */
		} else {
			pushtoken();
			break;
		}
		if (nlok) {
			while (readtoken(0) == TNL)
				;
			pushtoken();
		}
		t = readtoken(LEXCMD);
		pushtoken();
		if (!startscommand(t))
			break;
		n2 = parseandor();
		n = mkbin(NSEMI, n, n2);
	}
	return n;
}

/* and_or: pipeline, joined by `&&' or `||', which are left-associative. */
static Node *
parseandor(void)
{
	Node *n;
	int t;

	n = parsepipeline();
	for (;;) {
		t = readtoken(0);
		if (t != TANDAND && t != TOROR) {
			pushtoken();
			break;
		}
		skipnl();
		n = mkbin(t == TANDAND ? NAND : NOR, n, parsepipeline());
	}
	return n;
}

static Node *
parsepipeline(void)
{
	Node *n, *first, **tail;
	int bang, count;

	bang = 0;
	if (readtoken(LEXCMD) == TWORD && rwof(lasttoken()) == RWBANG)
		bang = 1;
	else
		pushtoken();
	first = parsecommand();
	tail = &first->next;
	count = 1;
	while (readtoken(0) == TPIPE) {
		skipnl();
		*tail = parsecommand();
		tail = &(*tail)->next;
		count++;
	}
	pushtoken();
	if (count > 1) {
		n = mknode(NPIPE);
		n->u.pipe.cmds = first;
		n->u.pipe.n = count;
	} else {
		n = first;
	}
	if (bang) {
		first = mknode(NNOT);
		first->u.un.child = n;
		n = first;
	}
	return n;
}

static Node *
parsefuncdef(const char *name)
{
	Node *n;
	int q;

	readtoken(0);
	if (lasttoken()->type != TRPAREN)
		synexpect("\")\"");
	skipnl();
	n = mknode(NFUNC);
	n->u.func.name = rmquotes(name, pa, &q);
	if (!validname(n->u.func.name, -1))
		synerror("`%s' is not a valid function name",
		         n->u.func.name);
	readtoken(LEXCMD);
	switch (rwof(lasttoken())) {
	case RWLBRACE:
	case RWIF:
	case RWWHILE:
	case RWUNTIL:
	case RWFOR:
	case RWCASE:
		pushtoken();
		n->u.func.body = parsecommand();
		break;
	default:
		if (lasttoken()->type == TLPAREN) {
			pushtoken();
			n->u.func.body = parsecommand();
			break;
		}
		synexpect("a compound command");
	}
	return n;
}

static Node *
parsecommand(void)
{
	Node *n;
	Token *t;
	char *name;
	int type, rw;

	type = readtoken(LEXCMD);
	if (type == TLPAREN) {
		n = mknode(NSUBSHELL);
		n->u.un.child = parselist(1);
		readtoken(0);
		if (lasttoken()->type != TRPAREN)
			synexpect("\")\"");
		n->redirs = parseredirs();
		return n;
	}
	if (type != TWORD) {
		pushtoken();
		return parsesimple(NULL);
	}
	t = lasttoken();
	rw = reswordof(t->text);
	switch (rw) {
	case RWIF:
	case RWWHILE:
	case RWUNTIL:
	case RWFOR:
	case RWCASE:
	case RWLBRACE:
		n = parsecompound(rw);
		n->redirs = parseredirs();
		return n;
	}
	/*
	 * `name ( )' starts a function definition (grammar rule 8), and
	 * only one token of lookahead tells the two cases apart. So the
	 * command word travels into parsesimple() by hand.
	 */
	if (assignword(t->text)) {
		pushtoken();
		return parsesimple(NULL);
	}
	name = t->text;
	if (readtoken(0) == TLPAREN)
		return parsefuncdef(name);
	pushtoken();
	return parsesimple(name);
}

static Node *
parsesimple(char *first)
{
	Node *n;
	NArg *a, **atail, **wtail;
	NRedir *r, **rtail;
	int t, haveword;

	n = mknode(NCMD);
	atail = &n->u.cmd.assigns;
	wtail = &n->u.cmd.args;
	rtail = &n->redirs;
	haveword = 0;
	if (first) {
		a = arenaalloc(pa, sizeof(*a));
		a->next = NULL;
		a->text = first;
		*wtail = a;
		wtail = &a->next;
		haveword = 1;
	}
	for (;;) {
		t = readtoken(haveword ? 0 : LEXCMD);
		if (isredirtok(t)) {
			pushtoken();
			r = parseredirs();
			if (r) {
				*rtail = r;
				while (*rtail)
					rtail = &(*rtail)->next;
			}
			continue;
		}
		if (t != TWORD) {
			pushtoken();
			break;
		}
		a = arenaalloc(pa, sizeof(*a));
		a->next = NULL;
		a->text = lasttoken()->text;
		if (!haveword && assignword(a->text)) {
			*atail = a;
			atail = &a->next;
			continue;
		}
		haveword = 1;
		*wtail = a;
		wtail = &a->next;
	}
	if (!n->u.cmd.args && !n->u.cmd.assigns && !n->redirs)
		synexpect("a command");
	return n;
}

/* The default file descriptor of each redirection operator (2.7). */
static const struct {
	int tok;
	int type;
	int fd;
} redirs[] = {
    {TLESS, RIN, 0},
    {TGREAT, ROUT, 1},
    {TCLOBBER, RCLOBBER, 1},
    {TDGREAT, RAPPEND, 1},
    {TLESSGREAT, RINOUT, 0},
    {TLESSAND, RDUPIN, 0},
    {TGREATAND, RDUPOUT, 1},
    {TDLESS, RHERE, 0},
    {TDLESSDASH, RHERE, 0},
};

static NRedir *
parseredirs(void)
{
	NRedir *head, *r, **tail;
	int t, fd, i;

	head = NULL;
	tail = &head;
	for (;;) {
		t = readtoken(0);
		fd = -1;
		if (t == TIONUM) {
			fd = (int)strtol(lasttoken()->text, NULL, 10);
			t = readtoken(0);
			if (!isredirtok(t) || t == TIONUM)
				synexpect("a redirection operator");
		}
		if (!isredirtok(t)) {
			pushtoken();
			break;
		}
		for (i = 0; (size_t)i < LEN(redirs); i++) {
			if (redirs[i].tok == t)
				break;
		}
		r = arenaalloc(pa, sizeof(*r));
		memset(r, 0, sizeof(*r));
		r->type = redirs[i].type;
		r->fd = fd < 0 ? redirs[i].fd : fd;
		r->hstrip = t == TDLESSDASH;
		if (readtoken(0) != TWORD)
			synexpect("a file name");
		r->fname = lasttoken()->text;
		if (r->type == RHERE)
			heredocexpect(r);
		*tail = r;
		tail = &r->next;
	}
	return head;
}

static Node *
parsedogroup(void)
{
	Node *n;

	expectrw(RWDO, "\"do\"");
	n = parselist(1);
	expectrw(RWDONE, "\"done\"");
	return n;
}

static Node *
parseif(void)
{
	Node *n;

	n = mknode(NIF);
	n->u.ifn.cond = parselist(1);
	expectrw(RWTHEN, "\"then\"");
	n->u.ifn.thn = parselist(1);
	readtoken(LEXCMD);
	switch (rwof(lasttoken())) {
	case RWELIF:
		n->u.ifn.els = parseif();
		return n;
	case RWELSE:
		n->u.ifn.els = parselist(1);
		expectrw(RWFI, "\"fi\"");
		return n;
	case RWFI:
		return n;
	}
	synexpect("\"fi\"");
	/* NOTREACHED */
	return n;
}

static Node *
parsefor(void)
{
	Node *n;
	NArg *a, **tail;
	int q;

	n = mknode(NFOR);
	if (readtoken(0) != TWORD)
		synexpect("a variable name");
	n->u.forn.var = rmquotes(lasttoken()->text, pa, &q);
	if (q || !validname(n->u.forn.var, -1))
		synerror("`%s' is not a valid variable name",
		         lasttoken()->text);
	skipnl();
	if (readtoken(LEXCMD) == TWORD && rwof(lasttoken()) == RWIN) {
		n->u.forn.hasin = 1;
		tail = &n->u.forn.words;
		while (readtoken(0) == TWORD) {
			a = arenaalloc(pa, sizeof(*a));
			a->next = NULL;
			a->text = lasttoken()->text;
			*tail = a;
			tail = &a->next;
		}
		if (lasttoken()->type != TSEMI && lasttoken()->type != TNL)
			synexpect("\";\" or a newline");
		skipnl();
	} else {
		/*
		 * No `in' clause, so the loop runs over the positional
		 * parameters (section 2.9.4.2). The grammar writes this
		 * form as `for name do', but every shell in use also
		 * accepts a separator before `do', so this one does too.
		 */
		pushtoken();
		if (readtoken(0) != TSEMI)
			pushtoken();
		skipnl();
	}
	n->u.forn.body = parsedogroup();
	return n;
}

static Node *
parsecase(void)
{
	Node *n;
	NCase *c, **ctail;
	NArg *a, **ptail;
	int t;

	n = mknode(NCASE);
	if (readtoken(0) != TWORD)
		synexpect("a word");
	n->u.casen.word = lasttoken()->text;
	skipnl();
	expectrw(RWIN, "\"in\"");
	skipnl();
	ctail = &n->u.casen.items;
	for (;;) {
		t = readtoken(LEXCMD);
		if (t == TWORD && rwof(lasttoken()) == RWESAC)
			break;
		if (t == TEOF)
			synexpect("\"esac\"");
		if (t != TLPAREN)
			pushtoken();
		c = arenaalloc(pa, sizeof(*c));
		memset(c, 0, sizeof(*c));
		ptail = &c->pats;
		for (;;) {
			if (readtoken(0) != TWORD)
				synexpect("a pattern");
			a = arenaalloc(pa, sizeof(*a));
			a->next = NULL;
			a->text = lasttoken()->text;
			*ptail = a;
			ptail = &a->next;
			t = readtoken(0);
			if (t == TPIPE)
				continue;
			if (t == TRPAREN)
				break;
			synexpect("\")\" or \"|\"");
		}
		*ctail = c;
		ctail = &c->next;
		while (readtoken(0) == TNL)
			;
		pushtoken();
		t = readtoken(LEXCMD);
		pushtoken();
		if (startscommand(t))
			c->body = parselist(1);
		t = readtoken(LEXCMD);
		if (t == TDSEMI) {
			skipnl();
			continue;
		}
		if (t == TWORD && rwof(lasttoken()) == RWESAC)
			break;
		synexpect("\";;\" or \"esac\"");
	}
	return n;
}

static Node *
parsecompound(int rw)
{
	Node *n;

	switch (rw) {
	case RWIF:
		return parseif();
	case RWWHILE: /* FALLTHROUGH */
	case RWUNTIL:
		n = mknode(rw == RWWHILE ? NWHILE : NUNTIL);
		n->u.loop.cond = parselist(1);
		n->u.loop.body = parsedogroup();
		return n;
	case RWFOR:
		return parsefor();
	case RWCASE:
		return parsecase();
	case RWLBRACE:
		n = mknode(NBRACE);
		n->u.un.child = parselist(1);
		expectrw(RWRBRACE, "\"}\"");
		return n;
	}
	synexpect("a compound command");
	/* NOTREACHED */
	return NULL;
}

/*
 * One complete command. The caller owns the arena, so the caller
 * decides when the tree dies.
 */
Node *
parsecmd(Arena *a, int *stat)
{
	Node *n;
	int t;

	pa = a;
	*stat = 0;
	lexinit(a);
	setprompt(1);
	t = readtoken(LEXCMD);
	if (t == TEOF) {
		*stat = PSEOF;
		return NULL;
	}
	if (t == TNL)
		return NULL;
	pushtoken();
	setprompt(2);
	n = parselist(0);
	t = readtoken(0);
	if (t != TNL && t != TEOF)
		synexpect("end of command");
	return n;
}
