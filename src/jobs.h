/*
 * msh - a POSIX shell.
 *
 * The job table, process groups and the terminal.
 */
#ifndef JOBS_H
#define JOBS_H

#include <sys/types.h>

#include "msh.h"

enum { JRUNNING,
       JSTOPPED,
       JDONE };

typedef struct procstat ProcStat;
struct procstat {
	pid_t pid;
	int status; /* the shell status once done; -1 while running */
	int stopped;
	char *cmd;
};

typedef struct job Job;
struct job {
	int used;
	int jobid;
	int state;
	int nproc;
	int nprocmax;
	int stopstatus;
	int changed; /* the state changed and `jobs' has not reported it */
	int bg;      /* the job was started asynchronously */
	pid_t pgrp;
	char *cmd; /* the command as written, for `jobs' */
	ProcStat *ps;
};

void jobsinit(void);
Job *makejob(int nproc, const char *cmd);
void addproc(Job *j, pid_t pid);
pid_t forkchild(Job *j, int bg);
int waitforjob(Job *j);
int dowaitall(void);
int dowaitjob(Job *j);
int dowaitpid(pid_t pid);
void showjobs(int mode, int *ids, int nids);
Job *getjob(const char *name);
int jobidof(Job *j);
void setjobctl(int on);
void reapjobs(int block);
void checkjobs(void);
void freejob(Job *j);
void releasejob(Job *j);
void clearjobs(void);
int startjob(Job *j, int fg);
void install_default_jobsigs(void);
void NORETURN raise_interrupt(void);
void NORETURN forkexit(int status);
void setintpending(void);
int intsuppressed(void);
void flushall(void);
int jobwarning(void);

extern pid_t shellpgrp;
extern int ttyfd;

#endif /* JOBS_H */
