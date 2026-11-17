/*
 * msh - a POSIX shell.
 *
 * The tree printer. It writes a syntax tree back out as shell text, so
 * that the parser can be checked against the grammar by reading its
 * output, and so that `type' can show the body of a function.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "msh.h"
#include "tree.h"

static void prnode(SBuf *b, Node *n);

/* One row per redirection type, in the order of the enum. */
static const char *const redirops[] = {"<", ">", ">|", ">>", "<>", "<&", ">&",
                                       "<<"};

static void
prredirs(SBuf *b, NRedir *r)
{
	char num[16];

	for (; r; r = r->next) {
		sbputc(b, ' ');
		snprintf(num, sizeof(num), "%d", r->fd);
		sbputs(b, num);
		if (r->type == RHERE) {
			sbputs(b, "<<");
			sbputs(b, r->fname);
			continue;
		}
		sbputs(b, redirops[r->type]);
		sbputs(b, r->fname);
	}
}

static void
prargs(SBuf *b, NArg *a, const char *sep)
{
	for (; a; a = a->next) {
		sbputs(b, a->text);
		if (a->next)
			sbputs(b, sep);
	}
}

static void
prnode(SBuf *b, Node *n)
{
	Node *c;
	NCase *k;

	if (!n) {
		sbputc(b, ':');
		return;
	}
	switch (n->type) {
	case NCMD:
		if (n->u.cmd.assigns) {
			prargs(b, n->u.cmd.assigns, " ");
			if (n->u.cmd.args)
				sbputc(b, ' ');
		}
		prargs(b, n->u.cmd.args, " ");
		break;
	case NPIPE:
		for (c = n->u.pipe.cmds; c; c = c->next) {
			prnode(b, c);
			if (c->next)
				sbputs(b, " | ");
		}
		break;
	case NAND: /* FALLTHROUGH */
	case NOR:
		prnode(b, n->u.bin.left);
		sbputs(b, n->type == NAND ? " && " : " || ");
		prnode(b, n->u.bin.right);
		break;
	case NSEMI:
		prnode(b, n->u.bin.left);
		sbputs(b, "; ");
		prnode(b, n->u.bin.right);
		break;
	case NBACKGND:
		prnode(b, n->u.un.child);
		sbputs(b, " &");
		break;
	case NNOT:
		sbputs(b, "! ");
		prnode(b, n->u.un.child);
		break;
	case NSUBSHELL:
		sbputc(b, '(');
		prnode(b, n->u.un.child);
		sbputc(b, ')');
		break;
	case NBRACE:
		sbputs(b, "{ ");
		prnode(b, n->u.un.child);
		sbputs(b, "; }");
		break;
	case NIF:
		sbputs(b, "if ");
		prnode(b, n->u.ifn.cond);
		sbputs(b, "; then ");
		prnode(b, n->u.ifn.thn);
		if (n->u.ifn.els) {
			sbputs(b, "; else ");
			prnode(b, n->u.ifn.els);
		}
		sbputs(b, "; fi");
		break;
	case NWHILE: /* FALLTHROUGH */
	case NUNTIL:
		sbputs(b, n->type == NWHILE ? "while " : "until ");
		prnode(b, n->u.loop.cond);
		sbputs(b, "; do ");
		prnode(b, n->u.loop.body);
		sbputs(b, "; done");
		break;
	case NFOR:
		sbputs(b, "for ");
		sbputs(b, n->u.forn.var);
		if (n->u.forn.hasin) {
			sbputs(b, " in ");
			prargs(b, n->u.forn.words, " ");
		}
		sbputs(b, "; do ");
		prnode(b, n->u.forn.body);
		sbputs(b, "; done");
		break;
	case NCASE:
		sbputs(b, "case ");
		sbputs(b, n->u.casen.word);
		sbputs(b, " in ");
		for (k = n->u.casen.items; k; k = k->next) {
			prargs(b, k->pats, "|");
			sbputs(b, ") ");
			if (k->body) {
				prnode(b, k->body);
				sbputc(b, ' ');
			}
			sbputs(b, ";; ");
		}
		sbputs(b, "esac");
		break;
	case NFUNC:
		sbputs(b, n->u.func.name);
		sbputs(b, "() ");
		prnode(b, n->u.func.body);
		break;
	}
	prredirs(b, n->redirs);
}

/*
 * The command as text, which the job table needs in order to name a
 * job the way `jobs' reports it.
 */
