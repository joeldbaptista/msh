/*
 * msh - a POSIX shell.
 *
 * The parameter store. Variables live in a hash table of linked lists,
 * which is the simplest structure that answers the only two questions
 * asked of it: find one name, and list every name. Values live on the
 * normal heap, because a variable outlives the command that set it.
 *
 * Functions live in a plain list. The body of a function lives in the
 * parse arena of the command that defined it, so the definition holds
 * a reference to that arena and the arena outlives the command.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "exec.h"
#include "msh.h"
#include "tree.h"
#include "vars.h"

#define NBUCKET 61

static Var *vtab[NBUCKET];
static Func *funcs;
struct shparam shparam;

/*
 * The variables whose assignment has a side effect. The table is the
 * whole list, so adding one means adding a row and a case.
 */
static const char *const specials[] = {"PATH", "IFS", "OPTIND", "CDPATH",
                                       "PS1", "PS2", "PS4", "MAIL", "MAILPATH"};

static unsigned
hash(const char *s, int len)
{
	unsigned h;
	int i;

	h = 5381;
	for (i = 0; i < len; i++)
		h = h * 33 + (unsigned char)s[i];
	return h % NBUCKET;
}

int
validname(const char *s, int len)
{
	int i;

	if (len < 0)
		len = (int)strlen(s);
	if (len == 0)
		return 0;
	for (i = 0; i < len; i++) {
		if (s[i] >= 'a' && s[i] <= 'z')
			continue;
		if (s[i] >= 'A' && s[i] <= 'Z')
			continue;
		if (s[i] == '_')
			continue;
		if (i > 0 && s[i] >= '0' && s[i] <= '9')
			continue;
		return 0;
	}
	return 1;
}

static int
isspecialname(const char *name)
{
	size_t i;

	for (i = 0; i < LEN(specials); i++) {
		if (strcmp(name, specials[i]) == 0)
			return 1;
	}
	return 0;
}

Var *
lookupvar(const char *name)
{
	Var *v;
	int len;

	len = (int)strlen(name);
	for (v = vtab[hash(name, len)]; v; v = v->next) {
		if (strcmp(v->name, name) == 0)
			return v;
	}
	return NULL;
}

const char *
getvar(const char *name)
{
	Var *v;

	v = lookupvar(name);
	if (!v || (v->flags & VUNSET))
		return NULL;
	return v->val;
}

static Var *
makevar(const char *name)
{
	Var *v;
	unsigned h;

	v = lookupvar(name);
	if (v)
		return v;
	h = hash(name, (int)strlen(name));
	v = xmalloc(sizeof(*v));
	v->name = xstrdup(name);
	v->val = NULL;
	v->flags = VUNSET;
	if (isspecialname(name))
		v->flags |= VSPECIAL;
	v->next = vtab[h];
	vtab[h] = v;
	return v;
}

static void
sideeffect(Var *v)
{
	if (!(v->flags & VSPECIAL))
		return;
	if (strcmp(v->name, "PATH") == 0)
		pathchanged();
	else if (strcmp(v->name, "OPTIND") == 0)
		shparam.optoff = 0;
}

void
setvar(const char *name, const char *val, int flags)
{
	Var *v;

	v = makevar(name);
	if (v->flags & VREADONLY) {
		error("%s: is read only", name);
		/* NOTREACHED */
	}
	if (val) {
		free(v->val);
		v->val = xstrdup(val);
		v->flags &= ~VUNSET;
	}
	v->flags |= flags & (VEXPORT | VREADONLY);
	if (aflag)
		v->flags |= VEXPORT;
	sideeffect(v);
}

/*
 * One `name=value' string, as an assignment word gives it after
 * expansion.
 */
void
setvareq(const char *s, int flags)
{
	const char *eq;
	char *name;

	eq = strchr(s, '=');
	if (!eq) {
		setvar(s, NULL, flags);
		return;
	}
	name = xmalloc((size_t)(eq - s) + 1);
	memcpy(name, s, (size_t)(eq - s));
	name[eq - s] = '\0';
	setvar(name, eq + 1, flags);
	free(name);
}

int
unsetvar(const char *name)
{
	Var *v, **pp;
	unsigned h;

	h = hash(name, (int)strlen(name));
	for (pp = &vtab[h]; (v = *pp) != NULL; pp = &v->next) {
		if (strcmp(v->name, name) != 0)
			continue;
		if (v->flags & VREADONLY)
			return -1;
		*pp = v->next;
		if (v->flags & VSPECIAL) {
			v->flags &= ~(VEXPORT | VREADONLY);
			sideeffect(v);
		}
		free(v->name);
		free(v->val);
		free(v);
		return 0;
	}
	return 0;
}

void
exportvar(const char *name, int flags)
{
	Var *v;

	v = makevar(name);
	if ((flags & VREADONLY) && (v->flags & VUNSET) == 0)
		; /* readonly on a set variable needs no value */
	v->flags |= flags;
	sideeffect(v);
}

static int
varcmp(const void *a, const void *b)
{
	return strcmp((*(Var *const *)a)->name, (*(Var *const *)b)->name);
}

