/*
 * msh - a POSIX shell.
 *
 * The tree-walk evaluator. One function handles one node type, and a
 * table of function pointers indexed by the node type dispatches to
 * it, so the evaluator carries no switch over node types and a new
 * node type costs one row.
 *
 * Two pieces of state leave a command early. `evalskip' says that a
 * break, a continue or a return is unwinding, and the loops and the
 * function call clear it when it reaches them; that is simpler than a
 * longjmp, because the counts of `break n' belong to the loops that
 * lexically enclose the command. An error, in contrast, does use a
 * longjmp, because no intermediate frame wants to see it.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "builtin.h"
#include "exec.h"
#include "expand.h"
#include "jobs.h"
#include "msh.h"
#include "trap.h"
#include "tree.h"
#include "vars.h"

#define MAXFUNCNEST 500

int evalskip;
int skipcount;
Arena *cmdarena;

static int evalcmd(Node *n, int flags);
static int evalpipe(Node *n, int flags);
static int evalbin(Node *n, int flags);
static int evalsemi(Node *n, int flags);
static int evalbackgnd(Node *n, int flags);
static int evalsubshell(Node *n, int flags);
static int evalbrace(Node *n, int flags);
static int evalif(Node *n, int flags);
static int evalloop(Node *n, int flags);
static int evalfor(Node *n, int flags);
static int evalcase(Node *n, int flags);
static int evalfuncdef(Node *n, int flags);
static int evalnot(Node *n, int flags);

/* One row per node type, in the order of the enum in tree.h. */
static int (*const evaluators[])(Node *, int) = {
    evalcmd,      /* NCMD */
    evalpipe,     /* NPIPE */
    evalbin,      /* NAND */
    evalbin,      /* NOR */
    evalsemi,     /* NSEMI */
    evalbackgnd,  /* NBACKGND */
    evalsubshell, /* NSUBSHELL */
    evalbrace,    /* NBRACE */
    evalif,       /* NIF */
    evalloop,     /* NWHILE */
    evalloop,     /* NUNTIL */
    evalfor,      /* NFOR */
    evalcase,     /* NCASE */
    evalfuncdef,  /* NFUNC */
    evalnot       /* NNOT */
};

/*
 * Whether `set -e' applies to the status of this node. A list
 * operator, a negation and an asynchronous list are exempt by
 * construction, because the status they report is not the status of a
 * command that failed on its own.
 */
static int
checkexitnode(int type)
{
	switch (type) {
	case NAND:
	case NOR:
	case NSEMI:
	case NNOT:
	case NBACKGND:
	case NFUNC:
		return 0;
	}
	return 1;
}

int
evaltree(Node *n, int flags)
{
	char buf[32];
	int status, check;

	if (!n)
		return exitstatus;
	if (nflag)
		return 0;
	snprintf(buf, sizeof(buf), "%d", n->lno);
	setvar("LINENO", buf, 0);
	check = checkexitnode(n->type) && !(flags & EVTESTED);
	if (n->redirs && n->type != NCMD) {
		redirect(n->redirs, RSAVE);
		status = evaluators[n->type](n, flags);
		popredir();
	} else {
		status = evaluators[n->type](n, flags);
	}
	exitstatus = status;
	if (check && eflag && status != 0 && !evalskip)
		exitshell(status);
	dotrap();
	/*
	 * With `set -b' a finished background job is reported at once
	 * rather than before the next prompt (the `set' page, option
	 * -b). After a command is the earliest safe point.
	 */
	if (bflag && iflag)
		checkjobs();
	return status;
}

static int
evalsemi(Node *n, int flags)
{
	int status;

	status = evaltree(n->u.bin.left, flags);
	if (evalskip)
		return status;
	return evaltree(n->u.bin.right, flags);
}

static int
evalbin(Node *n, int flags)
{
	int status;

	status = evaltree(n->u.bin.left, flags | EVTESTED);
	if (evalskip)
		return status;
	if (n->type == NAND ? status != 0 : status == 0)
		return status;
	return evaltree(n->u.bin.right, flags);
}

