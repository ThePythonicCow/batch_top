/*
 * Usage: batch_top [-C] [-M] [-B] [-Q] [-s n] [-t n] [-c n] [-m n] [-u n] [-p n] [-q n] [-r n] [-b n] [-n n] [-L n] [-d diskstatpath,diskname]
 *
 * Default option settings:
 *
 *  Show CPU hogs: -C 0
 *  Show Mem hogs: -M 0
 *  Show Block I/O waiters: -B 0
 *  Show count of PHP tasks: -P 0
 *  Show count of httpd tasks: -H 0
 *  Outerloop time (secs): -s 10.000
 *  Innerloop time (secs): -t 10.000
 *  Min busy loadavg: -p 5.000
 *  Min busy CPU load: -c 80.0%
 *  Min busy Mem load: -m 80.0%
 *  Min busy Cpuset memory pressure: -u 100
 *  Busy tasks (1/1000 of CPU, aka mcpus): -q 100
 *  RSS mem hogs (1/1000 of RAM, aka mrams): -r 100
 *  Block I/O waiters (msecs per sec): -b 100
 *  Max number tasks to show: -n 10
 *  Length cmdline to display: -L 48
 *  Show_disks: [-d path,name]
 *
 * Use -Q option to Quiet above option setting display upon
 * initial command invocation
 *
 * Output command name and command pid of busy tasks, when CPU(s) are busy.
 * Various options control what other tasks are shown depending on system load,
 * and what attributes of each such shown task are shown.
 *
 *   - Slowly scan /proc/loadavg (every s seconds)
 *   -   If and while loadavg exceeds limit p (average number tasks runnable)
 *   -     In inner scan loop (every t seconds):
 *   -       Read in pid, cmd, cpu and memory for all /proc/<pid>/stat
 *   -        Compare to previous values read in
 *   -        For up to n tasks using either:
 *   - 		(if -C, the default) at least -q mcpus 1/1000-ths of all CPUs,
 *   -		(if -M) at least -r mrams (1/1000-ths of RAM in task RSS)
 *   -        print cmd name, pid, mcpus and mrams.
 *
 * The outer loop is low cost, just reading /proc/loadavg each loop,
 *	and outputting a five character time mark.
 *
 * The inner loop is more expensive, reading /proc/<pid>/stat for each
 *	process <pid> listed in /proc, and outputting the worst CPU and/or
 *	memory hogs.
 *
 * Compiles cleanly with:
 *
 *    cc -Wall -pedantic -Wextra -O3 -o batch_top  batch_top.c
 *
 * Paul Jackson
 * pj@usa.net
 * Development begun: 18 Aug 2012
 * Most recent update: 4 Aug 2014
 * One more round of changes: Oct and Dec 2018
 */

#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <argz.h>

long sysconf(int name);

char *cmd;
const char *usage =
	"[-C] [-M] [-B] [-Q] [-s n] [-t n] [-c n] [-m n] [-u n] [-p n] [-q n] "
	"[-r n] [-b n] [-n n] [-L n] [-d diskstatpath,diskname]";

void show_current_settings();

void show_usage_and_exit()
{
	fprintf(stderr, "Usage: %s %s\n", cmd, usage);
	show_current_settings();
	exit(1);
}

void fatal_usage(char *msg, int val)
{
	fprintf(stderr, "%s: Invalid option value %d: %s\n", cmd, val, msg);
	show_usage_and_exit();
}

#define perror_exit(msg, arg) __perror_exit(msg, arg, __FILE__, __LINE__, errno)

void __perror_exit(const char *msg, const char *arg,
		const char *file, int lineno, int err)
{
	if (arg) {
		fprintf(stderr, "%s: %s <%s> (file %s, line %d)\n",
			msg, strerror(err), arg, file, lineno);
	} else {
		fprintf(stderr, "%s: %s (file %s, line %d)\n",
			msg, strerror(err), file, lineno);
	}
	exit(1);
}

#define DEF_s 10.0	/* outer loop s default 10 seconds */
#define DEF_t 10.0	/* inner loop t default 10 seconds */

/* Inner loop per-task monitoring only done when "system busy" */
#define DEF_p   5.0	/* system busy when > 5 tasks runnable */
#define DEF_c  80.0	/* system busy when > 80% CPU load */
#define DEF_m  80.0	/* system busy when > 80% Mem load */
#define DEF_u 100	/* system busy when > 100 top cpuset memory pressure */

#define DEF_q 100	/* default cpu usage 100 msecs per sec */
#define DEF_b 100 	/* default diskwait 100 msecs per sec */
#define DEF_r 100	/* default rss size 100 mrams (100/1000 of RAM) */
#define DEF_n 10	/* default 10 max number of busy tasks to print */

#define DEF_L 48	/* default length cmdline to show for "hog" tasks */

double val_s = DEF_s;	/* outer loop cycle time in seconds */
double val_t = DEF_t;	/* inner loop cycle time in seconds */

double val_p = DEF_p;	/* load average that triggers inner loop */
double val_c = DEF_c;	/* CPU load % that triggers inner loop */
double val_m = DEF_m;	/* Mem load % that triggers inner loop */
int val_u = DEF_u;	/* Cpuset memory pressure that triggers inner loop */

long val_q = DEF_q;	/* msecs/sec of all CPU usage threshold of busy tasks */
long val_b = DEF_b;	/* msecs/sec waiting on block I/O (diskwait) */
long val_r = DEF_r;	/* kb/1000 kb of RAM in rss of a big task */
long val_n = DEF_n;	/* max number of busy tasks to print each inner loop */

/* By default, show just CPU hogs.  If both set, show both. */

int flag_C = 0;		/* If set, show CPU hogs (defaults to set, below) */
int flag_M = 0;		/* If set, show Memory hogs */
int flag_B = 0;		/* If set, show tasks slowed by block I/O diskwait */
int flag_P = 0;		/* If set, show count PHP tasks in header */
int flag_H = 0;		/* If set, show count httpd tasks in header */
int flag_Q = 0;		/* If set, don't display option settings */

int szcmdlinebuf = DEF_L;
char *cmdlinebuf;	/* dynamically allocated buf of size szcmdlinebuf */

int ncpus;		/* scale output mcpus values by number CPUs */

/*
 * Output can include measure of disk activity on select disks.
 *
 * Specify which disks you want using the "-d" option, such as in 
 * "-d /sys/block/sda/sda2/stat,sda", where the two comma separated
 * strings are (1) the full path to a sysfs block device stat file and
 * (2) the short name to be displayed for that device disk usage.
 *
 * The change in the value of column 11, "weighted # of milliseconds
 * spent doing I/Os", in msecs per second, will be displayed for each
 * requested disk when in the "inner loop."
 */

/*
 * diskstat_t.prev_time_in_queue :
 *
 * Previous value of msecs disk op in queue on device.
 *
 * This is the weighted # of milliseconds spent doing I/O It's field
 * 14 in /proc/diskstats.  It is field 11 in /sys/block/sda/stat or
 * /sys/block/sda/sda1/stat, using "sda" and "sda/sda1" as example
 * device names.
 *
 * See Documentation/iostats.txt (which is confusing.)
 *
 * Looking at kernel code, this value is the total count since boot of
 * the number of msecs that one disk op was in flight (in queue or being
 * operated on) for that device.
 *
 * For examples:
 *
 *   If from one sample to a next sample one second later, this field
 *   increased by 1000 that would mean there was an average of one disk
 *   op in flight for that device during that second.
 *
 *   If a disk op is in flight for 40 msecs, then when all is said and
 *   done, that disk op will have increased that time_in_queue by "40".
 *
 *   If 5 ops are all in flight for a device for an entire second, that
 *   device's time_in_queue will increase by 5000 over that second.
 *
 * The value is just 32 bits, so will wrap repeatedly.  So long as it's
 * sampled on a short enough interval, one can assume it wrapped either
 * zero times (if the 2nd sample is the same or larger than the 1st)
 * or exactly once (if the 2nd sample is less than the 1st.)
 *
 * If there is on average one disk op in flight all the time, then this
 * time_in_queue will wrap every 49 days.  If there is on average 100
 * disk ops in flight at all times (one *seriously* overloaded disk)
 * then this will wrap every 11 hours, 55 minutes.  So, for more
 * reasonable disk loads and faster inner loop cycle times, there is no
 * risk of this value wrapping more than once between two readings.
 *
 * The term "time_in_queue" comes from the kernel code, as that name
 * makes more sense than any terms in user space documentation.
 */

