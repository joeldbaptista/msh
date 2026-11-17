/*
 * msh - a POSIX shell.
 *
 * The parameter store: shell variables with their attributes, the
 * positional parameters and the shell functions.
 */
#ifndef VARS_H
#define VARS_H

#include "msh.h"
#include "tree.h"

enum {
	VEXPORT = 1,   /* export -- pass to a child */
	VREADONLY = 2, /* readonly -- assignment is an error */
	VUNSET = 4,    /* the name exists but holds no value */
	VSPECIAL = 8   /* a change has a side effect, as PATH does */
};

typedef struct var Var;
struct var {
	Var *next;
	char *name; /* name only, no `=' */
	char *val;  /* NULL when VUNSET */
	int flags;
};

typedef struct func Func;
struct func {
	Func *next;
	char *name;
	Node *body;
	Arena *arena; /* retained, because the body lives in it */
};

void varsinit(void);
Var *lookupvar(const char *name);
const char *getvar(const char *name);
void setvar(const char *name, const char *val, int flags);
void setvareq(const char *s, int flags);
int unsetvar(const char *name);
void exportvar(const char *name, int flags);
char **environment(void);
void listvars(int flags, const char *prefix);
int validname(const char *s, int len);
void setparams(char **argv);
void freeparams(char **argv);
char *singlequote(const char *s, Arena *a);

Func *lookupfunc(const char *name);
void definefunc(const char *name, Node *body, Arena *a);
int unsetfunc(const char *name);
void listfuncs(void);

/* PATH changes invalidate the command hash table, so vars.c tells it. */
void pathchanged(void);

#endif /* VARS_H */
