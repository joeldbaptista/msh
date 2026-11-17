/*
 * msh - a POSIX shell.
 *
 * The `sh' utility: the options, the start-up sequence and the main
 * loop. The main loop is one sentence: parse one complete command, run
 * it, free it, and do it again.
 */
#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "builtin.h"
#include "exec.h"
#include "expand.h"
#include "jobs.h"
#include "msh.h"
#include "trap.h"
#include "tree.h"
#include "vars.h"

/*
 * One row per option, in the order of the enum in msh.h. A row holds
 * the long name for `set -o', the letter for the short form, and the
 * current setting, so nothing else in the shell spells an option name.
 */
struct option opts[NOPTS] = {
    {"errexit", 'e', 0},
    {"noglob", 'f', 0},
    {"nounset", 'u', 0},
    {"xtrace", 'x', 0},
    {"verbose", 'v', 0},
    {"noexec", 'n', 0},
    {"monitor", 'm', 0},
    {"notify", 'b', 0},
    {"noclobber", 'C', 0},
    {"allexport", 'a', 0},
    {"ignoreeof", 0, 0},
    {"nolog", 0, 0},
    {"vi", 0, 0},
    {"emacs", 0, 0},
    {"pipefail", 0, 0},
};

int debugtokens;
int debugtree;
int iflag;
int rootshell;
int shlvlpid;
int loopnest;
int funcnest;
int lastbgpid;
int lineno;
char *arg0 = (char *)"msh";

static void
usage(void)
{
	fprintf(stderr,
	        "usage: %s [-abCefhimnuvx] [-o option] [-c command | -s | "
	        "file] [argument ...]\n"
	        "       -T traces the token stream and -P prints the syntax "
	        "tree; both are extensions\n",
	        argv0);
	exit(2);
}

static int
setoption(int letter, int val)
{
	int i;

	if (letter == 'h')
		return 0;
	for (i = 0; i < NOPTS; i++) {
		if (opts[i].letter == letter) {
			opts[i].val = (char)val;
			return 0;
		}
	}
	return -1;
}

static int
setoptionname(const char *name, int val)
{
	int i;

	for (i = 0; i < NOPTS; i++) {
		if (strcmp(opts[i].name, name) == 0) {
			opts[i].val = (char)val;
			return 0;
		}
	}
	return -1;
}

/*
 * The main loop. `top' marks the outermost loop of the shell, which is
 * the one that prompts and the one that reports a stopped job; a `.'
 * script and an `eval' string run the same loop with `top' clear.
 */
int
cmdloop(int top)
{
	struct jmploc h;
	AMark m;
	Arena *volatile a;
	Node *n;
	volatile int status;
	int stat, depth;

	depth = inputdepth();
	status = exitstatus;
	a = NULL;
	arenamark(stk, &m);
	pushjmphandler(&h);
	for (;;) {
		if (setjmp(h.loc) != 0) {
			/*
			 * An error unwound to here. Everything that the
			 * abandoned command held has to go: its tree, its
			 * redirections, its half-read input and its
			 * transient memory.
			 */
			if (a) {
				arenadrop(a);
				a = NULL;
			}
			cmdarena = NULL;
			clearredir();
			clearvarsaves();
			lexreset();
			popinputto(depth);
			arenarelease(stk, &m);
			evalskip = SKIPNONE;
			skipcount = 0;
			status = exitstatus;
			if (!iflag)
				break;
			resetinput();
		}
		if (top && iflag) {
			checkjobs();
			setprompt(1);
		}
		stat = 0;
		a = arenanew();
		cmdarena = a;
		n = parsecmd(a, &stat);
		if (stat & PSEOF) {
			cmdarena = NULL;
			arenadrop(a);
			a = NULL;
			if (top && iflag && opts[OIGNOREEOF].val) {
				warn("use \"exit\" to leave the shell");
				clearinputeof();
				continue;
			}
			break;
		}
		if (n && debugtree)
			dumptree(n, 2);
		if (n)
			status = evaltree(n, 0);
		cmdarena = NULL;
		arenadrop(a);
		a = NULL;
		arenarelease(stk, &m);
		if (evalskip) {
			if (!top)
				break;
			evalskip = SKIPNONE;
			skipcount = 0;
		}
	}
	popjmphandler(&h);
	return status;
}