typedef struct {
	const char *path;	/* e.g. "/sys/block/sda/stat" */
	const char *name;	/* e.g. "sda" or "sdb1" */
	uint32_t prev_time_in_queue;
} diskstat_t;

/*
 * disks_monitored is a pointer to a NULL-terminated array of diskstat_t
 * pointers.  Each "-d ..." disk being monitored gets one diskstat_t
 * structure, to hold its path, name and previous value of its stat file
 * field 11.  All these pointers, one per disk monitored, are placed in
 * a malloc'd array, and disks_monitored points to the first diskstat_t
 * pointer in that array.  When another monitoring of another disk is
 * requested on the command line, we malloc a new diskstat_t for it,
 * and realloc() the array of diskstat_t pointers to have space for one
 * more such pointer.
 */

diskstat_t **disks_monitored = NULL;

/*
 * Add one more disk to the list of those being monitored.
 * dskstatpath_comma_name is a string with two comma separated parts:
 *  1) the full path to the stat file for that disk or partition, and
 *  2) the short name to display in output for that disk or partition.
 *
 * If the sysfs is mounted at /sys, then a typical stat file path for
 * a whole disk would look like "/sys/block/sda/stat", and for one
 * partition on a disk would look like "/sys/block/sda/sda1/stat".
 * Field 11 in the first line of such a stat file, containing the
 * accumulated time_in_queue for that drive or partition, is the one
 * of interest.
 */

void monitor_disk(char *dskstatpath_comma_name)
{
	int newlen;
	char *dskstatpath;	/* path to dskstat file */
	char *dskname;		/* display name of dskstat file */
	diskstat_t **dspp;
	diskstat_t *newdsk;

	if (!disks_monitored) {
		if ((disks_monitored = calloc(sizeof(*dspp), 1)) == NULL)
			perror_exit("calloc", "list disks monitored");
	}

	for (dspp = disks_monitored; *dspp; dspp++)
		continue;
	dspp++;			/* space to add new "dsk" */
	dspp++;			/* space for terminating NULL */

	newlen = sizeof(*dspp) * (dspp - disks_monitored);
	if ((disks_monitored = realloc(disks_monitored, newlen)) == NULL)
		perror_exit("realloc", "list monitored disks");
	if ((newdsk = malloc(sizeof(*newdsk))) == NULL)
		perror_exit("malloc", "newdisk struct");

	if ((dskstatpath = strtok(dskstatpath_comma_name, ",")) == NULL)
		perror_exit("strtok", "empty -d option value");
	if ((dskname = strtok(NULL, ",")) == NULL) {
		fprintf(stderr, "%s: -d option takes comma separated sysfs "
			"block dev stat path and display name\n", cmd);
		show_usage_and_exit();
	}
	newdsk->name = strdup(dskname);
	newdsk->path = strdup(dskstatpath);
	if (!newdsk->name || !newdsk->path)
		perror("strdup");
	newdsk->prev_time_in_queue = 0;
	for (dspp = disks_monitored; *dspp; dspp++)
		continue;
	*dspp++ = newdsk;
	*dspp++ = NULL;
}

/*
 * Convert disks_monitored array of string pointers into a single
 * string for display purposes.
 *
 * Returned string is malloc'c; up to caller to free.
 */

char *listdisks()
{
	char *argz = NULL;
	size_t argz_len = 0;
	diskstat_t **dspp;
	int e;			/* errno return from argz_* routines */

	if (!disks_monitored) {
		if ((argz = strdup("  Show_disks: [-d path,name]\n")) == NULL)
			perror_exit("strdup", "Show disks string");
		return argz;
	}

	if ((e = argz_add(&argz, &argz_len, "  Show disks:")) != 0) {
		errno = e;
		perror_exit("argz_add", "Show disks");
	}

	for (dspp = disks_monitored; *dspp; dspp++) {
		diskstat_t *dsp = *dspp;
		char *buf;
		int buflen;

		buflen = strlen(dsp->path) + strlen(dsp->name) + 16;
		if ((buf = malloc(buflen)) == NULL)
			perror_exit("malloc", "disk usage param");
		snprintf(buf, buflen, "-d %s,%s", dsp->path, dsp->name);
		if ((e = argz_add(&argz, &argz_len, buf)) != 0) {
			errno = e;
			perror_exit("argz_add", "-d parameter");
		}
		free(buf);
	}
	if ((e = argz_add(&argz, &argz_len, "\n")) != 0) {
		errno = e;
		perror_exit("argz_add", "newline");
	}
	argz_stringify(argz, argz_len, ' ');
	return argz;
}

void show_current_settings()
{
	char *dm;

	printf("Option settings:\n");
	printf("  Show CPU hogs: -C %d\n", flag_C);
	printf("  Show Mem hogs: -M %d\n", flag_M);
	printf("  Show Block I/O waiters: -B %d\n", flag_B);
	printf("  Show count of PHP tasks: -P %d\n", flag_P ? 1 : 0);
	printf("  Show count of httpd tasks: -H %d\n", flag_H ? 1 : 0);
	printf("  Outerloop time (secs): -s %.3f\n", val_s);
	printf("  Innerloop time (secs): -t %.3f\n", val_t);
	printf("  Min busy loadavg: -p %.3f\n", val_p);
	printf("  Min busy CPU load: -c %.1f%%\n", val_c);
	printf("  Min busy Mem load: -m %0.1f%%\n", val_m);
	printf("  Min busy Cpuset memory pressure: -u %d\n", val_u);
	printf("  Busy tasks (1/1000 of CPU, aka mcpus): -q %ld\n", val_q);
	printf("  RSS mem hogs (1/1000 of RAM, aka mrams): -r %ld\n", val_r);
	printf("  Block I/O waiters (msecs per sec): -b %ld\n", val_b);
	printf("  Max number tasks to show: -n %ld\n", val_n);
	printf("  Length cmdline to display: -L %d\n", szcmdlinebuf);

	dm = listdisks();
	printf("%s", dm);
	free(dm);

	printf("Use -Q option to Quiet above option setting display.\n");
	printf("\n");
	fflush(stdout);
}

/*
 * TASK_COMM_LEN define mimics kernel's include/linux/sched.h.
 * It's OK if we don't have the same value as the kernel we're
 * running on.  We'll just end up with the shorter of the two
 * in affect.
 */
#define TASK_COMM_LEN 16

/*
 * When taking snapshot of all tasks, stash for each task
 *  1) its command name,
 *  2) its process id (pid), and
 *  3) its total milliseconds CPU time so far,
 *	converted from ticks using sysconf(_SC_CLK_TCK),
 *	both user and system time,
 *	both the current task and all waited for children
 *  4) its memory usage (Resident Set Size - rss) in kbytes.
 *  5) its aggregate block I/O delays in msecs
 */
struct task_usage {
 	char cmd[TASK_COMM_LEN];
 	pid_t pid;
 	uint64_t cpumsecs;		/* total msecs CPU usage */
 	uint64_t rssmram;		/* 1/1000ths of RAM in RSS */
 	uint64_t diskwait;		/* total msecs block I/O delays */
};

typedef struct {
	struct task_usage *tu_array;	/* dynamic array of task_usage's */
	int tu_nelem;			/* number elements in tu_array */
} task_usages_t;

/*
 * It is easy to estimate number of tasks (pids) on system;
 * just stat /proc and check st_nlink.  Each task has a subdirectory
 * under /proc, and there are as well as a dozen or so other /proc
 * subdirectories.  So st_nlink will be count of tasks in system,
 * plus a dozen or so for a nice fudge factor.
 */

int est_num_tasks()
{
	struct stat sb;

	if (stat("/proc", &sb) < 0)
		perror_exit("stat", "/proc");
	return sb.st_nlink;
}

uint64_t kernel_clock_ticks_per_second()
{
	static uint64_t ticks_per_sec;	/* clock ticks per second */

	/* Get kernel clock ticks per second just once */
	if (ticks_per_sec == 0)
		ticks_per_sec = (unsigned) sysconf(_SC_CLK_TCK);
	if (ticks_per_sec <= 0) {
		fprintf(stderr, "Unable to get kernel ticks per second\n");
		exit(7);
	}
	return ticks_per_sec;
}

/*
 * Kernel vm page size in kbytes.
 */

uint64_t kernel_page_size()
{
	static uint64_t pgsz; 

	if (pgsz == 0)
		pgsz = (getpagesize() / 1024);
	return pgsz;
}