char *
treetext(Node *n, Arena *a)
{
	SBuf b;

	sbinit(&b, a);
	prnode(&b, n);
	return sbdone(&b);
}

void
printtree(Node *n, int fd)
{
	FILE *f;

	f = fd == 2 ? stderr : stdout;
	fputs(treetext(n, stk), f);
	fflush(f);
}

/*
 * The same tree as indented text, one node per line. This form shows
 * the shape that the grammar gave the command, which the shell-text
 * form hides, so it is the one that `-P' prints.
 */
/* One row per node type, in the order of the enum in tree.h. */
static const char *const nodename[] = {
    "command",
    "pipeline",
    "and",
    "or",
    "list",
    "background",
    "subshell",
    "group",
    "if",
    "while",
    "until",
    "for",
    "case",
    "function",
    "not",
};

static void
indent(FILE *f, int depth)
{
	int i;

	for (i = 0; i < depth; i++)
		fputs("  ", f);
}

static void
dumpargs(FILE *f, int depth, const char *label, NArg *a)
{
	if (!a)
		return;
	indent(f, depth);
	fprintf(f, "%s:", label);
	for (; a; a = a->next)
		fprintf(f, " %s", a->text);
	fputc('\n', f);
}

static void
dumpredirs(FILE *f, int depth, NRedir *r)
{
	for (; r; r = r->next) {
		indent(f, depth);
		if (r->type == RHERE)
			fprintf(f, "redirect: %d<<%s%s\n", r->fd, r->fname,
			        r->hquoted ? " (literal)" : "");
		else
			fprintf(f, "redirect: %d%s%s\n", r->fd,
			        redirops[r->type], r->fname);
	}
}

static void
dumpnode(FILE *f, Node *n, int depth)
{
	Node *c;
	NCase *k;

	if (!n) {
		indent(f, depth);
		fputs("(empty)\n", f);
		return;
	}
	indent(f, depth);
	fprintf(f, "%s (line %d)\n", nodename[n->type], n->lno);
	dumpredirs(f, depth + 1, n->redirs);
	switch (n->type) {
	case NCMD:
		dumpargs(f, depth + 1, "assign", n->u.cmd.assigns);
		dumpargs(f, depth + 1, "word", n->u.cmd.args);
		break;
	case NPIPE:
		for (c = n->u.pipe.cmds; c; c = c->next)
			dumpnode(f, c, depth + 1);
		break;
	case NAND: /* FALLTHROUGH */
	case NOR:
	case NSEMI:
		dumpnode(f, n->u.bin.left, depth + 1);
		dumpnode(f, n->u.bin.right, depth + 1);
		break;
	case NBACKGND: /* FALLTHROUGH */
	case NNOT:
	case NSUBSHELL:
	case NBRACE:
		dumpnode(f, n->u.un.child, depth + 1);
		break;
	case NIF:
		dumpnode(f, n->u.ifn.cond, depth + 1);
		dumpnode(f, n->u.ifn.thn, depth + 1);
		if (n->u.ifn.els)
			dumpnode(f, n->u.ifn.els, depth + 1);
		break;
	case NWHILE: /* FALLTHROUGH */
	case NUNTIL:
		dumpnode(f, n->u.loop.cond, depth + 1);
		dumpnode(f, n->u.loop.body, depth + 1);
		break;
	case NFOR:
		indent(f, depth + 1);
		fprintf(f, "variable: %s%s\n", n->u.forn.var,
		        n->u.forn.hasin ? "" : " (positional parameters)");
		dumpargs(f, depth + 1, "word", n->u.forn.words);
		dumpnode(f, n->u.forn.body, depth + 1);
		break;
	case NCASE:
		indent(f, depth + 1);
		fprintf(f, "word: %s\n", n->u.casen.word);
		for (k = n->u.casen.items; k; k = k->next) {
			dumpargs(f, depth + 1, "pattern", k->pats);
			dumpnode(f, k->body, depth + 2);
		}
		break;
	case NFUNC:
		indent(f, depth + 1);
		fprintf(f, "name: %s\n", n->u.func.name);
		dumpnode(f, n->u.func.body, depth + 1);
		break;
	}
}

void
dumptree(Node *n, int fd)
{
	FILE *f;

	f = fd == 2 ? stderr : stdout;
	fflush(stdout);
	dumpnode(f, n, 0);
	fflush(f);
}