/*
 * Every variable, sorted, so that the output of `set', `export -p' and
 * `readonly -p' is stable and can be read back as shell input.
 */
static Var **
sortedvars(int *np)
{
	Var **v, *p;
	int n, i, j;

	n = 0;
	for (i = 0; i < NBUCKET; i++) {
		for (p = vtab[i]; p; p = p->next)
			n++;
	}
	v = arenaalloc(stk, sizeof(*v) * (size_t)(n ? n : 1));
	j = 0;
	for (i = 0; i < NBUCKET; i++) {
		for (p = vtab[i]; p; p = p->next)
			v[j++] = p;
	}
	if (n > 1)
		qsort(v, (size_t)n, sizeof(*v), varcmp);
	*np = n;
	return v;
}

/*
 * A value quoted so that the shell reads it back unchanged. Single
 * quotes are the only form that needs no other escape, and a single
 * quote inside the value ends the quoting, prints an escaped quote and
 * starts again.
 */
char *
singlequote(const char *s, Arena *a)
{
	SBuf b;

	sbinit(&b, a);
	sbputc(&b, '\'');
	for (; *s; s++) {
		if (*s == '\'')
			sbputs(&b, "'\\''");
		else
			sbputc(&b, *s);
	}
	sbputc(&b, '\'');
	return sbdone(&b);
}

void
listvars(int flags, const char *prefix)
{
	Var **v;
	int n, i;

	v = sortedvars(&n);
	for (i = 0; i < n; i++) {
		if (flags && !(v[i]->flags & flags))
			continue;
		if (v[i]->flags & VUNSET) {
			if (prefix)
				printf("%s %s\n", prefix, v[i]->name);
			continue;
		}
		if (prefix)
			printf("%s %s=%s\n", prefix, v[i]->name,
			       singlequote(v[i]->val, stk));
		else
			printf("%s=%s\n", v[i]->name,
			       singlequote(v[i]->val, stk));
	}
}

/*
 * The environment of a child: every exported variable that has a
 * value. The array lives in the transient arena, which is enough,
 * because the caller is about to call execve().
 */
char **
environment(void)
{
	Var **v;
	char **env;
	SBuf b;
	int n, i, j;

	v = sortedvars(&n);
	env = arenaalloc(stk, sizeof(*env) * (size_t)(n + 1));
	j = 0;
	for (i = 0; i < n; i++) {
		if (!(v[i]->flags & VEXPORT) || (v[i]->flags & VUNSET))
			continue;
		sbinit(&b, stk);
		sbputs(&b, v[i]->name);
		sbputc(&b, '=');
		sbputs(&b, v[i]->val);
		env[j++] = sbdone(&b);
	}
	env[j] = NULL;
	return env;
}

void
setparams(char **argv)
{
	int n;

	for (n = 0; argv[n]; n++)
		;
	shparam.p = argv;
	shparam.nparam = n;
	shparam.optind = 1;
	shparam.optoff = 0;
}

void
freeparams(char **argv)
{
	int i;

	if (!argv)
		return;
	for (i = 0; argv[i]; i++)
		free(argv[i]);
	free(argv);
}

Func *
lookupfunc(const char *name)
{
	Func *f;

	for (f = funcs; f; f = f->next) {
		if (strcmp(f->name, name) == 0)
			return f;
	}
	return NULL;
}

void
definefunc(const char *name, Node *body, Arena *a)
{
	Func *f;

	f = lookupfunc(name);
	if (f) {
		arenadrop(f->arena);
	} else {
		f = xmalloc(sizeof(*f));
		f->name = xstrdup(name);
		f->next = funcs;
		funcs = f;
	}
	f->body = body;
	f->arena = a;
	arenaretain(a);
}

int
unsetfunc(const char *name)
{
	Func *f, **pp;

	for (pp = &funcs; (f = *pp) != NULL; pp = &f->next) {
		if (strcmp(f->name, name) != 0)
			continue;
		*pp = f->next;
		arenadrop(f->arena);
		free(f->name);
		free(f);
		return 0;
	}
	return -1;
}

void
listfuncs(void)
{
	Func *f;

	for (f = funcs; f; f = f->next) {
		printf("%s() ", f->name);
		printtree(f->body, 1);
		printf("\n");
	}
}

void
varsinit(void)
{
	char **e;
	const char *p;
	char buf[32];

	for (e = environ; *e; e++) {
		if (strchr(*e, '='))
			setvareq(*e, VEXPORT);
	}
	if (!getvar("IFS"))
		setvar("IFS", " \t\n", 0);
	if (!getvar("PS1"))
		setvar("PS1", geteuid() == 0 ? "# " : "$ ", 0);
	if (!getvar("PS2"))
		setvar("PS2", "> ", 0);
	if (!getvar("PS4"))
		setvar("PS4", "+ ", 0);
	snprintf(buf, sizeof(buf), "%ld", (long)getppid());
	setvar("PPID", buf, 0);
	setvar("OPTIND", "1", 0);
	p = getvar("PWD");
	if (!p) {
		if (getcwd(buf, sizeof(buf)))
			setvar("PWD", buf, VEXPORT);
	}
}