/*
 * char *flgets(char *buf, int buflen, FILE *fp)
 *
 * Obtain one line from input file fp.  Copy up to first
 * buflen-1 chars of line into buffer buf, discarding rest
 * of line.  Stop reading at newline, discarding newline.
 * Nul terminate result and return pointer to buffer buf
 * on success, or NULL if nothing more to read or failure.
 */

static char *flgets(char *buf, int buflen, FILE * fp)
{
	int c = -1;
	char *bp;

	bp = buf;
	while ((--buflen > 0) && ((c = getc(fp)) >= 0)) {
		if (c == '\n')
			goto newline;
		*bp++ = c;
	}
	if ((c < 0) && (bp == buf))
		return NULL;

	if (c > 0) {
		while ((c = getc(fp)) >= 0) {
			if (c == '\n')
				break;
		}
	}

newline:
	*bp++ = '\0';
	return buf;
}

/* Return size of RAM, in kbytes. */

uint64_t ram_size_in_kbytes()
{
	FILE *fp;
	char *bp;
	char buf[1024];
	static uint64_t ramsz;

	if (ramsz != 0L)
		return ramsz;

	if ((fp = fopen("/proc/meminfo", "r")) == NULL)
		perror_exit("fopen", "/proc/meminfo");

	while ((bp = flgets(buf, sizeof(buf), fp)) != NULL) {
		const char *label = "MemTotal:";

		if (strncmp(buf, label, strlen(label)) == 0) {
			ramsz = strtoul(buf + strlen(label) + 1, NULL, 10);
			break;
		}
	}

	fclose(fp);

	if (ramsz == 0L) {
		fprintf(stderr, "Unable to find MemTotal in /proc/meminfo\n");
		exit(6);
	}

	return ramsz;
}

/* Return number of CPUs on system */

int get_ncpus()
{
	int n = 0;
	DIR *fdirp;
	struct dirent *ent;
	const char *sys_cpu_path = "/sys/devices/system/cpu";

	if ((fdirp = opendir(sys_cpu_path)) == NULL) {
		printf("Unable to scan %s directory\n", sys_cpu_path);
		return 1;	/* don't scale mcpus by num CPUs */
	}
	while ((ent = readdir(fdirp)) != NULL) {
		if (strncmp(ent->d_name, "cpu", 3) == 0 &&
				isdigit(ent->d_name[3])) {
			n++;
		}
	}
	closedir(fdirp);

	if (n == 0) {
		printf("Found no cpu# in %s\n", sys_cpu_path);
		return 1;
	}
	return n;
}

/*
 * Read a /proc/<pid>/stat file for cpu usage, memory size (rss),
 * and command name.
 *
 * Extracting the command name from /proc/<pid>/stat is a
 * bit tricky.  The command name appears in the second field,
 * surrounded by parentheses, as in "(cmd)".  However the command
 * name might have space separated fields and close parentheses
 * embedded within it, which could cause premature termination of
 * the command name parsing, -and- misaligned assignment of subsequent
 * fields.
 *
 * Present day Linux kernels have a max command name string
 * length of 16, as can be found in this define in the kernel's
 * include/linux/sched.h header file:
 *	#define TASK_COMM_LEN 16
 * However I see patches in the Linux kernel queue to extend this
 * length, once some bugs are fixed (code assuming a length of 16
 * without using this define.)
 *
 * Also the number of fields in this line grows slowly over time,
 * as more are added.  Hopefully Linux will not add another field
 * which might in any way add another close parenthesis ')'.
 *
 * Numerous details in following routine adapted from the stat2proc()
 * routine, in the file minimal.c, of the package procps-3.2.8 (the
 * packages that provides such commands as 'top'.)
 *
 * If successful, writes cmd, *p_pid, *p_cpusecs and *p_rss with
 * the command name, pid, total (user+sys) (self+children) cpu seconds,
 * and resident set size (in pages) of the task whose pid (as decimal
 * ASCII string) is pidstr, and returns 0.
 *
 * If fails, returns various negative numbers, depending on what broke.
 * If the task being examined exits before we complete the open or
 * the read of its /proc/<pid>/stat file, then this is to be expected.
 * Such an open would fail ENOENT, and such a read would fail ESRCH.
 * Return -1 in these not surprising cases.  The other negative returns
 * should not happen if this code and the underlying system are bug free.
 * Return -2 or less in these various cases.
 *
 * The units of p_cpumsecs are milliseconds of CPU.  This routine converts
 * from the kernel tick units in the /proc/<pid>/stat file to milliseconds
 * or msecs.
 *
 * The units of p_rssmram are 1/1000th of the systems RAM size (termed
 * here millirams or mrams)  This routine converts from the kernel vm page
 * size units in the /proc/<pid>/stat file to millirams.
 */

int read_stat_file(const char *pidstr, char *cmd, int cmdlen, pid_t *p_pid,
	uint64_t *p_cpumsecs, uint64_t *p_rssmram, uint64_t *p_diskwait)
{ 
	char buf[800];		/* length suggested in procps minimal.c */
	int fd;			/* open file desc on /proc/<pid>/stat */
	int num;		/* various integer return values */
	char *openparen;	/* pointer to '(' before the command field */
	char *closeparen;	/* pointer to ')' after the command field */
	char *restofline;	/* remaining fields past command field */
	pid_t pid2;		/* input pidstr, as pid_t (integer) type */

	uint64_t utime, stime;		/* ticks in user, sys mode */
	long int cutime, cstime;	/* ticks of waited-for children */
	long unsigned ucutime, ucstime;	/* ticks of waited-for children */
	uint64_t ticks_per_sec;		/* clock Ticks Per Second */
	uint64_t cputicks;		/* total accumlated CPU ticks */
	uint64_t cpumsecs;		/* total accumulated CPU msecs */
	uint64_t rsspages;		/* Resident Set Size (pages) */
	uint64_t rsskbytes;		/* Resident Set Size (kbytes) */
	uint64_t rssmram;		/* Resident Set Size (mrams) */
	uint64_t pgsz;			/* kbytes per kernel page */
	uint64_t ramsz;			/* kbytes in all of RAM */
	uint64_t blockioticks;		/* total block I/O delays (ticks) */
	uint64_t diskwait;		/* total block I/O delays (msecs) */

	/* Ensure next sprintf doesn't overflow buf with silly long pidstr */
	if (strlen(pidstr) > 30)
		return -2;		/* silly long pidstr */

	sprintf(buf, "/proc/%s/stat", pidstr);
	if ((fd = open(buf, O_RDONLY, 0)) < 0)
		return -1;		/* maybe task just exited */
	num = read(fd, buf, sizeof(buf));	/* re-use buf[] */
	close(fd);
	if (num < 0)
		return -1;		/* maybe task just exited */
	buf[num] = '\0';

	openparen = strchr(buf, '(');
	closeparen = strrchr(buf, ')');
	if (openparen == NULL || closeparen == NULL)
		return -4;		/* broken stat file format */
	*closeparen = '\0';		/* split into "PID (cmd" and rest */
	restofline = closeparen + 2;	/* skip space after closeparen */
	strncpy(cmd, openparen + 1, cmdlen);
	cmd[cmdlen - 1] = '\0';

	/*
	 * the first field in /proc/<pid>/stat really should be
	 * the same as the pid in the pathname of that file.
	 */
	sscanf(buf, "%d", p_pid);
	sscanf(pidstr, "%d", &pid2);
	if (*p_pid != pid2)
		return -5;		/* seriously weird pid's */
	if (strlen(restofline) < 50)
		return -6;		/* hopelessly short stat line */
	if (*p_pid == 0 || pid2 == 0)
		return -8;		/* how did that zero pid get here */

	num = sscanf(restofline,
		"%*c "			/* state */
		"%*d %*d %*d %*d %*d "	/* ppid, pgrp, ses, tty, tpgid */
		"%*u "			/* flags */
		"%*u %*u "		/* minflt, cminflt */
		"%*u %*u "		/* majflt, cmajflt */
		"%lu %lu %ld %ld "	/* utime, stime, cutime, cstime */
		"%*d %*d "		/* priority, nice */
		"%*d %*d "		/* num_threads, itrealvalue */
		"%*u %*u "		/* starttime, vsize */
		"%lu %*u "		/* rss, rsslim */
		"%*u %*u %*u "		/* startcode, endcode, startstack */
		"%*u %*u "		/* kstkesp, kstkeip */
		"%*u %*u %*u %*u "	/* signal, blocked, ignore, catch */
		"%*u "			/* wchan */
		"%*u %*u "		/* nswap, cnswap */
		"%*d %*d %*u %*u "	/* exitsig, cpu, rt_pri, schedpol */
		"%lu "			/* delayacct_blkio_ticks (2.6.18)*/
		,

	       &utime, &stime, &cutime, &cstime, &rsspages, &blockioticks
	    );

	if (num != 6)
		return -7;		/* didn't get the 6 values */

	/* Convert rss from pages to mrams (1/1000'ths of RAM size) */
	pgsz = kernel_page_size();
	ramsz = ram_size_in_kbytes();
	rsskbytes = rsspages * pgsz;	/* convert pages to kbytes */
	rssmram = 1000 * rsskbytes;	/* multiply before divide ... */
	rssmram /= ramsz;		/* ... for better precision */
	*p_rssmram = rssmram;		/* RSS size in milli-rams */

	/*
	 * Convert total accumlated CPU ticks to msecs.
	 *
	 * The kernel and top (procps) have cutime and cstime as
	 * signed longs, not unsigned.  This seems strange, so I
	 * discard (take as if 0) cutime or cstime if less than zero.
	 */

	ucutime = (cutime < 0L ? 0UL : (unsigned) cutime);
	ucstime = (cstime < 0L ? 0UL : (unsigned) cstime);
	ticks_per_sec = kernel_clock_ticks_per_second();
	cputicks = utime + stime + ucutime + ucstime;
	cpumsecs = 1000 * cputicks;	/* multiply before divide ... */
	cpumsecs /= ticks_per_sec;	/* ... for better precision */
	*p_cpumsecs = cpumsecs;		/* CPU usage in mcpus */

	/* Convert total block I/O delay ticks to msecs */
	diskwait = 1000 * blockioticks;
	diskwait /= ticks_per_sec;
	*p_diskwait = diskwait;

	return 0;			/* Success */
}

