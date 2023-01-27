#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#ifdef __linux__
#include <linux/watchdog.h>
#endif
#if HAVE_OPING
#include <oping.h>
#endif
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/poll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

#include "igt_aux.h"
#include "igt_core.h"
#include "igt_taints.h"
#include "executor.h"
#include "output_strings.h"
#include "runnercomms.h"

#define KMSG_HEADER "[IGT] "
#define KMSG_WARN 4

static struct {
	int *fds;
	size_t num_dogs;
} watchdogs;

__attribute__((format(printf, 2, 3)))
static void __logf__(FILE *stream, const char *fmt, ...)
{
	int saved_errno = errno;
	struct timespec tv;
	va_list ap;

	if (clock_gettime(CLOCK_BOOTTIME, &tv))
		clock_gettime(CLOCK_REALTIME, &tv);
	fprintf(stream, "[%ld.%06ld] ", tv.tv_sec, tv.tv_nsec / 1000);

	va_start(ap, fmt);
	errno = saved_errno;
	vfprintf(stream, fmt, ap);
	va_end(ap);
}
#define outf(fmt...) __logf__(stdout, fmt)
#define errf(fmt...) __logf__(stderr, fmt)

static void __close_watchdog(int fd)
{
	ssize_t ret = write(fd, "V", 1);

	if (ret == -1)
		errf("Failed to stop a watchdog: %m\n");

	close(fd);
}

static void close_watchdogs(struct settings *settings)
{
	size_t i;

	if (settings && settings->log_level >= LOG_LEVEL_VERBOSE)
		outf("Closing watchdogs\n");

	if (settings == NULL && watchdogs.num_dogs != 0)
		errf("Closing watchdogs from exit handler!\n");

	for (i = 0; i < watchdogs.num_dogs; i++) {
		__close_watchdog(watchdogs.fds[i]);
	}

	free(watchdogs.fds);
	watchdogs.num_dogs = 0;
	watchdogs.fds = NULL;
}

static void close_watchdogs_atexit(void)
{
	close_watchdogs(NULL);
}

static void init_watchdogs(struct settings *settings)
{
	int i;
	char name[32];
	int fd;

	memset(&watchdogs, 0, sizeof(watchdogs));

	if (!settings->use_watchdog)
		return;

	if (settings->log_level >= LOG_LEVEL_VERBOSE) {
		outf("Initializing watchdogs\n");
	}

	atexit(close_watchdogs_atexit);

	for (i = 0; ; i++) {
		snprintf(name, sizeof(name), "/dev/watchdog%d", i);
		if ((fd = open(name, O_RDWR | O_CLOEXEC)) < 0)
			break;

		watchdogs.num_dogs++;
		watchdogs.fds = realloc(watchdogs.fds, watchdogs.num_dogs * sizeof(int));
		watchdogs.fds[i] = fd;

		if (settings->log_level >= LOG_LEVEL_VERBOSE)
			outf("  %s\n", name);
	}
}

static int watchdogs_set_timeout(int timeout)
{
	size_t i;
	int orig_timeout = timeout;

	for (i = 0; i < watchdogs.num_dogs; i++) {
		if (ioctl(watchdogs.fds[i], WDIOC_SETTIMEOUT, &timeout)) {
			__close_watchdog(watchdogs.fds[i]);
			watchdogs.fds[i] = -1;
			continue;
		}

		if (timeout < orig_timeout) {
			/*
			 * Timeout of this caliber refused. We want to
			 * use the same timeout for all devices.
			 */
			return watchdogs_set_timeout(timeout);
		}
	}

	return timeout;
}

static void ping_watchdogs(void)
{
	size_t i;
	int ret;

	for (i = 0; i < watchdogs.num_dogs; i++) {
		ret = ioctl(watchdogs.fds[i], WDIOC_KEEPALIVE, NULL);
		if (ret == -1)
			errf("Failed to ping a watchdog: %m\n");
	}
}

#if HAVE_OPING
static pingobj_t *pingobj = NULL;

static bool load_ping_config_from_file(void)
{
	GError *error = NULL;
	GKeyFile *key_file = NULL;
	const char *ping_hostname;

	/* Load igt config file */
	key_file = igt_load_igtrc();
	if (!key_file)
		return false;

	ping_hostname =
		g_key_file_get_string(key_file, "DUT",
				      "PingHostName", &error);

	g_clear_error(&error);
	g_key_file_free(key_file);

	if (!ping_hostname)
		return false;

	if (ping_host_add(pingobj, ping_hostname)) {
		fprintf(stderr,
			"abort on ping: Cannot use hostname from config file\n");
		return false;
	}

	return true;
}

static bool load_ping_config_from_env(void)
{
	const char *ping_hostname;

	ping_hostname = getenv("IGT_PING_HOSTNAME");
	if (!ping_hostname)
		return false;

	if (ping_host_add(pingobj, ping_hostname)) {
		fprintf(stderr,
			"abort on ping: Cannot use hostname from environment\n");
		return false;
	}

	return true;
}

/*
 * On some hosts, getting network back up after suspend takes
 * upwards of 10 seconds. 20 seconds should be enough to see
 * if network comes back at all, and hopefully not too long to
 * make external monitoring freak out.
 */
#define PING_ABORT_DEADLINE 20

static bool can_ping(void)
{
	igt_until_timeout(PING_ABORT_DEADLINE) {
		pingobj_iter_t *iter;

		ping_send(pingobj);

		for (iter = ping_iterator_get(pingobj);
		     iter != NULL;
		     iter = ping_iterator_next(iter)) {
			double latency;
			size_t len = sizeof(latency);

			ping_iterator_get_info(iter,
					       PING_INFO_LATENCY,
					       &latency,
					       &len);
			if (latency >= 0.0)
				return true;
		}
	}

	return false;
}

#endif

static void ping_config(void)
{
#if HAVE_OPING
	double single_attempt_timeout = 1.0;

	if (pingobj)
		return;

	pingobj = ping_construct();

	/* Try env first, then config file */
	if (!load_ping_config_from_env() && !load_ping_config_from_file()) {
		fprintf(stderr,
			"abort on ping: No host to ping configured\n");
		ping_destroy(pingobj);
		pingobj = NULL;
		return;
	}

	ping_setopt(pingobj, PING_OPT_TIMEOUT, &single_attempt_timeout);
#endif
}

static char *handle_ping(void)
{
#if HAVE_OPING
	if (pingobj && !can_ping()) {
		char *reason;

		asprintf(&reason,
			 "Ping host did not respond to ping, network down");
		return reason;
	}
#endif

	return NULL;
}

static char *handle_lockdep(void)
{
	const char *header = "Lockdep not active\n\n/proc/lockdep_stats contents:\n";
	int fd = open("/proc/lockdep_stats", O_RDONLY);
	const char *debug_locks_line = " debug_locks:";
	char buf[4096], *p;
	ssize_t bufsize = 0;
	int val;

	if (fd < 0)
		return NULL;

	strcpy(buf, header);

	if ((bufsize = read(fd, buf + strlen(header), sizeof(buf) - strlen(header) - 1)) < 0)
		return NULL;
	bufsize += strlen(header);
	buf[bufsize] = '\0';
	close(fd);

	if ((p = strstr(buf, debug_locks_line)) != NULL &&
	    sscanf(p + strlen(debug_locks_line), "%d", &val) == 1 &&
	    val != 1) {
		return strdup(buf);
	}

	return NULL;
}

static char *handle_taint(void)
{
	unsigned long taints, bad;
	const char *explain;
	char *reason;

	bad = igt_kernel_tainted(&taints);
	if (!bad)
		return NULL;

	asprintf(&reason, "Kernel badly tainted (%#lx, %#lx) (check dmesg for details):\n",
		 taints, bad);

	while ((explain = igt_explain_taints(&bad))) {
		char *old_reason = reason;
		asprintf(&reason, "%s\t%s\n", old_reason, explain);
		free(old_reason);
	}

	return reason;
}

static const struct {
	int condition;
	char *(*handler)(void);
} abort_handlers[] = {
	{ ABORT_LOCKDEP, handle_lockdep },
	{ ABORT_TAINT, handle_taint },
	{ ABORT_PING, handle_ping },
	{ 0, 0 },
};

