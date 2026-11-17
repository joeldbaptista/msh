/*
 * msh - a POSIX shell.
 *
 * The built-in utilities. The table is the whole description of a
 * built-in: its name, its behaviour as a function pointer, and the
 * flags that the command search and section 2.14 need. So adding a
 * built-in means writing one function and adding one row.
 */
#ifndef BUILTIN_H
#define BUILTIN_H

#include "msh.h"

enum {
	BSPECIAL = 1, /* a special built-in of section 2.14 */
	BASSIGN = 2,  /* an assignment before it stays in effect */
	BNOFORK = 4   /* must run in the shell process, even in a pipeline */
};

typedef int (*BuiltinFn)(int argc, char **argv);

struct builtin {
	const char *name;
	BuiltinFn fn;
	int flags;
};

extern const struct builtin builtins[];
extern const int nbuiltins;

int findbuiltin(const char *name);

/*
 * A small option scanner for the built-ins. The shell cannot use the
 * one in the C library, because `getopts' is itself a built-in with
 * its own state, and the two would share the library's globals.
 */
typedef struct bopt BOpt;
struct bopt {
	char **argv;
	int argc;
	int ind;   /* the next argument to look at */
	int off;   /* the next letter inside that argument */
	char *arg; /* the option argument, when the option takes one */
	int opt;   /* the letter that failed, for the message */
	int plus;  /* the option came with `+' instead of `-' */
};

void boptinit(BOpt *o, int argc, char **argv);
int bopt(BOpt *o, const char *spec);
long bnumber(const char *s, const char *what);

int bltin_true(int argc, char **argv);
int bltin_false(int argc, char **argv);
int bltin_colon(int argc, char **argv);
int bltin_exit(int argc, char **argv);
int bltin_shift(int argc, char **argv);
int bltin_times(int argc, char **argv);
int bltin_break(int argc, char **argv);
int bltin_continue(int argc, char **argv);
int bltin_return(int argc, char **argv);
int bltin_eval(int argc, char **argv);
int bltin_exec(int argc, char **argv);
int bltin_dot(int argc, char **argv);
int bltin_set(int argc, char **argv);
int bltin_export(int argc, char **argv);
int bltin_readonly(int argc, char **argv);
int bltin_unset(int argc, char **argv);
int bltin_cd(int argc, char **argv);
int bltin_pwd(int argc, char **argv);
int bltin_umask(int argc, char **argv);
int bltin_read(int argc, char **argv);
int bltin_getopts(int argc, char **argv);
int bltin_command(int argc, char **argv);
int bltin_type(int argc, char **argv);
int bltin_hash(int argc, char **argv);
int bltin_alias(int argc, char **argv);
int bltin_unalias(int argc, char **argv);
int bltin_trap(int argc, char **argv);
int bltin_jobs(int argc, char **argv);
int bltin_wait(int argc, char **argv);
int bltin_fg(int argc, char **argv);
int bltin_bg(int argc, char **argv);
int bltin_ulimit(int argc, char **argv);

/* Alias substitution belongs to the lexer, and the store belongs here. */
typedef struct alias Alias;
struct alias {
	Alias *next;
	char *name;
	char *val;
	int inuse;
};

Alias *lookupalias(const char *name);
void setalias(const char *name, const char *val);
int unsetalias(const char *name);
void unsetallaliases(void);
void listaliases(void);

#endif /* BUILTIN_H */