task_usages_t get_task_usages()
{
	int nt, i;
	task_usages_t task_usages;
	DIR *procdir;
	struct dirent *ent;
	int ret;

	nt = est_num_tasks();
	task_usages.tu_array = malloc(nt * sizeof(struct task_usage));

	i = 0;			/* index task_usages.tup[0 .. nt-1] */
	procdir = opendir("/proc");
	while ((ent = readdir(procdir)) != NULL && i < nt) {
		struct task_usage *tup;

		if (!isdigit(ent->d_name[0]))
			continue;
		tup = task_usages.tu_array + i;
		if ((ret = read_stat_file(ent->d_name, tup->cmd,
			sizeof(tup->cmd), &tup->pid, &tup->cpumsecs,
			&tup->rssmram, &tup->diskwait)) < 0) {
				if (ret <= -2) {
				    fprintf(stderr,
					"read_stat_file(%s) ==> %d\n",
					ent->d_name, ret);
				    exit(4);
				}
				continue;
			}
		i++;
	}
	task_usages.tu_nelem = i;
	closedir(procdir);
	return task_usages;
}

int min(int a, int b)
{
	return a < b ? a : b;
}

int max(int a, int b)
{
	return a > b ? a : b;
}

/*
 * pread(2) doesn't work on special files for some Linux kernels.
 * Rather such calls can fail with ESPIPE "Illegal seek" (even though
 * I only ever try to pread() from offset 0L of the file.)
 *
 * So keep a table of file descriptors that I want to pread on, along
 * with the path to that file, and if the kernel we're running on starts
 * failing pread calls ESPIPE, fall back to an open/read/close sequence.
 */

/*
 * Table of file descriptors and paths that we might try to pread on
 * indexed by the file descriptor we first opened it on.
 */
#define MAX_PREAD_FD 32
char *pread_fds[MAX_PREAD_FD];
int broken_pread = 0;

void stash_pread_fd(int fd, const char *path)
{
	if (fd < 0 || fd >= MAX_PREAD_FD)
		perror_exit("stash_pread_fd", "fd out of range");
	if ((pread_fds[fd] = strdup(path)) == NULL)
		perror_exit("malloc", path);
}

/*
 * Replacement wrapper for pread(2) -- handles open/read/close
 * workaround in case kernel refused to pread on all of the files
 * that we attempt to pread from.
 */
ssize_t my_pread(int fd, void *buf, size_t count, off_t offset)
{
	ssize_t sz;

	if (broken_pread) {
		int fd2;

		errno = 0;
		if (fd >= MAX_PREAD_FD)
			perror_exit("my_pread", "fd unexpectedly large");
		if (pread_fds[fd] == NULL) {
			fprintf(stderr, "my_pread(fd %d)\n", fd);
			perror_exit("pread_fds", "unexpectedly NULL");
		}
		if ((fd2 = open(pread_fds[fd], O_RDONLY)) < 0)
			perror_exit("open", pread_fds[fd]);
		if (offset != (off_t) 0) {
			if (lseek(fd2, offset, SEEK_SET) == (off_t) -1)
				perror_exit("lseek", pread_fds[fd]);
		}
		if ((sz = read(fd2, buf, count)) < 0)
			perror_exit("read", pread_fds[fd]);
		close(fd2);
		return sz;
	}

	if ((sz = pread(fd, buf, count, offset)) < 0) {
		if (errno == ESPIPE) {
			broken_pread = 1;
			return my_pread(fd, buf, count, offset);
		}
		perror_exit("pread", "unexpected errno");
	}

	return sz;
}

/*
 * CPU load is computed using change in total and idle ticks
 * since previous time read, so we keep the previous ticks
 * in these globals, rather than pass them around as args.
 */
unsigned long prev_cpu_active, prev_cpu_total;

/*
 * Store into active and total the current total ticks that all CPU's
 * combined have spent active (not in idle) and in total, since boot.
 *
 * These numbers are obtained from the first line ("cpu") of /proc/stat,
 * adding up the numbers to get the total, and then subtracting the idle
 * value from the total to get the active.
 *
 * It happens that the "ticks" units used in /proc/stat are not
 * necessarily the same as the sysconf(_SC_CLK_TCK) values that are
 * used in per-task /proc/<pid>/stat files, but that doesn't matter
 * for the following code, which ends up only being used to compute
 * the ratio of idle to total.
 */

void get_cumulative_cpu_stats(unsigned long *active, unsigned long *total)
{
	char buf[256];
        char *p, *q;
        int fldnum;		/* number numeric field being read (1 ...) */
        unsigned long f;	/* value read from this field */
        unsigned long sum_ticks;
        unsigned long idle_ticks;
        int min_1st_line_len = 5;	/* 1st line /proc/stat >= 5 chars */
        const char *statfile = "/proc/stat";
        static int fd = -1;

        if (fd == -1) {
	        if ((fd = open(statfile, O_RDONLY)) < 0) {
	                perror_exit("open", statfile);
	        }
		stash_pread_fd(fd, statfile);
	}
        if (my_pread(fd, buf, sizeof(buf), (off_t)0) < min_1st_line_len)
                perror_exit("short read", statfile);

        if (strncmp(buf, "cpu ", strlen("cpu ")) != 0)
                perror_exit("first line not cpu", statfile);
        if ((p = strchr(buf, '\n')) == NULL)
                perror_exit("first line too long", statfile);

        sum_ticks = 0UL;
        *p = '\0';              /* nul-terminate first line */
        p = buf;
        fldnum = 1;
        idle_ticks = 0UL;

        while ((q = strtok(p, " ")) != NULL) {
                p = NULL;
                if (!isdigit(*q))
                        continue;
                f = strtoull(q, NULL, 10);
                if (fldnum == 4)		/* 4th number is idle ticks */
                	idle_ticks = f;
                sum_ticks += f;
                fldnum++;
        }
        if (fldnum < 5)
        	perror_exit("first line too few fields", statfile);

        *total = sum_ticks;
        *active = sum_ticks - idle_ticks;
}

/*
 * Determines how much CPU (in ticks) has been spent in total and active
 * since the previous values were recorded.  Then update the previous values
 * to the present ones just read, and return the ratio (active/total) of
 * these changes since the previous values.
 */

