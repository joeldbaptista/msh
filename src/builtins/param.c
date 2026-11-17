/*
 * msh - a POSIX shell.
 *
 * The built-ins that work on parameters and options: `set', `export',
 * `readonly', `unset', `getopts' and `read'.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../builtin.h"
#include "../exec.h"
#include "../expand.h"
#include "../jobs.h"
#include "../msh.h"
#include "../trap.h"
#include "../tree.h"
#include "../vars.h"

static int
setoptbyletter(int letter, int val)
{
	int i;

	if (letter == 'h')
		return 0; /* this shell always remembers a found path */
	for (i = 0; i < NOPTS; i++) {
		if (opts[i].letter != letter)
			continue;
		opts[i].val = (char)val;
		if (i == OMONITOR)
			setjobctl(val);
		return 0;
	}
	return -1;
}

static int
setoptbyname(const char *name, int val)
{
	int i;

	for (i = 0; i < NOPTS; i++) {
		if (strcmp(opts[i].name, name) != 0)
			continue;
		opts[i].val = (char)val;
		if (i == OMONITOR)
			setjobctl(val);
		return 0;
	}
	warn("set: %s: bad option name", name);
	return -1;
}

/*
 * `set -o' lists the options for a reader, and `set +o' lists them as
 * commands that reproduce the current settings.
 */
static void
listoptions(int human)
{
	int i;

	for (i = 0; i < NOPTS; i++) {
		if (human)
			printf("%-16s%s\n", opts[i].name,
			       opts[i].val ? "on" : "off");
		else
			printf("set %co %s\n", opts[i].val ? '-' : '+',
			       opts[i].name);
	}
}

int
bltin_set(int argc, char **argv)
{
	char **v;
	char *a;
	int i, k, val, newparams, n;

	if (argc == 1) {
		listvars(0, NULL);
		return 0;
	}
	newparams = 0;
	for (i = 1; i < argc; i++) {
		a = argv[i];
		if (a[0] != '-' && a[0] != '+')
			break;
		if (a[1] == '\0')
			continue; /* a lone `-' or `+' does nothing here */
		if (a[0] == '-' && a[1] == '-' && a[2] == '\0') {
			i++;
			newparams = 1;
			break;
		}
		val = a[0] == '-';
		for (k = 1; a[k]; k++) {
			if (a[k] != 'o') {
				if (setoptbyletter(a[k], val) < 0) {
					warn("set: %c: bad option", a[k]);
					return 2;
				}
				continue;
			}
			if (i + 1 < argc && argv[i + 1][0] != '-' &&
			    argv[i + 1][0] != '+') {
				if (setoptbyname(argv[++i], val) < 0)
					return 2;
				continue;
			}
			listoptions(val);
		}
	}
	if (i < argc)
		newparams = 1;
	if (!newparams)
		return 0;
	n = argc - i;
	v = xmalloc(sizeof(*v) * (size_t)(n + 1));
	for (k = 0; k < n; k++)
		v[k] = xstrdup(argv[i + k]);
	v[n] = NULL;
	freeparams(shparam.p);
	setparams(v);
	return 0;
}

static int
exportlike(int argc, char **argv, int flag, const char *word)
{
	BOpt o;
	char *eq;
	int c, i, pflag;

	pflag = 0;
	boptinit(&o, argc, argv);
	while ((c = bopt(&o, "p")) != -1) {
		if (c == 'p') {
			pflag = 1;
			continue;
		}
		warn("%s: %c: bad option", word, o.opt);
		return 2;
	}
	if (pflag || o.ind >= argc) {
		listvars(flag, word);
		return 0;
	}
	for (i = o.ind; i < argc; i++) {
		eq = strchr(argv[i], '=');
		/*
		 * A name that is not a name is a variable assignment
		 * error, and section 2.8.1 ends a non-interactive shell
		 * on one of those; error() is what carries that out.
		 */
		if (!validname(argv[i], eq ? (int)(eq - argv[i]) : -1))
			error("%s: %s: bad variable name", word, argv[i]);
		if (eq)
			setvareq(argv[i], flag);
		else
			exportvar(argv[i], flag);
	}
	return 0;
}

int
bltin_export(int argc, char **argv)
{
	return exportlike(argc, argv, VEXPORT, "export");
}

int
bltin_readonly(int argc, char **argv)
{
	return exportlike(argc, argv, VREADONLY, "readonly");
}

int
bltin_unset(int argc, char **argv)
{
	BOpt o;
	int c, i, dofunc, status;

	dofunc = 0;
	boptinit(&o, argc, argv);
	while ((c = bopt(&o, "fv")) != -1) {
		if (c == 'f') {
			dofunc = 1;
			continue;
		}
		if (c == 'v') {
			dofunc = 0;
			continue;
		}
		warn("unset: %c: bad option", o.opt);
		return 2;
	}
	status = 0;
	for (i = o.ind; i < argc; i++) {
		if (dofunc) {
			unsetfunc(argv[i]);
			continue;
		}
		if (unsetvar(argv[i]) < 0)
			error("unset: %s: is read only", argv[i]);
	}
	return status;
}

/*
 * `getopts'. The state between calls lives in OPTIND, which a script
 * may reset, and in an offset inside the current argument, which has
 * no place in the specification and so lives in the shell. A change to
 * OPTIND from outside clears that offset.
 */