static int
evalnot(Node *n, int flags)
{
	int status;

	status = evaltree(n->u.un.child, flags | EVTESTED);
	if (evalskip)
		return status;
	return status == 0 ? 1 : 0;
}

/*
 * What a forked child has to forget: it is no longer the shell that
 * the user is talking to, it owns no jobs, and section 2.12 resets
 * every trap that the parent caught.
 */
void
forkreset(void)
{
	rootshell = 0;
	iflag = 0;
	mflag = 0;
	setprompt(0);
	clearjobs();
	clearcaughttraps();
}

static int
evalbrace(Node *n, int flags)
{
	return evaltree(n->u.un.child, flags);
}

static int
evalsubshell(Node *n, int flags)
{
	Job *j;
	pid_t pid;
	int status;

	flushall();
	j = makejob(1, treetext(n, stk));
	pid = forkchild(j, 0);
	if (pid == 0) {
		forkreset();
		evaltree(n->u.un.child, flags | EVEXIT);
		exitshell(exitstatus);
		/* NOTREACHED */
	}
	status = waitforjob(j);
	releasejob(j);
	return status;
}

static int
evalbackgnd(Node *n, int flags)
{
	Job *j;
	pid_t pid;
	int fd;

	(void)flags;
	flushall();
	j = makejob(1, treetext(n->u.un.child, stk));
	pid = forkchild(j, 1);
	if (pid == 0) {
		forkreset();
		/*
		 * Section 2.9.3: with job control off, standard input of
		 * an asynchronous list is /dev/null unless the list
		 * redirects it itself.
		 */
		if (!mflag) {
			fd = open("/dev/null", O_RDONLY);
			if (fd >= 0) {
				dup2(fd, 0);
				if (fd != 0)
					close(fd);
			}
			ignoresig(SIGINT);
			ignoresig(SIGQUIT);
		}
		evaltree(n->u.un.child, EVEXIT);
		exitshell(exitstatus);
		/* NOTREACHED */
	}
	lastbgpid = (int)pid;
	if (iflag)
		fprintf(stderr, "[%d] %ld\n", jobidof(j), (long)pid);
	return 0;
}

/*
 * A pipeline. One pipe joins each pair of commands, one child runs
 * each command, and the shell waits for the last one. Every descriptor
 * that a child does not need is closed in that child, or the reader
 * would never see end of file.
 */
static int
evalpipe(Node *n, int flags)
{
	Node *c;
	Job *j;
	int pip[2], prevfd, status, i;

	(void)flags;
	flushall();
	j = makejob(n->u.pipe.n, treetext(n, stk));
	prevfd = -1;
	i = 0;
	for (c = n->u.pipe.cmds; c; c = c->next, i++) {
		pip[0] = -1;
		pip[1] = -1;
		if (c->next && pipe(pip) < 0)
			error("cannot create pipe: %s", strerror(errno));
		if (forkchild(j, 0) == 0) {
			forkreset();
			if (prevfd >= 0) {
				dup2(prevfd, 0);
				close(prevfd);
			}
			if (pip[1] >= 0) {
				close(pip[0]);
				if (pip[1] != 1) {
					dup2(pip[1], 1);
					close(pip[1]);
				}
			}
			evaltree(c, EVEXIT);
			exitshell(exitstatus);
			/* NOTREACHED */
		}
		if (prevfd >= 0)
			close(prevfd);
		if (pip[1] >= 0)
			close(pip[1]);
		prevfd = pip[0];
	}
	if (prevfd >= 0)
		close(prevfd);
	status = waitforjob(j);
	if (pipefail) {
		for (i = 0; i < j->nproc; i++) {
			if (j->ps[i].status != 0)
				status = j->ps[i].status;
		}
	}
	releasejob(j);
	return status;
}

