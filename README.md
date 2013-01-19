cgmemtime measures the high-water RSS memory usage of a process
and its descendant processes.

To be able to do so it puts the process into its own
[cgroup][cg].

For example process A allocates 10 [MiB][mib] and forks a child B that
allocates 20 MiB and that forks a child C that allocates 30 MiB.
All three processes share a time window where their allocations
result in corresponding [RSS (resident set size)][rss] memory usage.

The question now is: How much memory is actually used as a result
of running A?

Answer: 60 MiB

cgmemtime is the tool to answer such questions.

(It also measures the runtime.)

Date: 2012-12-06

## Usage

Before running cgmemtime the first time one has to setup a
hierarchy under /sys/fs/cgroup:

    $ sudo ./cgmemtime --setup -g myusergroup --perm 775

Which creates by default:

    /sys/fs/cgroup/memory/cgmemtime

Now you can use cgmemtime like this:

    $ ./cgmemtime ./testa x 10 20 30
    [..]
    Child high-water RSS                    :      10720 KiB
    Recursive and accumulated high-water RSS:      61824 KiB

Or to produce machine readable output:

    $ ./cgmemtime -t ./testa x 10 20 30

It also has some options (cf. `-h`).

## Dependencies

cgmemtime runs on a Linux system that comes with [cgroups support][cg].
For example Fedora 17 comes with cgroups (Control Groups)
enabled by default.  Every system using [systemd][systemd] has
cgroups support.

For example Ubuntu 10.04 LTS does not have cgroups, but 12.04
should have it. RHEL/CentOS should provide cgroups support since
version 6.

Other than that you need a C compiler, GNU make and the usual
development headers.

## Compile

Just:

    $ make

Which creates `cgmemtime` and `testa`. `testa` is a small forking
allocation test program.

## Testing

There is a shell script that contains some test cases. After
setting up the cgroup hierachy via

    $ sudo ./cgmemtime --setup -g myusergroup --perm 775

you can run the test suite:

    $ bash test.sh

## Contact

Don't hesitate to mail feedback (comments, questions, ...) to:

    Georg Sauthoff <mail@georg.so>

## Licence

[GPLv3+][gpl]

## Accuracy

The reported high-water RSS usage values are as accurate as the
`usage_in_bytes` value exported by the cgroup memory resource
controller.

The [kernel documentation][cgmem] states:

