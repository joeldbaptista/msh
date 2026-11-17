# msh - a POSIX shell

VERSION = 0.1

PREFIX = /usr/local
BINDIR = $(PREFIX)/bin

CC = cc
STD = -std=c99 -D_POSIX_C_SOURCE=200809L
WARN = -Wall -Wextra -pedantic -Werror
OPT = -O2
EXTRA =

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
STD += -D_DARWIN_C_SOURCE
endif

CFLAGS = $(STD) $(WARN) $(OPT) $(EXTRA)

SRC = \
	src/memory.c \
	src/error.c \
	src/input.c \
	src/lexer.c \
	src/parser.c \
	src/tree.c \
	src/vars.c \
	src/expand.c \
	src/arith.c \
	src/match.c \
	src/redir.c \
	src/exec.c \
	src/eval.c \
	src/jobs.c \
	src/trap.c \
	src/msh.c \
	src/builtins/table.c \
	src/builtins/core.c \
	src/builtins/param.c \
	src/builtins/dir.c \
	src/builtins/cmd.c \
	src/builtins/job.c

HDR = \
	src/msh.h \
	src/tree.h \
	src/vars.h \
	src/expand.h \
	src/exec.h \
	src/jobs.h \
	src/trap.h \
	src/builtin.h

OBJ = $(SRC:.c=.o)

all: msh

msh: $(OBJ)
	$(CC) $(EXTRA) $(LDFLAGS) -o $@ $(OBJ)

$(OBJ): $(HDR)

# A build with the sanitizers that work on this machine. AddressSanitizer
# is left out, because a binary built with it does not run here.
debug:
	$(MAKE) clean
	$(MAKE) EXTRA='-g -O0 -fsanitize=undefined' all

frmt:
	clang-format -i $(SRC) $(HDR)

clean:
	rm -f msh $(OBJ)

install: msh
	mkdir -p $(DESTDIR)$(BINDIR)
	cp -f msh $(DESTDIR)$(BINDIR)/msh

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/msh

.PHONY: all debug fuzz frmt clean install uninstall
