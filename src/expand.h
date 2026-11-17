/*
 * msh - a POSIX shell.
 *
 * Word expansion (section 2.6), pattern matching (section 2.13) and
 * arithmetic expansion.
 */
#ifndef EXPAND_H
#define EXPAND_H

#include "msh.h"
#include "tree.h"

/*
 * A field list. Expansion of one word gives zero, one or more fields,
 * and the caller collects the fields of every word of a command.
 */
typedef struct field Field;
struct field {
	Field *next;
	char *text;
};

typedef struct fieldlist FieldList;
struct fieldlist {
	Field *head;
	Field **tail;
	int n;
};

void flinit(FieldList *l);
void fladd(FieldList *l, char *s);
char **flargv(FieldList *l);

/* What the caller wants from expansion of one word. */
enum {
	EXSPLIT = 1,  /* apply field splitting */
	EXGLOB = 2,   /* apply pathname expansion */
	EXTILDE = 4,  /* apply tilde expansion at the start */
	EXASSIGN = 8, /* tilde also after `=' and `:', as an assignment */
	EXHERE = 16,  /* a here-document body, where quotes are literal */
	EXBRACE = 32  /* the word of a `${...}' form, where `}' escapes */
};

#define EXWORD (EXSPLIT | EXGLOB | EXTILDE)

void expandword(const char *word, int flags, FieldList *l);
char *expandone(const char *word, int flags);
char *expandassignvalue(const char *word);
char *expandhere(const char *body, int quoted);
void expandpattern(const char *word, EStr *e);
char *expandprompt(const char *s);

int patmatch(EStr *pat, const char *str);
int patmatchrange(EStr *pat, int from, int to, const char *str);
int patmatchlen(EStr *pat, int from, int to, const char *s, int slen);
void globfields(EStr *e, int from, int to, FieldList *l);
char *esrange(EStr *e, int from, int to);

long arith(const char *s);

/* Command substitution, used by expansion and by the prompt. */
char *cmdsubst(const char *cmd, int len, int backq);

#endif /* EXPAND_H */