double read_cpuload()
{
	double load;
	unsigned long active, total;
	unsigned long delta_active, delta_total;

	get_cumulative_cpu_stats(&active, &total);

	if (active < prev_cpu_active) {
		fprintf(stderr,"\n ... cpu load active ticks shrank from %lu to %lu.\n", prev_cpu_active, active);
		return 0UL;
	}
	if (total < prev_cpu_total) {
		fprintf(stderr,"\n ... cpu load total ticks shrank from %lu to %lu.\n", prev_cpu_total, total);
		return 0UL;
	}
	if (total == 0UL) {
		fprintf(stderr,"\n ... cpu load total ticks shrank from %lu to ZERO.\n", prev_cpu_total);
		return 0UL;
	}
	delta_active = active - prev_cpu_active;
	delta_total = total - prev_cpu_total;

	if (delta_total == 0UL)
		delta_total = 1UL;	/* avoid divide by zero */
	load = (double) delta_active / (double) delta_total;

	prev_cpu_active = active;
	prev_cpu_total = total;

	return load;
}

/*
 * double read_memload()
 *
 * Calculate memory load ("MemLoad"), percentage of memory in use that
 * could not easily be repurposed.  Memory in use for buffers and caches
 * of data that is also on disk can be easily repurposed, with no need to
 * first write its contents to disk.  The portion of the total memory (RAM)
 * that is in use for other purposes (not easily repurposed) is the memory
 * load.
 *
 * The first three lines of /proc/meminfo provide the data needed to calculate
 * memory load:
 *	MemTotal:        xxx kB
 *	MemFree:         xxx kB
 *	MemAvailable:    xxx kB
 *
 * Memory load is calculated as:
 *	MemLoad = (MemTotal - MemAvailable) / MemTotal
 *
 * Memory load is some fraction between 0 and 1.  It is displayed by this
 * program as a percentage (multiplied by 100 on output).
 *
 * The procps command "free" is an important intellectual ancestor of this code.
 */

double read_memload()
{
	unsigned long MemTotal, MemFree, MemAvailable;
	double MemLoad;
	char buf[512];
	const char *memfile = "/proc/meminfo";
	char *fmt;
	int minread = 40;	/* Fail if read < 40 bytes from meminfo */
	static int fd = -1;

	if (fd < 0) {
		if ((fd = open(memfile, O_RDONLY)) < 0)
			perror_exit("open", memfile);
		stash_pread_fd(fd, memfile);
	}
        if (my_pread(fd, buf, sizeof(buf), (off_t)0) < minread)
                perror_exit("short read", memfile);

        fmt =	"MemTotal: %lu kB\n"
        	"MemFree: %lu kB\n"
        	"MemAvailable: %lu kB\n";

        if (sscanf(buf, fmt, &MemTotal, &MemFree, &MemAvailable) != 3)
        	perror_exit("sscanf", memfile);

        if (MemTotal == 0UL)
        	perror_exit("zero MemTotal", memfile);

        if (MemAvailable > MemTotal)
        	perror_exit("Avail mem > total mem!", memfile);

        MemLoad = (double) (MemTotal - MemAvailable) / MemTotal;
        return MemLoad;
}

/*
 * pathcat(a, b) -- Concatenate strings a and b, separated by '/'
 *		    in returned malloc'd string.
 */
char *pathcat(const char *a, const char *b)
{
	int len = strlen(a) + strlen(b) + 2;
	char *p = malloc(len);

	if (p == NULL)
		perror_exit("malloc", "pathcat");
	sprintf(p, "%s/%s", a, b);
	return p;
}

/*
 * Read cpuset memory_pressure, a measure of recent memory pressure
 * sufficient to force pages out of the memory cache.  This measure responds
 * very rapidly, in a few seconds (exponential decaying filter with half-life
 * of 10 seconds), to tasks needing more memory than is readily available on
 * the free list.
 *
 * Requires 2.16 Linux kernel or better, and requires that
 * cpuset memory pressure is enabled by writing "1" to the special file
 * /dev/cpuset/memory_pressure_enabled.  Must adapt to more recent Linux
 * kernels that renamed /dev/cpuset files to have a "cpuset." prefix, as in
 * /dev/cpuset/cpuset.memory_pressure & /dev/cpuset/cpuset.memory_pressure_enabled,
 * so that the names make sense when included in a cgroup mounted elsewhere,
 * and must adapt to having the cpuset file system mounted anywhere.
 *
 * Only considers memory pressure in top cpuset; if multiple cpusets are
 * being used on a system, memory pressure in other (non-root) cpusets will
 * not be noticed.
 */

/*
 * Helper routine for memory_pressure_fd() -- given an open file descriptor
 * on a cpuset mount directory and the name of a memory_pressure_enabled file
 * in that directory, return 1 iff that file contains (starts with) "1", else
 * return 0.  Don't error exit if cps_dir_fd or mpe_path aren't valid; just
 * return 0.
 */
 int cmp_enabled_at(int cps_dir_fd, const char *mpe_path)
 {
 	int mpe_fd;
 	char firstchar;
 	int ret;

 	if ((mpe_fd = openat(cps_dir_fd, mpe_path, O_RDONLY)) < 0)
 		return 0;
 	ret = read(mpe_fd, &firstchar, 1);
 	close(mpe_fd);
 	if (ret != 1)
 		return 0;
 	if (firstchar != '1')
 		return 0;

 	return 1;
 }

/*
 * If cpuset memory_pressure is enabled on a system, return an open file
 * descriptor on the memory pressure file in the top cpuset.  Otherwise
 * return -1.
 *
 * If cpusets are not enabled (not mounted) or if memory pressure is not
 * enabled, then on the first call, print a "Note" to stdout.
 */
int memory_pressure_fd()
{
	static int cmp_fd = -1;		/* Cpuset Mempry Pressure fd */
	static int initialized = 0;	/* trigger one time initialization */

        /* Potential Cpuset Memory Pressure (cmp) enabled paths */
        char *mppaths[][2] = {
        	{ "memory_pressure_enabled", "memory_pressure" },
        	{ "cpuset.memory_pressure_enabled", "cpuset.memory_pressure" },
        	{ NULL, NULL }
        };

	if (!initialized) {
		char buf[PATH_MAX];	/* line of input from /proc/mounts */
		int cps_dir_fd = -1;	/* open fd on cpuset mount directory */
		char *savmnt = NULL;	/* copy of "mnt" for stash_pread_fd() */
		FILE *fp;		/* "/proc/mounts" FILE stream */
		int i;			/* scan mppaths[] */

		initialized = 1;

	        if ((fp = fopen("/proc/mounts", "r")) == NULL)
	                perror_exit("fopen", "/proc/mounts");

	        while (flgets(buf, sizeof(buf), fp) != NULL) {
	        	char *p, *q;		/* control strtok() loop */
	        	char *mnt = NULL;	/* mount path is in field 2 */
	        	int fieldnum = 0;	/* count the field we're in */
	        	int found_line = 0;	/* set if cpuset line */

	        	p = buf;
	        	while ((q = strtok(p, ", ")) != NULL) {
	        		p = NULL;
				fieldnum += 1;
	        		if (fieldnum == 2) {
	        			mnt = q;
					continue;
				}
	        		if (fieldnum > 2 && strcmp(q, "cpuset") == 0) {
	        			found_line = 1;
	        			break;
	        		}
	        	}

/* Linux prior to 2.6.39 lacks O_PATH - ok to make it no-op for old kernels */
#ifndef O_PATH
#define O_PATH 0
#endif

			if (mnt && found_line) {
				if ((savmnt = strdup(mnt)) == NULL)
					perror_exit("strdup", "savmnt");
	        		if ((cps_dir_fd = open(savmnt, O_RDONLY|O_PATH)) < 0)
	        			perror_exit("open", savmnt);
	        		break;
	        	}
	        }
	        fclose(fp);

	        if (cps_dir_fd < 0)
	        	goto no_cpuset_memory_pressure;

	        /*
	         * Is cpuset.memory_pressure_enabled or memory_pressure_enabled
	         * in the cpuset control directory? If so, set cmp_fd
	         * to an open file descriptor on the cpuset memory pressure
	         * file in that directory.
	         */
	        for (i = 0; mppaths[i][0] != NULL; i++) {
	        	if (cmp_enabled_at(cps_dir_fd, mppaths[i][0])) {
	        		char *fname = mppaths[i][1];
	        		char *p;

	        		if ((cmp_fd = openat(cps_dir_fd, fname,
	        				O_RDONLY)) < 0) {
	        			perror_exit("openat", fname);
	        		}
	        		close(cps_dir_fd);
	        		p = pathcat(savmnt, fname);
	        		stash_pread_fd(cmp_fd, p);
	        		free(p);
	        		free(savmnt);
	        		return cmp_fd;
	        	}
	        }

  no_cpuset_memory_pressure:
  		 puts("Note: Cpuset not mounted or memory pressure not enabled.\n"
  		      "      This may cause less output.");
        }

	return cmp_fd;

}

