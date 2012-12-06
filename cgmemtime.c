

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>

struct Options {
  bool help;
  bool machine_readable;
  bool force_empty;

  bool setup_mode;
  char user[32];
  char group[32];
  char perm[5];

  char cgfs_base[128];
  char cgfs_top[128];
  char sub_group[512];
  int argc;
  char **argv;
};
typedef struct Options Options;

struct Output {
  struct timeval child_user;
  struct timeval child_wall;
  struct timeval child_sys;

  size_t child_rss_highwater;

  size_t cg_rss_highwater;
};
typedef struct Output Output;


static void init_options(Options *opts)
{
  *opts = (Options) {
    .user = "root",
    .group = "root",
    .perm = "755",

    .cgfs_base = "/sys/fs/cgroup",
    .cgfs_top = "memory/cgmemtime"
  };
}

static int parse_options(int argc, char **argv, Options *opts)
{
  init_options(opts);

  int i = 1;
  enum Base { NONE, BASE, ROOT, USER, GROUP, PERM};
  enum Base next = NONE;
  for (; i<argc; ++i) {
    const char *o = argv[i];
    if (next > NONE) {
      switch (next) {
        case BASE:
          strncpy(opts->cgfs_base, o, 127);
          break;
        case ROOT:
          strncpy(opts->cgfs_top, o, 127);
          break;
        case USER:
          strncpy(opts->user, o, 31);
          break;
        case GROUP:
          strncpy(opts->group, o, 31);
          break;
        case PERM:
          strncpy(opts->perm, o, 4);
          break;
        default:
          break;
      }
      next = NONE;
      continue;
    }
    if (!strcmp(o, "-h") || !strcmp(o, "--help")) {
      opts->help = true;
    } else if (!strcmp(o, "-t") || !strcmp(o, "--tabular")) {
      opts->machine_readable = true;
    } else if (!strcmp(o, "--base")) {
      next = BASE;
    } else if (!strcmp(o, "--root")) {
      next = ROOT;
    } else if (!strcmp(o, "--setup")) {
      opts->setup_mode = true;
    } else if (!strcmp(o, "-u") || !strcmp(o, "--user")) {
      next = USER;
    } else if (!strcmp(o, "-g") || !strcmp(o, "--group")) {
      next = GROUP;
    } else if (!strcmp(o, "--perm")) {
      next = PERM;
    } else if (!strcmp(o, "--force-empty")) {
      opts->force_empty = true;
    } else {
      break;
    }
  }
  if (i==argc && !opts->help && !opts->setup_mode) {
    fprintf(stderr, "No program to execute given.\n");
    return -1;
  }
  if (i<argc && (opts->help || opts->setup_mode)) {
    fprintf(stderr, "Spurious arguments.\n");
    return -2;
  }
  opts->argc = argc-i;
  opts->argv = argv+i;
  return 0;
}

static void help(const char *prog)
{
  printf("Call: %s (OPTION)* PROGRAM (PROGRAM_OPTION)*\n"
      "\n"
      "Prints the high-water mark of resident set size (RSS) memory usage of"
      "PROGRAM and all its descendants.\n"
      "\n"
      "That means that memory usage is measured recursively and accumulatively.\n"
      "\n"
      "Options:\n"
      "\t-h, --help\tthis screen\n"
      "\t-t, --tabular\tmachine readable output\n"
      "\t--base STR\tbase of cgroup fs (default: /sys/fs/cgroup)\n"
      "\t--root STR\troot of the configured hierachy (default: memory/cgmemtime)\n"
      "\t\ti.e. BASE/ROOT has to be writable\n"
      "\t\te.g. /sys/fs/cgroup/memory/cgmemtime has to be writable\n"
      "\t--force-empty\t'call' force-empty before rmdir\n"
      "\t\t (e.g. forces cleanup of cached pages)\n"
      "\n"
      "\t--setup\tsetup mode (create sysfs hierachy under BASE)\n"
      "\t-u, --user STR\tuser name (default: root)\n"
      "\t-g, --group  STR\tgroup name (default: root)\n"
      "\t--perm OCTAL\tpermissions of the cgroup directory (default: 755)\n"
      "\n"
      ,
      prog);
}

