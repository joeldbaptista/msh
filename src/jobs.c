/*
 * msh - a POSIX shell.
 *
 * Jobs, process groups and the terminal.
 *
 * The job table is an array of jobs, and each job holds an array of
 * the processes in its pipeline. With job control on, every job is a
 * process group of its own, and the foreground job owns the terminal;
 * setpgid() runs in both the parent and the child, because either one
 * may reach it first.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "exec.h"
#include "jobs.h"
#include "msh.h"
#include "trap.h"
#include "tree.h"
#include "vars.h"

pid_t shellpgrp;
int ttyfd = -1;

static Job *jobtab;
static int njobs;
static int lastjob = -1;
static int prevjob = -1;

static void setcurrentjob(Job *j);

static int
shellstatus(int raw)
{
	if (WIFEXITED(raw))
		return WEXITSTATUS(raw);
	if (WIFSIGNALED(raw))
		return 128 + WTERMSIG(raw);
	if (WIFSTOPPED(raw))
		return 128 + WSTOPSIG(raw);
	return 0;
}

void
jobsinit(void)
{
	njobs = 4;
	jobtab = xmalloc(sizeof(*jobtab) * (size_t)njobs);
	memset(jobtab, 0, sizeof(*jobtab) * (size_t)njobs);
}

static void
growjobs(void)
{
	int old;

	old = njobs;
	njobs *= 2;
	jobtab = xrealloc(jobtab, sizeof(*jobtab) * (size_t)njobs);
	memset(jobtab + old, 0, sizeof(*jobtab) * (size_t)(njobs - old));
}

int
jobidof(Job *j)
{
	return j->jobid;
}

static int
jobindex(Job *j)
{
	return (int)(j - jobtab);
}

void
freejob(Job *j)
{
	if (!j->used)
		return;
	free(j->cmd);
	j->cmd = NULL;
	free(j->ps);
	j->ps = NULL;
	j->used = 0;
	if (lastjob == jobindex(j))
		lastjob = prevjob;
	if (prevjob == jobindex(j))
		prevjob = -1;
}

/*
 * Let go of a job that the shell has finished reporting on. A job that
 * is still running, or that has stopped, has to stay in the table:
 * `jobs', `fg' and `bg' are about to be asked for it.
 */
void
releasejob(Job *j)
{
	if (j->used && j->state == JDONE)
		freejob(j);
}

void
clearjobs(void)
{
	int i;

	for (i = 0; i < njobs; i++)
		freejob(&jobtab[i]);
	lastjob = -1;
	prevjob = -1;
}

Job *
makejob(int nproc, const char *cmd)
{
	Job *j;
	int i, maxid;

	for (;;) {
		for (i = 0; i < njobs; i++) {
			if (!jobtab[i].used)
				goto found;
		}
		growjobs();
	}
found:
	j = &jobtab[i];
	maxid = 0;
	for (i = 0; i < njobs; i++) {
		if (jobtab[i].used && jobtab[i].jobid > maxid)
			maxid = jobtab[i].jobid;
	}
	memset(j, 0, sizeof(*j));
	j->used = 1;
	j->cmd = cmd ? xstrdup(cmd) : NULL;
	j->jobid = maxid + 1;
	j->state = JRUNNING;
	j->nproc = 0;
	j->ps = xmalloc(sizeof(*j->ps) * (size_t)(nproc ? nproc : 1));
	memset(j->ps, 0, sizeof(*j->ps) * (size_t)(nproc ? nproc : 1));
	for (i = 0; i < nproc; i++)
		j->ps[i].status = -1;
	j->nprocmax = nproc ? nproc : 1;
	return j;
}

void
addproc(Job *j, pid_t pid)
{
	if (j->nproc >= j->nprocmax)
		return;
	j->ps[j->nproc].pid = pid;
	j->ps[j->nproc].status = -1;
	j->nproc++;
}

/*
 * The terminal belongs to the foreground process group. Giving it away
 * and taking it back are the two places where a shell with job control
 * differs most from one without.
 */