/*
 * Return the cpuset memory pressure, or 1.0 if that's unavailable.
 */
int read_mempres()
{
	char buf[256];			/* input buffer for pread() */
	static int initialized = 0;	/* trigger one time initialization */
	static int cmp_fd = -1;		/* file desc. on top cpuset mem pres */

	if (val_u == 0) return 0;
	if (!initialized) {
		initialized = 1;
		cmp_fd = memory_pressure_fd();
	}
	if (cmp_fd < 0)
		return 1;
	if (my_pread(cmp_fd, buf, sizeof(buf), (off_t)0) < 1)
		perror_exit("pread", "cpuset memory pressure");

	return strtod(buf, NULL);
}

/*
 * show_hogs(prior, latest):
 *
 * Show top val_n tasks using or having more than specified CPU, Memory
 * or block I/O disk waits.
 *
 * The Linux kernel returns the <pid> subdirectory entries of /proc
 * already in numerically sorted order (as a side affect of a bug
 * fix to reliably return all of them.)
 *
 * So don't need to sort prior and latest on pid; rather just fail
 * if we ever see them out of order.  Go directly to the joining
 * of these two on the pid field, calculating the cpumsecs and diskwait
 * used in the elapsed time between these two snapshots.  It is OK to
 * fail, during that join, if we notice they're not sorted.  The check()
 * define, below, makes this test.
 *
 * Then sort the result on the various fields (CPU, Memory or Diskwait)
 * for which we want the hogs.
 *
 * The cpumsecs field below is the portion of one CPU used in the
 * time interval between prior and latest, scaled so that 1000
 * means all of one CPU was used.  This value can be over 1000 if
 * the process has multiple threads on multiple CPUs.
 *
 * Say for example we're asked to show tasks using over half of all
 * CPU (val_q == 500).  Say some task had used 9000 ticks (user + sys,
 * self + children) at the prior snapshot, and had used 9700 ticks
 * at the latest snapshot.  Then during that interval, that task
 * used an additional 9700 - 9000 == 700 ticks.  Say that the
 * kernel_clock_ticks_per_second was 100 (ticks/second).  Then that
 * task used 7 CPU seconds (cpusecs) during that interval.  Say that
 * the inner loop is running every 10 seconds (val_t == 10).  Then
 * that task used 7/10 or 70% of all CPU during that interval.  We would
 * set the scaled cpumsecs to 700 (70% expressed per thousands.)  If this
 * task were in the top val_n busiest CPU hogs in that interval, then
 * we would decide to display that because 700 > 500 (val_q * 10, to
 * the same scale.)
 *
 * We bother with this scaling of cpumsecs in order to collapse it to an
 * integer, expecting that the qsort comparison routine will run faster
 * comparing int's than comparing double's.  We have to qsort the list
 * of potential tasks on each possible hog type field, so that we can find
 * the val_n busiest hogs on the list.
 *
 * The cpumsecs, rssmrams, and diskwait fields are kept in the join'd
 * structure even though they can be recomputed from the indexed prior and
 * latest snapshot structures (via the i and j indices), because these
 * fields are needed by the qsort comparison routines, which we want to be
 * fast, to minimize qsort time.
 */

typedef struct {
	int i, j;		/* index into prior, latest with same pid */
	unsigned cpumsecs;	/* msecs per sec of cpu usage */
	unsigned rssmrams;	/* milli-ram's in RSS */
	unsigned diskwait;	/* msecs per sec of block I/O diskwait */
	int showme;		/* set if this task is to be displayed */
} join_on_pid_t;

/*
 * The indices i and j in the join_on_pid_t elements (of the joinp
 * array, allocated below) index into the prior and latest arrays,
 * where the pid, cmd, rssmram, accumulated cpumsecs and diskwait
 * of that task are stored.  The PID, CMD, and RAM macros
 * facilitate accessing these pid, cmd, and rssmram, values.
 */

#define PID(p, i) 	((p).tu_array[(i)].pid)
#define CMD(p, i)	((p).tu_array[(i)].cmd)
#define RAM(p, i)	((p).tu_array[(i)].rssmram)

/*
 * To show the worst cpu hogs ("top" CPU users) we need to sort all
 * tasks on the amount of CPU (here in units of cpumsecs) used in the
 * last time interval (val_t).  The following comparison routine is
 * passed to qsort(), for this sorting.
 */

int cpumsecs_cmp(const void *p1, const void *p2)
{
	join_on_pid_t *jp1 = (join_on_pid_t *) p1;
	join_on_pid_t *jp2 = (join_on_pid_t *) p2;

	return (jp2->cpumsecs) - (jp1->cpumsecs);
}

/*
 * To show the worst mem hogs ("top" RSS size) we need to sort all
 * tasks on the amount of RAM (here in units of rssmram) used in the
 * lastest snapshot.  The following comparison routine is passed to
 * qsort(), for this sorting.
 */

int rssmrams_cmp(const void *p1, const void *p2)
{
	join_on_pid_t *jp1 = (join_on_pid_t *) p1;
	join_on_pid_t *jp2 = (join_on_pid_t *) p2;

	return (jp2->rssmrams) - (jp1->rssmrams);
}

/*
 * To show the worst diskwait hogs ("top" RSS size) we need to sort all
 * tasks on the amount of block I/O diskwait (msecs) used in the
 * lastest interval (val_t).  The following comparison routine is passed to
 * qsort(), for this sorting.
 */

int diskwait_cmp(const void *p1, const void *p2)
{
	join_on_pid_t *jp1 = (join_on_pid_t *) p1;
	join_on_pid_t *jp2 = (join_on_pid_t *) p2;

	return (jp2->diskwait) - (jp1->diskwait);
}

char *get_cmdline(pid_t pid)
{
	int fd;
	char cmdlinepath[32];
	int cnt, i;

	sprintf(cmdlinepath, "/proc/%d/cmdline", pid);

	if ((fd = open(cmdlinepath, O_RDONLY)) < 0) {
		sprintf(cmdlinebuf, "%*s", szcmdlinebuf, "<unknown>");
		cmdlinebuf[szcmdlinebuf-1] = '\0';
		return cmdlinebuf;
	}
	cnt = read(fd, cmdlinebuf, szcmdlinebuf);
	close(fd);

	/*
	 * cmdline has embedded NUL's between args,
	 * plus a terminating NUL (which we might not
	 * have read, if length of cmdline > szcmdlinebuf).
	 *
	 * Replace embedded NUL's with spaces.
	 */

	for (i = 0; i < cnt-1; i++) {
		if (cmdlinebuf[i] == '\0')
			cmdlinebuf[i] = ' ';
	}
	cmdlinebuf[i] = '\0';

	return cmdlinebuf;
}

char *skipwhitespace(char *p)
{
	while(*p == ' ' || *p == '\t')
		p++;
	if (*p && isgraph(*p))
		return p;
	else
		return NULL;
}

char *skipfieldchars(char *p)
{
	while (isgraph(*p))
		p++;
	return p;
}

/*
 * Get field number 'fieldnum' from file 'path'.
 *
 * Fields are numbered  with field 1.  Fields are separated by one or more
 * spaces or tabs. Only the first line of file is considered.  Leading spaces
 * on line are ignored.
 *
 * Return malloc'c copy of field string if found.  Return NULL if
 * field not found, or if malloc fails.
 */

char *getfield(const char *path, int fieldnum)
{
	char buf[256];
	char *startoffield = NULL;
	int cnt;
	char *p;
	int fd;

	if ((fd = open(path, O_RDONLY)) < 0)
		perror_exit("open", path);
	if ((cnt = read(fd, buf, sizeof(buf))) < 0)
		perror_exit("read", path);
	if (close(fd) < 0)
		perror_exit("close", path);

	buf[cnt-1] = '\0';

	errno = 0;	/* stays 0 unless strdup() failure sets */
	p = buf;
	while (fieldnum--) {
		if ((p = skipwhitespace(p)) == NULL)
			return NULL;
		if (fieldnum == 0) 
			startoffield = p;
		if ((p = skipfieldchars(p)) == NULL)
			return NULL;
		if (fieldnum == 0) {
			char *fdup;
			*p = '\0';
			if ((fdup = strdup(startoffield)) == NULL)
				perror_exit("strdup", "requested field");
			return fdup;
		}
	}
	return NULL;
}

/*
 * How many of our tasks are PHP tasks?
 */