static char *need_to_abort(const struct settings* settings)
{
	typeof(*abort_handlers) *it;

	for (it = abort_handlers; it->condition; it++) {
		char *abort;

		if (!(settings->abort_mask & it->condition))
			continue;

		abort = it->handler();
		if (!abort)
			continue;

		if (settings->log_level >= LOG_LEVEL_NORMAL)
			errf("Aborting: %s\n", abort);

		return abort;
	}

	return NULL;
}

static void prune_subtest(struct job_list_entry *entry, const char *subtest)
{
	char *excl;

	/*
	 * Subtest pruning is done by adding exclusion strings to the
	 * subtest list. The last matching item on the subtest
	 * selection command line flag decides whether to run a
	 * subtest, see igt_core.c for details.  If the list is empty,
	 * the expected subtest set is unknown, so we need to add '*'
	 * first so we can start excluding.
	 */

	if (entry->subtest_count == 0) {
		entry->subtest_count++;
		entry->subtests = realloc(entry->subtests, entry->subtest_count * sizeof(*entry->subtests));
		entry->subtests[0] = strdup("*");
	}

	excl = malloc(strlen(subtest) + 2);
	excl[0] = '!';
	strcpy(excl + 1, subtest);

	entry->subtest_count++;
	entry->subtests = realloc(entry->subtests, entry->subtest_count * sizeof(*entry->subtests));
	entry->subtests[entry->subtest_count - 1] = excl;
}

static bool prune_from_journal(struct job_list_entry *entry, int fd)
{
	char *subtest;
	FILE *f;
	size_t pruned = 0;
	size_t old_count = entry->subtest_count;

	/*
	 * Each journal line is a subtest that has been started, or
	 * the line 'exit:$exitcode (time)', or 'timeout:$exitcode (time)'.
	 */

	f = fdopen(fd, "r");
	if (!f)
		return false;

	while (fscanf(f, "%ms", &subtest) == 1) {
		if (!strncmp(subtest, EXECUTOR_EXIT, strlen(EXECUTOR_EXIT))) {
			/* Fully done. Mark that by making the binary name invalid. */
			fscanf(f, " (%*fs)");
			entry->binary[0] = '\0';
			free(subtest);
			continue;
		}

		if (!strncmp(subtest, EXECUTOR_TIMEOUT, strlen(EXECUTOR_TIMEOUT))) {
			fscanf(f, " (%*fs)");
			free(subtest);
			continue;
		}

		prune_subtest(entry, subtest);

		free(subtest);
		pruned++;
	}

	fclose(f);

	/*
	 * If we know the subtests we originally wanted to run, check
	 * if we got an equal amount already.
	 */
	if (old_count > 0 && pruned >= old_count)
		entry->binary[0] = '\0';

	return pruned > 0;
}

struct prune_comms_data
{
	struct job_list_entry *entry;
	int pruned;
	bool got_exit;
};

static bool prune_handle_subtest_start(const struct runnerpacket *packet,
				       runnerpacket_read_helper helper,
				       void *userdata)
{
	struct prune_comms_data *data = userdata;

	prune_subtest(data->entry, helper.subteststart.name);
	data->pruned++;

	return true;
}

static bool prune_handle_exit(const struct runnerpacket *packet,
			      runnerpacket_read_helper helper,
			      void *userdata)
{
	struct prune_comms_data *data = userdata;

	data->got_exit = true;

	return true;
}

static bool prune_from_comms(struct job_list_entry *entry, int fd)
{
	struct prune_comms_data data = {
		.entry = entry,
		.pruned = 0,
		.got_exit = false,
	};
	struct comms_visitor visitor = {
		.subtest_start = prune_handle_subtest_start,
		.exit = prune_handle_exit,

		.userdata = &data,
	};
	size_t old_count = entry->subtest_count;

	if (comms_read_dump(fd, &visitor) == COMMSPARSE_ERROR)
		return false;

	/*
	 * If we know the subtests we originally wanted to run, check
	 * if we got an equal amount already.
	 */
	if (old_count > 0 && data.pruned >= old_count)
		entry->binary[0] = '\0';

	/*
	 * If we don't know how many subtests there should be but we
	 * got an exit, also consider the test fully finished.
	 */
	if (data.got_exit)
		entry->binary[0] = '\0';

	return data.pruned > 0;
}

static const char *filenames[_F_LAST] = {
	[_F_JOURNAL] = "journal.txt",
	[_F_OUT] = "out.txt",
	[_F_ERR] = "err.txt",
	[_F_DMESG] = "dmesg.txt",
	[_F_SOCKET] = "comms",
};

