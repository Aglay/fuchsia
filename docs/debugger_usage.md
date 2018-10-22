# zxdb: Fuchsia native debugger user guide

This is the command usage guide for zxdb. Please see the [setup and
troubleshooting guide](debugger.md) to get running.

## Quick start

```
connect 192.168.3.1:2345
break main
run /system/bin/cowsay moo
next
print argv[1]
continue
quit
```

There is an extensive built-in help system. Type `help` and the command prompt
for a list of commands, or `help <command>` for details about that command
(much more than is in this document) as well as options and examples.

## Interaction model

Most command-line debuggers use an exclusive model for input: you’re either
interacting with the debugged process’ stdin and stdout, or you’re interacting
with the debugger. In contrast, zxdb has an asynchronous model similar to most
GUI debuggers. In this model, the user is exclusively interacting with the
debugger while arbitrary processes or threads are running or stopped.

Currently there is no way to see or interact with a process’ stdin and stdout
from the debugger. See [DX-595](https://fuchsia.atlassian.net/browse/DX-595) and
[DX-596](https://fuchsia.atlassian.net/browse/DX-596).

zxdb has a regular noun/verb model for typed commands. The rest of this section
gives an overview of the syntax that applies to all commands. Specific commands
will be covered in the “Task guide” section below.

### Nouns

The possible nouns (and their abbreviations) are:

  * `process` (`pr`)
  * `thread` (`t`)
  * `frame` (`f`)
  * `breakpoint` (`bp`)

#### Listing nouns

If you type a noun by itself, it lists the available objects of that type:

List attached processes

```
[zxdb] process
  # State       Koid Name
▶ 1 Not running 3471 /pkgfs/packages/debug_agent_tests/0/test/zxdb_test_app
```

List threads in the current process:

```
[zxdb] thread
  # State   Koid Name
▶ 1 Blocked 1348 initial-thread
  2 Blocked 1356 some-other-thread
```

List stack frames in the current thread (the thread must be stopped):

```
[zxdb] frame
▶ 0 fxl::CommandLineFromIterators<const char *const *>() • command_line.h:203
  1 fxl::CommandLineFromArgcArgv() • command_line.h:224
  2 main() • main.cc:174
```

#### Selecting defaults

If you type a noun and its index, you select that as the default for subsequent
commands. It also tells you the stats about the new default.

Select thread 3 to be the default for future commands:

```
[zxdb] thread 3
Thread 3 Blocked koid=9940 worker-thread
```

Select breakpoint 2 to be the default:

```
[zxdb] breakpoint 2
Breakpoint 2 (Software) on Global, Enabled, stop=All, @ MyFunction
```

### Verbs

By default, a verb (`run`, `next`, `print`, etc.) applies to the current
defaults. So to evaluate an expression in the context of the current stack
frame, just type `print` by itself:

```
[zxdb] print argv[1]
"--foo=bar"
```

You can override the default context by prefixing the verb with a noun and its
index. So to evaluate an expression in the context of a specific stack frame
(in this case, frame 2 of the current thread):

```
[zxdb] frame 2 print argv[1]
"--foo=bar"
```

You can keep adding different types of context. This specifies the process,
thread, and frame for the print command:

```
[zxdb] process 1 thread 1 frame 2 print argv[1]
"--foo=bar"
```

# Attaching and running

### Connecting to the target system

zxdb currently runs only in remote mode. This means that the debugger fromtend
(zxdb) runs on your development Linux or Mac workstation, while the target
Fuchsia system is running connected over a network or in QEMU.

The set up guide provides more information on how to do this. In short, first
run the debug agent on the target system with a port number:

```
$ debug_agent --port=2345
```

Then on the host use the `-c` command-line option or the connect command to
connect to the target’s IP and the port you specified above:

```
[zxdb] connect 192.168.3.1:2345
```

Use `disconnect` to close the connection, or `quit` to disconnect and close the
frontend. Note that currently you may need to also exit the debug agent before
reconnecting ([DX-517](https://fuchsia.atlassian.net/browse/DX-517)).

### Debugging modules

Debugging processes started by the application manager is currently only
possible by attaching to them once they’re started (see “Attaching to an
existing process” below). Work on a better experience is ongoing
[DX-322](https://fuchsia.atlassian.net/browse/DX-322).

If necessary you can hack something by busy-looping for 30 seconds or something
at the beginning of your application to give you time to attach. Unfortunately
zxdb can not currently set variables
([DX-597](https://fuchsia.atlassian.net/browse/DX-597)) which makes it
impossible to busy-loop on a variable.

### Debugging drivers

Debugging driver start-up is not currently possible
([DX-598](https://fuchsia.atlassian.net/browse/DX-598)).

You can debug running drivers by attaching like any other process (see
“Attaching to an existing process” below). The caveat is that since zxdb uses
the network to communicate between the zxdb client and the debug agent, you can
not debug any network-related drivers.

### Debugging crash dumps

Work on this capability is ongoing
([DX-603](https://fuchsia.atlassian.net/browse/DX-603)).

### Directly running a new process

You can start a new process from the debugger. Most applications in Fuchsia are
launched in a specific context from the application manager or the dev manager
and these won’t work when started from the debugger directly. But certain
processes like command line utilities and most tests can be.

To start a process, provide the full path and any command line arguments
(optional):

```
[zxdb] run /path/to/process --command --line=args go here
```

Most tests are in `/pkgfs/<complicated_path>` while some legacy utilities are
in `/system/bin`. It can be hard to find the full path in some cases, so
Fuchsia’s `find` utility is your friend. On the target Fuchsia system:

```
$ find . -name my_test
```

The run command will immediately start running the process. Many utilities run
and exit right away, so you’ll see:

```
[zxdb] run /system/bin/cowsay moo
Process 1 Running koid=10734 /system/bin/cowsay
Exited with code 1: Process 1 Not running /system/bin/cowsay
```

This is expected because the process did its work and exited. Currently stdout
and stdin from the running process are inaccessible so you won’t see any
printed output. In many cases you’ll want to set a breakpoint before running to
catch it at some point (see below for more on breakpoints):

```
[zxdb] break main
Breakpoint 1 (Software) on Global, Enabled, stop=All, @ main
Pending: No matches for location, it will be pending library loads.
[zxdb] run
```

In this this example we didn’t need to supply any parameters to `run` because
the process and command line switches carry over from the previous invocation.
It gives a warning that there are no matching symbols because the process is
not started yet. This is expected: the symbols will be resolved when the
process (and therefore symbols) are loaded.

To terminate the process:

```
[zxdb] kill
```

Or keep the process running outside of the debugger:

```
[zxdb] detach
```

### Attaching to an existing process

You can attach to most running processes given the process’ KOID. You can get
the KOID by running `ps` on the target Fuchsia system. zxdb also has a built-in
`ps` command:

```
[zxdb] ps
j: 1030 root
  j: 1079 zircon-drivers
    p: 1926 devhost:sys
...
```

Then to attach:

```
[zxdb] attach 3517
Process 1 Running koid=1249 pwrbtn-monitor
```

When you’re done, you can choose to `detach` (keep running) or `kill`
(terminate) the process.

### Debugging multiple processes

You can debug many arbitrary processes at the same time. When you start, one
“process context” (the container that may or may not have a running process)
 is created for you to use. When you run or attach, that process because
associated with that context.

To debug a second program, create a new context with:

```
[zxdb] process new
```

This will clone the current process’ settings into a new context but not run
anything yet. You can then run or attach as normal.

Recall from the “Interaction model” section you can list the current processes
with:

```
[zxdb] process
  # State       Koid Name
▶ 1 Running     1249 pwrbtn-monitor
  2 Not running 7235 pwrbtn-monitor
```

Select one of those as the default by providing its index (not KOID):

```
[zxdb] process 2
```

Or apply commands to a specific process (even if it’s not the default) with:

```
[zxdb] process 2 pause
```

# Running

### Working with breakpoints

Breakpoints stop execution when some code is executed. To create a breakpoint,
use the `break` command (`b` for short) and give it a location:

```
[zxdb] break main
Breakpoint 3 (Software) on Global, Enabled, stop=All, @ main
   180 
 ◉ 181 int main(int argc, char**argv) {
   182     fbl::unique_fd dirfd;
```

A location can be expressed in many different ways.

Plain function name:

```
break main
```

Member function or functions inside namespaces:

```
break my_namespace::MyClass::MyFunction
```

Source file + line number (separate with a colon):

```
break mymain.cc:22
```

Line number within the current frame’s current source file (useful when
stepping):

```
break 23
```

Memory address:

```
break 0xf72419a01
```

To list all breakpoints:

```
[zxdb] breakpoint
```

_Note: this is the “breakpoint” noun (a noun by itself lists the things
associated with it). It is not plural._

To clear a specific breakpoint, give that breakpoint index as the context for
the clear command (see “Interaction model” above). Here’s we’re using the
abbreviation for `breakpoint` (`bp`):

```
[zxdb] bp 2 clear
```

Or you can clear the current breakpoint:

```
[zxdb] clear
```

Whenever you create or stop on a breakpoint, that breakpoint becomes the
default automatically so clear always clears the one you just hit. Note that
unlike GDB, “clear” takes a breakpoint context before the verb and there are
never any arguments after it. Support for GDB-like “clear <location>” is
[DX-594](https://fuchsia.atlassian.net/browse/DX-594).

### Working with threads

To list the current process’ threads (see “Interaction model” above for more):

```
[zxdb] thread
  # State   Koid Name
▶ 1 Blocked 1323 initial-thread
  2 Running 3462 worker-thread
```

Often when you attach to a process the thread will be “blocked”, meaning it is
stopped on a system call. For asynchronous programs this will typically be some
kind of wait.

Most thread control and introspection commands only work when a thread is
suspended (not blocked or running). A thread will be suspended when it is
stopped at a breakpoint or crashes. You can explicitly suspend a thread with
the `pause` command:

```
[zxdb] thread 2 pause
```

Running `pause` by itself with no context will pause all threads of all
processes currently attached:

```
[zxdb] pause
```

Unpause a thread with `continue`. As before, `continue` with no context will
resume all threads:

```
[zxdb] continue
```

Or continue a specific thread:

```
[zxdb] thread 1 continue
```

### Working with stack frames

A stack frame is a function call. When a function calls another function, a new
nested frame is created. So listing the frames of a thread tells you the call
stack. You can only see the stack frames when a thread is suspended (see
“Working with threads” above).

To list the current thread’s stack frames (the `f` abbreviation will also
work).

```
[zxdb] frame
▶ 0 fxl::CommandLineFromIterators<const char *const *>() • command_line.h:203
  1 fxl::CommandLineFromArgcArgv() • command_line.h:224
  2 main() • main.cc:174
```

And to select a given frame as the default:

```
[zxdb] frame 2
```

Frames are numbered with “0” being the top of the stack. Increasing numbers go
backwards in time.

For more context, you can use the `backtrace` command. This is identical
to `frame` but gives more detailed address information as well as function
parameters. This command can be abbreviated `bt`:

```
[zxdb] bt
▶ 0 fxl::CommandLineFromIteratorsFindFirstPositionalArg<const char *const *>() • command_line.h:185
      IP = 0x10f982cf2ad0, BP = 0x66b45a01af50, SP = 0x66b45a01af38
      first = (const char* const*) 0x59f4e1268dc0
      last = (const char* const*) 0x59f4e1268dc8
      first_positional_arg = (const char* const**) 0x0
  1 fxl::CommandLineFromIterators<const char *const *>() • command_line.h:204
      IP = 0x10f982cf2ac0, BP = 0x66b45a01af50, SP = 0x66b45a01af40
      first = <'first' is not available at this address. >
      last = <'last' is not available at this address. >
...
```

Each stack frame has a code location. Use the `list` command to look at source
code. By itself, it lists the source code around the current stack frame’s
instruction pointer:

```
[zxdb] list
   183 inline CommandLine CommandLineFromIteratorsFindFirstPositionalArg(
   184     InputIterator first, InputIterator last,
 ▶ 185     InputIterator* first_positional_arg) {
   186   if (first_positional_arg)
   187     *first_positional_arg = last;
```

You can list code around the current instruction pointer of other stack frames,
too:

```
[zxdb] frame 3 list
```

Or you can list specific things like functions:

```
[zxdb] list MyClass::MyFunc
```

File/line numbers:

```
[zxdb] list foo.cc:43
```

Or whole files:

```
[zxdb] list --all myfile.cc:1
```

### Printing values

The `print` command can evaluate simple C/C++ expressions in the context of a
stack frame. When a thread is suspended (see “Working with threads” above) just
type:

```
[zxdb] print i
34
```

Expressions can use most simple C/C++ syntax:

```
[zxdb] print &foo->bar[baz]
(const MyStruct*) 0x59f4e1268f70

```

You can also evaluate expressions in the context of other stack frames without
switching to them (see “Interaction model” above for more):

```
[zxdb] frame 2 print argv[0]
"/system/bin/cowsay"
```

Often you will want to see all local variables:

```
[zxdb] locals
argc = 1
argv = (const char* const*) 0x59999ec02dc0
```

Things that don’t currently work are:

  * Math ([DX-600](https://fuchsia.atlassian.net/browse/DX-600))
  * Function calls ([DX-599](https://fuchsia.atlassian.net/browse/DX-599))
  * Casting ([DX-479](https://fuchsia.atlassian.net/browse/DX-479))
  * Global and static variables ([DX-481](https://fuchsia.atlassian.net/browse/DX-481))
  * Pretty-printing (especially for STL) ([DX-601](https://fuchsia.atlassian.net/browse/DX-601))

### Controlling execution (stepping, etc.)

When a thread is suspended (see “Working with threads” above) you can control
its execution:

`next` / `n`: Advances to the next line, stepping over function calls.

```
[zxdb] n
```

`step` / `s`: Advances to the next line. If a function call happens before the
next line, that function will be stepped into and execution will stop at the
beginning of it.

```
[zxdb] s
```

`finish` / `fi`: Exits the function and stops right after the call.

```
[zxdb] finish
```

`until` / `u`: Given a location (the same as breakpoints, see above), continues
the thread until execution gets there. For example, to run until line 45 of the
current file:

```
[zxdb] u 45
```

There different things you can do with context. For example, to run until
execution gets back to a given stack frame:

```
[zxdb] frame 2 until
```

### Assembly language

There are commands that deal with assembly language:

  * `disassemble` / `di`: Disassemble at the current location (or a given
    location)

  * `nexti` / `ni`: Step to the next instruction, stepping over function calls.

  * `stepi` / `si`: Step the the next instruction, following function calls.

  * `regs`: Get the CPU registers.

zxdb maintains information about whether the last command was an assembly
command or a source-code and will show that information on stepping or
breakpoint hits. To switch to assembly-language mode, type `disassemble`, and
to switch back to source-code mode, type `list`.

### Low-level memory

  * `mem-read` / `x`: Dumps memory

  * `stack`: Provides a low-level analysis of the stack. This is a handy
    command for low-level debugging.