static void
givetty(pid_t pgrp)
{
	if (!mflag || ttyfd < 0)
		return;
	tcsetpgrp(ttyfd, pgrp);
}

static void
taketty(void)
{
	if (!mflag || ttyfd < 0)
		return;
	tcsetpgrp(ttyfd, shellpgrp);
}

pid_t
forkchild(Job *j, int bg)
{
	pid_t pid, pgrp;

	fflush(stdout);
	fflush(stderr);
	pid = fork();
	if (pid < 0)
		error("cannot fork: %s", strerror(errno));
	pgrp = j->nproc == 0 ? 0 : j->pgrp;
	if (pid == 0) {
		if (mflag) {
			if (pgrp == 0)
				pgrp = getpid();
			setpgid(0, pgrp);
			if (!bg)
				tcsetpgrp(ttyfd, pgrp);
			install_default_jobsigs();
		}
		return 0;
	}
	if (mflag) {
		if (pgrp == 0)
			pgrp = pid;
		setpgid(pid, pgrp);
	}
	if (j->nproc == 0)
		j->pgrp = mflag ? pgrp : pid;
	j->bg = bg;
	addproc(j, pid);
	/*
	 * The most recent asynchronous job is the current job, which is
	 * what `fg' and `bg' act on when given no operand.
	 */
	if (bg)
		setcurrentjob(j);
	return pid;
}

static Job *
jobofpid(pid_t pid, int *which)
{
	int i, k;

	for (i = 0; i < njobs; i++) {
		if (!jobtab[i].used)
			continue;
		for (k = 0; k < jobtab[i].nproc; k++) {
			if (jobtab[i].ps[k].pid != pid)
				continue;
			*which = k;
			return &jobtab[i];
		}
	}
	return NULL;
}

static void
setjobstate(Job *j)
{
	int i, running, stopped;

	running = 0;
	stopped = 0;
	for (i = 0; i < j->nproc; i++) {
		if (j->ps[i].status < 0)
			running++;
		else if (j->ps[i].stopped)
			stopped++;
	}
	if (running > 0)
		j->state = JRUNNING;
	else if (stopped > 0)
		j->state = JSTOPPED;
	else
		j->state = JDONE;
}

/*
 * Collect one change of state from the kernel. With `block' the call
 * waits; without it the call reports only what has already happened.
 */
static int
waitone(int block)
{
	Job *j;
	pid_t pid;
	int raw, which;

	for (;;) {
		pid = waitpid(-1, &raw, (block ? 0 : WNOHANG) | WUNTRACED);
		if (pid >= 0)
			break;
		if (errno == EINTR) {
			if (pendingsigsset())
				return -1;
			continue;
		}
		return -1;
	}
	if (pid == 0)
		return 0;
	j = jobofpid(pid, &which);
	if (!j)
		return 1;
	j->ps[which].status = shellstatus(raw);
	j->ps[which].stopped = WIFSTOPPED(raw) != 0;
	if (WIFSTOPPED(raw))
		j->stopstatus = shellstatus(raw);
	setjobstate(j);
	if (j->state != JRUNNING)
		j->changed = 1;
	return 1;
}

void
reapjobs(int block)
{
	while (waitone(block) > 0)
		block = 0;
}

static void
setcurrentjob(Job *j)
{
	int idx;

	idx = jobindex(j);
	if (lastjob != idx) {
		prevjob = lastjob;
		lastjob = idx;
	}
}

/*
 * Wait for a foreground job. The status of the job is the status of
 * its last process, as section 2.9.2 requires of a pipeline. The
 * caller frees the job, because it may still want to read the status
 * of the other processes in it.
 */
int
waitforjob(Job *j)
{
	int status;

	while (j->state == JRUNNING) {
		if (waitone(1) < 0)
			break;
	}
	taketty();
	if (j->state == JSTOPPED) {
		setcurrentjob(j);
		j->changed = 0;
		if (iflag)
			fprintf(stderr, "[%d] + Stopped\n", j->jobid);
		return j->stopstatus;
	}
	status = j->nproc > 0 ? j->ps[j->nproc - 1].status : 0;
	if (status < 0)
		status = 0;
	return status;
}

