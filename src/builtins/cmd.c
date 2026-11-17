/*
 * msh - a POSIX shell.
 *
 * The built-ins that inspect or bypass the command search: `command',
 * `type' and `hash'; and the alias store, whose substitution belongs
 * to the lexer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../builtin.h"
#include "../exec.h"
#include "../expand.h"
#include "../msh.h"
#include "../tree.h"
#include "../vars.h"

static Alias *aliases;

Alias *
lookupalias(const char *name)
{
	Alias *a;

	for (a = aliases; a; a = a->next) {
		if (strcmp(a->name, name) == 0)
			return a;
	}
	return NULL;
}

void
setalias(const char *name, const char *val)
{
	Alias *a;

	a = lookupalias(name);
	if (a) {
		free(a->val);
		a->val = xstrdup(val);
		return;
	}
	a = xmalloc(sizeof(*a));
	a->name = xstrdup(name);
	a->val = xstrdup(val);
	a->inuse = 0;
	a->next = aliases;
	aliases = a;
}

int
unsetalias(const char *name)
{
	Alias *a, **pp;

	for (pp = &aliases; (a = *pp) != NULL; pp = &a->next) {
		if (strcmp(a->name, name) != 0)
			continue;
		*pp = a->next;
		free(a->name);
		free(a->val);
		free(a);
		return 0;
	}
	return -1;
}

void
unsetallaliases(void)
{
	while (aliases)
		unsetalias(aliases->name);
}

static int
aliascmp(const void *a, const void *b)
{
	return strcmp((*(Alias *const *)a)->name, (*(Alias *const *)b)->name);
}

/*
 * The `alias' page gives the format as "%s=%s\n", with the value
 * quoted so that it reads back unchanged. The word `alias' is not part
 * of it, although `type' and `command -v' do print one.
 */
void
listaliases(void)
{
	Alias **v, *a;
	int n, i;

	n = 0;
	for (a = aliases; a; a = a->next)
		n++;
	if (n == 0)
		return;
	v = arenaalloc(stk, sizeof(*v) * (size_t)n);
	i = 0;
	for (a = aliases; a; a = a->next)
		v[i++] = a;
	qsort(v, (size_t)n, sizeof(*v), aliascmp);
	for (i = 0; i < n; i++)
		printf("%s=%s\n", v[i]->name, singlequote(v[i]->val, stk));
}

int
bltin_alias(int argc, char **argv)
{
	Alias *a;
	char *eq;
	int i, status;

	if (argc == 1) {
		listaliases();
		return 0;
	}
	status = 0;
	for (i = 1; i < argc; i++) {
		eq = strchr(argv[i], '=');
		if (eq) {
			*eq = '\0';
			if (!*argv[i]) {
				warn("alias: an alias name is required");
				status = 1;
				continue;
			}
			setalias(argv[i], eq + 1);
			*eq = '=';
			continue;
		}
		a = lookupalias(argv[i]);
		if (!a) {
			warn("alias: %s: not found", argv[i]);
			status = 1;
			continue;
		}
		printf("%s=%s\n", a->name, singlequote(a->val, stk));
	}
	return status;
}

int
bltin_unalias(int argc, char **argv)
{
	BOpt o;
	int c, i, status;

	boptinit(&o, argc, argv);
	while ((c = bopt(&o, "a")) != -1) {
		if (c == 'a') {
			unsetallaliases();
			return 0;
		}
		warn("unalias: %c: bad option", o.opt);
		return 2;
	}
	status = 0;
	for (i = o.ind; i < argc; i++) {
		if (unsetalias(argv[i]) < 0) {
			warn("unalias: %s: not found", argv[i]);
			status = 1;
		}
	}
	return status;
}

/*
 * How one name would be interpreted. `-v' gives a form that the shell
 * could run again, and `-V' gives a sentence.
 */