static int
evalif(Node *n, int flags)
{
	int status;

	status = evaltree(n->u.ifn.cond, flags | EVTESTED);
	if (evalskip)
		return status;
	if (status == 0)
		return evaltree(n->u.ifn.thn, flags);
	if (n->u.ifn.els)
		return evaltree(n->u.ifn.els, flags);
	return 0;
}

/*
 * Act on a break or a continue that has reached a loop. The result
 * says whether this loop stops.
 */
static int
skiploop(void)
{
	if (evalskip == SKIPRETURN)
		return 1;
	if (--skipcount > 0)
		return 1; /* an outer loop is the target */
	skipcount = 0;
	if (evalskip == SKIPCONT) {
		evalskip = SKIPNONE;
		return 0;
	}
	evalskip = SKIPNONE;
	return 1;
}

static int
evalloop(Node *n, int flags)
{
	int status, cond;

	status = 0;
	loopnest++;
	for (;;) {
		cond = evaltree(n->u.loop.cond, flags | EVTESTED);
		if (evalskip) {
			if (skiploop())
				break;
			continue;
		}
		if (n->type == NWHILE ? cond != 0 : cond == 0)
			break;
		status = evaltree(n->u.loop.body, flags);
		if (evalskip && skiploop())
			break;
	}
	loopnest--;
	return status;
}

static int
evalfor(Node *n, int flags)
{
	FieldList l;
	NArg *a;
	Field *f;
	AMark m;
	int status, i;

	arenamark(stk, &m);
	flinit(&l);
	if (n->u.forn.hasin) {
		for (a = n->u.forn.words; a; a = a->next)
			expandword(a->text, EXWORD, &l);
	} else {
		for (i = 0; i < shparam.nparam; i++)
			fladd(&l, shparam.p[i]);
	}
	status = 0;
	loopnest++;
	for (f = l.head; f; f = f->next) {
		setvar(n->u.forn.var, f->text, 0);
		status = evaltree(n->u.forn.body, flags);
		if (evalskip && skiploop())
			break;
	}
	loopnest--;
	arenarelease(stk, &m);
	return status;
}

/*
 * A `case'. Section 2.9.4.3 expands each pattern only until one
 * matches, so the loop stops at the first match.
 */
static int
evalcase(Node *n, int flags)
{
	EStr pat;
	NCase *c;
	NArg *a;
	AMark m;
	char *word;
	int status;

	arenamark(stk, &m);
	word = expandone(n->u.casen.word, EXTILDE);
	status = 0;
	for (c = n->u.casen.items; c; c = c->next) {
		for (a = c->pats; a; a = a->next) {
			esinit(&pat);
			expandpattern(a->text, &pat);
			if (!patmatch(&pat, word))
				continue;
			arenarelease(stk, &m);
			return evaltree(c->body, flags);
		}
	}
	arenarelease(stk, &m);
	return status;
}

static int
evalfuncdef(Node *n, int flags)
{
	(void)flags;
	if (!cmdarena)
		error("%s: cannot define a function here", n->u.func.name);
	definefunc(n->u.func.name, n->u.func.body, cmdarena);
	return 0;
}

/* A copy of an argument vector on the heap, as the shell's own is. */
static char **
dupargv(char **argv, int from)
{
	char **v;
	int n, i;

	for (n = 0; argv[n]; n++)
		;
	v = xmalloc(sizeof(*v) * (size_t)(n - from + 1));
	for (i = from; i < n; i++)
		v[i - from] = xstrdup(argv[i]);
	v[n - from] = NULL;
	return v;
}

/*
 * A function call. The positional parameters belong to the call, so
 * they are saved and restored; `return' unwinds to here.
 */
