# msh - POSIX shell

`msh` (read "mesh") is a shell written in C to the Shell Command
Language of POSIX.1-2017, Issue 7. It has no dependency beyond the
POSIX C library. 

## Build and run

```sh
make                  # ./msh
make frmt             # clang-format in place
make clean
```

The flags are `-std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra
-pedantic -Werror -O2`, and on macOS the Makefile adds
`-D_DARWIN_C_SOURCE`.

```sh
./msh                       # interactive, if standard input is a terminal
./msh script.sh arg ...     # run a script
./msh -c 'echo hello'       # run one command string
./msh -s arg ...            # read standard input, with positional parameters
./msh -n script.sh          # parse but do not run
./msh -T -n -c 'a>b|c'      # print the token stream (an extension)
./msh -P -n -c 'if x; then y; fi'   # print the syntax tree (an extension)
```

**The language.** Token recognition by the ten rules of section 2.3,
the whole grammar of section 2.10, every redirection operator of
section 2.7, every word expansion of section 2.6 in the order that
section gives, the pattern matching of section 2.13 including bracket
expressions and character classes, pathname expansion, every compound
command, and function definition and call.

**The utility.** The options `-a -b -c -C -e -f -h -i -m -n -o -s -u
-v -x` and their `+` forms, `$0` and the positional parameters, the
`ENV` file, `PS1`, `PS2`, `PS4`, `set -o ignoreeof`, and the prompt and
job reporting that an interactive session needs.

**The built-ins.** The special built-ins of section 2.14 — `break`,
`:`, `continue`, `.`, `eval`, `exec`, `exit`, `export`, `readonly`,
`return`, `set`, `shift`, `times`, `trap`, `unset` — and the regular
built-ins that section 2.9.1.1 requires the shell to find before the
`PATH` search — `alias`, `bg`, `cd`, `command`, `false`, `fg`,
`getopts`, `hash`, `jobs`, `pwd`, `read`, `true`, `type`, `ulimit`,
`umask`, `unalias`, `wait`. Alias substitution is connected to the
lexer, including the rule that an alias value ending in a blank makes
the next word a candidate too. The output of `alias`, `export -p`,
`readonly -p`, `set` and `trap` is quoted so that a shell can read it
back.

**Signals and jobs.** Trap actions run at points where running a
command is safe, the `EXIT` trap runs when the shell or a subshell
leaves, section 2.12 resets a caught trap in a subshell, each job is a
process group of its own when job control is on, and the terminal
passes to the foreground job and back.

**From Issue 8.** `set -o pipefail` and `$'...'` quoting.

## Architecture

Seven modules around one main loop. The main loop is one sentence:
parse one complete command, run it, free it, and read the next one.

| Module | Files |
| --- | --- |
| Input reader | `src/input.c` |
| Lexer | `src/lexer.c` |
| Parser and syntax tree | `src/parser.c`, `src/tree.c`, `src/tree.h` |
| Evaluator | `src/eval.c` |
| Executor | `src/exec.c`, `src/redir.c` |
| Expander | `src/expand.c`, `src/arith.c`, `src/match.c` |
| Built-ins | `src/builtins/*.c` |
| Shell state | `src/vars.c`, `src/jobs.c`, `src/trap.c` |
| Memory and errors | `src/memory.c`, `src/error.c` |
| The `sh` utility | `src/msh.c` |

### Memory: arenas

An **arena** is a chain of blocks that the program frees as one unit.
There are three uses.

- The **parse arena** of one complete command holds its syntax tree and
  every word in it. It dies when the command has run — unless the
  command defined a function, in which case the definition holds a
  reference to the arena and the arena outlives the command. So a
  function body needs no deep copy, and redefining a function frees the
  old body.
- The **transient arena** holds the strings that expansion produces. A
  mark and a release bracket every simple command, so a loop that runs
  a million commands does not grow it. A mark is also taken around the
  word list of a `for` and around the word of a `case`, because those
  have to outlive the commands inside them.
- Long-lived state — variables, functions, aliases, the job table, trap
  actions — is on the normal heap, because it outlives every command.

### Errors: `setjmp` and `longjmp`

A syntax error or an expansion error has to abandon the current command
from deep inside the call stack. `error()` reports and jumps to the
nearest handler, and the chain of handlers reaches the main loop. The
arena makes that safe, because the jump skips every `free()` between
the error and the main loop and the main loop releases the transient
arena to its own mark afterwards. The few records that must survive the
jump — the saved file descriptors of a redirection — are on the heap
for exactly that reason.

Section 2.8.1 makes an error in a *special* built-in end a
non-interactive shell, and leaves a *regular* built-in merely failing.
`evalbuiltin()` catches the jump and re-raises it only when the table
says the built-in is special, so no built-in has to know the rule.

### Quoting: a flag beside every byte

This is the decision that the expander rests on, and section 2.6 forces
it: field splitting and pathname expansion must skip a character that
quoting protected, and quote removal happens only at the end, so the
fact that a character was quoted has to survive until then.

Expansion writes into an `EStr`, which holds the bytes of the result
and, beside them, one flag per byte saying whether that byte came from
inside quotes. A quoting character is never stored. So quote removal is
reading the bytes out, the pattern matcher can tell a `*` that came
from the text from a `*` that quoting made literal, and nothing needs
an in-band escape byte.

One byte of the result is special: a 0 byte is a hard field break,
which only `$@` and `"$@"` produce, and its flag records whether that
`$@` was quoted. That is what tells an empty parameter, which owes one
empty field, from no parameter at all, which owes none.

### Behaviour as data

Where the specification gives a list, the code holds a table and a
small loop reads it. The operators of section 2.10.2, the reserved
words, the redirection defaults of section 2.7, the shell options, the
signal names, the resource limits, the character classes of a bracket
expression, the precedence of every arithmetic operator, the node types
of the evaluator and the built-ins are all tables. Adding a built-in
means writing one function and adding one row; adding a node type to
the evaluator means writing one function and adding one row.

## Sources

The specification decides; the rest only teach how to build it.

- POSIX.1-2017, Shell and Utilities, chapter 2, and the `sh` page and
  the page of each built-in.
- dash, run as a reference for comparison tests.
- The sources of dash and of BusyBox ash, for the lexer and expansion
  techniques. Also FreeBSD `sh` for a misc of details. 
- Stevens and Rago, *Advanced Programming in the UNIX Environment*, for
  file descriptors, process groups and signals.
- The GNU C Library manual, "Implementing a Job Control Shell".