static int open_at_end(int dirfd, const char *name)
{
	int fd = openat(dirfd, name, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
	char last;

	if (fd >= 0) {
		if (lseek(fd, -1, SEEK_END) >= 0 &&
		    read(fd, &last, 1) == 1 &&
		    last != '\n') {
			write(fd, "\n", 1);
		}
		lseek(fd, 0, SEEK_END);
	}

	return fd;
}

static int open_for_reading(int dirfd, const char *name)
{
	return openat(dirfd, name, O_RDONLY);
}

bool open_output_files(int dirfd, int *fds, bool write)
{
	int i;
	int (*openfunc)(int, const char*) = write ? open_at_end : open_for_reading;

	for (i = 0; i < _F_LAST; i++) {
		if ((fds[i] = openfunc(dirfd, filenames[i])) < 0) {
			/* Ignore failure to open socket comms for reading */
			if (i == _F_SOCKET && !write) continue;

			while (--i >= 0)
				close(fds[i]);
			return false;
		}
	}

	return true;
}

void close_outputs(int *fds)
{
	int i;

	for (i = 0; i < _F_LAST; i++) {
		close(fds[i]);
	}
}

/* Returns the number of bytes written to disk, or a negative number on error */
static long dump_dmesg(int kmsgfd, int outfd)
{
	/*
	 * Write kernel messages to the log file until we reach
	 * 'now'. Unfortunately, /dev/kmsg doesn't support seeking to
	 * -1 from SEEK_END so we need to use a second fd to read a
	 * message to match against, or stop when we reach EAGAIN.
	 */

	int comparefd;
	unsigned flags;
	unsigned long long seq, cmpseq, usec;
	bool underflow_once = false;
	char cont;
	char buf[2048];
	ssize_t r;
	long written = 0;

	if (kmsgfd < 0)
		return 0;

	comparefd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (comparefd < 0) {
		errf("Error opening another fd for /dev/kmsg\n");
		return -1;
	}
	lseek(comparefd, 0, SEEK_END);

	while (1) {
		if (comparefd >= 0) {
			r = read(comparefd, buf, sizeof(buf) - 1);
			if (r < 0) {
				if (errno != EAGAIN && errno != EPIPE) {
					errf("Warning: Error reading kmsg comparison record: %m\n");
					close(comparefd);
					return 0;
				}
			} else {
				buf[r] = '\0';
				if (sscanf(buf, "%u,%llu,%llu,%c;",
					   &flags, &cmpseq, &usec, &cont) == 4) {
					/* Reading comparison record done. */
					close(comparefd);
					comparefd = -1;
				}
			}
		}

		r = read(kmsgfd, buf, sizeof(buf));
		if (r < 0) {
			if (errno == EPIPE) {
				if (!underflow_once) {
					errf("Warning: kernel log ringbuffer underflow, some records lost.\n");
					underflow_once = true;
				}
				continue;
			} else if (errno == EINVAL) {
				errf("Warning: Buffer too small for kernel log record, record lost.\n");
				continue;
			} else if (errno != EAGAIN) {
				errf("Error reading from kmsg: %m\n");
				return -errno;
			}

			/* EAGAIN, so we're done dumping */
			close(comparefd);
			return written;
		}

		write(outfd, buf, r);
		written += r;

		if (comparefd < 0 && sscanf(buf, "%u,%llu,%llu,%c;",
					    &flags, &seq, &usec, &cont) == 4) {
			/*
			 * Comparison record has been read, compare
			 * the sequence number to see if we have read
			 * enough.
			 */
			if (seq >= cmpseq)
				return written;
		}
	}
}

static bool kill_child(int sig, pid_t child)
{
	/*
	 * Send the signal to the child directly, and to the child's
	 * process group.
	 */
	kill(-child, sig);
	if (kill(child, sig) && errno == ESRCH) {
		errf("Child process does not exist. This shouldn't happen.\n");
		return false;
	}

	return true;
}

static const char *get_cmdline(pid_t pid, char *buf, ssize_t len)
{
	int fd;

	if (snprintf(buf, len, "/proc/%d/cmdline", pid) > len)
		return "unknown";

	fd = open(buf, O_RDONLY);
	if (fd < 0)
		return "unknown";

	len = read(fd, buf, len - 1);
	close(fd);
	if (len < 0)
		return "unknown";

	/* cmdline is the whole argv[], completed with NUL-terminators */
	for (size_t i = 0; i < len; i++)
		if (buf[i] == '\0')
			buf[i] = ' ';

	/* chomp away the trailing spaces */
	while (len && buf[len - 1] == ' ')
		--len;

	buf[len] = '\0'; /* but make sure that we return a valid string! */
	return buf;
}

static bool sysrq(char cmd)
{
	bool success = false;
	int fd;

	fd = open("/proc/sysrq-trigger", O_WRONLY);
	if (fd >= 0) {
		success = write(fd, &cmd, 1) == 1;
		close(fd);
	}

	return success;
}

static void kmsg_log(int severity, const char *msg)
{
	char *str = NULL;
	int len, fd;

	len = asprintf(&str, "<%d>%s%s", severity, KMSG_HEADER, msg);
	if (!str)
		return;

	fd = open("/dev/kmsg", O_WRONLY);
	if (fd != -1) {
		write(fd, str, len);
		close(fd);
	}

	free(str);
}

static const char *show_kernel_task_state(const char *msg)
{
	kmsg_log(KMSG_WARN, msg);
	sysrq('t'); /* task state, stack traces and cpu run lists */
	sysrq('m'); /* task memory usage */

	return msg;
}

static bool disk_usage_limit_exceeded(struct settings *settings,
				      size_t disk_usage)
{
	return settings->disk_usage_limit != 0 &&
		disk_usage > settings->disk_usage_limit;
}

static const char *need_to_timeout(struct settings *settings,
				   int killed,
				   unsigned long taints,
				   double time_since_activity,
				   double time_since_subtest,
				   double time_since_kill,
				   size_t disk_usage)
{
	int decrease = 1;

	if (killed) {
		/*
		 * Timeout after being killed is a hardcoded amount
		 * depending on which signal we already used. The
		 * exception is SIGKILL which just immediately bails
		 * out if the kernel is tainted, because there's
		 * little to no hope of the process dying gracefully
		 * or at all.
		 *
		 * Note that if killed == SIGKILL, the caller needs
		 * special handling anyway and should ignore the
		 * actual string returned.
		 */
		const double kill_timeout = killed == SIGKILL ? 20.0 : 120.0;

		if ((killed == SIGKILL && is_tainted(taints)) ||
		    time_since_kill > kill_timeout)
			return "Timeout. Killing the current test with SIGKILL.\n";

		/*
		 * We don't care for the other reasons to timeout if
		 * we're already killing the test.
		 */
		return NULL;
	}

	/*
	 * If we're configured to care about taints,
	 * decrease timeouts in use if there's a taint,
	 * or kill the test if no timeouts have been requested.
	 */
	if (settings->abort_mask & ABORT_TAINT &&
	    is_tainted(taints)) {
		/* list of timeouts that may postpone immediate kill on taint */
		if (settings->per_test_timeout || settings->inactivity_timeout)
			decrease = 10;
		else
			return "Killing the test because the kernel is tainted.\n";
	}

	if (settings->per_test_timeout != 0 &&
	    time_since_subtest > settings->per_test_timeout / decrease) {
		if (decrease > 1)
			return "Killing the test because the kernel is tainted.\n";
		return show_kernel_task_state("Per-test timeout exceeded. Killing the current test with SIGQUIT.\n");
	}

	if (settings->inactivity_timeout != 0 &&
	    time_since_activity > settings->inactivity_timeout / decrease ) {
		if (decrease > 1)
			return "Killing the test because the kernel is tainted.\n";
		return show_kernel_task_state("Inactivity timeout exceeded. Killing the current test with SIGQUIT.\n");
	}

	if (disk_usage_limit_exceeded(settings, disk_usage))
		return "Disk usage limit exceeded.\n";

	return NULL;
}

static int next_kill_signal(int killed)
{
	switch (killed) {
	case 0:
		return SIGQUIT;
	case SIGQUIT:
		return SIGKILL;
	case SIGKILL:
	default:
		assert(!"Unreachable");
		return SIGKILL;
	}
}

static void write_packet_with_canary(int fd, struct runnerpacket *packet, bool sync)
{
	uint32_t canary = socket_dump_canary();

	write(fd, &canary, sizeof(canary));
	write(fd, packet, packet->size);
	if (sync)
		fdatasync(fd);
}

/* TODO: Refactor this macro from here and from various tests to lib */
#define KB(x) ((x) * 1024)

/*
 * Returns:
 *  =0 - Success
 *  <0 - Failure executing
 *  >0 - Timeout happened, need to recreate from journal
 */
static int monitor_output(pid_t child,
			  int outfd, int errfd, int socketfd,
			  int kmsgfd, int sigfd,
			  int *outputs,
			  double *time_spent,
			  struct settings *settings,
			  char **abortreason)
{
	fd_set set;
	char *buf;
	size_t bufsize;
	char *outbuf = NULL;
	size_t outbufsize = 0;
	char current_subtest[256] = {};
	struct signalfd_siginfo siginfo;
	ssize_t s;
	int n, status;
	int nfds = outfd;
	const int interval_length = 1;
	int wd_timeout;
	int killed = 0; /* 0 if not killed, signal number otherwise */
	struct timespec time_beg, time_now, time_last_activity, time_last_subtest, time_killed;
	unsigned long taints = 0;
	bool aborting = false;
	size_t disk_usage = 0;
	bool socket_comms_used = false; /* whether the test actually uses comms */

	igt_gettime(&time_beg);
	time_last_activity = time_last_subtest = time_killed = time_beg;

	if (errfd > nfds)
		nfds = errfd;
	if (socketfd > nfds)
		nfds = socketfd;
	if (kmsgfd > nfds)
		nfds = kmsgfd;
	if (sigfd > nfds)
		nfds = sigfd;
	nfds++;

	/*
	 * If we're still alive, we want to kill the test process
	 * instead of cutting power. Use a healthy 2 minute watchdog
	 * timeout that gets automatically reduced if the device
	 * doesn't support it.
	 *
	 * watchdogs_set_timeout() is a no-op and returns the given
	 * timeout if we don't have use_watchdog set in settings.
	 */
	wd_timeout = watchdogs_set_timeout(120);

	if (wd_timeout < 120) {
		/*
		 * Watchdog timeout smaller, warn the user. With the
		 * short select() timeout we're using we're able to
		 * ping the watchdog regardless.
		 */
		if (settings->log_level >= LOG_LEVEL_VERBOSE) {
			outf("Watchdog doesn't support the timeout we requested (shortened to %d seconds).\n",
			     wd_timeout);
		}
	}

	bufsize = KB(256);
	buf = malloc(bufsize);

	while (outfd >= 0 || errfd >= 0 || sigfd >= 0) {
		const char *timeout_reason;
		struct timeval tv = { .tv_sec = interval_length };

		FD_ZERO(&set);
		if (outfd >= 0)
			FD_SET(outfd, &set);
		if (errfd >= 0)
			FD_SET(errfd, &set);
		if (socketfd >= 0)
			FD_SET(socketfd, &set);
		if (kmsgfd >= 0)
			FD_SET(kmsgfd, &set);
		if (sigfd >= 0)
			FD_SET(sigfd, &set);

		n = select(nfds, &set, NULL, NULL, &tv);
		ping_watchdogs();

		if (n < 0) {
			/* TODO */
			return -1;
		}

		igt_gettime(&time_now);

		/* TODO: Refactor these handlers to their own functions */
		if (outfd >= 0 && FD_ISSET(outfd, &set)) {
			char *newline;

			time_last_activity = time_now;

			s = read(outfd, buf, bufsize);
			if (s <= 0) {
				if (s < 0) {
					errf("Error reading test's stdout: %m\n");
				}

				close(outfd);
				outfd = -1;
				goto out_end;
			}

			write(outputs[_F_OUT], buf, s);
			disk_usage += s;
			if (settings->sync) {
				fdatasync(outputs[_F_OUT]);
			}

			outbuf = realloc(outbuf, outbufsize + s);
			memcpy(outbuf + outbufsize, buf, s);
			outbufsize += s;

			while ((newline = memchr(outbuf, '\n', outbufsize)) != NULL) {
				size_t linelen = newline - outbuf + 1;

				if (linelen > strlen(STARTING_SUBTEST) &&
				    !memcmp(outbuf, STARTING_SUBTEST, strlen(STARTING_SUBTEST))) {
					write(outputs[_F_JOURNAL], outbuf + strlen(STARTING_SUBTEST),
					      linelen - strlen(STARTING_SUBTEST));
					if (settings->sync) {
						fdatasync(outputs[_F_JOURNAL]);
					}
					memcpy(current_subtest, outbuf + strlen(STARTING_SUBTEST),
					       linelen - strlen(STARTING_SUBTEST));
					current_subtest[linelen - strlen(STARTING_SUBTEST)] = '\0';

					time_last_subtest = time_now;
					disk_usage = s;

					if (settings->log_level >= LOG_LEVEL_VERBOSE) {
						fwrite(outbuf, 1, linelen, stdout);
					}
				}
				if (linelen > strlen(SUBTEST_RESULT) &&
				    !memcmp(outbuf, SUBTEST_RESULT, strlen(SUBTEST_RESULT))) {
					char *delim = memchr(outbuf, ':', linelen);

					if (delim != NULL) {
						size_t subtestlen = delim - outbuf - strlen(SUBTEST_RESULT);
						if (memcmp(current_subtest, outbuf + strlen(SUBTEST_RESULT),
							   subtestlen)) {
							/* Result for a test that didn't ever start */
							write(outputs[_F_JOURNAL],
							      outbuf + strlen(SUBTEST_RESULT),
							      subtestlen);
							write(outputs[_F_JOURNAL], "\n", 1);
							if (settings->sync) {
								fdatasync(outputs[_F_JOURNAL]);
							}
							current_subtest[0] = '\0';
						}

						if (settings->log_level >= LOG_LEVEL_VERBOSE) {
							fwrite(outbuf, 1, linelen, stdout);
						}
					}
				}
				if (linelen > strlen(STARTING_DYNAMIC_SUBTEST) &&
				    !memcmp(outbuf, STARTING_DYNAMIC_SUBTEST, strlen(STARTING_DYNAMIC_SUBTEST))) {
					time_last_subtest = time_now;
					disk_usage = s;

					if (settings->log_level >= LOG_LEVEL_VERBOSE) {
						fwrite(outbuf, 1, linelen, stdout);
					}
				}
				if (linelen > strlen(DYNAMIC_SUBTEST_RESULT) &&
				    !memcmp(outbuf, DYNAMIC_SUBTEST_RESULT, strlen(DYNAMIC_SUBTEST_RESULT))) {
					char *delim = memchr(outbuf, ':', linelen);

					if (delim != NULL) {
						if (settings->log_level >= LOG_LEVEL_VERBOSE) {
							fwrite(outbuf, 1, linelen, stdout);
						}
					}
				}

				memmove(outbuf, newline + 1, outbufsize - linelen);
				outbufsize -= linelen;
			}
		}
	out_end:

		if (errfd >= 0 && FD_ISSET(errfd, &set)) {
			time_last_activity = time_now;

			s = read(errfd, buf, bufsize);
			if (s <= 0) {
				if (s < 0) {
					errf("Error reading test's stderr: %m\n");
				}
				close(errfd);
				errfd = -1;
			} else {
				write(outputs[_F_ERR], buf, s);
				disk_usage += s;
				if (settings->sync) {
					fdatasync(outputs[_F_ERR]);
				}
			}
		}

		if (socketfd >= 0 && FD_ISSET(socketfd, &set)) {
			struct runnerpacket *packet;

			time_last_activity = time_now;

			/* Fully drain everything */
			while (true) {
				s = recv(socketfd, buf, bufsize, MSG_DONTWAIT);

				if (s < 0) {
					if (errno == EAGAIN)
						break;

					errf("Error reading from communication socket: %m\n");

					close(socketfd);
					socketfd = -1;
					goto socket_end;
				}

				packet = (struct runnerpacket *)buf;
				if (s < sizeof(*packet) || s != packet->size) {
					struct runnerpacket *message, *override;

					errf("Socket communication error: Received %zd bytes, expected %zd\n",
					     s, s >= sizeof(packet->size) ? packet->size : sizeof(*packet));
					message = runnerpacket_log(STDOUT_FILENO,
								   "\nrunner: Socket communication error, invalid packet size. "
								   "Packet is discarded, test result and logs might be incorrect.\n");
					write_packet_with_canary(outputs[_F_SOCKET], message, false);
					free(message);

					override = runnerpacket_resultoverride("warn");
					write_packet_with_canary(outputs[_F_SOCKET], override, settings->sync);
					free(override);

					/* Continue using socket comms, hope for the best. */
					goto socket_end;
				}

				write_packet_with_canary(outputs[_F_SOCKET], packet, settings->sync);

				/*
				 * runner sends EXEC itself before executing
				 * the test, other types indicate the test
				 * really uses socket comms
				 */
				if (packet->type != PACKETTYPE_EXEC)
					socket_comms_used = true;

				if (packet->type == PACKETTYPE_SUBTEST_START ||
				    packet->type == PACKETTYPE_DYNAMIC_SUBTEST_START) {
					time_last_subtest = time_now;
					disk_usage = 0;
				}

				disk_usage += packet->size;

				if (settings->log_level >= LOG_LEVEL_VERBOSE) {
					runnerpacket_read_helper helper = {};
					const char *time;

					if (packet->type == PACKETTYPE_SUBTEST_START ||
					    packet->type == PACKETTYPE_SUBTEST_RESULT ||
					    packet->type == PACKETTYPE_DYNAMIC_SUBTEST_START ||
					    packet->type == PACKETTYPE_DYNAMIC_SUBTEST_RESULT)
						helper = read_runnerpacket(packet);

					switch (helper.type) {
					case PACKETTYPE_SUBTEST_START:
						if (helper.subteststart.name)
							outf("Starting subtest: %s\n", helper.subteststart.name);
						break;
					case PACKETTYPE_SUBTEST_RESULT:
						if (helper.subtestresult.name && helper.subtestresult.result) {
							time = "<unknown>";
							if (helper.subtestresult.timeused)
								time = helper.subtestresult.timeused;
							outf("Subtest %s: %s (%ss)\n",
							     helper.subtestresult.name,
							     helper.subtestresult.result,
							     time);
						}
						break;
					case PACKETTYPE_DYNAMIC_SUBTEST_START:
						if (helper.dynamicsubteststart.name)
							outf("Starting dynamic subtest: %s\n", helper.dynamicsubteststart.name);
						break;
					case PACKETTYPE_DYNAMIC_SUBTEST_RESULT:
						if (helper.dynamicsubtestresult.name && helper.dynamicsubtestresult.result) {
							time = "<unknown>";
							if (helper.dynamicsubtestresult.timeused)
								time = helper.dynamicsubtestresult.timeused;
							outf("Dynamic subtest %s: %s (%ss)\n",
							     helper.dynamicsubtestresult.name,
							     helper.dynamicsubtestresult.result,
							     time);
						}
						break;
					default:
						break;
					}
				}
			}
		}
	socket_end:

		if (kmsgfd >= 0 && FD_ISSET(kmsgfd, &set)) {
			long dmesgwritten;

			time_last_activity = time_now;

			dmesgwritten = dump_dmesg(kmsgfd, outputs[_F_DMESG]);
			if (settings->sync)
				fdatasync(outputs[_F_DMESG]);

			if (dmesgwritten < 0) {
				close(kmsgfd);
				kmsgfd = -1;
			} else {
				disk_usage += dmesgwritten;
			}
		}

		if (sigfd >= 0 && FD_ISSET(sigfd, &set)) {
			double time;

			s = read(sigfd, &siginfo, sizeof(siginfo));
			if (s < 0) {
				errf("Error reading from signalfd: %m\n");
				continue;
			} else if (siginfo.ssi_signo == SIGCHLD) {
				if (child != waitpid(child, &status, WNOHANG)) {
					errf("Failed to reap child\n");
					status = 9999;
				} else if (WIFEXITED(status)) {
					status = WEXITSTATUS(status);
					if (status >= 128) {
						status = 128 - status;
					}
				} else if (WIFSIGNALED(status)) {
					status = -WTERMSIG(status);
				} else {
					status = 9999;
				}
			} else {
				/* We're dying, so we're taking them with us */
				if (settings->log_level >= LOG_LEVEL_NORMAL) {
					char comm[120];

					outf("Abort requested by %s [%d] via %s, terminating children\n",
					     get_cmdline(siginfo.ssi_pid, comm, sizeof(comm)),
					     siginfo.ssi_pid,
					     strsignal(siginfo.ssi_signo));
				}

				if (siginfo.ssi_signo == SIGHUP) {
					/*
					 * If taken down with SIGHUP,
					 * arrange the current test to
					 * be marked as notrun instead
					 * of incomplete. For other
					 * signals we don't need to do
					 * anything, the lack of a
					 * completion marker of any
					 * kind in the logs will mark
					 * those tests as
					 * incomplete. Note that since
					 * we set 'aborting' to true
					 * we're going to skip all
					 * other journal writes later.
					 */

					if (settings->log_level >= LOG_LEVEL_NORMAL)
						outf("Exiting gracefully, currently running test will have a 'notrun' result\n");

					if (socket_comms_used) {
						struct runnerpacket *message, *override;

						message = runnerpacket_log(STDOUT_FILENO, "runner: Exiting gracefully, overriding this test's result to be notrun\n");
						write_packet_with_canary(outputs[_F_SOCKET], message, false); /* possible sync after the override packet */
						free(message);

						override = runnerpacket_resultoverride("notrun");
						write_packet_with_canary(outputs[_F_SOCKET], override, settings->sync);
						free(override);
					} else {
						dprintf(outputs[_F_JOURNAL], "%s%d (%.3fs)\n",
							EXECUTOR_EXIT,
							-SIGHUP, 0.0);
						if (settings->sync)
							fdatasync(outputs[_F_JOURNAL]);
					}
				}

				aborting = true;
				killed = SIGQUIT;
				if (!kill_child(killed, child))
					return -1;
				time_killed = time_now;

				continue;
			}

			time = igt_time_elapsed(&time_beg, &time_now);
			if (time < 0.0)
				time = 0.0;

			if (!aborting) {
				bool timeoutresult = false;

				if (killed)
					timeoutresult = true;

				/* If we're stopping because we killed
				 * the test for tainting, let's not
				 * call it a timeout. Since the test
				 * execution was still going on, we
				 * probably didn't yet get the subtest
				 * result line printed. Such a case is
				 * parsed as an incomplete unless the
				 * journal says timeout, ergo to make
				 * the result an incomplete we avoid
				 * journaling a timeout here.
				 */
				if (killed && is_tainted(taints)) {
					timeoutresult = false;

					/*
					 * Also inject a message to
					 * the test's stdout. As we're
					 * shooting for an incomplete
					 * anyway, we don't need to
					 * care if we're not between
					 * full lines from stdout. We
					 * do need to make sure we
					 * have newlines on both ends
					 * of this injection though.
					 */
					if (socket_comms_used) {
						struct runnerpacket *message;
						char killmsg[256];

						snprintf(killmsg, sizeof(killmsg),
							 "runner: This test was killed due to a kernel taint (0x%lx).\n", taints);
						message = runnerpacket_log(STDOUT_FILENO, killmsg);
						write_packet_with_canary(outputs[_F_SOCKET], message, settings->sync);
						free(message);
					} else {
						dprintf(outputs[_F_OUT],
							"\nrunner: This test was killed due to a kernel taint (0x%lx).\n",
							taints);
						if (settings->sync)
							fdatasync(outputs[_F_OUT]);
					}
				}

				/*
				 * Same goes for stopping because we
				 * exceeded the disk usage limit.
				 */
				if (killed && disk_usage_limit_exceeded(settings, disk_usage)) {
					timeoutresult = false;

					if (socket_comms_used) {
						struct runnerpacket *message;
						char killmsg[256];

						snprintf(killmsg, sizeof(killmsg),
							 "runner: This test was killed due to exceeding disk usage limit. "
							 "(Used %zd bytes, limit %zd)\n",
							 disk_usage,
							 settings->disk_usage_limit);
						message = runnerpacket_log(STDOUT_FILENO, killmsg);
						write_packet_with_canary(outputs[_F_SOCKET], message, settings->sync);
						free(message);
					} else {
						dprintf(outputs[_F_OUT],
							"\nrunner: This test was killed due to exceeding disk usage limit. "
							"(Used %zd bytes, limit %zd)\n",
							disk_usage,
							settings->disk_usage_limit);
						if (settings->sync)
							fdatasync(outputs[_F_OUT]);
					}
				}

				if (socket_comms_used) {
					struct runnerpacket *exitpacket;
					char timestr[32];

					snprintf(timestr, sizeof(timestr), "%.3f", time);

					if (timeoutresult) {
						struct runnerpacket *override;

						override = runnerpacket_resultoverride("timeout");
						write_packet_with_canary(outputs[_F_SOCKET], override, false); /* sync after exitpacket */
						free(override);
					}

					exitpacket = runnerpacket_exit(status, timestr);
					write_packet_with_canary(outputs[_F_SOCKET], exitpacket, settings->sync);
					free(exitpacket);
				} else {
					const char *exitline;

					exitline = timeoutresult ? EXECUTOR_TIMEOUT : EXECUTOR_EXIT;
					dprintf(outputs[_F_JOURNAL], "%s%d (%.3fs)\n",
						exitline,
						status, time);
					if (settings->sync) {
						fdatasync(outputs[_F_JOURNAL]);
					}
				}

				if (status == IGT_EXIT_ABORT) {
					errf("Test exited with IGT_EXIT_ABORT, aborting.\n");
					aborting = true;
					*abortreason = strdup("Test exited with IGT_EXIT_ABORT");
				}

				if (time_spent)
					*time_spent = time;
			}

			child = 0;
			sigfd = -1; /* we are dying, no signal handling for now */
		}

		timeout_reason = need_to_timeout(settings, killed,
						 igt_kernel_tainted(&taints),
						 igt_time_elapsed(&time_last_activity, &time_now),
						 igt_time_elapsed(&time_last_subtest, &time_now),
						 igt_time_elapsed(&time_killed, &time_now),
						 disk_usage);

		if (timeout_reason) {
			if (killed == SIGKILL) {
				/* Nothing that can be done, really. Let's tell the caller we want to abort. */

				if (settings->log_level >= LOG_LEVEL_NORMAL) {
					errf("Child refuses to die, tainted 0x%lx. Aborting.\n",
					     taints);
					if (kill(child, 0) && errno == ESRCH)
						errf("The test process no longer exists, "
						     "but we didn't get informed of its demise...\n");
					asprintf(abortreason, "Child refuses to die, tainted 0x%lx.", taints);
				}

				dump_dmesg(kmsgfd, outputs[_F_DMESG]);
				if (settings->sync)
					fdatasync(outputs[_F_DMESG]);

				close_watchdogs(settings);
				free(buf);
				free(outbuf);
				close(outfd);
				close(errfd);
				close(socketfd);
				close(kmsgfd);
				return -1;
			}

			if (settings->log_level >= LOG_LEVEL_NORMAL) {
				outf("%s", timeout_reason);
				fflush(stdout);
			}

			killed = next_kill_signal(killed);
			if (!kill_child(killed, child))
				return -1;
			time_killed = time_now;
		}
	}

	dump_dmesg(kmsgfd, outputs[_F_DMESG]);
	if (settings->sync)
		fdatasync(outputs[_F_DMESG]);

	free(buf);
	free(outbuf);
	close(outfd);
	close(errfd);
	close(socketfd);
	close(kmsgfd);

	if (aborting)
		return -1;

	return killed;
}

static void __attribute__((noreturn))
execute_test_process(int outfd, int errfd, int socketfd,
		     struct settings *settings,
		     struct job_list_entry *entry)
{
	char *argv[6] = {};
	size_t rootlen;

	dup2(outfd, STDOUT_FILENO);
	dup2(errfd, STDERR_FILENO);

	setpgid(0, 0);

	rootlen = strlen(settings->test_root);
	argv[0] = malloc(rootlen + strlen(entry->binary) + 2);
	strcpy(argv[0], settings->test_root);
	argv[0][rootlen] = '/';
	strcpy(argv[0] + rootlen + 1, entry->binary);

	if (entry->subtest_count) {
		size_t argsize;
		const char *dynbegin;
		size_t i;

		argv[1] = strdup("--run-subtest");

		if ((dynbegin = strchr(entry->subtests[0], '@')) != NULL)
			argsize = dynbegin - entry->subtests[0];
		else
			argsize = strlen(entry->subtests[0]);

		argv[2] = malloc(argsize + 1);
		memcpy(argv[2], entry->subtests[0], argsize);
		argv[2][argsize] = '\0';

		if (dynbegin) {
			argv[3] = strdup("--dynamic-subtest");
			argv[4] = strdup(dynbegin + 1);
		}

		for (i = 1; i < entry->subtest_count; i++) {
			char *sub = entry->subtests[i];
			size_t sublen = strlen(sub);

			assert(dynbegin == NULL);

			argv[2] = realloc(argv[2], argsize + sublen + 2);
			argv[2][argsize] = ',';
			strcpy(argv[2] + argsize + 1, sub);
			argsize += sublen + 1;
		}
	}

	if (socketfd >= 0) {
		struct runnerpacket *packet;

		packet = runnerpacket_exec(argv);
		write(socketfd, packet, packet->size);
	}

	execv(argv[0], argv);
	fprintf(stderr, "Cannot execute %s\n", argv[0]);
	exit(IGT_EXIT_INVALID);
}

static int digits(size_t num)
{
	int ret = 0;
	while (num) {
		num /= 10;
		ret++;
	}

	if (ret == 0) ret++;
	return ret;
}

static int print_time_left(struct execute_state *state,
			   struct settings *settings,
			   char *buf, int rem)
{
	int width;

	if (settings->overall_timeout <= 0)
		return 0;

	width = digits(settings->overall_timeout);
	return snprintf(buf, rem, "(%*.0fs left) ", width, state->time_left);
}

static char *entry_display_name(struct job_list_entry *entry)
{
	size_t size = strlen(entry->binary) + 1;
	char *ret = malloc(size);

	sprintf(ret, "%s", entry->binary);

	if (entry->subtest_count > 0) {
		size_t i;
		const char *delim = "";

		size += 3; /* strlen(" (") + strlen(")") */
		ret = realloc(ret, size);
		strcat(ret, " (");

		for (i = 0; i < entry->subtest_count; i++) {
			size += strlen(delim) + strlen(entry->subtests[i]);
			ret = realloc(ret, size);

			strcat(ret, delim);
			strcat(ret, entry->subtests[i]);

			delim = ", ";
		}
		/* There's already room for this */
		strcat(ret, ")");
	}

	return ret;
}

/*
 * Returns:
 *  =0 - Success
 *  <0 - Failure executing
 *  >0 - Timeout happened, need to recreate from journal
 */
static int execute_next_entry(struct execute_state *state,
			      size_t total,
			      double *time_spent,
			      struct settings *settings,
			      struct job_list_entry *entry,
			      int testdirfd, int resdirfd,
			      int sigfd, sigset_t *sigmask,
			      char **abortreason)
{
	int dirfd;
	int outputs[_F_LAST];
	int kmsgfd;
	int outpipe[2] = { -1, -1 };
	int errpipe[2] = { -1, -1 };
	int socket[2] = { -1, -1 };
	int outfd, errfd, socketfd;
	char name[32];
	pid_t child;
	int result;
	size_t idx = state->next;

	snprintf(name, sizeof(name), "%zd", idx);
	mkdirat(resdirfd, name, 0777);
	if ((dirfd = openat(resdirfd, name, O_DIRECTORY | O_RDONLY | O_CLOEXEC)) < 0) {
		errf("Error accessing individual test result directory\n");
		return -1;
	}

	if (!open_output_files(dirfd, outputs, true)) {
		errf("Error opening output files\n");
		result = -1;
		goto out_dirfd;
	}

	if (settings->sync) {
		fsync(dirfd);
		fsync(resdirfd);
	}

	if (pipe(outpipe) || pipe(errpipe)) {
		errf("Error creating pipes: %m\n");
		result = -1;
		goto out_pipe;
	}

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, socket)) {
		errf("Error creating sockets: %m\n");
		result = -1;
		goto out_pipe;
	}

	if ((kmsgfd = open("/dev/kmsg", O_RDONLY | O_CLOEXEC | O_NONBLOCK)) < 0) {
		errf("Warning: Cannot open /dev/kmsg\n");
	} else {
		/* TODO: Checking of abort conditions in pre-execute dmesg */
		lseek(kmsgfd, 0, SEEK_END);
	}


	if (settings->log_level >= LOG_LEVEL_NORMAL) {
		char buf[100];
		char *displayname;
		int width = digits(total);
		int len;

		len = snprintf(buf, sizeof(buf),
			       "[%0*zd/%0*zd] ", width, idx + 1, width, total);

		len += print_time_left(state, settings,
				       buf + len, sizeof(buf) - len);

		displayname = entry_display_name(entry);
		len += snprintf(buf + len, sizeof(buf) - len, "%s", displayname);
		free(displayname);

		outf("%s\n", buf);
	}

	/*
	 * Flush outputs before forking so our (buffered) output won't
	 * end up in the test outputs.
	 */
	fflush(stdout);
	fflush(stderr);

	child = fork();
	if (child < 0) {
		errf("Failed to fork: %m\n");
		result = -1;
		goto out_kmsgfd;
	} else if (child == 0) {
		char envstring[16];

		outfd = outpipe[1];
		errfd = errpipe[1];
		socketfd = socket[1];
		close(outpipe[0]);
		close(errpipe[0]);
		close(socket[0]);

		sigprocmask(SIG_UNBLOCK, sigmask, NULL);

		if (socketfd >= 0 && !getenv("IGT_RUNNER_DISABLE_SOCKET_COMMUNICATION")) {
			snprintf(envstring, sizeof(envstring), "%d", socketfd);
			setenv("IGT_RUNNER_SOCKET_FD", envstring, 1);
		}
		setenv("IGT_SENTINEL_ON_STDERR", "1", 1);

		execute_test_process(outfd, errfd, socketfd, settings, entry);
		/* unreachable */
	}

	outfd = outpipe[0];
	errfd = errpipe[0];
	socketfd = socket[0];
	close(outpipe[1]);
	close(errpipe[1]);
	close(socket[1]);
	outpipe[1] = errpipe[1] = socket[1] = -1;

	result = monitor_output(child, outfd, errfd, socketfd,
				kmsgfd, sigfd,
				outputs, time_spent, settings,
				abortreason);