/*
 * The file named by ENV runs before an interactive session, after its
 * name has been expanded (the `sh' page).
 */
static void
readenvfile(void)
{
	struct jmploc h;
	const char *env;
	char *path;
	char *argv[2];

	env = getvar("ENV");
	if (!env || !*env)
		return;
	if (setjmphandler(&h) == 0) {
		path = expandone(env, EXTILDE);
		if (*path && access(path, R_OK) == 0) {
			argv[0] = (char *)".";
			argv[1] = path;
			bltin_dot(2, argv);
		}
	}
	popjmphandler(&h);
}

int
main(int argc, char **argv)
{
	char **params;
	char *cmdstr, *file, *a;
	int i, k, val, sflag, status;

	stk = arenanew();
	arg0 = argv[0] ? argv[0] : (char *)"msh";
	/*
	 * $0 keeps the name the shell was invoked with, path and all,
	 * but a diagnostic names the shell alone, as every other
	 * utility does.
	 */
	argv0 = strrchr(arg0, '/') ? strrchr(arg0, '/') + 1 : arg0;
	shlvlpid = (int)getpid();
	rootshell = 1;
	setvbuf(stdout, NULL, _IOLBF, 0);
	varsinit();
	jobsinit();
	cmdstr = NULL;
	file = NULL;
	sflag = 0;
	for (i = 1; i < argc; i++) {
		a = argv[i];
		if (a[0] != '-' && a[0] != '+')
			break;
		if (a[1] == '\0') {
			i++;
			break; /* a lone `-' ends the options */
		}
		if (a[0] == '-' && a[1] == '-' && a[2] == '\0') {
			i++;
			break;
		}
		val = a[0] == '-';
		for (k = 1; a[k]; k++) {
			switch (a[k]) {
			case 'c':
				if (i + 1 >= argc)
					usage();
				cmdstr = argv[++i];
				continue;
			case 's':
				sflag = 1;
				continue;
			case 'T':
				debugtokens = val;
				continue;
			case 'P':
				debugtree = val;
				continue;
			case 'i':
				iflag = val;
				continue;
			case 'o':
				if (i + 1 >= argc)
					usage();
				if (setoptionname(argv[++i], val) < 0) {
					warn("%s: bad option name",
					     argv[i]);
					exit(2);
				}
				continue;
			default:
				if (setoption(a[k], val) < 0)
					usage();
				continue;
			}
		}
	}
	if (cmdstr && i < argc)
		arg0 = argv[i++];
	if (!cmdstr && !sflag && i < argc) {
		file = argv[i++];
		arg0 = file;
	}
	params = xmalloc(sizeof(*params) * (size_t)(argc - i + 1));
	for (k = i; k < argc; k++)
		params[k - i] = xstrdup(argv[k]);
	params[argc - i] = NULL;
	setparams(params);
	if (!cmdstr && !file && isatty(0) && isatty(2))
		iflag = 1;
	trapsinit();
	if (cmdstr) {
		pushstring(cmdstr, NULL);
		status = cmdloop(1);
		exitshell(status);
		/* NOTREACHED */
	}
	if (file) {
		int fd;

		fd = open(file, O_RDONLY);
		if (fd < 0) {
			warn("%s: cannot open", file);
			exit(127);
		}
		fcntl(fd, F_SETFD, FD_CLOEXEC);
		pushfile(fd, file);
	} else {
		pushfile(0, NULL);
	}
	if (iflag) {
		setjobctl(1);
		readenvfile();
		setprompt(1);
	}
	status = cmdloop(1);
	if (iflag && jobwarning() > 0) {
		warn("there are stopped jobs");
		showjobs(0, NULL, 0);
		if (jobwarning() > 0) {
			exitstatus = status;
			return status;
		}
	}
	exitshell(status);
	/* NOTREACHED */
	return status;
}