/*
 * `wait' for one job, and then forget it, because its status has been
 * reported once (section 2.9.3).
 */
int
dowaitjob(Job *j)
{
	int status;

	status = waitforjob(j);
	if (j->state == JDONE)
		freejob(j);
	return status;
}

/*
 * `wait' for one process. A process that this shell did not start, or
 * has already reported, gives status 127 (the `wait' page).
 */
int
dowaitpid(pid_t pid)
{
	Job *j;
	int which, status;

	j = jobofpid(pid, &which);
	if (!j)
		return 127;
	while (j->ps[which].status < 0) {
		if (waitone(1) < 0)
			break;
	}
	status = j->ps[which].status;
	if (status < 0)
		status = 0;
	if (j->state == JDONE)
		freejob(j);
	return status;
}

/*
 * `wait' with no operand: every job, until none is left running.
 */
int
dowaitall(void)
{
	int i, any;

	for (;;) {
		any = 0;
		for (i = 0; i < njobs; i++) {
			if (jobtab[i].used &&
			    jobtab[i].state == JRUNNING)
				any = 1;
		}
		if (!any)
			break;
		if (waitone(1) < 0)
			break;
	}
	for (i = 0; i < njobs; i++) {
		if (jobtab[i].used && jobtab[i].state == JDONE)
			freejob(&jobtab[i]);
	}
	return 0;
}

static const char *
statetext(Job *j)
{
	switch (j->state) {
	case JRUNNING:
		return "Running";
	case JSTOPPED:
		return "Stopped";
	}
	return "Done";
}

/*
 * One line of `jobs' output: the job number, a mark for the current
 * and the previous job, the state, and the command (the `jobs' page).
 */
static void
showjob(Job *j, int mode)
{
	int mark;

	if (mode == 'p') {
		printf("%ld\n", (long)j->ps[0].pid);
		return;
	}
	mark = jobindex(j) == lastjob ? '+'
	                              : (jobindex(j) == prevjob ? '-' : ' ');
	if (mode == 'l')
		printf("[%d] %c %ld %s %s\n", j->jobid, mark,
		       (long)j->ps[0].pid, statetext(j), j->cmd ? j->cmd : "");
	else
		printf("[%d] %c %s %s\n", j->jobid, mark, statetext(j),
		       j->cmd ? j->cmd : "");
}

void
showjobs(int mode, int *ids, int nids)
{
	int i, k;

	reapjobs(0);
	for (i = 0; i < njobs; i++) {
		if (!jobtab[i].used)
			continue;
		if (nids > 0) {
			for (k = 0; k < nids; k++) {
				if (jobtab[i].jobid == ids[k])
					break;
			}
			if (k == nids)
				continue;
		}
		showjob(&jobtab[i], mode);
		jobtab[i].changed = 0;
		if (jobtab[i].state == JDONE)
			freejob(&jobtab[i]);
	}
}

/*
 * Report the jobs whose state changed, which an interactive shell does
 * before each prompt, and which `set -b' asks for at once.
 */
void
checkjobs(void)
{
	int i;

	reapjobs(0);
	for (i = 0; i < njobs; i++) {
		if (!jobtab[i].used || !jobtab[i].changed)
			continue;
		if (!jobtab[i].bg)
			continue;
		showjob(&jobtab[i], 0);
		jobtab[i].changed = 0;
		if (jobtab[i].state == JDONE)
			freejob(&jobtab[i]);
	}
}

/*
 * A job name: `%%' and `%+' are the current job, `%-' the previous
 * one, `%n' job number n, and a bare number is also job number n.
 */