out_kmsgfd:
	close(kmsgfd);
out_pipe:
	close_outputs(outputs);
	close(outpipe[0]);
	close(outpipe[1]);
	close(errpipe[0]);
	close(errpipe[1]);
	close_outputs(outputs);
out_dirfd:
	close(dirfd);

	return result;
}

static int remove_file(int dirfd, const char *name)
{
	return unlinkat(dirfd, name, 0) && errno != ENOENT;
}

static bool clear_test_result_directory(int dirfd)
{
	int i;

	for (i = 0; i < _F_LAST; i++) {
		if (remove_file(dirfd, filenames[i])) {
			errf("Error deleting %s from test result directory: %m\n",
			     filenames[i]);
			return false;
		}
	}

	return true;
}

static bool clear_old_results(char *path)
{
	struct dirent *entry;
	char name[PATH_MAX];
	int dirfd;
	size_t i;
	DIR *dir;

	if ((dirfd = open(path, O_DIRECTORY | O_RDONLY)) < 0) {
		if (errno == ENOENT) {
			/* Successfully cleared if it doesn't even exist */
			return true;
		}

		errf("Error clearing old results: %m\n");
		return false;
	}

	if (remove_file(dirfd, "uname.txt") ||
	    remove_file(dirfd, "starttime.txt") ||
	    remove_file(dirfd, "endtime.txt") ||
	    remove_file(dirfd, "aborted.txt")) {
		close(dirfd);
		errf("Error clearing old results: %m\n");
		return false;
	}

	for (i = 0; true; i++) {
		int resdirfd;

		snprintf(name, sizeof(name), "%zd", i);
		if ((resdirfd = openat(dirfd, name, O_DIRECTORY | O_RDONLY)) < 0)
			break;

		if (!clear_test_result_directory(resdirfd)) {
			close(resdirfd);
			close(dirfd);
			return false;
		}
		close(resdirfd);
		if (unlinkat(dirfd, name, AT_REMOVEDIR)) {
			errf("Warning: Result directory %s contains extra files\n",
			     name);
		}
	}

	strcpy(name, path);
	strcat(name, "/" CODE_COV_RESULTS_PATH);
	if ((dir = opendir(name)) != NULL) {
		char *p;

		strcat(name, "/");
		p = name + strlen(name);

		while ((entry = readdir(dir)) != NULL) {
			if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
				continue;

			strcpy(p, entry->d_name);
			if (unlink(name))  {
				errf("Error removing %s\n", name);
			}
		}

		closedir(dir);
		if (unlinkat(dirfd, CODE_COV_RESULTS_PATH, AT_REMOVEDIR)) {
			errf("Warning: Result directory %s/%s contains extra files\n",
			     path, CODE_COV_RESULTS_PATH);
		}
	}

	close(dirfd);

	return true;
}