static int
evalfunc(Func *fn, char **argv, int flags)
{
	struct shparam saved;
	struct jmploc h;
	Arena *savearena;
	int status, savenest, saveloop;

	if (funcnest >= MAXFUNCNEST)
		error("%s: too deeply nested", fn->name);
	saved = shparam;
	setparams(dupargv(argv, 1));
	savenest = funcnest;
	saveloop = loopnest;
	savearena = cmdarena;
	funcnest++;
	loopnest = 0;
	cmdarena = fn->arena;
	arenaretain(fn->arena);
	if (setjmphandler(&h) == 0) {
		status = evaltree(fn->body, flags & ~EVEXIT);
	} else {
		popjmphandler(&h);
		freeparams(shparam.p);
		shparam = saved;
		funcnest = savenest;
		loopnest = saveloop;
		cmdarena = savearena;
		arenadrop(fn->arena);
		errexit();
		/* NOTREACHED */
		return 2;
	}
	popjmphandler(&h);
	if (evalskip == SKIPRETURN) {
		evalskip = SKIPNONE;
		skipcount = 0;
		status = exitstatus;
	}
	freeparams(shparam.p);
	shparam = saved;
	funcnest = savenest;
	loopnest = saveloop;
	cmdarena = savearena;
	arenadrop(fn->arena);
	return status;
}

void
xtrace(char **argv)
{
	const char *ps4;
	char *p;
	int i;

	ps4 = getvar("PS4");
	p = expandprompt(ps4 ? ps4 : "+ ");
	fflush(stdout);
	fputs(p, stderr);
	for (i = 0; argv[i]; i++)
		fprintf(stderr, "%s%s", i ? " " : "", argv[i]);
	fputc('\n', stderr);
	fflush(stderr);
}

/*
 * Run one built-in. Section 2.8.1 makes an error in a special built-in
 * end a non-interactive shell, and leaves a regular built-in only
 * failing, so the error is caught here and re-raised only for the
 * special ones. The flag in the table decides, and no built-in has to
 * know the rule.
 */
int
evalbuiltin(int idx, int argc, char **argv)
{
	struct jmploc h;
	int status;

	if (setjmphandler(&h) != 0) {
		popjmphandler(&h);
		fflush(stdout);
		if (builtins[idx].flags & BSPECIAL)
			errexit();
		return 2;
	}
	status = builtins[idx].fn(argc, argv);
	popjmphandler(&h);
	fflush(stdout);
	return status;
}

/*
 * Run an external command in a child and wait for it. The `command'
 * built-in needs this on its own, so it is not folded into evalcmd().
 */
int
evalextern(const char *path, char **argv)
{
	Job *j;
	int status;

	flushall();
	j = makejob(1, argv[0]);
	if (forkchild(j, 0) == 0) {
		forkreset();
		defaultsigs();
		execcommand(path, argv);
		/* NOTREACHED */
	}
	status = waitforjob(j);
	releasejob(j);
	return status;
}

/*
 * The saved state of the variables that an assignment prefix
 * displaced. Section 2.9.1 keeps such an assignment out of the shell
 * when the command is a regular built-in, so the old values come back
 * afterwards.
 *
 * The records are on a stack of their own, and on the heap, for the
 * same reason the saved file descriptors are: an error jumps over the
 * code that would restore them, and the main loop has to be able to
 * restore them itself.
 */
typedef struct varsave VarSave;
struct varsave {
	VarSave *prev;
	int n;
	char **name;
	char **val; /* NULL when the variable was unset */
	int *flags;
};

static VarSave *varsavestack;

static void
savevars(char **names, int n)
{
	VarSave *sv;
	Var *v;
	int i;

	sv = xmalloc(sizeof(*sv));
	sv->n = n;
	sv->name = xmalloc(sizeof(*sv->name) * (size_t)(n ? n : 1));
	sv->val = xmalloc(sizeof(*sv->val) * (size_t)(n ? n : 1));
	sv->flags = xmalloc(sizeof(*sv->flags) * (size_t)(n ? n : 1));
	for (i = 0; i < n; i++) {
		sv->name[i] = xstrdup(names[i]);
		v = lookupvar(names[i]);
		sv->val[i] = v && !(v->flags & VUNSET) ? xstrdup(v->val)
		                                       : NULL;
		sv->flags[i] = v ? v->flags : 0;
	}
	sv->prev = varsavestack;
	varsavestack = sv;
}