static int cat(const char *file, char *out, size_t len)
{
  if (!out || !len)
    return -2;
  int fd = open(file, O_RDONLY);
  if (fd == -1) {
    fprintf(stderr, "Opening %s failed: ", file);
    perror(0);
    return -3;
  }
  ssize_t r = read(fd, out, len-1);
  if (r == -1) {
    fprintf(stderr, "Reading from %s failed: ", file);
    perror(0);
    return -1;
  }
  out[r] = 0;
  int ret = close(fd);
  if (ret == -1) {
    fprintf(stderr, "Closing %s failed: ", file);
    perror(0);
    return -4;
  }
  return 0;
}

static int verify_max_zero(const Options *opts)
{
  char file[512] = {0};
  snprintf(file, 512, "%s/memory.max_usage_in_bytes", opts->sub_group);
  char out[32] = {0};
  int ret = cat(file, out, 32);
  if (ret)
    return -1;
  if (strcmp(out, "0\n")) {
    fprintf(stderr, "File %s contains |%s| "
        "instead of zero followed by a newline\n", file, out);
    return -2;
  }
  return 0;
}

static int setup_cg(Options *opts)
{
  char pid_str[22] = {0};
  snprintf(pid_str, 22, "%zd", (ssize_t)getpid());
  char sub_group[512] = {0};
  snprintf(sub_group, 512, "%s/%s/%s",
      opts->cgfs_base, opts->cgfs_top, pid_str);
  int ret = mkdir(sub_group, 0755);
  if (ret == -1) {
    fprintf(stderr, "Could not create new sub-cgroup %s: ", sub_group);
    perror(0);
    return -1;
  }
  strncpy(opts->sub_group, sub_group, 511);
  ret = verify_max_zero(opts);
  if (ret)
    return -1;
  return 0;
}

static int echo(const char *out, const char *file)
{
  if (!out)
    return -2;
  size_t len = strlen(out);
  if (!len)
    return -5;
  int fd = open(file, O_WRONLY);
  if (fd == -1) {
    fprintf(stderr, "Opening %s failed: ", file);
    perror(0);
    return -3;
  }
  ssize_t r = write(fd, out, len);
  if (r == -1) {
    fprintf(stderr, "Writing to %s failed: ", file);
    perror(0);
    return -1;
  }
  if (r != len) {
    fprintf(stderr, "Wrote only %zd of %zu bytes ", r, len);
    return -1;
  }
  int ret = close(fd);
  if (ret == -1) {
    fprintf(stderr, "Closing %s failed: ", file);
    perror(0);
    return -4;
  }
  return 0;
}

static int force_empty(const Options *opts)
{
  if (!opts->force_empty)
    return 0;
  char file[256] = {0};
  snprintf(file, 256, "%s/memory.force_empty", opts->sub_group);
  int ret = echo("0", file);
  if (ret)
    return -1;
  return 0;
}

static int cleanup_cg(const Options *opts)
{
  int ret_fe = force_empty(opts);
  int ret = 0;
  ret = rmdir(opts->sub_group);
  if (ret == -1) {
    fprintf(stderr, "Could not remove sub-cgroup %s: ", opts->sub_group);
    perror(0);
    return -1;
  }
  if (ret_fe)
    return -2;
  return 0;
}

static int run_child(const Options *opts)
{
  int ret = execvp(*opts->argv, opts->argv);
  fprintf(stderr, "Execvp of %s failed (ret: %d): ", *opts->argv, ret);
  perror(0);
  if (errno == ENOENT)
    exit(127);
  exit(23);
  return -1;
}