static double timeofday_double(void)
{
	struct timeval tv;

	if (!gettimeofday(&tv, NULL))
		return tv.tv_sec + tv.tv_usec / 1000000.0;
	return 0.0;
}

static void init_time_left(struct execute_state *state,
			   struct settings *settings)
{
	if (settings->overall_timeout <= 0)
		state->time_left = -1;
	else
		state->time_left = settings->overall_timeout;
}

bool initialize_execute_state_from_resume(int dirfd,
					  struct execute_state *state,
					  struct settings *settings,
					  struct job_list *list)
{
	struct job_list_entry *entry;
	int resdirfd, fd, i;

	clear_settings(settings);
	free_job_list(list);
	memset(state, 0, sizeof(*state));
	state->resuming = true;

	if (!read_settings_from_dir(settings, dirfd) ||
	    !read_job_list(list, dirfd)) {
		close(dirfd);
		fprintf(stderr, "Failure reading metadata\n");
		return false;
	}

	if (!settings->allow_non_root && (getuid() != 0)) {
		fprintf(stderr, "Runner needs to run with UID 0 (root).\n");
		return false;
	}

	init_time_left(state, settings);

	for (i = list->size; i >= 0; i--) {
		char name[32];

		snprintf(name, sizeof(name), "%d", i);
		if ((resdirfd = openat(dirfd, name, O_DIRECTORY | O_RDONLY)) >= 0)
			break;
	}