static void
restorevars(void)
{
	VarSave *sv;
	int i;

	sv = varsavestack;
	if (!sv)
		return;
	varsavestack = sv->prev;
	for (i = 0; i < sv->n; i++) {
		/*
		 * A read-only variable cannot have been changed by the
		 * prefix, so it has nothing to restore, and writing to
		 * it here would raise a second error inside the first.
		 */
		if (sv->flags[i] & VREADONLY) {
			free(sv->name[i]);
			free(sv->val[i]);
			continue;
		}
		if (sv->val[i])
			setvar(sv->name[i], sv->val[i],
			       sv->flags[i] & VEXPORT);
		else
			unsetvar(sv->name[i]);
		free(sv->name[i]);
		free(sv->val[i]);
	}
	free(sv->name);
	free(sv->val);
	free(sv->flags);
	free(sv);
}

void
clearvarsaves(void)
{
	while (varsavestack)
		restorevars();
}

/*
 * A simple command. The order of the steps is the order of section
 * 2.9.1: expand the words that are neither assignments nor
 * redirections, perform the redirections, then expand and apply the
 * assignments.
 */
static int
evalcmd(Node *n, int flags)
{
	FieldList args;
	CmdEntry e;
	NArg *a;
	AMark m;
	char **argv, **names;
	char *val;
	int argc, nassign, i, status, isexec, special;

	arenamark(stk, &m);
	flinit(&args);
	for (a = n->u.cmd.args; a; a = a->next)
		expandword(a->text, EXWORD, &args);
	argv = flargv(&args);
	argc = args.n;
	nassign = 0;
	for (a = n->u.cmd.assigns; a; a = a->next)
		nassign++;

	if (argc == 0) {
		/*
		 * Only assignments and redirections. The status is the
		 * status of the last command substitution that the
		 * assignments contained, and zero when there was none.
		 */
		if (n->redirs) {
			redirect(n->redirs, RSAVE);
			popredir();
		}
		exitstatus = 0;
		for (a = n->u.cmd.assigns; a; a = a->next) {
			val = expandassignvalue(a->text);
			if (xflag) {
				char *v[2];

				v[0] = val;
				v[1] = NULL;
				xtrace(v);
			}
			setvareq(val, 0);
		}
		status = exitstatus;
		arenarelease(stk, &m);
		return status;
	}

	if (xflag)
		xtrace(argv);
	findcommand(argv[0], &e, 1, 1);
	isexec = e.type == CSPECIAL &&
	         strcmp(builtins[e.idx].name, "exec") == 0;
	special = e.type == CSPECIAL || e.type == CFUNC;

	if (e.type == CPATH) {
		/* An external command runs in a child of this shell. */
		Job *j;

		flushall();
		j = makejob(1, treetext(n, stk));
		if (forkchild(j, 0) == 0) {
			forkreset();
			defaultsigs();
			for (a = n->u.cmd.assigns; a; a = a->next)
				setvareq(expandassignvalue(a->text), VEXPORT);
			redirect(n->redirs, 0);
			execcommand(e.path, argv);
			/* NOTREACHED */
		}
		status = waitforjob(j);
		releasejob(j);
		arenarelease(stk, &m);
		return status;
	}
	if (e.type == CNOTFOUND || e.type == CNOTEXEC) {
		warn("%s: %s", argv[0],
		     e.type == CNOTFOUND ? "not found" : "cannot execute");
		arenarelease(stk, &m);
		return e.type == CNOTFOUND ? 127 : 126;
	}

	/* A built-in or a function, which run in this shell process. */
	names = NULL;
	if (n->redirs)
		redirect(n->redirs, isexec ? 0 : RSAVE);
	if (nassign > 0 && !special) {
		names = arenaalloc(stk, sizeof(*names) * (size_t)nassign);
		i = 0;
		for (a = n->u.cmd.assigns; a; a = a->next) {
			names[i] = arenastrdup(stk, a->text);
			*strchr(names[i], '=') = '\0';
			i++;
		}
		savevars(names, nassign);
	}
	for (a = n->u.cmd.assigns; a; a = a->next)
		setvareq(expandassignvalue(a->text), special ? 0 : VEXPORT);

	if (e.type == CFUNC)
		status = evalfunc(e.fn, argv, flags);
	else
		status = evalbuiltin(e.idx, argc, argv);

	if (nassign > 0 && !special)
		restorevars();
	if (n->redirs && !isexec)
		popredir();
	arenarelease(stk, &m);
	return status;
}

