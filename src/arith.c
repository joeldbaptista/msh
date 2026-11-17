/*
 * msh - a POSIX shell.
 *
 * Arithmetic expansion. The parser climbs precedence: one table holds
 * every binary operator with its precedence, and one loop reads
 * operators of at least the precedence that the caller asked for. So
 * adding an operator means adding a row, not changing the control
 * flow.
 *
 * Evaluation is on `long', as the shell language requires signed
 * integer arithmetic of at least that width. `&&', `||' and the
 * conditional operator do not evaluate the branch they discard, so a
 * counter records how deep the scan is inside a discarded branch, and
 * while it is above zero an assignment is skipped and a division by
 * zero is not an error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "expand.h"
#include "msh.h"
#include "tree.h"
#include "vars.h"

#define MAXDEPTH 32

enum {
	TKEOF,
	TKNUM,
	TKNAME,
	TKLP,
	TKRP,
	TKQUEST,
	TKCOLON,
	TKCOMMA,
	TKNOT,
	TKBNOT,
	TKASSIGN,
	TKPLUSEQ,
	TKMINUSEQ,
	TKMULEQ,
	TKDIVEQ,
	TKMODEQ,
	TKSHLEQ,
	TKSHREQ,
	TKBANDEQ,
	TKBXOREQ,
	TKBOREQ,
	TKOR,
	TKAND,
	TKBOR,
	TKBXOR,
	TKBAND,
	TKEQ,
	TKNE,
	TKLT,
	TKLE,
	TKGT,
	TKGE,
	TKSHL,
	TKSHR,
	TKPLUS,
	TKMINUS,
	TKMUL,
	TKDIV,
	TKMOD
};

/*
 * The operator spellings, longest first, so that `<<=' wins over `<<'
 * and `<<' wins over `<'.
 */
static const struct {
	const char *s;
	int tk;
} spellings[] = {
    {"<<=", TKSHLEQ},
    {">>=", TKSHREQ},
    {"||", TKOR},
    {"&&", TKAND},
    {"==", TKEQ},
    {"!=", TKNE},
    {"<=", TKLE},
    {">=", TKGE},
    {"<<", TKSHL},
    {">>", TKSHR},
    {"+=", TKPLUSEQ},
    {"-=", TKMINUSEQ},
    {"*=", TKMULEQ},
    {"/=", TKDIVEQ},
    {"%=", TKMODEQ},
    {"&=", TKBANDEQ},
    {"^=", TKBXOREQ},
    {"|=", TKBOREQ},
    {"(", TKLP},
    {")", TKRP},
    {"?", TKQUEST},
    {":", TKCOLON},
    {",", TKCOMMA},
    {"!", TKNOT},
    {"~", TKBNOT},
    {"=", TKASSIGN},
    {"|", TKBOR},
    {"^", TKBXOR},
    {"&", TKBAND},
    {"<", TKLT},
    {">", TKGT},
    {"+", TKPLUS},
    {"-", TKMINUS},
    {"*", TKMUL},
    {"/", TKDIV},
    {"%", TKMOD},
};

/* The binary operators and their precedence, lowest binding first. */
static const struct {
	int tk;
	int prec;
} binops[] = {
    {TKOR, 1},
    {TKAND, 2},
    {TKBOR, 3},
    {TKBXOR, 4},
    {TKBAND, 5},
    {TKEQ, 6},
    {TKNE, 6},
    {TKLT, 7},
    {TKLE, 7},
    {TKGT, 7},
    {TKGE, 7},
    {TKSHL, 8},
    {TKSHR, 8},
    {TKPLUS, 9},
    {TKMINUS, 9},
    {TKMUL, 10},
    {TKDIV, 10},
    {TKMOD, 10},
};

typedef struct atok ATok;
struct atok {
	int type;
	long num;
	char *name;
};

static ATok *toks;
static int ntoks;
static int pos;
static int noeval;
static int depth;

static long parseassign(void);

static void NORETURN
arerror(const char *msg)
{
	error("arithmetic: %s", msg);
	/* NOTREACHED */
}