static int store_cg_rss_highwater(const Options *opts, Output *output)
{
  char file[512] = {0};
  snprintf(file, 512, "%s/memory.max_usage_in_bytes", opts->sub_group);
  char out[32] = {0};
  int ret = cat(file, out, 32);
  if (ret)
    return -1;
  output->cg_rss_highwater = atoi(out);
  return 0;
}

static int add_pid_to_cg(const Options *opts, pid_t pid)
{
  char file[512] = {0};
  snprintf(file, 512, "%s/tasks", opts->sub_group);
  char pid_str[32] = {0};
  snprintf(pid_str, 32, "%zd", (ssize_t)pid);
  int ret = echo(pid_str, file);
  if (ret)
    return -1;
  return 0;
}

static int store_child_wall(const struct timeval *start,
    const struct timeval *end, Output *output)
{
  struct timeval t = {0};
  t.tv_sec = end->tv_sec - start->tv_sec;
  if (end->tv_usec <= start->tv_usec)
    t.tv_usec = end->tv_usec - start->tv_usec;
  else {
    --t.tv_sec;
    t.tv_usec = (1000*1000) - (start->tv_usec - end->tv_usec);
  }
  output->child_wall = t;
  return 0;
}

static int execute(const Options *opts, Output *output)
{
  struct timeval start_time = {0};
  struct timeval end_time = {0};
  int ret_g = gettimeofday(&start_time, 0);
  if (ret_g == -1) {
    perror(0);
    return -4;
  }

  int ret = fork();
  if (ret < 0) {
    perror("fork failed");
    return -1;
  }
  if (ret) { // parent
    int status = 0;
    struct rusage usg = {0};
    pid_t p = wait4(ret, &status, 0, &usg);
    if (p == -1) {
      perror("Could not wait on child");
      return -1;
    }
    ret_g = gettimeofday(&end_time, 0);
    if (ret_g == -1) {
      perror(0);
      return -4;
    }
    store_child_wall(&start_time, &end_time, output);
    output->child_rss_highwater = usg.ru_maxrss * 1024;
    output->child_user = usg.ru_utime;
    output->child_sys = usg.ru_stime;
    store_cg_rss_highwater(opts, output);
    if (WIFEXITED(status))
      return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
      return 128 + WTERMSIG(status);
    fprintf(stderr,
        "wait4 returned without error or termination condition?!?");
    return -1;
  } else { // child
    int ret = add_pid_to_cg(opts, getpid());
    if (ret)
      return -3;
    // does not return
    run_child(opts);
    return -2;
  }
  return 0;
}

static void print_timeval(FILE *o, const struct timeval *t)
{
  fprintf(o, "%8.3f s",
      (double) t->tv_sec + (double)t->tv_usec / (1000.0 * 1000.0));
}

static void print_timeval_m(FILE *o, const struct timeval *t)
{
  fprintf(o, "%g",
      (double) t->tv_sec + (double)t->tv_usec / (1000.0 * 1000.0));
}

static int pretty_print(const Output *out)
{
  fprintf(stderr, "Child user: ");
  print_timeval(stderr, &out->child_user);
  fprintf(stderr, "\n");
  fprintf(stderr, "Child sys : ");
  print_timeval(stderr, &out->child_sys);
  fprintf(stderr, "\n");
  fprintf(stderr, "Child wall: ");
  print_timeval(stderr, &out->child_wall);
  fprintf(stderr, "\n");
  fprintf(stderr, "Child high-water RSS                    : %10zu KiB\n",
      out->child_rss_highwater/1024
      );
  fprintf(stderr, "Recursive and accumulated high-water RSS: %10zu KiB\n",
      out->cg_rss_highwater/1024
      );
  return 0;
}