static int
describe(const char *name, int verbose, int usefunc)
{
	CmdEntry e;
	Alias *a;

	a = usefunc ? lookupalias(name) : NULL;
	if (a) {
		if (verbose)
			printf("%s is an alias for %s\n", name, a->val);
		else
			printf("alias %s=%s\n", name,
			       singlequote(a->val, stk));
		return 0;
	}
	if (usefunc && reswordof(name) != RWNONE) {
		if (verbose)
			printf("%s is a shell keyword\n", name);
		else
			printf("%s\n", name);
		return 0;
	}
	findcommand(name, &e, 1, usefunc);
	switch (e.type) {
	case CSPECIAL: /* FALLTHROUGH */
	case CBUILTIN:
		if (verbose)
			printf("%s is a shell builtin\n", name);
		else
			printf("%s\n", name);
		return 0;
	case CFUNC:
		if (verbose)
			printf("%s is a shell function\n", name);
		else
			printf("%s\n", name);
		return 0;
	case CPATH:
		if (verbose)
			printf("%s is %s\n", name, e.path);
		else
			printf("%s\n", e.path);
		return 0;
	}
	if (verbose)
		warn("%s: not found", name);
	return 127;
}

int
bltin_type(int argc, char **argv)
{
	int i, status;

	status = 0;
	for (i = 1; i < argc; i++) {
		if (describe(argv[i], 1, 1) != 0)
			status = 127;
	}
	return status;
}

/*
 * `command' runs a command without looking at the functions, so a
 * function can call the utility it shadows.
 */
int
bltin_command(int argc, char **argv)
{
	CmdEntry e;
	BOpt o;
	const char *savepath;
	int c, vflagged, Vflagged, defpathflag, i, status;

	vflagged = 0;
	Vflagged = 0;
	defpathflag = 0;
	boptinit(&o, argc, argv);
	while ((c = bopt(&o, "pvV")) != -1) {
		switch (c) {
		case 'p':
			defpathflag = 1;
			continue;
		case 'v':
			vflagged = 1;
			continue;
		case 'V':
			Vflagged = 1;
			continue;
		}
		warn("command: %c: bad option", o.opt);
		return 2;
	}
	if (o.ind >= argc)
		return vflagged || Vflagged ? 2 : 0;
	savepath = NULL;
	if (defpathflag) {
		savepath = getvar("PATH");
		if (savepath)
			savepath = arenastrdup(stk, savepath);
		setvar("PATH", defpath(), 0);
	}
	status = 0;
	if (vflagged || Vflagged) {
		for (i = o.ind; i < argc; i++) {
			if (describe(argv[i], Vflagged, 0) != 0)
				status = 127;
		}
		goto out;
	}
	findcommand(argv[o.ind], &e, 1, 0);
	switch (e.type) {
	case CSPECIAL: /* FALLTHROUGH */
	case CBUILTIN:
		status = evalbuiltin(e.idx, argc - o.ind, argv + o.ind);
		break;
	case CPATH:
		status = evalextern(e.path, argv + o.ind);
		break;
	default:
		warn("%s: %s", argv[o.ind],
		     e.type == CNOTFOUND ? "not found" : "cannot execute");
		status = e.type == CNOTFOUND ? 127 : 126;
	}
out:
	if (defpathflag) {
		if (savepath)
			setvar("PATH", savepath, 0);
		else
			unsetvar("PATH");
	}
	return status;
}

int
bltin_hash(int argc, char **argv)
{
	BOpt o;
	int c, i;

	boptinit(&o, argc, argv);
	while ((c = bopt(&o, "r")) != -1) {
		if (c == 'r') {
			clearhash(0);
			return 0;
		}
		warn("hash: %c: bad option", o.opt);
		return 2;
	}
	if (o.ind >= argc) {
		listhash();
		return 0;
	}
	for (i = o.ind; i < argc; i++)
		hashcommand(argv[i]);
	return 0;
}