> For efficiency, as other kernel components, memory cgroup uses
some optimization to avoid unnecessary cacheline false sharing.
usage_in_bytes is affected by the method and doesn't show 'exact'
value of memory(and swap) usage, it's an fuzz value for efficient
access. (Of course, when necessary, it's synchronized.) If you
want to know more exact memory usage, you should use
RSS+CACHE(+SWAP) value in memory.stat(see 5.2).

([Section 5.5][cgmem])

We can't use memory.stat because it does not include high-water
memory usage information.

Doing some tests with e.g. `./testa` the reported values seem to
be exact enough, though.

## Manually testing cgroups

Setup new cgroup (as root):

    # cgcreate -t juser:juser -g memory:/juser-cgroup

No task should be part of that cgroup in the beginning:

    $ cat /sys/fs/cgroup/memory/juser-cgroup/tasks

Highwater RSS usage - should be 0:

    $ cat /sys/fs/cgroup/memory/juser-cgroup/memory.max_usage_in_bytes

Should report more accurate meassurements - but does not include highwater
marks:

    $ cat /sys/fs/cgroup/memory/juser-cgroup/memory.stat

Current RSS usage in that group:

    $ cat /sys/fs/cgroup/memory/juser-cgroup/memory.usage_in_bytes

Add new task to the group:

    $ cgexec -g memory:/juser-cgroup ./testa c 10 20 30 40

Should report about 100 MiB (because ./testa forks 3 times and the processes
allocate different amounts of memory, i.e.  10. 20, 30 and 40 MiBs - at the
same time):

    $ cat /sys/fs/cgroup/memory/juser-cgroup/memory.max_usage_in_bytes

Resets the highwater mark:

    # echo 0 > /sys/fs/cgroup/memory/juser-cgroup/memory.max_usage_in_bytes

New reset value. It is not to exactly 0 - the [kernel documentation][cgmem]
mentions fuzz due to optimaztions of `memory.usage_in_bytes`.
 
    $ cat /sys/fs/cgroup/memory/juser-cgroup/memory.max_usage_in_bytes

And indeed, the above value should now equal this one:

    $ cat /sys/fs/cgroup/memory/juser-cgroup/memory.usage_in_bytes

Since the cgroups does not have any tasks now, we can use:

    # echo 0 > /sys/fs/cgroup/memory/juser-cgroup/memory.force_empty

Now the values should be both 0:

    $ cat /sys/fs/cgroup/memory/juser-cgroup/memory.max_usage_in_bytes
    $ cat /sys/fs/cgroup/memory/juser-cgroup/memory.usage_in_bytes


To remove the cgroup again:

    # cgdelete memory:/juser-cgroup


Note that all the `cg*` commands can be replaces with combinations of
`mkdir`/`chmod`/`chown`/`echo` commands that manipulate the filesystem under
`/sys/fs/cgroup/memory/`.

## Related tools

There are also other tools available which measure memory usage
of processes. One way to categorize them is a two-fold
classification: tools that use polling and tools that don't.

In that context - when you are only interested in the high-water
usage - polling is the inferior approach. As described in
previous sections, cgmemtine does not use polling. At the time of
writing, I am not aware of any other tool that uses Linux Control
Groups for memory measurements.

### Non-Polling based tools

#### Highwater measurements

- [GNU time][gtime] - uses something like `wait4()` or `waitpid()` and
  `getrusage()`, thus on systems where available it is able to
  display the high-water RSS usage of a single child process,
  when using the verbose mode.
- [tstime][tstime] - uses the taskstructs API of the Linux kernel to get
  the high-water RSS _and_ the highwater VMEM usage of a child.
  Does not follow descendant processes. Provides also a process
  monitor mode that displays stats for all exiting processes. But
  the taskstats API is kind of cumbersome to use and on current
  kernels only accessible as root.

#### Snapshot measurements

- [smem][smem] - Tool written in Python that analyses proc files
  like `/proc/$$/smaps` and generates a memory usage report of
  one ore multiple processes for one point in time. It is designed
  to provide a system-wide view, but one can also filter processes
  (or even loaded libraries) by various criteria. Smem distributes
  shared memory between all dependent processes (the result is
  called proportional set size - PSS - of a process). It does not
  take swapped-out memory into account.

### Polling based tools

- [memtime][memtime] - Uses polling of `/proc/$PID/stat` to
  measure high-water RSS/VMEM usage of a child. It supports Linux
  and Solaris styles of `/proc`.  Polling is in general a
  sub-optimal solution (e.g. short-running processes are not
  accurately measured, it wastes resources etc.). memtime is not
  maintained and has 64 Bit issues (last release 2002).
- [tmem][tmem] - Polls `/proc/$PID/status`, thus has access to
  more detailed memory measures, e.g.  VmPeak, VmSize, VmLck,
  VmPin, VmHWM, VmRSS, VmData, VmStk, VmExe, VmLib, VMPTE and
  VMSwap.
- [memusg][memusg] - Python script that polls the VmSize values
  of a group of processes via the command `ps` and displays its
  high-water mark.  That means that it forks/execs `ps` and parses
  its output 10 times a second. For a given command line it creates
  a new session (via setsid()) and executes it in that session.
  Thus, children of the watched process are likely part of that
  session, too. Memusg then sums the VMSize value of each process
  of that session up and returns the maximum when the session
  leader exits. Note, that this method is not reliable, because
  child processes may still be alive after the session leader has
  exited and they may also create new sessions during their runtime,
  thus escaping the measurement via memusg.


[mib]: http://en.wikipedia.org/wiki/Mebibyte
[cg]: http://www.kernel.org/doc/Documentation/cgroups/cgroups.txt
[cgmem]: http://www.kernel.org/doc/Documentation/cgroups/memory.txt
[systemd]: http://en.wikipedia.org/wiki/Systemd
[rss]: http://en.wikipedia.org/wiki/Resident_set_size
[gpl]: http://www.gnu.org/licenses/gpl.html
[gtime]: http://www.gnu.org/software/time/
[tstime]: https://bitbucket.org/gsauthof/tstime
[memtime]: http://www.update.uu.se/~johanb/memtime/
[tmem]: http://locklessinc.com/articles/memory_usage/
[smem]: http://www.selenic.com/smem/
[memusg]: https://github.com/jhclark/memusg

