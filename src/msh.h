/*
 * msh - a POSIX shell.
 *
 * Common declarations: arenas, string buffers, expanded strings,
 * error handling, shell options and global shell state.
 */
#ifndef MSH_H
#define MSH_H

#include <setjmp.h>
#include <stddef.h>

#define LEN(a) (sizeof(a) / sizeof((a)[0]))

#ifdef __GNUC__
#define NORETURN __attribute__((noreturn))
#else
#define NORETURN
#endif

/*
 * Memory.
 *
 * An arena is a chain of blocks that the program frees as one unit.
 * Three arenas exist. The parse arena of a complete command holds its
 * syntax tree, and lives until the command has run. The transient
 * arena holds the strings that expansion produces, and a mark and a
 * release bracket every simple command, so a loop does not grow it.
 * Long-lived state, such as variables and functions, lives on the
 * normal heap.
 *
 * A mark is valid only while no older block has been released, and a
 * release frees every block that the arena gained after the mark. A
 * longjmp over a release is safe, because the main loop releases the
 * transient arena to its own mark.
 */

typedef struct ablock ABlock;
typedef struct arena Arena;
typedef struct amark AMark;

struct ablock {
	ABlock *next;
	char *mem;
	size_t cap;
	size_t used;
};

struct arena {
	ABlock *cur;
	int refs;
};

struct amark {
	ABlock *blk;
	size_t used;
};

Arena *arenanew(void);
void *arenaalloc(Arena *a, size_t n);
char *arenastrdup(Arena *a, const char *s);
char *arenastrndup(Arena *a, const char *s, size_t n);
void arenamark(Arena *a, AMark *m);
void arenarelease(Arena *a, AMark *m);
void arenaretain(Arena *a);
void arenadrop(Arena *a);

extern Arena *stk; /* the transient arena */

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/*
 * A growable byte buffer over one arena.
 */
typedef struct sbuf SBuf;
struct sbuf {
	Arena *a;
	char *s;
	int len;
	int cap;
};

void sbinit(SBuf *b, Arena *a);
void sbputc(SBuf *b, int c);
void sbputn(SBuf *b, const char *s, int n);
void sbputs(SBuf *b, const char *s);
char *sbdone(SBuf *b);

/*
 * An expanded string: the bytes, plus one flag per byte that says
 * whether the byte came from inside quotes. Field splitting, pathname
 * expansion and pattern matching all have to skip quoted characters,
 * and quote removal happens only at the end, so the flag has to
 * survive until then.
 *
 * A byte of 0 is a hard field break, which $@ and "$@" produce between
 * parameters. A shell word can hold no other 0 byte, so the marker
 * needs no escape, and its own flag records whether that $@ was
 * quoted, which is what tells an empty parameter from no parameter.
 */
typedef struct estr EStr;
struct estr {
	char *s;
	char *q;
	int len;
	int cap;
};

void esinit(EStr *e);
void esputc(EStr *e, int c, int quoted);
void esputn(EStr *e, const char *s, int n, int quoted);
void esputs(EStr *e, const char *s, int quoted);
void esbreak(EStr *e, int quoted);
char *escstr(EStr *e); /* quote removal: bytes only, NUL-terminated */

/*
 * Errors.
 *
 * A syntax error or an expansion error abandons the current command
 * from deep inside the call stack, so error() reports and jumps to the
 * nearest handler. The handler chain reaches the main loop.
 */
enum { EXNONE,
       EXERROR,
       EXEXIT };

struct jmploc {
	jmp_buf loc;
	struct jmploc *prev;
};

extern struct jmploc *handler;
extern int exception;
extern int exitstatus;
extern const char *argv0;

void warn(const char *fmt, ...);
void NORETURN error(const char *fmt, ...);
void NORETURN errexit(void);
void NORETURN exitshell(int status);
void intoff(void);
void inton(void);

#define setjmphandler(h) \
	((h)->prev = handler, handler = (h), setjmp((h)->loc))
#define popjmphandler(h) (handler = (h)->prev)

/*
 * The main loop pushes its handler once and then calls setjmp() again
 * on every pass, because a longjmp leaves the handler chain as it was
 * and pushing again would make the chain point at itself.
 */
#define pushjmphandler(h) ((h)->prev = handler, handler = (h))

/*
 * Options. The table is the single description of every option, so
 * `set -o`, `set +o` and the short-letter forms all read from it.
 */
enum {
	OERREXIT,
	ONOGLOB,
	ONOUNSET,
	OXTRACE,
	OVERBOSE,
	ONOEXEC,
	OMONITOR,
	ONOTIFY,
	ONOCLOBBER,
	OALLEXPORT,
	OIGNOREEOF,
	ONOLOG,
	OVI,
	OEMACS,
	OPIPEFAIL,
	NOPTS
};

struct option {
	const char *name;
	char letter;
	char val;
};

extern struct option opts[NOPTS];

#define eflag opts[OERREXIT].val
#define fflag opts[ONOGLOB].val
#define uflag opts[ONOUNSET].val
#define xflag opts[OXTRACE].val
#define vflag opts[OVERBOSE].val
#define nflag opts[ONOEXEC].val
#define mflag opts[OMONITOR].val
#define bflag opts[ONOTIFY].val
#define Cflag opts[ONOCLOBBER].val
#define aflag opts[OALLEXPORT].val
#define pipefail opts[OPIPEFAIL].val

/*
 * Two debugging switches, which are extensions and so have no place in
 * the option table: `-T' prints the token stream as the lexer produces
 * it, and `-P' prints the syntax tree of each complete command. Both
 * write to standard error.
 */
extern int debugtokens;
extern int debugtree;

/* Shell state that every module reads. */
extern int iflag;     /* interactive */
extern int rootshell; /* nonzero in the original shell process */
extern int shlvlpid;  /* pid of the original shell process */
extern int loopnest;  /* loops that lexically enclose the command */
extern int funcnest;  /* function calls in progress */
extern int dotnest;   /* `.' scripts in progress */
extern char *arg0;    /* $0 */
extern int lastbgpid; /* $! */
extern int lineno;    /* LINENO */
extern char **environ;

/*
 * Positional parameters. `p' is a NULL-terminated vector, and optind
 * and optoff hold the state of getopts.
 */
struct shparam {
	int nparam;
	char **p;
	int optind;
	int optoff;
};

extern struct shparam shparam;

int cmdloop(int top);
int inputdepth(void);
void popinputto(int depth);
void NORETURN shellexec(char **argv, const char *path, int idx);
char *padvance(const char **path, const char *name);
int ispathname(const char *s);

#endif /* MSH_H */