static int
isnamestart(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int
isnamecont(int c)
{
	return isnamestart(c) || (c >= '0' && c <= '9');
}

static void
lexarith(const char *s)
{
	SBuf b;
	size_t i;
	const char *p;
	int n, cap;

	cap = 32;
	toks = arenaalloc(stk, sizeof(*toks) * (size_t)cap);
	ntoks = 0;
	pos = 0;
	p = s;
	for (;;) {
		while (*p == ' ' || *p == '\t' || *p == '\n')
			p++;
		if (ntoks + 2 > cap) {
			ATok *t;

			t = arenaalloc(stk, sizeof(*t) * (size_t)(cap * 2));
			memcpy(t, toks, sizeof(*t) * (size_t)ntoks);
			toks = t;
			cap *= 2;
		}
		if (!*p) {
			toks[ntoks].type = TKEOF;
			toks[ntoks].num = 0;
			toks[ntoks].name = NULL;
			ntoks++;
			/* a second one, so a lookahead never runs off */
			toks[ntoks] = toks[ntoks - 1];
			return;
		}
		toks[ntoks].name = NULL;
		toks[ntoks].num = 0;
		if (*p >= '0' && *p <= '9') {
			toks[ntoks].type = TKNUM;
			toks[ntoks].num = strtol(p, (char **)&p, 0);
			ntoks++;
			continue;
		}
		if (isnamestart(*p)) {
			sbinit(&b, stk);
			while (isnamecont(*p))
				sbputc(&b, *p++);
			toks[ntoks].type = TKNAME;
			toks[ntoks].name = sbdone(&b);
			ntoks++;
			continue;
		}
		n = -1;
		for (i = 0; i < LEN(spellings); i++) {
			size_t l;

			l = strlen(spellings[i].s);
			if (strncmp(p, spellings[i].s, l) == 0) {
				n = spellings[i].tk;
				p += l;
				break;
			}
		}
		if (n < 0)
			arerror("unexpected character");
		toks[ntoks].type = n;
		ntoks++;
	}
}

static long
namevalue(const char *name)
{
	const char *v;
	long n;

	v = getvar(name);
	if (!v || !*v)
		return 0;
	if (depth >= MAXDEPTH)
		arerror("expansion too deep");
	depth++;
	n = arith(v);
	depth--;
	return n;
}

static int
isassignop(int t)
{
	return t >= TKASSIGN && t <= TKBOREQ;
}

static long
apply(int op, long a, long b)
{
	switch (op) {
	case TKPLUS: /* FALLTHROUGH */
	case TKPLUSEQ:
		return a + b;
	case TKMINUS: /* FALLTHROUGH */
	case TKMINUSEQ:
		return a - b;
	case TKMUL: /* FALLTHROUGH */
	case TKMULEQ:
		return a * b;
	case TKDIV: /* FALLTHROUGH */
	case TKDIVEQ:
		if (b == 0) {
			if (noeval)
				return 0;
			arerror("division by zero");
		}
		return a / b;
	case TKMOD: /* FALLTHROUGH */
	case TKMODEQ:
		if (b == 0) {
			if (noeval)
				return 0;
			arerror("division by zero");
		}
		return a % b;
	case TKSHL: /* FALLTHROUGH */
	case TKSHLEQ:
		return a << b;
	case TKSHR: /* FALLTHROUGH */
	case TKSHREQ:
		return a >> b;
	case TKBAND: /* FALLTHROUGH */
	case TKBANDEQ:
		return a & b;
	case TKBXOR: /* FALLTHROUGH */
	case TKBXOREQ:
		return a ^ b;
	case TKBOR: /* FALLTHROUGH */
	case TKBOREQ:
		return a | b;
	case TKEQ:
		return a == b;
	case TKNE:
		return a != b;
	case TKLT:
		return a < b;
	case TKLE:
		return a <= b;
	case TKGT:
		return a > b;
	case TKGE:
		return a >= b;
	}
	arerror("unknown operator");
	/* NOTREACHED */
	return 0;
}

static long
parseunary(void)
{
	long v;

	switch (toks[pos].type) {
	case TKPLUS:
		pos++;
		return parseunary();
	case TKMINUS:
		pos++;
		return -parseunary();
	case TKNOT:
		pos++;
		return !parseunary();
	case TKBNOT:
		pos++;
		return ~parseunary();
	case TKNUM:
		v = toks[pos].num;
		pos++;
		return v;
	case TKNAME:
		v = namevalue(toks[pos].name);
		pos++;
		return v;
	case TKLP:
		pos++;
		v = parseassign();
		while (toks[pos].type == TKCOMMA) {
			pos++;
			v = parseassign();
		}
		if (toks[pos].type != TKRP)
			arerror("expected \")\"");
		pos++;
		return v;
	}
	arerror("expected an operand");
	/* NOTREACHED */
	return 0;
}

static int
precof(int t)
{
	size_t i;

	for (i = 0; i < LEN(binops); i++) {
		if (binops[i].tk == t)
			return binops[i].prec;
	}
	return 0;
}

static long
parsebin(int minprec)
{
	long l, r;
	int op, prec;

	l = parseunary();
	for (;;) {
		op = toks[pos].type;
		prec = precof(op);
		if (prec == 0 || prec < minprec)
			return l;
		pos++;
		if (op == TKAND || op == TKOR) {
			/*
			 * The discarded side is parsed but not evaluated,
			 * because the parser must still get past it.
			 */
			if ((op == TKAND && l == 0) ||
			    (op == TKOR && l != 0)) {
				noeval++;
				parsebin(prec + 1);
				noeval--;
				l = op == TKAND ? 0 : 1;
				continue;
			}
			r = parsebin(prec + 1);
			l = r != 0;
			continue;
		}
		r = parsebin(prec + 1);
		l = apply(op, l, r);
	}
}

static long
parseternary(void)
{
	long c, a, b;

	c = parsebin(1);
	if (toks[pos].type != TKQUEST)
		return c;
	pos++;
	if (c == 0)
		noeval++;
	a = parseassign();
	if (c == 0)
		noeval--;
	if (toks[pos].type != TKCOLON)
		arerror("expected \":\"");
	pos++;
	if (c != 0)
		noeval++;
	b = parseternary();
	if (c != 0)
		noeval--;
	return c ? a : b;
}

static long
parseassign(void)
{
	long v;
	char *name;
	int op;

	if (toks[pos].type == TKNAME && isassignop(toks[pos + 1].type)) {
		name = toks[pos].name;
		op = toks[pos + 1].type;
		pos += 2;
		v = parseassign();
		if (op != TKASSIGN)
			v = apply(op, namevalue(name), v);
		if (!noeval) {
			char buf[32];

			snprintf(buf, sizeof(buf), "%ld", v);
			setvar(name, buf, 0);
		}
		return v;
	}
	return parseternary();
}

long
arith(const char *s)
{
	ATok *savetoks;
	long v;
	int savent, savepos;

	savetoks = toks;
	savent = ntoks;
	savepos = pos;
	lexarith(s);
	if (toks[pos].type == TKEOF) {
		v = 0;
	} else {
		v = parseassign();
		while (toks[pos].type == TKCOMMA) {
			pos++;
			v = parseassign();
		}
		if (toks[pos].type != TKEOF)
			arerror("unexpected trailing text");
	}
	toks = savetoks;
	ntoks = savent;
	pos = savepos;
	return v;
}