static int machine_print(const Output *out)
{
  print_timeval_m(stderr, &out->child_user);
  fprintf(stderr, ";");
  print_timeval_m(stderr, &out->child_sys);
  fprintf(stderr, ";");
  print_timeval_m(stderr, &out->child_wall);
  fprintf(stderr, ";");
  fprintf(stderr, "%zu;%zu",
      out->child_rss_highwater/1024,
      out->cg_rss_highwater/1024
      );
  fprintf(stderr, "\n");
  return 0;
}

static void print(const Options *opts, const Output *output)
{
  if (opts->machine_readable)
    machine_print(output);
  else
    pretty_print(output);
}

static int verify_tasks_empty(const Options *opts)
{
  char out[1024] = {0};
  char file[512] = {0};
  snprintf(file, 512, "%s/tasks", opts->sub_group);
  int ret = cat(file, out, 1024);
  if (ret)
    return -1;
  if (*out) {
    fprintf(stderr,
        "Child terminated but following descendants are still running: %s\n",
        out);
    return -2;
  }
  return 0;
}

static int get_uid_gid(const Options *opts, uid_t *uid, gid_t *gid)
{
  errno = 0;
  struct passwd *pwnam = getpwnam(opts->user);
  if (!pwnam) {
    fprintf(stderr, "Could not find uid of %s: ", opts->user);
    if (errno)
      perror(0);
    else
      fprintf(stderr, "\n");
    return -1;
  }
  *uid = pwnam->pw_uid;
  errno = 0;
  struct group *grnam = getgrnam(opts->group);
  if (!grnam) {
    fprintf(stderr, "Could not find gid of %s: ", opts->group);
    if (errno)
      perror(0);
    else
      fprintf(stderr, "\n");
    return -2;
  }
  *gid = grnam->gr_gid;
  return 0;
}

static int get_perm(const Options *opts, mode_t *perm)
{
  errno = 0;
  long p = strtol(opts->perm, 0, 8);
  if (errno) {
    fprintf(stderr, "Could not convert octal permission string %s: ",
        opts->perm);
    perror(0);
    return -1;
  }
  *perm = p;
  return 0;
}

static int setup_root(const Options *opts)
{
  uid_t uid = 0;
  gid_t gid = 0;
  int ret = 0;
  ret = get_uid_gid(opts, &uid, &gid);
  if (ret)
    return -1;
  mode_t perm = 0755;
  ret = get_perm(opts, &perm);
  if (ret)
    return -2;
  char name[512] = {0};
  snprintf(name, 512, "%s/%s", opts->cgfs_base, opts->cgfs_top);
  ret = mkdir(name, perm);
  if (ret) {
    fprintf(stderr, "Could not mkdir %s: ", name);
    perror(0);
    return -3;
  }
  // mkdir honors umask ...
  ret = chmod(name, perm);
  if (ret) {
    fprintf(stderr, "Could not chmod %o %s: ", perm, name);
    perror(0);
    return -5;
  }
  ret = chown(name, uid, gid);
  if (ret) {
    fprintf(stderr, "Could not chown %s (uid: %zd, gid: %zd): ",
        name, (ssize_t)uid, (ssize_t)gid);
    perror(0);
    return -4;
  }
  return 0;
}

int main(int argc, char **argv)
{
  Options opts = {0};
  int ret = 0;
  ret = parse_options(argc, argv, &opts);
  if (ret)
    return 24;
  if (opts.help) {
    help(*argv);
    return 0;
  }
  if (opts.setup_mode) {
    ret = setup_root(&opts);
    if (ret)
      return 25;
    return 0;
  }
  ret = setup_cg(&opts);
  if (ret)
    return 26;
  Output output = {0};
  ret = execute(&opts, &output);
  int ret2 = 0;
  ret2 = verify_tasks_empty(&opts);
  int r = cleanup_cg(&opts);
  ret2 = ret2 || r;
  if (ret >= 0) {
    print(&opts, &output);
    if (!ret && ret2)
      return 27;
    return ret;
  }
  if (ret || ret2)
    return 28;
  return 0;
}