int
bltin_getopts(int argc, char **argv)
{
	const char *optstr, *name, *p;
	char **args;
	char *arg;
	char val[2];
	char buf[32];
	int nargs, ind, off, c, silent;

	if (argc < 3)
		error("getopts: usage: getopts optstring name [arg...]");
	optstr = argv[1];
	name = argv[2];
	silent = optstr[0] == ':';
	if (argc > 3) {
		args = argv + 3;
		nargs = argc - 3;
	} else {
		args = shparam.p;
		nargs = shparam.nparam;
	}
	p = getvar("OPTIND");
	ind = p ? (int)strtol(p, NULL, 10) : 1;
	if (ind < 1)
		ind = 1;
	off = ind == shparam.optind ? shparam.optoff : 0;
	arg = NULL;
	for (;;) {
		if (ind > nargs)
			goto done;
		arg = args[ind - 1];
		if (off == 0) {
			if (arg[0] != '-' || arg[1] == '\0')
				goto done;
			if (arg[1] == '-' && arg[2] == '\0') {
				ind++;
				goto done;
			}
			off = 1;
		}
		c = (unsigned char)arg[off];
		if (c == '\0') {
			ind++;
			off = 0;
			continue;
		}
		off++;
		break;
	}
	val[0] = (char)c;
	val[1] = '\0';
	p = c == ':' ? NULL : strchr(optstr, c);
	if (!p) {
		if (arg[off] == '\0') {
			ind++;
			off = 0;
		}
		setvar(name, "?", 0);
		if (silent)
			setvar("OPTARG", val, 0);
		else {
			unsetvar("OPTARG");
			warn("getopts: illegal option -- %c", c);
		}
		goto save;
	}
	if (p[1] == ':') {
		if (arg[off] != '\0') {
			setvar("OPTARG", arg + off, 0);
			ind++;
			off = 0;
		} else if (ind < nargs) {
			setvar("OPTARG", args[ind], 0);
			ind += 2;
			off = 0;
		} else {
			ind++;
			off = 0;
			if (silent) {
				setvar(name, ":", 0);
				setvar("OPTARG", val, 0);
			} else {
				setvar(name, "?", 0);
				unsetvar("OPTARG");
				warn("getopts: option requires an argument "
				     "-- %c",
				     c);
			}
			goto save;
		}
	} else if (arg[off] == '\0') {
		ind++;
		off = 0;
	}
	setvar(name, val, 0);
save:
	snprintf(buf, sizeof(buf), "%d", ind);
	setvar("OPTIND", buf, 0);
	shparam.optind = ind;
	shparam.optoff = off;
	return 0;
done:
	snprintf(buf, sizeof(buf), "%d", ind);
	setvar("OPTIND", buf, 0);
	shparam.optind = ind;
	shparam.optoff = 0;
	setvar(name, "?", 0);
	unsetvar("OPTARG");
	return 1;
}

static int
inifs(int c, const char *ifs)
{
	return c != '\0' && strchr(ifs, c) != NULL;
}

static int
isifsws(int c, const char *ifs)
{
	return (c == ' ' || c == '\t' || c == '\n') && inifs(c, ifs);
}

/*
 * `read'. The line comes a byte at a time, because the shell must not
 * read past the newline: the bytes after it may belong to the script
 * that is running, or to the next reader of the same file.
 */
int
bltin_read(int argc, char **argv)
{
	EStr e;
	BOpt o;
	const char *ifs;
	char **names;
	char ch;
	int c, raw, nnames, i, j, status, n, start, sawdelim;

	raw = 0;
	boptinit(&o, argc, argv);
	while ((c = bopt(&o, "r")) != -1) {
		if (c == 'r') {
			raw = 1;
			continue;
		}
		warn("read: %c: bad option", o.opt);
		return 2;
	}
	names = argv + o.ind;
	nnames = argc - o.ind;
	if (nnames < 1)
		error("read: a variable name is required");
	for (i = 0; i < nnames; i++) {
		if (!validname(names[i], -1))
			error("read: %s: bad variable name", names[i]);
	}
	syncinput();
	esinit(&e);
	status = 0;
	for (;;) {
		n = (int)read(0, &ch, 1);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			status = 1;
			break;
		}
		if (ch == '\n')
			break;
		if (ch == '\\' && !raw) {
			n = (int)read(0, &ch, 1);
			if (n <= 0)
				break;
			if (ch == '\n')
				continue; /* a continued line */
			esputc(&e, ch, 1);
			continue;
		}
		esputc(&e, ch, 0);
	}
	ifs = getvar("IFS");
	if (!ifs)
		ifs = " \t\n";
	i = 0;
	while (i < e.len && !e.q[i] && isifsws(e.s[i], ifs))
		i++;
	for (j = 0; j < nnames; j++) {
		start = i;
		if (j == nnames - 1) {
			n = e.len;
			while (n > start && !e.q[n - 1] &&
			       isifsws(e.s[n - 1], ifs))
				n--;
			setvar(names[j], esrange(&e, start, n), 0);
			i = e.len;
			continue;
		}
		while (i < e.len && !(!e.q[i] && inifs(e.s[i], ifs)))
			i++;
		setvar(names[j], esrange(&e, start, i), 0);
		sawdelim = 0;
		while (i < e.len && !e.q[i] && inifs(e.s[i], ifs)) {
			if (!isifsws(e.s[i], ifs)) {
				if (sawdelim)
					break;
				sawdelim = 1;
			}
			i++;
		}
	}
	return status;
}
