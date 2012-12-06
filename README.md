

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

    $ cgexec -g memory:/juser-cgroup ./main c 10 20 30 40

Should report about 100 MiBs (because ./main forks 3 times and the processes
allocate different amounts of memory, i.e.  10. 20, 30 and 40 MiBs - at the
same time):

    $ cat /sys/fs/cgroup/memory/juser-cgroup/memory.max_usage_in_bytes

Resets the highwater mark:

    # echo 0 > /sys/fs/cgroup/memory/juser-cgroup/memory.max_usage_in_bytes

New reset value. It is not to exactly 0 - the [kernel documentation][1]
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


[1]: http://www.kernel.org/doc/Documentation/cgroups/memory.txt


