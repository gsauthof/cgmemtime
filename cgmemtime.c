// cgmemtime - high-water memory usage of a command including descendant processes
//
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: © 2022 Georg Sauthoff <mail@gms.tf>

// for asprintf(), clone3(), ...
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>        // open(), ...
#include <sys/resource.h> // struct rusage
#include <linux/sched.h>  // clone3(), ...
#include <sched.h>        // clone3(), ...
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>        // fprintf(), ...
#include <stdlib.h>       // exit(), ...
#include <string.h>
#include <sys/stat.h>     // mkdirat(), ...
#include <sys/syscall.h>  // SYS_*, ...
#include <sys/wait.h>     // waitid macros, ...
#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 36
    #include <linux/wait.h>   // waitid macros, ...
#endif
#include <time.h>         // clock_gettime(), ...
#include <unistd.h>       // getopt(), ...


static int clone3(struct clone_args *args)
{
    long r = syscall(SYS_clone3, args, sizeof *args);
    return r;
}

static int raw_waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options,
        struct rusage *rusage)
{
    long r = syscall(SYS_waitid, idtype, id, infop, options, rusage);
    return r;
}


struct Args {
    const char *cg_fs_dir;
    char *cg_dir;
    char **child_argv;
    bool machine_readable;
    char delim;
};
typedef struct Args Args;
static void help(FILE *o, const char *argv0)
{
    fprintf(o, "%s - high-water memory usage of a command including descendants\n"
            "Usage: %s [OPTION..] COMMAND [COMMAND_OPTION..]\n"
            "\n"
            "Options:\n"
            "    -h            Display this help text\n"
            "    -m CGFS       Cgroup v2 base (default: /sys/fs/cgroup)\n"
            "    -c BASE_DIR   Cgroup directory, must end in XXXXXX\n"
            "                  (default: $CGFS/user.slice/user-$UID.slice/user@$UID.service/$RANDOM)\n"
            "    -t            machine readable output (delimited columns)\n"
            "    -d CHAR       column delimiter (default: ';')\n"
            "\n"
            "2022, Georg Sauthoff, GPLv3+\n"
            "\n"
            , argv0, argv0);
}

static void parse_args(int argc, char **argv, Args *args)
{
    *args = (const Args){
        .cg_fs_dir = "/sys/fs/cgroup",
        .delim = ';'
    };
    char c = 0;
    bool found_cmd = false;
    // '-' prefix: no reordering of arguments, non-option (positional) arguments are
    // returned as argument to the 1 option
    // ':': preceding option takes a mandatory argument
    while (!found_cmd && (c = getopt(argc, argv, "-c:d:m:ht")) != -1) { 
        switch (c) {
            case 1:
                found_cmd = true;
                break;
            case '?':
                fprintf(stderr, "unexpected option character: %c\n", optopt);
                exit(125);
                break;
            case 'm':
                args->cg_fs_dir = optarg;
                break;
            case 'c':
                args->cg_dir = optarg;
                break;
            case 'd':
                args->delim = *optarg;
                break;
            case 'h':
                help(stdout, argv[0]);
                exit(0);
                break;
            case 't':
                args->machine_readable = true;
                break;
        }
    }
    if (!found_cmd) {
        fprintf(stderr, "No command found in the arguments!\n");
        exit(125);
    }
    int i = optind - 1;
    args->child_argv = argv + i;

    size_t uid = getuid();
    if (asprintf(&args->cg_dir, "%s/user.slice/user-%zu.slice/user@%zu.service/cgmt-XXXXXX",
            args->cg_fs_dir, uid, uid) == -1) {
        fprintf(stderr, "couldn't allocate cg string\n");
        exit(125);
    }
}

static int add_memory_controller(int cg_fd, const Args *args)
{
    const char fn[] = "cgroup.subtree_control";
    int fd = openat(cg_fd, fn, O_WRONLY);
    if (fd == -1) {
        fprintf(stderr, "Can't open %s/%s (errno: %d - %s)\n", args->cg_dir, fn,
                errno, strerrorname_np(errno));
        return -1;
    }
    const char s[] = "+memory";
    ssize_t l = write(fd, s, sizeof s - 1);
    if (l == -1) {
        fprintf(stderr, "Write to %s/%s failed (errno: %d - %s)\n", args->cg_dir, fn,
                errno, strerrorname_np(errno));
        return -2;
    }
    if (l != sizeof s - 1) {
        fprintf(stderr, "Partial write to %s/%s\n", args->cg_dir, fn);
        return -3;
    }
    if (close(fd) == -1) {
        fprintf(stderr, "Can't close %s/%s (errno: %d - %s)\n", args->cg_dir, fn,
                errno, strerrorname_np(errno));
        return -4;
    }
    return 0;
}

