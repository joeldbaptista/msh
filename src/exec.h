/*
 * msh - a POSIX shell.
 *
 * The command search of section 2.9.1.1, the hash table of found
 * paths, redirections, and the evaluator.
 */
#ifndef EXEC_H
#define EXEC_H

#include "msh.h"
#include "tree.h"

/* What the command search found. The order is the search order. */
enum { CSPECIAL,
       CFUNC,
       CBUILTIN,
       CPATH,
       CNOTFOUND,
       CNOTEXEC };

typedef struct cmdentry CmdEntry;
struct cmdentry {
	int type;
	int idx;         /* CSPECIAL, CBUILTIN: index in the builtin table */
	char *path;      /* CPATH: the full path name */
	struct func *fn; /* CFUNC */
};

void findcommand(const char *name, CmdEntry *e, int usehash, int usefunc);
void hashcommand(const char *name);
void clearhash(int verbose);
void listhash(void);
void NORETURN execcommand(const char *path, char **argv);
const char *defpath(void);

/* Redirections. */
enum { RSAVE = 1,
       RNOFORK = 2 };

typedef struct redirsave RedirSave;
struct redirsave {
	RedirSave *prev;
	int n;
	int *from;
	int *to; /* -1 when the descriptor was closed */
};

void redirect(NRedir *r, int flags);
void popredir(void);
void dropredir(void);
void clearredir(void);
void clearvarsaves(void);
int savefd(int fd);
int openhere(NRedir *r);

/*
 * The evaluator.
 *
 * EVEXIT says that the shell has nothing to do after this command, so
 * the last command of a subshell can replace the process instead of
 * forking again. EVTESTED says that the status of this command is
 * being tested, which is one of the cases where `set -e' does not
 * apply (the `set' page, option -e).
 */
enum { EVFORK = 1,
       EVEXIT = 2,
       EVTESTED = 4 };

/* What stops the evaluator early. */
enum { SKIPNONE,
       SKIPBREAK,
       SKIPCONT,
       SKIPRETURN };

int evaltree(Node *n, int flags);
void evalstring(const char *s, const char *name);
int evalbuiltin(int idx, int argc, char **argv);
int evalextern(const char *path, char **argv);
void forkreset(void);
void xtrace(char **argv);

extern int evalskip;    /* SKIPNONE, or what is unwinding */
extern int skipcount;   /* the count of `break n' or `continue n' */
extern Arena *cmdarena; /* the parse arena of the command running now */

#endif /* EXEC_H */