Job *
getjob(const char *name)
{
	int id, i;
	char *end;

	if (!name || !*name) {
		if (lastjob >= 0 && jobtab[lastjob].used)
			return &jobtab[lastjob];
		return NULL;
	}
	if (*name == '%')
		name++;
	if (*name == '%' || *name == '+' || *name == '\0') {
		if (lastjob >= 0 && jobtab[lastjob].used)
			return &jobtab[lastjob];
		return NULL;
	}
	if (*name == '-') {
		if (prevjob >= 0 && jobtab[prevjob].used)
			return &jobtab[prevjob];
		return NULL;
	}
	id = (int)strtol(name, &end, 10);
	if (*end)
		return NULL;
	for (i = 0; i < njobs; i++) {
		if (jobtab[i].used && jobtab[i].jobid == id)
			return &jobtab[i];
	}
	return NULL;
}

/*
 * Resume a stopped job, in the foreground or in the background. The
 * continue signal goes to the whole process group, because every
 * process of the pipeline stopped.
 */
int
startjob(Job *j, int fg)
{
	int i;

	for (i = 0; i < j->nproc; i++) {
		if (j->ps[i].stopped) {
			j->ps[i].stopped = 0;
			j->ps[i].status = -1;
		}
	}
	j->state = JRUNNING;
	j->bg = !fg;
	setcurrentjob(j);
	/*
	 * Both `fg' and `bg' write the command they resume to standard
	 * output, and `bg' writes the job number with it.
	 */
	if (fg)
		printf("%s\n", j->cmd ? j->cmd : "");
	else
		printf("[%d] %s\n", j->jobid, j->cmd ? j->cmd : "");
	fflush(stdout);
	if (fg)
		givetty(j->pgrp);
	if (kill(-j->pgrp, SIGCONT) < 0 && errno == ESRCH) {
		for (i = 0; i < j->nproc; i++)
			kill(j->ps[i].pid, SIGCONT);
	}
	if (!fg)
		return 0;
	return waitforjob(j);
}

/*
 * Turn job control on. The shell takes a process group of its own and
 * the terminal with it, and it stops reacting to the signals that
 * terminal control sends, because it is the one sending them.
 */
void
setjobctl(int on)
{
	int fd;

	if (on) {
		if (ttyfd < 0) {
			fd = open("/dev/tty", O_RDWR);
			if (fd < 0)
				return;
			ttyfd = fcntl(fd, F_DUPFD, 10);
			if (ttyfd < 0)
				ttyfd = fd;
			else
				close(fd);
			fcntl(ttyfd, F_SETFD, FD_CLOEXEC);
		}
		if (!isatty(ttyfd))
			return;
		ignoresig(SIGTTOU);
		ignoresig(SIGTTIN);
		ignoresig(SIGTSTP);
		shellpgrp = getpid();
		if (setpgid(0, shellpgrp) < 0 && errno != EPERM)
			return;
		tcsetpgrp(ttyfd, shellpgrp);
		mflag = 1;
		return;
	}
	if (ttyfd >= 0 && shellpgrp > 0)
		tcsetpgrp(ttyfd, shellpgrp);
	mflag = 0;
}

/*
 * A child of a shell with job control must react to the terminal
 * signals in the ordinary way, because the shell's own dispositions
 * are not what a command wants.
 */
void
install_default_jobsigs(void)
{
	signal(SIGTTOU, SIG_DFL);
	signal(SIGTTIN, SIG_DFL);
	signal(SIGTSTP, SIG_DFL);
}

int
jobwarning(void)
{
	int i, n;

	n = 0;
	reapjobs(0);
	for (i = 0; i < njobs; i++) {
		if (jobtab[i].used && jobtab[i].state == JSTOPPED)
			n++;
	}
	return n;
}

/*
 * An interrupt that reached a point where unwinding is safe. In an
 * interactive shell it abandons the command; otherwise it ends the
 * shell with the status that the signal implies.
 */
void
raise_interrupt(void)
{
	exitstatus = 128 + SIGINT;
	if (!iflag) {
		defaultsigs();
		kill(getpid(), SIGINT);
		_exit(128 + SIGINT);
	}
	if (ttyfd >= 0)
		taketty();
	fflush(stdout);
	fputc('\n', stderr);
	errexit();
	/* NOTREACHED */
}

void
forkexit(int status)
{
	exitshell(status);
	/* NOTREACHED */
}
