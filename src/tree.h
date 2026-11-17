/*
 * msh - a POSIX shell.
 *
 * The token set of section 2.3, the syntax tree of section 2.10, and
 * the interfaces of the input reader, the lexer and the parser.
 */
#ifndef TREE_H
#define TREE_H

#include "msh.h"

/*
 * Input.
 *
 * One interface serves three sources: a file descriptor, a string (for
 * `-c', `eval' and `.') and a terminal. Sources nest, so `eval' pushes
 * a string and pops it at end of input.
 */
enum { INFD,
       INSTR };

void pushfile(int fd, const char *name);
void pushstring(const char *s, const char *name);
void popinput(void);
int pgetc(void);
void pungetc(int c);
int inputlineno(void);
const char *inputname(void);
void resetinput(void);
int atinputeof(void);
void setprompt(int which);
void flushinput(void);
void syncinput(void);
void clearinputeof(void);

/*
 * Tokens. Every operator of section 2.10.2 has its own token, because
 * the grammar distinguishes them.
 */
enum {
	TEOF,
	TNL,
	TWORD,
	TIONUM,
	TSEMI,      /* ;  */
	TAND,       /* &  */
	TPIPE,      /* |  */
	TANDAND,    /* && */
	TOROR,      /* || */
	TDSEMI,     /* ;; */
	TLPAREN,    /* (  */
	TRPAREN,    /* )  */
	TLESS,      /* <  */
	TGREAT,     /* >  */
	TDLESS,     /* << */
	TDGREAT,    /* >> */
	TLESSAND,   /* <& */
	TGREATAND,  /* >& */
	TLESSGREAT, /* <> */
	TDLESSDASH, /* <<- */
	TCLOBBER,   /* >| */
	NTOKENS
};

extern const char *const tokname[NTOKENS];

typedef struct narg NArg;
typedef struct nredir NRedir;
typedef struct ncase NCase;
typedef struct node Node;

/*
 * Lexer flags. The parser tells the lexer what the grammar permits at
 * this point, because section 2.10 makes token recognition depend on
 * the parser state.
 */
enum { LEXCMD = 1 }; /* a reserved word or an alias may start here */

typedef struct token Token;
struct token {
	int type;
	char *text; /* TWORD, TIONUM: the token, quotes intact */
	int quoted; /* TWORD: the word held a quoting character */
	int lno;
};

void lexinit(Arena *a);
int readtoken(int flags);
void pushtoken(void);
Token *lasttoken(void);
void lexreset(void);
int lexheredocspending(void);
void heredocexpect(NRedir *r);
char *rmquotes(const char *s, Arena *a, int *quoted);

/* Node types of the syntax tree. */
enum {
	NCMD,      /* simple command */
	NPIPE,     /* pipeline */
	NAND,      /* && */
	NOR,       /* || */
	NSEMI,     /* two commands in sequence */
	NBACKGND,  /* & */
	NSUBSHELL, /* ( ... ) */
	NBRACE,    /* { ...; } */
	NIF,
	NWHILE,
	NUNTIL,
	NFOR,
	NCASE,
	NFUNC,
	NNOT /* ! pipeline */
};

/* Redirection types, in the order of section 2.7. */
enum {
	RIN,      /* <  */
	ROUT,     /* >  */
	RCLOBBER, /* >| */
	RAPPEND,  /* >> */
	RINOUT,   /* <> */
	RDUPIN,   /* <& */
	RDUPOUT,  /* >& */
	RHERE     /* << and <<- */
};

struct narg {
	NArg *next;
	char *text;
};

struct nredir {
	NRedir *next;
	int type;
	int fd;      /* the file descriptor that the redirection changes */
	char *fname; /* RIN..RDUPOUT: the word after the operator */
	char *hdoc;  /* RHERE: the body */
	int hquoted; /* RHERE: the delimiter was quoted, so do not expand */
	int hstrip;  /* RHERE: the operator was `<<-' */
};

struct ncase {
	NCase *next;
	NArg *pats;
	Node *body;
};

struct node {
	int type;
	int lno;
	Node *next; /* the members of a pipeline chain through this */
	NRedir *redirs;
	union {
		struct {
			NArg *assigns;
			NArg *args;
		} cmd;
		struct {
			Node *left;
			Node *right;
		} bin;
		struct {
			Node *child;
		} un;
		struct {
			Node *cmds; /* chained through Node.next */
			int n;
		} pipe;
		struct {
			Node *cond;
			Node *thn;
			Node *els;
		} ifn;
		struct {
			Node *cond;
			Node *body;
		} loop;
		struct {
			char *var;
			NArg *words;
			int hasin; /* 0: no `in', so loop over "$@" */
			Node *body;
		} forn;
		struct {
			char *word;
			NCase *items;
		} casen;
		struct {
			char *name;
			Node *body;
		} func;
	} u;
};

/*
 * The parser returns one tree per complete command, and the tree lives
 * in the arena that parsecmd() was given.
 */
enum { PSEOF = 1,
       PSERROR = 2 };

Node *parsecmd(Arena *a, int *stat);
void NORETURN synerror(const char *fmt, ...);
void printtree(Node *n, int fd);
char *treetext(Node *n, Arena *a);
void dumptree(Node *n, int fd);

/* Reserved words, resolved by the parser and not by the lexer. */
int reswordof(const char *s);
enum {
	RWNONE = 0,
	RWIF,
	RWTHEN,
	RWELSE,
	RWELIF,
	RWFI,
	RWDO,
	RWDONE,
	RWCASE,
	RWESAC,
	RWWHILE,
	RWUNTIL,
	RWFOR,
	RWLBRACE,
	RWRBRACE,
	RWBANG,
	RWIN
};

#endif /* TREE_H */