static int check_cgroupfs(const Args *args)
{
    char *fn = 0;
    if (asprintf(&fn, "%s/cgroup.subtree_control", args->cg_fs_dir) == -1) {
        fprintf(stderr, "can't allocate cgroup.subtree_control string\n");
        return -1;
    }
    int r = 0;
    int fd = open(fn, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "Can't open %s (errno: %d - %s)\n",
                fn, errno, strerrorname_np(errno));
        r = -2;
        goto error;
    }
    char buf[1024] = {0};
    ssize_t l = read(fd, buf, sizeof buf - 1);
    if (l == -1) {
        fprintf(stderr, "Can't read %s (errno: %d - %s)\n",
                fn, errno, strerrorname_np(errno));
        r = -3;
        goto error;
    }
    const char *p = strstr(buf, "memory");
    if (!p || !(p[6] == 0 || p[6] == ' ')) {
        fprintf(stderr, "Cgroup memory controller isn't enabled in %s"
                " (systemd should enable it, by default)\n", fn);
        r = -23;
        goto error;
    }
error:
    if (fd != -1)
        close(fd);
    free(fn);
    return r;
}

static int setup_cgroup(const Args *args, int *ofd)
{
    if (!mkdtemp(args->cg_dir)) {
        fprintf(stderr, "Can't create %s (errno: %d - %s)\n", args->cg_dir,
                errno, strerrorname_np(errno));
        return -1;
    }
    int fd = open(args->cg_dir, O_PATH);
    if (fd == -1) {
        fprintf(stderr, "Can't open %s (errno: %d - %s)\n", args->cg_dir,
                errno, strerrorname_np(errno));
        return -2;
    }

    // otherwise, without the nested setup we can't add a process to the parent cgroup
    // because we also need to write its cgroup.subtree_control file Cgroup v2
    // disallows doing both (yields EBUSY) - cf. https://unix.stackexchange.com/a/713343/1131
    const char fn[] = "leaf";
    if (mkdirat(fd, fn, 0700) == -1) {
        fprintf(stderr, "Can't mdir %s/%s (errno: %d - %s)\n", args->cg_dir, fn,
                errno, strerrorname_np(errno));
        return -3;
    }

    if (add_memory_controller(fd, args))
         return -3;

    int cg_fd = openat(fd, fn, O_PATH);
    if (fd == -1) {
        fprintf(stderr, "Can't open %s/%s (errno: %d - %s)\n", args->cg_dir, fn,
                errno, strerrorname_np(errno));
        return -4;
    }

    *ofd = fd;
    return cg_fd;
}

struct Result {
  struct timeval  child_user;
  struct timeval  child_sys;
  struct timespec child_wall;

  size_t child_rss_highwater;
  size_t cg_rss_highwater;
};
typedef struct Result Result;


static void print_timeval(FILE *o, const struct timeval *t)
{
    fprintf(o, "%7.3f s",
            (double) t->tv_sec + (double)t->tv_usec / (1000.0 * 1000.0));
}

static void print_timeval_m(FILE *o, const struct timeval *t)
{
    fprintf(o, "%g",
            (double) t->tv_sec + (double)t->tv_usec / (1000.0 * 1000.0));
}

static void print_timespec(FILE *o, const struct timespec *t)
{
    fprintf(o, "%7.3f s",
            (double) t->tv_sec + (double)t->tv_nsec / 1000000000.0);
}

static void print_timespec_m(FILE *o, const struct timespec *t)
{
    fprintf(o, "%g",
            (double) t->tv_sec + (double)t->tv_nsec / 1000000000.0);
}

static int pretty_print(FILE *o, const Result *res)
{
    fputc('\n', o);
    fputs("user: ", o);
    print_timeval(o, &res->child_user);
    fputc('\n', o);
    fputs("sys:  ", o);
    print_timeval(o, &res->child_sys);
    fputc('\n', o);
    fputs("wall: ", o);
    print_timespec(o, &res->child_wall);
    fputc('\n', o);
    fprintf(o, "child_RSS_high: %10zu KiB\n",
            res->child_rss_highwater / 1024
           );
    fprintf(o, "group_mem_high: %10zu KiB\n",
            res->cg_rss_highwater / 1024
           );
    return 0;
}

static int machine_print(FILE *o, const Args *args, const Result *res)
{
    print_timeval_m(o, &res->child_user);
    fputc(args->delim, o);
    print_timeval_m(o, &res->child_sys);
    fputc(args->delim, o);
    print_timespec_m(o, &res->child_wall);
    fputc(args->delim, o);
    fprintf(o, "%zu%c%zu",
            res->child_rss_highwater / 1024,
            args->delim,
            res->cg_rss_highwater    / 1024
           );
    fputc('\n', o);
    return 0;
}

static void print_result(FILE *o, const Args *args, const Result *res)
{
    if (args->machine_readable)
        machine_print(o, args, res);
    else
        pretty_print(o, res);
}


static struct timespec sub_ts(const struct timespec *a, const struct timespec *b)
{
    struct timespec r = {
        .tv_sec  = a->tv_sec  - b->tv_sec,
        .tv_nsec = a->tv_nsec - b->tv_nsec
    };
    if (r.tv_nsec < 0) {
        --r.tv_sec;
        r.tv_nsec += 1000000000l;
    }
    return r;
}