int get_cnt_php(task_usages_t latest)
{
	int ni, i;
	int cnt = 0;

	ni = latest.tu_nelem;
	for (i = 0; i < ni; i++) {
		if (strstr(CMD(latest, i), "php") != NULL)
			cnt++;
	}
	return cnt;
}

/*
 * How many of our tasks are httpd tasks?
 */

int get_cnt_httpd(task_usages_t latest)
{
	int ni, i;
	int cnt = 0;

	ni = latest.tu_nelem;
	for (i = 0; i < ni; i++) {
		if (strstr(CMD(latest, i), "httpd") != NULL)
			cnt++;
	}
	return cnt;
}

/*
 * Extract from /sys/block/.../stat how much disk has been used on each
 * monitored disk in disks_monitored, since the previous time we were
 * called.  Return results as malloc'd ready to display string.
 * Be sure to free() result once done using it.
 *
 * Update the prev_time_in_queue values to the current value for each
 * monitored disk, and update the static "prev_now", so that the next
 * time this routine is called, it can compute the difference in usage
 * (delta_usage) for the elapsed time between the two calls.
 *
 * Output is in units of "mdsk", which is a made up unit for how many
 * milliseconds of disk being used (command in flight, either in queue
 * or being processed) per second of elapsed time.
 *
 * In other words, the output values for diskusage are scaled so that
 * a disk made 100% busy by a single threaded task will display about
 * "1000" for diskusage (1000 msecs per sec of one op in flight.)
 */

char *get_disks_monitored()
{
	time_t now;		/* time now (seconds since epoch) */
	static time_t prev_now;	/* time last called (secs since epoch) */
	diskstat_t **dspp;	/* scans disks_monitored */
	char *argz = NULL;
	size_t argz_len = 0;
	int e;			/* errno return from argz_* routines */

	if (!disks_monitored) {	/* empty result if no disks monitored */
		if ((argz = strdup("")) == NULL)
			perror_exit("strdup", "empty string");
		return argz;
	}

	now = time(NULL);
	if ((e = argz_add(&argz, &argz_len, "; diskusage")) != 0) {
		errno = e;
		perror_exit("argz_add", "diskusage");
	}

	for (dspp = disks_monitored; *dspp; dspp++) {
		uint32_t cur_time_in_queue;
		time_t delta_time;
		uint32_t delta_usage;
		uint32_t mdsk;
		char *mstr;
		int mlen;
		char *fld11_str;
		diskstat_t *dsp;

		dsp = *dspp;
		if ((fld11_str = getfield(dsp->path, 11)) == NULL) {
			fprintf(stderr, "File %s no field 11\n", dsp->path);
			exit(1);
		}
		if ((sscanf(fld11_str, "%u", &cur_time_in_queue)) != 1)
			perror_exit("sscanf", "cur_time_in_queue");
		free(fld11_str);

		delta_usage = cur_time_in_queue - dsp->prev_time_in_queue;
		delta_time = now - prev_now;
		if (delta_time < 1)
			delta_time = 1;
		mdsk = delta_usage / (uint32_t) delta_time;
		mlen = strlen(dsp->name) + 16;
		if ((mstr = malloc(mlen)) == NULL)
			perror_exit("malloc", "mstr");
		if (snprintf(mstr, mlen, "%s:%u", dsp->name, mdsk) < 0)
			perror_exit("snprintf", "diskusage");
		if ((e = argz_add(&argz, &argz_len, mstr)) != 0) {
			errno = e;
			perror_exit("argz_add", "diskusage");
		}
		free(mstr);

		dsp->prev_time_in_queue = cur_time_in_queue;
	}

	prev_now = now;
	argz_stringify(argz, argz_len, ' ');
	return argz;
}

void show_hogs(task_usages_t prior, task_usages_t latest)
{
	int ni, nj;		/* number elements in prior, latest */
	int i, j;		/* scans ni, nj */
	join_on_pid_t *joinp;	/* join on pid, with delta cpusecs */
	join_on_pid_t *jp;	/* scans joinp[] */
	join_on_pid_t *jpend;	/* end (one past last valid entry) of joinp */
	join_on_pid_t *jpmax;	/* at most val_n above joinp */
	int got_some;		/* one or more CPU or RAM hogs found */

	ni = prior.tu_nelem;
	nj = latest.tu_nelem;
	if ((joinp = malloc(sizeof(*joinp) * max(ni, nj))) == NULL)
		perror_exit("malloc", "joinp");

	/* Join prior and latest on pid field, into joinp, */
	i = j = 0;
	jp = joinp;

	while (i < ni && j < nj) {
		if (j > 0 && PID(latest, j-1) > PID(latest, j)) {
			fprintf(stderr, "/proc pids out of order - fail\n");
			fprintf(stderr,
				"j %d, nj %d, j-1 pid %d, j pid %d\n",
				j, nj, PID(latest, j-1), PID(latest, j));
			exit(9);
		}
		if (PID(prior, i) == PID(latest, j)) {
			jp->i = i++;
			jp->j = j++;
			jp++;
		} else if (PID(prior, i) < PID(latest, j)) {
			i++;
		} else {
			j++;
		}
	}

	jpend = jp;

	/* Set joinp's cpumsecs, rssmrams, diskwait fields. */
	for (jp = joinp; jp < jpend; jp++) {
		uint64_t prior_cpumsecs, latest_cpumsecs;
		uint64_t prior_diskwait, latest_diskwait;

		prior_cpumsecs = prior.tu_array[jp->i].cpumsecs;
		latest_cpumsecs = latest.tu_array[jp->j].cpumsecs;

		prior_diskwait = prior.tu_array[jp->i].diskwait;
		latest_diskwait = latest.tu_array[jp->j].diskwait;

		jp->cpumsecs = (latest_cpumsecs - prior_cpumsecs) / val_t;
		jp->cpumsecs /= ncpus;
		jp->rssmrams = RAM(latest, jp->j);
		jp->diskwait = (latest_diskwait - prior_diskwait) / val_t;
		jp->showme = 0;
	}

	/*
	 * Show up to first val_n with cpumsecs > val_q, rssmrams > val_r,
	 * or diskwait > val_b.
	 */
	jpmax = joinp + min(val_n, jpend - joinp);

	got_some = 0;

	/*
	 * Sort the joined results by each of rssmrams (flag_M), cpumsecs
	 * (flag_C) or block I/O diskwait (flag_B) asked for.  Sort cpumsecs
	 * last, so that if multiple asked for, results are displayed in
	 * descending order of CPU usage.
	 */

	 if (flag_M) {
		qsort(joinp, jpend - joinp, sizeof(*joinp), rssmrams_cmp);
		for (jp = joinp; jp < jpmax; jp++) {
			if (jp->rssmrams >= val_r) {
				jp->showme = 1;
				got_some = 1;
			}
		}
	}
	if (flag_B) {
		qsort(joinp, jpend - joinp, sizeof(*joinp), diskwait_cmp);
		for (jp = joinp; jp < jpmax; jp++) {
			if (jp->diskwait >= val_b) {
				jp->showme = 1;
				got_some = 1;
			}
		}
	}
	if (flag_C) {
		qsort(joinp, jpend - joinp, sizeof(*joinp), cpumsecs_cmp);
		for (jp = joinp; jp < jpmax; jp++) {
			if (jp->cpumsecs >= val_q) {
				jp->showme = 1;
				got_some = 1;
			}
		}
	}

	/*
	 * If no tasks have high CPU, RAM or DISKWAIT, say so briefly, but
	 * skip printing header for empty list.  We left the date time line
	 * without a trailing newline, so add one, somehow.
	 */

	if (got_some == 0) {
		printf(" - no individual tasks are hogs.\n");
		goto done;
	} else {
		printf("\n");
	}

	printf("    %8s  %16s  %10s  %10s  %10s  %-s\n",
		"pid", "cmd", "mcpus", "mrams", "diskwait", "cmdline");

	for (jp = joinp; jp < jpend; jp++) {
		if (jp->showme) {
			printf("    %8d  %16s  %10d  %10d  %10u  %-.*s\n",
				PID(latest, jp->j),
				CMD(latest, jp->j),
				jp->cpumsecs,
				jp->rssmrams,
				jp->diskwait,
				szcmdlinebuf,
				get_cmdline(PID(latest, jp->j)));
		}
	}

done:
	free(joinp);
}