	if (i < 0)
		/* Nothing has been executed yet, state is fine as is */
		goto success;

	entry = &list->entries[i];
	state->next = i;

	if ((fd = openat(resdirfd, filenames[_F_SOCKET], O_RDONLY)) >= 0) {
		if (!prune_from_comms(entry, fd)) {
			/*
			 * No subtests, or incomplete before the first
			 * subtest. Not suitable to re-run.
			 */
			state->next = i + 1;
		} else if (entry->binary[0] == '\0') {
			/* Full completed */
			state->next = i + 1;
		}

		close (fd);
	}

	if ((fd = openat(resdirfd, filenames[_F_JOURNAL], O_RDONLY)) >= 0) {
		if (!prune_from_journal(entry, fd)) {
			/*
			 * The test does not have subtests, or
			 * incompleted before the first subtest
			 * began. Either way, not suitable to
			 * re-run.
			 */
			state->next = i + 1;
		} else if (entry->binary[0] == '\0') {
			/* This test is fully completed */
			state->next = i + 1;
		}

		close(fd);
	}

 success:
	close(resdirfd);
	close(dirfd);

	return true;
}

bool initialize_execute_state(struct execute_state *state,
			      struct settings *settings,
			      struct job_list *job_list)
{
	if (!settings->allow_non_root && (getuid() != 0)) {
		fprintf(stderr, "Runner needs to run with UID 0 (root).\n");
		return false;
	}