/*
 * Parse and run a string: the `-c' option, `eval', `.' and a command
 * substitution all arrive here. Each complete command gets its own
 * parse arena, which a function definition can keep.
 */
void
evalstring(const char *s, const char *name)
{
	struct jmploc h;
	Arena *volatile a;
	Node *n;
	int stat;

	pushstring(s, name);
	a = NULL;
	for (;;) {
		a = arenanew();
		if (setjmphandler(&h) != 0) {
			popjmphandler(&h);
			arenadrop(a);
			popinput();
			errexit();
			/* NOTREACHED */
		}
		n = parsecmd(a, &stat);
		if (stat & PSEOF) {
			popjmphandler(&h);
			arenadrop(a);
			break;
		}
		if (n) {
			Arena *saved;

			saved = cmdarena;
			cmdarena = a;
			evaltree(n, 0);
			cmdarena = saved;
		}
		popjmphandler(&h);
		arenadrop(a);
		if (evalskip)
			break;
	}
	popinput();
}

/*
 * Command substitution. The command runs in a child, which is both
 * what section 2.12 requires and what keeps the parent free of the
 * child's parse state. The parent reads the output and drops the
 * trailing newlines.
 */
char *
cmdsubst(const char *cmd, int len, int backq)
{
	SBuf b;
	char *text, *res;
	char buf[1024];
	pid_t pid;
	int pip[2], n, status;

	text = arenastrndup(stk, cmd, (size_t)len);
	if (backq) {
		/*
		 * Inside backquotes a backslash escapes `$', a
		 * backquote and a backslash, and nothing else, so undo
		 * those before the text becomes shell input.
		 */
		SBuf u;
		char *p;

		sbinit(&u, stk);
		for (p = text; *p; p++) {
			if (*p == '\\' &&
			    (p[1] == '$' || p[1] == '`' || p[1] == '\\'))
				p++;
			sbputc(&u, *p);
		}
		text = sbdone(&u);
	}
	flushall();
	if (pipe(pip) < 0)
		error("cannot create pipe: %s", strerror(errno));
	pid = fork();
	if (pid < 0) {
		close(pip[0]);
		close(pip[1]);
		error("cannot fork: %s", strerror(errno));
	}
	if (pid == 0) {
		close(pip[0]);
		if (pip[1] != 1) {
			dup2(pip[1], 1);
			close(pip[1]);
		}
		forkreset();
		handler = NULL;
		evalskip = SKIPNONE;
		evalstring(text, NULL);
		exitshell(exitstatus);
		/* NOTREACHED */
	}
	close(pip[1]);
	sbinit(&b, stk);
	for (;;) {
		n = (int)read(pip[0], buf, sizeof(buf));
		if (n > 0) {
			sbputn(&b, buf, n);
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		break;
	}
	close(pip[0]);
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			break;
	}
	exitstatus = WIFEXITED(status) ? WEXITSTATUS(status)
	                               : 128 + WTERMSIG(status);
	res = sbdone(&b);
	while (b.len > 0 && res[b.len - 1] == '\n')
		res[--b.len] = '\0';
	return res;
}
