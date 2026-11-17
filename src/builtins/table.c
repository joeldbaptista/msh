/*
 * msh - a POSIX shell.
 *
 * The table of built-in utilities, and the argument handling that they
 * share.
 *
 * The table is the whole description of a built-in: its name, the
 * function that is its behaviour, and the flags that the command
 * search and section 2.14 read. It is sorted by name, so the search is
 * a binary search, and adding a built-in means writing one function
 * and adding one row.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../builtin.h"
#include "../msh.h"

const struct builtin builtins[] = {
    {".", bltin_dot, BSPECIAL | BASSIGN},
    {":", bltin_colon, BSPECIAL | BASSIGN},
    {"alias", bltin_alias, 0},
    {"bg", bltin_bg, 0},
    {"break", bltin_break, BSPECIAL},
    {"cd", bltin_cd, 0},
    {"command", bltin_command, 0},
    {"continue", bltin_continue, BSPECIAL},
    {"eval", bltin_eval, BSPECIAL | BASSIGN},
    {"exec", bltin_exec, BSPECIAL | BASSIGN},
    {"exit", bltin_exit, BSPECIAL},
    {"export", bltin_export, BSPECIAL | BASSIGN},
    {"false", bltin_false, 0},
    {"fg", bltin_fg, 0},
    {"getopts", bltin_getopts, 0},
    {"hash", bltin_hash, 0},
    {"jobs", bltin_jobs, 0},
    {"pwd", bltin_pwd, 0},
    {"read", bltin_read, 0},
    {"readonly", bltin_readonly, BSPECIAL | BASSIGN},
    {"return", bltin_return, BSPECIAL},
    {"set", bltin_set, BSPECIAL},
    {"shift", bltin_shift, BSPECIAL},
    {"times", bltin_times, BSPECIAL},
    {"trap", bltin_trap, BSPECIAL},
    {"true", bltin_true, 0},
    {"type", bltin_type, 0},
    {"ulimit", bltin_ulimit, 0},
    {"umask", bltin_umask, 0},
    {"unalias", bltin_unalias, 0},
    {"unset", bltin_unset, BSPECIAL},
    {"wait", bltin_wait, 0},
};

const int nbuiltins = (int)LEN(builtins);

int
findbuiltin(const char *name)
{
	int lo, hi, mid, cmp;

	lo = 0;
	hi = nbuiltins - 1;
	while (lo <= hi) {
		mid = lo + (hi - lo) / 2;
		cmp = strcmp(name, builtins[mid].name);
		if (cmp == 0)
			return mid;
		if (cmp < 0)
			hi = mid - 1;
		else
			lo = mid + 1;
	}
	return -1;
}

void
boptinit(BOpt *o, int argc, char **argv)
{
	o->argv = argv;
	o->argc = argc;
	o->ind = 1;
	o->off = 1;
	o->arg = NULL;
	o->opt = 0;
	o->plus = 0;
}

/*
 * One option. `spec' lists the letters, and a letter followed by `:'
 * takes an argument. A leading `+' in the argument is accepted as
 * well, because `set' uses it to turn an option off; the caller reads
 * `plus' to find out which sign it saw.
 */
int
bopt(BOpt *o, const char *spec)
{
	const char *p;
	char *a;
	int c;

	o->arg = NULL;
	if (o->ind >= o->argc)
		return -1;
	a = o->argv[o->ind];
	if (o->off == 1) {
		if (a[0] != '-' && a[0] != '+')
			return -1;
		if (a[1] == '\0')
			return -1; /* a lone `-' is an operand */
		if (a[0] == '-' && a[1] == '-' && a[2] == '\0') {
			o->ind++;
			return -1;
		}
		o->plus = a[0] == '+';
	}
	c = (unsigned char)a[o->off];
	if (c == '\0') {
		o->ind++;
		o->off = 1;
		return bopt(o, spec);
	}
	o->off++;
	if (a[o->off] == '\0') {
		o->ind++;
		o->off = 1;
	}
	p = strchr(spec, c);
	if (!p || c == ':') {
		o->opt = c;
		return '?';
	}
	if (p[1] != ':')
		return c;
	if (o->off != 1) {
		o->arg = a + o->off;
		o->ind++;
		o->off = 1;
		return c;
	}
	if (o->ind >= o->argc) {
		o->opt = c;
		return ':';
	}
	o->arg = o->argv[o->ind++];
	return c;
}

long
bnumber(const char *s, const char *what)
{
	char *end;
	long n;

	n = strtol(s, &end, 10);
	if (*s == '\0' || *end != '\0')
		error("%s: %s: bad number", what, s);
	return n;
}