void show_task_usages(task_usages_t prior, task_usages_t latest,
		double lavg, double cpu_load, double mem_load,
		int mem_pres, int cnt_php, int cnt_httpd, char *dsk_str)
{
	time_t now;
	struct tm *tmp;
	char tmbuf[128];
	char php_str[128];
	char httpd_str[128];

	now = time(NULL);
	if ((tmp = localtime(&now)) == NULL)
		perror_exit("localtime", NULL);
	if (strftime(tmbuf, sizeof(tmbuf), "%c", tmp) == 0) {
		fprintf(stderr, "strftime returned zero.\n");
		exit(9);
	}

	if (flag_P)
		sprintf(php_str, "; cnt PHP %2d", cnt_php);
	else
		php_str[0] = '\0';

	if (flag_H)
		sprintf(httpd_str, "; cnt httpd %2d", cnt_httpd);
	else
		httpd_str[0] = '\0';

	/* This outputs no trailing newline - see show_hogs() */
	printf("\n%s - loadavg %5.2f; CPU load %3.0f%%; "
		"Mem load %2.0f%%; Mem pres %4d%s%s%s",
		tmbuf, lavg, cpu_load * (double) 100,
		mem_load * (double) 100, mem_pres, php_str, httpd_str, dsk_str);

	show_hogs(prior, latest);

	fflush(stdout);
}

void free_task_usages(task_usages_t t)
{
	free(t.tu_array);
}

/*
 * Read the first load average (over 1 minute) from /proc/loadavg.
 */

double read_loadavg()
{
	static int fd = -1;
	char buf[32];
	const char *loadfile = "/proc/loadavg";

	if (fd < 0) {
		if ((fd = open(loadfile, O_RDONLY)) < 0)
			perror_exit("open", loadfile);
		stash_pread_fd(fd, loadfile);
	}
	if (my_pread(fd, buf, sizeof(buf), (off_t)0) < 1)
		perror_exit("pread", loadfile);
	return strtod(buf, NULL);
}

int system_is_loaded(double lavg, double cpu_load, double mem_load, int mem_pres)
{
	return lavg > val_p || 100.*cpu_load > val_c ||
		100.*mem_load > val_m || mem_pres > val_u;
}

void emit_time_marker_start()
{
	printf("%ld.", time(NULL));
}

void emit_time_marker()
{
	printf("%ld.", time(NULL) % 10000);
	fflush(stdout);
}

void emit_time_marker_eol()
{
	puts("");
}

int main(int argc, char *argv[])
{
	extern int optind;
	extern char *optarg;
	int c;			/* most recently parsed char in argv[] */
	useconds_t osleepusecs;	/* outer loop sleep in microseconds */
	useconds_t isleepusecs;	/* inner loop sleep in microseconds */

	cmd = argv[0];
	while ((c = getopt(argc, argv, "CMBQP:H:s:t:p:c:m:u:q:r:b:n:L:d:")) != EOF) {
		switch (c) {
		case 'C':			/* show CPU hogs */
			flag_C = 1;
			break;
		case 'M':			/* show Memory hogs */
			flag_M = 1;
			break;
		case 'B':			/* show Block I/O diskwait */
			flag_B = 1;
			break;
		case 'P':			/* show cnt PHP tasks in header */
			flag_P = strtod(optarg, NULL);
			break;
		case 'H':			/* show cnt httpd tasks in header */
			flag_H = strtod(optarg, NULL);
			break;
		case 's':			/* outer loop (secs) */
			val_s = strtod(optarg, NULL);
			if (val_s < 0.001)
				fatal_usage("-s val < 0.001", val_s);
			break;
		case 't':			/* inner loop (secs) */
			val_t = strtod(optarg, NULL);
			if (val_t < 0.001)
				fatal_usage("-t val < 0.001", val_t);
			break;
		case 'p':			/* min busy loadavg */
			val_p = strtod(optarg, NULL);
			if (val_p < 0.001)
				fatal_usage("-p val < 0.001", val_p);
			break;
		case 'c':			/* min CPU load % */
			val_c = strtod(optarg, NULL);
			if (val_c < .1)
				fatal_usage("-c val < .1%", val_c);
			if (val_c > 100.)
				fatal_usage("-c val > 100%", val_c);
			break;
		case 'm':			/* min Mem load % */
			val_m = strtod(optarg, NULL);
			if (val_m < .1)
				fatal_usage("-m val < .1%", val_m);
			if (val_m > 100.)
				fatal_usage("-m val > 100%", val_m);
			break;
		case 'u':			/* min cpuset memory pressure */
			val_u = strtod(optarg, NULL);
			if (val_u < 0)
				fatal_usage("-u val < 0", val_u);
			break;
		case 'q':			/* busy task milli-CPU's */
			val_q = strtol(optarg, NULL, 10);
			if (val_q < 1)
				fatal_usage("-q val < 1", val_q);
			break;
		case 'r':			/* big rss task milli-RAM */
			val_r = strtol(optarg, NULL, 10);
			if (val_r < 1)
				fatal_usage("-r val < 1", val_r);
			break;
		case 'b':
			val_b = strtol(optarg, NULL, 10);
			if (val_b < 1)
				fatal_usage("-b val < 1", val_b);
			break;
		case 'n':			/* max num tasks to show */
			val_n = strtol(optarg, NULL, 10);
			if (val_n < 1)
				fatal_usage("-n val < 1", val_n);
			break;
		case 'L':
			szcmdlinebuf = strtod(optarg, NULL);
			if (szcmdlinebuf < 2 || szcmdlinebuf > 1000)
				fatal_usage("-L val not in [2, 1000]", szcmdlinebuf);
			break;
		case 'd':
			monitor_disk(optarg);
			break;
		case 'Q':			/* Quiet option display */
			flag_Q = 1;
			break;
		default:	/* '?' */
			show_usage_and_exit();
		}
	}
	if (optind < argc)
		show_usage_and_exit();
 
	if ( ! (flag_C || flag_M || flag_B) )	/* set default C flag if none set */
		flag_C = 1;

	tzset();			/* Initialize timezone information */

	ncpus = get_ncpus();

	osleepusecs = (useconds_t) (val_s * 1000000.0);
	isleepusecs = (useconds_t) (val_t * 1000000.0);

	if ((cmdlinebuf = malloc(szcmdlinebuf)) == NULL)
		perror_exit("malloc", "cmdlinebug");

	if (!flag_Q)
		show_current_settings();

	/*
	 * Initialize global CPU load values.
	 */
	read_cpuload();

	/*
	 * Error check the -d settings (easy to get wrong),
	 * by wasting one call to use them.
	 */
	free(get_disks_monitored());

	/*
	 * Outer loop: Silently examine only a few system wide parameters, 
	 * in order to detect when the system starts to become loaded.
	 */
	for (;;) {
		task_usages_t prior, latest;
		double load_avg, cpu_load, mem_load;
		int mem_pres;

		emit_time_marker_start();
		do {
			emit_time_marker();
			usleep(osleepusecs);
			load_avg = read_loadavg();
			cpu_load = read_cpuload();
			mem_load = read_memload();
			mem_pres = read_mempres();
		} while (!system_is_loaded(load_avg, cpu_load, mem_load, mem_pres));
		emit_time_marker_eol();

		/*
		 * Before entering inner loop, sample the per-thread stats.
		 *
		 * Some of the per-thread stats need two consecutive samples
		 * (called prior and latest) before we can compute differences
		 * and rates.  So, we sample, sleep, and then enter inner loop,
		 * where we repeatedly sample, display and sleep, until the
		 * system is no longer loaded.
		 */
		prior = get_task_usages();
		free(get_disks_monitored());

		usleep(min(osleepusecs, isleepusecs));

		/*
		 * Inner loop: displays system wide loading measures, and
		 * also examines every task  in order to display those tasks
		 * using the most CPU, Mem or Disk resources.
		 */
		for (;;) {
			char *dsk_str;
			int cnt_php;
			int cnt_httpd;

			latest = get_task_usages();
			dsk_str = get_disks_monitored();
			cnt_php = flag_P ? get_cnt_php(latest) : 0;
			cnt_httpd = flag_H ? get_cnt_httpd(latest) : 0;

			show_task_usages(prior, latest, load_avg, cpu_load,
				mem_load, mem_pres, cnt_php, cnt_httpd, dsk_str);
			free_task_usages(prior);
			prior = latest;
			free(dsk_str);

			usleep(isleepusecs);

			load_avg = read_loadavg();
			cpu_load = read_cpuload();
			mem_load = read_memload();
			mem_pres = read_mempres();

			if (!system_is_loaded(load_avg, cpu_load, mem_load, mem_pres))
				break;
		}
		free_task_usages(prior);
	}
	exit(0);
}
