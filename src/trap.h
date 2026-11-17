/*
 * msh - a POSIX shell.
 *
 * Signal dispositions and trap actions. A handler only records that
 * the signal arrived, and the main loop runs the action at a point
 * where running a command is safe.
 */
#ifndef TRAP_H
#define TRAP_H

#include <signal.h>

#include "msh.h"

void trapsinit(void);
int settrap(const char *signame, const char *action);
void listtraps(void);
void clearalltraps(void);
void clearcaughttraps(void);
int signame2num(const char *s);
const char *signum2name(int sig);
void dotrap(void);
int pendingsigsset(void);
void runexittrap(void);
void setsignal(int sig);
void ignoresig(int sig);
void defaultsigs(void);
extern volatile sig_atomic_t pendingsigs;
extern volatile sig_atomic_t gotsigchld;

#endif /* TRAP_H */