	memset(state, 0, sizeof(*state));

	if (!validate_settings(settings))
		return false;

	if (!serialize_settings(settings) ||
	    !serialize_job_list(job_list, settings))
		return false;

	if (settings->overwrite &&
	    !clear_old_results(settings->results_path))
		return false;

	init_time_left(state, settings);

	state->dry = settings->dry_run;

	return true;
}

static void reduce_time_left(struct settings *settings,
			     struct execute_state *state,
			     double time_spent)
{
	if (state->time_left < 0)
		return;

	if (time_spent > state->time_left)
		state->time_left = 0.0;
	else
		state->time_left -= time_spent;
}

static bool overall_timeout_exceeded(struct execute_state *state)
{
	return state->time_left == 0.0;
}

static void write_abort_file(int resdirfd,
			     const char *reason,
			     const char *testbefore,
			     const char *testafter)
{
	int abortfd;

	if ((abortfd = openat(resdirfd, "aborted.txt", O_CREAT | O_WRONLY | O_EXCL, 0666)) >= 0) {
		/*
		 * Ignore failure to open, there's
		 * already an abort probably (if this
		 * is a resume)
		 */
		dprintf(abortfd, "Aborting.\n");
		dprintf(abortfd, "Previous test: %s\n", testbefore);
		dprintf(abortfd, "Next test: %s\n\n", testafter);
		write(abortfd, reason, strlen(reason));
		close(abortfd);
	}
}

static void oom_immortal(void)
{
	int fd;
	const char never_kill[] = "-1000";

	fd = open("/proc/self/oom_score_adj", O_WRONLY);
	if (fd < 0) {
		errf("Warning: Cannot adjust oom score.\n");
		return;
	}
	if (write(fd, never_kill, sizeof(never_kill)) != sizeof(never_kill))
		errf("Warning: Adjusting oom score failed.\n");

	close(fd);
}

static bool should_die_because_signal(int sigfd)
{
	struct signalfd_siginfo siginfo;
	int ret;
	struct pollfd sigpoll = { .fd = sigfd, .events = POLLIN | POLLRDBAND };

	ret = poll(&sigpoll, 1, 0);

	if (ret != 0) {
		if (ret == -1) {
			errf("Poll on signalfd failed with %m\n");
			return true; /* something is wrong, let's die */
		}

		ret = read(sigfd, &siginfo, sizeof(siginfo));

		if (ret == -1) {
			errf("Error reading from signalfd: %m\n");
			return false; /* we may want to retry later */
		}

		if (siginfo.ssi_signo == SIGCHLD) {
			errf("Runner got stray SIGCHLD while not executing any tests.\n");
		} else {
			errf("Runner is being killed by %s\n",
			     strsignal(siginfo.ssi_signo));
			return true;
		}

	}

	return false;
}

static char *code_coverage_name(struct settings *settings)
{
	const char *start, *end, *fname;
	char *name;
	int size;

	if (settings->name && *settings->name)
		return settings->name;
	else if (!settings->test_list)
		return NULL;

	/* Use only the base of the test_list, without path and extension */
	fname = settings->test_list;

	start = strrchr(fname,'/');
	if (!start)
		start = fname;

	end = strrchr(start, '.');
	if (end)
		size = end - start;
	else
		size = strlen(start);

	name = malloc(size + 1);
	strncpy(name, fname, size);
	name[size]  = '\0';

	return name;
}

static void run_as_root(char * const argv[], int sigfd, char **abortreason)
{
	struct signalfd_siginfo siginfo;
	int status = 0, ret;
	pid_t child;

	child = fork();
	if (child < 0) {
		*abortreason = strdup("Failed to fork");
		return;
	}

	if (child == 0) {
		execv(argv[0], argv);
		perror (argv[0]);
		exit(IGT_EXIT_INVALID);
	}

	if (sigfd >= 0) {
		while (1) {
			ret = read(sigfd, &siginfo, sizeof(siginfo));
			if (ret < 0) {
				errf("Error reading from signalfd: %m\n");
				continue;
			} else if (siginfo.ssi_signo == SIGCHLD) {
				if (child != waitpid(child, &status, WNOHANG)) {
					errf("Failed to reap child\n");
					status = 9999;
					continue;
				}
				break;
			}
		}
	} else {
		waitpid(child, &status, 0);
	}

	if (WIFSIGNALED(status))
		asprintf(abortreason, "%s received signal %d while running\n",argv[0], WTERMSIG(status));
	else if (!WIFEXITED(status))
		asprintf(abortreason, "%s aborted with unknown status\n", argv[0]);
	else if (WEXITSTATUS(status))
		asprintf(abortreason, "%s returned error %d\n", argv[0], WEXITSTATUS(status));
}

