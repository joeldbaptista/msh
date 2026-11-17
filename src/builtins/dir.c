/*
 * msh - a POSIX shell.
 *
 * The built-ins that work on the working directory and on the file
 * creation mask: `cd', `pwd' and `umask'.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../builtin.h"
#include "../exec.h"
#include "../expand.h"
#include "../msh.h"
#include "../tree.h"
#include "../vars.h"

/*
 * The logical form of a path: `.' components go, and a `..' component
 * cancels the component before it. This is path arithmetic, not a
 * question put to the file system, which is exactly what the default
 * `-L' behaviour of `cd' asks for (section 2 of the `cd' page).
 */
static char *
canonpath(const char *p)
{
	SBuf b;
	char *out;
	int i, n;

	sbinit(&b, stk);
	if (*p != '/')
		sbputc(&b, '/'); /* the caller gave an absolute path */
	sbputs(&b, p);
	out = sbdone(&b);
	n = 0;
	for (i = 0; out[i]; i++) {
		if (out[i] != '/') {
			out[n++] = out[i];
			continue;
		}
		/* collapse a run of slashes */
		if (n > 0 && out[n - 1] == '/')
			continue;
		out[n++] = '/';
	}
	out[n] = '\0';
	/* now remove `.' and `..' */
	{
		char *res;
		int j, k, start;

		res = arenaalloc(stk, (size_t)n + 2);
		j = 0;
		res[j++] = '/';
		i = 1;
		while (i <= n) {
			start = i;
			while (i < n && out[i] != '/')
				i++;
			k = i - start;
			if (k == 1 && out[start] == '.') {
				/* nothing */
			} else if (k == 2 && out[start] == '.' &&
			           out[start + 1] == '.') {
				if (j > 1) {
					j--; /* drop the trailing slash */
					while (j > 1 && res[j - 1] != '/')
						j--;
				}
			} else if (k > 0) {
				memcpy(res + j, out + start, (size_t)k);
				j += k;
				res[j++] = '/';
			}
			i++;
		}
		if (j > 1 && res[j - 1] == '/')
			j--;
		res[j] = '\0';
		return res;
	}
}

static void
setpwd(const char *dir)
{
	char buf[4096];

	setvar("OLDPWD", getvar("PWD") ? getvar("PWD") : "", VEXPORT);
	if (dir)
		setvar("PWD", dir, VEXPORT);
	else if (getcwd(buf, sizeof(buf)))
		setvar("PWD", buf, VEXPORT);
}

int
bltin_cd(int argc, char **argv)
{
	BOpt o;
	const char *dest, *cdpath;
	char *path, *cand;
	char buf[4096];
	int c, phys, print;

	phys = 0;
	print = 0;
	boptinit(&o, argc, argv);
	while ((c = bopt(&o, "LP")) != -1) {
		if (c == 'L') {
			phys = 0;
			continue;
		}
		if (c == 'P') {
			phys = 1;
			continue;
		}
		warn("cd: %c: bad option", o.opt);
		return 2;
	}
	if (argc - o.ind > 1) {
		warn("cd: too many arguments");
		return 2;
	}
	dest = o.ind < argc ? argv[o.ind] : NULL;
	if (!dest) {
		dest = getvar("HOME");
		if (!dest || !*dest) {
			warn("cd: HOME is not set");
			return 1;
		}
	} else if (strcmp(dest, "-") == 0) {
		dest = getvar("OLDPWD");
		if (!dest || !*dest) {
			warn("cd: OLDPWD is not set");
			return 1;
		}
		print = 1;
	}
	/*
	 * CDPATH applies only to an operand that is not a path name
	 * relative to the current directory (the `cd' page, step 5).
	 */
	cand = NULL;
	if (dest[0] != '/' &&
	    !(dest[0] == '.' &&
	      (dest[1] == '\0' || dest[1] == '/' ||
	       (dest[1] == '.' &&
	        (dest[2] == '\0' || dest[2] == '/'))))) {
		cdpath = getvar("CDPATH");
		while (cdpath && (cand = padvance(&cdpath, dest)) != NULL) {
			if (access(cand, X_OK) == 0) {
				print = 1;
				break;
			}
			cand = NULL;
		}
	}
	if (!cand)
		cand = (char *)dest;
	if (cand[0] == '/') {
		path = canonpath(cand);
	} else {
		SBuf b;
		const char *pwd;

		pwd = getvar("PWD");
		sbinit(&b, stk);
		if (pwd && pwd[0] == '/')
			sbputs(&b, pwd);
		else if (getcwd(buf, sizeof(buf)))
			sbputs(&b, buf);
		sbputc(&b, '/');
		sbputs(&b, cand);
		path = canonpath(sbdone(&b));
	}
	if (phys) {
		if (chdir(cand) < 0) {
			warn("cd: %s: %s", cand, strerror(errno));
			return 2;
		}
		setpwd(NULL);
	} else {
		if (chdir(path) < 0 && chdir(cand) < 0) {
			warn("cd: %s: %s", cand, strerror(errno));
			return 2;
		}
		setpwd(path);
	}
	if (print)
		printf("%s\n", getvar("PWD") ? getvar("PWD") : path);
	return 0;
}

int
bltin_pwd(int argc, char **argv)
{
	BOpt o;
	const char *pwd;
	char buf[4096];
	int c, phys;

	phys = 0;
	boptinit(&o, argc, argv);
	while ((c = bopt(&o, "LP")) != -1) {
		if (c == 'L') {
			phys = 0;
			continue;
		}
		if (c == 'P') {
			phys = 1;
			continue;
		}
		warn("pwd: %c: bad option", o.opt);
		return 2;
	}
	pwd = getvar("PWD");
	if (!phys && pwd && pwd[0] == '/' && !strstr(pwd, "/./") &&
	    !strstr(pwd, "/../")) {
		printf("%s\n", pwd);
		return 0;
	}
	if (!getcwd(buf, sizeof(buf))) {
		warn("pwd: %s", strerror(errno));
		return 1;
	}
	printf("%s\n", buf);
	return 0;
}

/*
 * The symbolic form of the mask names the permissions that the mask
 * leaves, not the ones it removes (the `umask' page).
 */
static void
printsymbolic(mode_t mask)
{
	static const char who[] = "ugo";
	static const char what[] = "rwx";
	int i, k, shift;

	for (i = 0; i < 3; i++) {
		shift = 6 - i * 3;
		if (i)
			putchar(',');
		printf("%c=", who[i]);
		for (k = 0; k < 3; k++) {
			if (!((mask >> (shift + 2 - k)) & 1))
				putchar(what[k]);
		}
	}
	putchar('\n');
}

int
bltin_umask(int argc, char **argv)
{
	BOpt o;
	mode_t mask;
	const char *s;
	int c, symbolic;
	long n;
	char *end;

	symbolic = 0;
	boptinit(&o, argc, argv);
	while ((c = bopt(&o, "S")) != -1) {
		if (c == 'S') {
			symbolic = 1;
			continue;
		}
		warn("umask: %c: bad option", o.opt);
		return 2;
	}
	mask = umask(0);
	umask(mask);
	if (o.ind >= argc) {
		if (symbolic)
			printsymbolic(mask);
		else
			printf("%04o\n", (unsigned)mask);
		return 0;
	}
	s = argv[o.ind];
	n = strtol(s, &end, 8);
	if (*s == '\0' || *end != '\0' || n < 0 || n > 0777) {
		warn("umask: %s: bad mask", s);
		return 1;
	}
	umask((mode_t)n);
	return 0;
}