static int read_cg_rss_high(int cg_fd, size_t *rss)
{
    int fd = openat(cg_fd, "memory.peak", O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "Can't open memory.peak: %d - %s\n", errno, strerrorname_np(errno));
        return -1;
    }
    char b[21] = {0};
    ssize_t l = read(fd, b, sizeof b - 1);
    if (l == -1) {
        fprintf(stderr, "Can't read memory.peak: %d - %s\n", errno, strerrorname_np(errno));
        close(fd);
        return -2;
    }
    if (close(fd) == -1) {
        fprintf(stderr, "Can't close memory.peak: %d - %s\n", errno, strerrorname_np(errno));
        return -3;
    }
    *rss = atol(b);
    return 0;
}

static int execute(const Args *args, int cg_fd, Result *res)
{
    int pid_fd = -1;
    struct clone_args ca = {
        .flags = CLONE_INTO_CGROUP | CLONE_PIDFD | CLONE_VFORK,
        .pidfd = (uintptr_t) &pid_fd,
        // otherwise, there is nothing to wait on
        .exit_signal = SIGCHLD,
        .cgroup = cg_fd
    };
    struct timespec start = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &start) == -1) {
        fprintf(stderr, "gettime start failed: %d - %s\n", errno, strerrorname_np(errno));
        return -1;
    }
    int pid = clone3(&ca);
    if (pid == -1) {
        fprintf(stderr, "clone3 failed: %d - %s\n", errno, strerrorname_np(errno));
        return -2;
    }
    if (!pid) { // child
        execvp(*args->child_argv, args->child_argv);
        fprintf(stderr, "execvp failed: %d - %s\n", errno, strerrorname_np(errno));
        if (errno == ENOENT)
            _exit(127);
        _exit(126);
    } else { // parent
        struct sigaction sa = {
            .sa_handler = SIG_IGN
        };
        // otherwise, Ctrl+C/+] also kill cgmemtime before it has a chance printing its summary
        if (sigaction(SIGINT, &sa, 0) == -1)
            fprintf(stderr, "failed to ignore SIGINT: %d - %s\n", errno, strerrorname_np(errno));
        if (sigaction(SIGQUIT, &sa, 0) == -1)
            fprintf(stderr, "failed to ignore SIGQUIT: %d - %s\n", errno, strerrorname_np(errno));

        siginfo_t info = {0};
        struct rusage usg = {0};
        if (raw_waitid(P_PIDFD, pid_fd, &info, WEXITED, &usg) == -1) {
            fprintf(stderr, "waitid failed: %d - %s\n", errno, strerrorname_np(errno));
            return -3;
        }
        struct timespec stop = {0};
        if (clock_gettime(CLOCK_MONOTONIC, &stop) == -1) {
            fprintf(stderr, "gettime stop failed: %d - %s\n", errno, strerrorname_np(errno));
            return -4;
        }
        res->child_user = usg.ru_utime;
        res->child_sys  = usg.ru_stime;
        res->child_wall = sub_ts(&stop, &start);
        res->child_rss_highwater = usg.ru_maxrss * 1024;
        if (read_cg_rss_high(cg_fd, &res->cg_rss_highwater))
            return -5;
        if (info.si_code == CLD_KILLED)
            return 128 + info.si_status;
        return info.si_status;
    }
    return 0;
}

static int teardown_cgroup(const Args *args, int cgp_fd, int cg_fd)
{
    const char fn[] = "leaf";
    if (close(cg_fd) == -1) {
        fprintf(stderr, "Can't close %s/%s (errno: %d - %s)\n", args->cg_dir, fn,
                errno, strerrorname_np(errno));
        return -1;
    }
    if (unlinkat(cgp_fd, fn, AT_REMOVEDIR) == -1) {
        fprintf(stderr, "Can't remove %s/%s (errno: %d - %s)\n", args->cg_dir, fn,
                errno, strerrorname_np(errno));
        return -2;
    }
    if (close(cgp_fd) == -1) {
        fprintf(stderr, "Can't close %s (errno: %d - %s)\n", args->cg_dir,
                errno, strerrorname_np(errno));
        return -3;
    }
    if (rmdir(args->cg_dir) == -1) {
        fprintf(stderr, "Can't remove %s (errno: %d - %s)\n", args->cg_dir,
                errno, strerrorname_np(errno));
        return -4;
    }
    return 0;
}

int main(int argc, char **argv)
{
    Args args = {0};
    parse_args(argc, argv, &args);

    if (check_cgroupfs(&args)) {
        return 124;
    }

    int cgp_fd = -1;
    int cg_fd = setup_cgroup(&args, &cgp_fd);
    if (cg_fd < 0)
        return 123;

    Result res = {0};
    int r = execute(&args, cg_fd, &res);
    print_result(stderr, &args, &res);

    if (teardown_cgroup(&args, cgp_fd, cg_fd))
        return 122;

    if (r < 0)
        return 121;
    return r;
}