static void code_coverage_start(struct settings *settings, int sigfd, char **abortreason)
{
	int fd;

	fd = open(GCOV_RESET, O_WRONLY);
	if (fd < 0) {
		asprintf(abortreason, "Failed to open %s", GCOV_RESET);
		return;
	}
	if (write(fd, "0\n", 2) < 0)
		*abortreason = strdup("Failed to reset gcov counters");

	close(fd);
}

static void code_coverage_stop(struct settings *settings, const char *job_name,
			       int sigfd, char **abortreason)
{
	int i, j = 0, last_was_escaped = 1;
	char fname[PATH_MAX];
	char name[PATH_MAX];
	char *argv[3] = {};

	/* If name is empty, use a default */
	if (!job_name || !*job_name)
		job_name = "code_coverage";

	/*
	 * Use only letters, numbers and '_'
	 *
	 * This way, the tarball name can be used as testname when lcov runs
	 */
	for (i = 0; i < strlen(job_name); i++) {
		if (!isalpha(job_name[i]) && !isalnum(job_name[i])) {
			if (last_was_escaped)
				continue;
			name[j++] = '_';
			last_was_escaped = 1;
		} else {
			name[j++] = job_name[i];
			last_was_escaped = 0;
		}
	}
	if (j && last_was_escaped)
		j--;
	name[j] = '\0';

	strcpy(fname, settings->results_path);
	strcat(fname, "/" CODE_COV_RESULTS_PATH "/");
	strcat(fname, name);

	argv[0] = settings->code_coverage_script;
	argv[1] = fname;

	outf("Storing code coverage results...\n");
	run_as_root(argv, sigfd, abortreason);
}

bool execute(struct execute_state *state,
	     struct settings *settings,
	     struct job_list *job_list)
{
	int resdirfd, testdirfd, unamefd, timefd, sigfd;
	struct environment_variable *env_var;
	struct utsname unamebuf;
	sigset_t sigmask;
	double time_spent = 0.0;
	bool status = true;

	if (state->dry) {
		outf("Dry run, not executing. Invoke igt_resume if you want to execute.\n");
		return true;
	}

	igt_list_for_each_entry(env_var, &settings->env_vars, link) {
		setenv(env_var->key, env_var->value, 1);
	}

	if ((resdirfd = open(settings->results_path, O_DIRECTORY | O_RDONLY)) < 0) {
		/* Initialize state should have done this */
		errf("Error: Failure opening results path %s\n",
		     settings->results_path);
		return false;
	}

	if (settings->enable_code_coverage) {
		if (!settings->cov_results_per_test) {
			char *reason = NULL;

			code_coverage_start(settings, -1, &reason);
			if (reason != NULL) {
				errf("%s\n", reason);
				free(reason);
				close(resdirfd);
				return false;
			}
		}

		mkdirat(resdirfd, CODE_COV_RESULTS_PATH, 0755);
	}

	if ((testdirfd = open(settings->test_root, O_DIRECTORY | O_RDONLY)) < 0) {
		errf("Error: Failure opening test root %s\n",
		     settings->test_root);
		close(resdirfd);
		return false;
	}

	/* TODO: On resume, don't rewrite, verify that content matches current instead */
	if ((unamefd = openat(resdirfd, "uname.txt", O_CREAT | O_WRONLY | O_TRUNC, 0666)) < 0) {
		errf("Error: Failure opening uname.txt: %m\n");
		close(testdirfd);
		close(resdirfd);
		return false;
	}

	if ((timefd = openat(resdirfd, "starttime.txt", O_CREAT | O_WRONLY | O_EXCL, 0666)) >= 0) {
		/*
		 * Ignore failure to open. If this is a resume, we
		 * don't want to overwrite. For other errors, we
		 * ignore the start time.
		 */
		dprintf(timefd, "%f\n", timeofday_double());
		close(timefd);
	}

	oom_immortal();

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGCHLD);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGTERM);
	sigaddset(&sigmask, SIGQUIT);
	sigaddset(&sigmask, SIGHUP);
	sigfd = signalfd(-1, &sigmask, O_CLOEXEC);
	sigprocmask(SIG_BLOCK, &sigmask, NULL);

	if (sigfd < 0) {
		/* TODO: Handle better */
		errf("Cannot mask signals\n");
		status = false;
		goto end;
	}

	init_watchdogs(settings);

	if (settings->abort_mask & ABORT_PING)
		ping_config();

	if (!uname(&unamebuf)) {
		dprintf(unamefd, "%s %s %s %s %s\n",
			unamebuf.sysname,
			unamebuf.nodename,
			unamebuf.release,
			unamebuf.version,
			unamebuf.machine);
	} else {
		dprintf(unamefd, "uname() failed\n");
	}
	close(unamefd);

	/* Check if we're already in abort-state at bootup */
	if (!state->resuming) {
		char *reason;

		if ((reason = need_to_abort(settings)) != NULL) {
			char *nexttest = entry_display_name(&job_list->entries[state->next]);
			write_abort_file(resdirfd, reason, "nothing", nexttest);
			free(reason);
			free(nexttest);

			status = false;

			goto end;
		}
	}

	for (; state->next < job_list->size;
	     state->next++) {
		char *reason = NULL;
		char *job_name;
		int result;

		if (should_die_because_signal(sigfd)) {
			status = false;
			goto end;
		}

		if (settings->cov_results_per_test) {
			code_coverage_start(settings, sigfd, &reason);
			job_name = entry_display_name(&job_list->entries[state->next]);
		}

		if (reason == NULL) {
			result = execute_next_entry(state,
						job_list->size,
						&time_spent,
						settings,
						&job_list->entries[state->next],
						testdirfd, resdirfd,
						sigfd, &sigmask,
						&reason);

			if (settings->cov_results_per_test) {
				code_coverage_stop(settings, job_name, sigfd, &reason);
				free(job_name);
			}
		}

		if (reason != NULL || (reason = need_to_abort(settings)) != NULL) {
			char *prev = entry_display_name(&job_list->entries[state->next]);
			char *next = (state->next + 1 < job_list->size ?
				      entry_display_name(&job_list->entries[state->next + 1]) :
				      strdup("nothing"));
			write_abort_file(resdirfd, reason, prev, next);
			free(prev);
			free(next);
			free(reason);
			status = false;
			break;
		}

		if (result < 0) {
			status = false;
			break;
		}

		reduce_time_left(settings, state, time_spent);

		if (overall_timeout_exceeded(state)) {
			if (settings->log_level >= LOG_LEVEL_NORMAL) {
				outf("Overall timeout time exceeded, stopping.\n");
			}

			break;
		}

		if (result > 0) {
			double time_left = state->time_left;

			close_watchdogs(settings);
			sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
			/* make sure that we do not leave any signals unhandled */
			if (should_die_because_signal(sigfd)) {
				status = false;
				goto end_post_signal_restore;
			}
			close(sigfd);
			close(testdirfd);
			if (!initialize_execute_state_from_resume(resdirfd, state, settings, job_list))
				return false;
			state->time_left = time_left;
			return execute(state, settings, job_list);
		}
	}

	if ((timefd = openat(resdirfd, "endtime.txt", O_CREAT | O_WRONLY | O_EXCL, 0666)) >= 0) {
		dprintf(timefd, "%f\n", timeofday_double());
		close(timefd);
	}

 end:
	if (settings->enable_code_coverage && !settings->cov_results_per_test) {
		char *reason = NULL;

		code_coverage_stop(settings, code_coverage_name(settings), -1, &reason);
		if (reason != NULL) {
			errf("%s\n", reason);
			free(reason);
			status = false;
		}
	}

	close_watchdogs(settings);
	sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
	/* make sure that we do not leave any signals unhandled */
	if (should_die_because_signal(sigfd))
		status = false;
 end_post_signal_restore:
	close(sigfd);
	close(testdirfd);
	close(resdirfd);
	return status;
}
