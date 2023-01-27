/*
 * Copyright © 2007, 2011, 2013, 2014, 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *    Daniel Vetter <daniel.vetter@ffwll.ch>
 *
 */

#ifdef HAVE_LIBGEN_H
#include <libgen.h>
#endif
#include <stdio.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <pciaccess.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <termios.h>
#include <assert.h>
#include <grp.h>

#include <proc/readproc.h>
#include <libudev.h>

#include "drmtest.h"
#include "i915_drm.h"
#include "intel_chipset.h"
#include "igt_aux.h"
#include "igt_debugfs.h"
#include "igt_gt.h"
#include "igt_params.h"
#include "igt_rand.h"
#include "igt_sysfs.h"
#include "config.h"
#include "intel_reg.h"
#include "ioctl_wrappers.h"
#include "igt_kms.h"
#include "igt_stats.h"
#include "igt_sysfs.h"

#ifdef HAVE_LIBGEN_H
#include <libgen.h>   /* for dirname() */
#endif

/**
 * SECTION:igt_aux
 * @short_description: Auxiliary libraries and support functions
 * @title: aux
 * @include: igt.h
 *
 * This library provides various auxiliary helper functions that don't really
 * fit into any other topic.
 */

static struct __igt_sigiter_global {
	pid_t tid;
	timer_t timer;
	struct timespec offset;
	struct {
		long hit, miss;
		long ioctls, signals;
	} stat;
} __igt_sigiter;

static void sigiter(int sig, siginfo_t *info, void *arg)
{
	__igt_sigiter.stat.signals++;
}

#if 0
#define SIG_ASSERT(expr) igt_assert(expr)
#else
#define SIG_ASSERT(expr)
#endif

static int
sig_ioctl(int fd, unsigned long request, void *arg)
{
	struct itimerspec its;
	int ret;

	SIG_ASSERT(__igt_sigiter.timer);
	SIG_ASSERT(__igt_sigiter.tid == gettid());

	memset(&its, 0, sizeof(its));
	if (timer_settime(__igt_sigiter.timer, 0, &its, NULL)) {
		/* oops, we didn't undo the interrupter (i.e. !unwound abort) */
		igt_ioctl = drmIoctl;
		return drmIoctl(fd, request, arg);
	}

	its.it_value = __igt_sigiter.offset;
	do {
		long serial;

		__igt_sigiter.stat.ioctls++;

		ret = 0;
		serial = __igt_sigiter.stat.signals;
		igt_assert(timer_settime(__igt_sigiter.timer, 0, &its, NULL) == 0);
		if (ioctl(fd, request, arg))
			ret = errno;
		if (__igt_sigiter.stat.signals == serial)
			__igt_sigiter.stat.miss++;
		if (ret == 0)
			break;

		if (ret == EINTR) {
			__igt_sigiter.stat.hit++;

			its.it_value.tv_sec *= 2;
			its.it_value.tv_nsec *= 2;
			while (its.it_value.tv_nsec >= NSEC_PER_SEC) {
				its.it_value.tv_nsec -= NSEC_PER_SEC;
				its.it_value.tv_sec += 1;
			}

			SIG_ASSERT(its.it_value.tv_nsec >= 0);
			SIG_ASSERT(its.it_value.tv_sec >= 0);
		}
	} while (ret == EAGAIN || ret == EINTR);

	memset(&its, 0, sizeof(its));
	timer_settime(__igt_sigiter.timer, 0, &its, NULL);

	errno = ret;
	return ret ? -1 : 0;
}

static bool igt_sigiter_start(struct __igt_sigiter *iter, bool enable)
{
	/* Note that until we can automatically clean up on failed/skipped
	 * tests, we cannot assume the state of the igt_ioctl indirection.
	 */
	SIG_ASSERT(igt_ioctl == drmIoctl);
	igt_ioctl = drmIoctl;

	if (enable) {
		struct timespec start, end;
		struct sigevent sev;
		struct sigaction act;
		struct itimerspec its;

		igt_ioctl = sig_ioctl;
		__igt_sigiter.tid = gettid();

		memset(&sev, 0, sizeof(sev));
		sev.sigev_notify = SIGEV_SIGNAL | SIGEV_THREAD_ID;
		sev.sigev_notify_thread_id = __igt_sigiter.tid;
		sev.sigev_signo = SIGRTMIN;
		igt_assert(timer_create(CLOCK_MONOTONIC, &sev, &__igt_sigiter.timer) == 0);

		memset(&its, 0, sizeof(its));
		igt_assert(timer_settime(__igt_sigiter.timer, 0, &its, NULL) == 0);

		memset(&act, 0, sizeof(act));
		act.sa_sigaction = sigiter;
		act.sa_flags = SA_SIGINFO;
		igt_assert(sigaction(SIGRTMIN, &act, NULL) == 0);

		/* Try to find the approximate delay required to skip over
		 * the timer_setttime and into the following ioctl() to try
		 * and avoid the timer firing before we enter the drmIoctl.
		 */
		igt_assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
		igt_assert(timer_settime(__igt_sigiter.timer, 0, &its, NULL) == 0);
		igt_assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);

		__igt_sigiter.offset.tv_sec = end.tv_sec - start.tv_sec;
		__igt_sigiter.offset.tv_nsec = end.tv_nsec - start.tv_nsec;
		if (__igt_sigiter.offset.tv_nsec < 0) {
			__igt_sigiter.offset.tv_nsec += NSEC_PER_SEC;
			__igt_sigiter.offset.tv_sec -= 1;
		}
		if (__igt_sigiter.offset.tv_sec < 0) {
			__igt_sigiter.offset.tv_nsec = 0;
			__igt_sigiter.offset.tv_sec = 0;
		}
		igt_assert(__igt_sigiter.offset.tv_sec == 0);

		igt_debug("Initial delay for interruption: %ld.%09lds\n",
			  __igt_sigiter.offset.tv_sec,
			  __igt_sigiter.offset.tv_nsec);
	}

	return true;
}

static bool igt_sigiter_stop(struct __igt_sigiter *iter, bool enable)
{
	if (enable) {
		struct sigaction act;

		SIG_ASSERT(igt_ioctl == sig_ioctl);
		SIG_ASSERT(__igt_sigiter.tid == gettid());
		igt_ioctl = drmIoctl;

		timer_delete(__igt_sigiter.timer);

		memset(&act, 0, sizeof(act));
		act.sa_handler = SIG_IGN;
		sigaction(SIGRTMIN, &act, NULL);

		memset(&__igt_sigiter, 0, sizeof(__igt_sigiter));
	}

	memset(iter, 0, sizeof(*iter));
	return false;
}

bool __igt_sigiter_continue(struct __igt_sigiter *iter, bool enable)
{
	if (iter->pass++ == 0)
		return igt_sigiter_start(iter, enable);

	/* If nothing reported SIGINT, nothing will on the next pass, so
	 * give up! Also give up if everything is now executing faster
	 * than current sigtimer.
	 */
	if (__igt_sigiter.stat.hit == 0 ||
	    __igt_sigiter.stat.miss == __igt_sigiter.stat.ioctls)
		return igt_sigiter_stop(iter, enable);

	igt_debug("%s: pass %d, missed %ld/%ld\n",
		  __func__, iter->pass - 1,
		  __igt_sigiter.stat.miss,
		  __igt_sigiter.stat.ioctls);

	SIG_ASSERT(igt_ioctl == sig_ioctl);
	SIG_ASSERT(__igt_sigiter.timer);

	__igt_sigiter.offset.tv_sec *= 2;
	__igt_sigiter.offset.tv_nsec *= 2;
	while (__igt_sigiter.offset.tv_nsec >= NSEC_PER_SEC) {
		__igt_sigiter.offset.tv_nsec -= NSEC_PER_SEC;
		__igt_sigiter.offset.tv_sec += 1;
	}
	SIG_ASSERT(__igt_sigiter.offset.tv_nsec >= 0);
	SIG_ASSERT(__igt_sigiter.offset.tv_sec >= 0);

	memset(&__igt_sigiter.stat, 0, sizeof(__igt_sigiter.stat));
	return true;
}

static struct igt_helper_process signal_helper;
long long int sig_stat;
__noreturn static void signal_helper_process(pid_t pid)
{
	/* Interrupt the parent process at 500Hz, just to be annoying */
	while (1) {
		usleep(1000 * 1000 / 500);
		if (kill(pid, SIGCONT)) /* Parent has died, so must we. */
			exit(0);
	}
}

static void sig_handler(int i)
{
	sig_stat++;
}

/**
 * igt_fork_signal_helper:
 *
 * Fork a child process using #igt_fork_helper to interrupt the parent process
 * with a SIGCONT signal at regular quick intervals. The corresponding dummy
 * signal handler is installed in the parent process.
 *
 * This is useful to exercise ioctl error paths, at least where those can be
 * exercises by interrupting blocking waits, like stalling for the gpu. This
 * helper can also be used from children spawned with #igt_fork.
 *
 * In tests with subtests this function can be called outside of failure
 * catching code blocks like #igt_fixture or #igt_subtest.
 *
 * Note that this just spews signals at the current process unconditionally and
 * hence incurs quite a bit of overhead. For a more focused approach, with less
 * overhead, look at the #igt_while_interruptible code block macro.
 */
void igt_fork_signal_helper(void)
{
	if (igt_only_list_subtests())
		return;

	/* We pick SIGCONT as it is a "safe" signal - if we send SIGCONT to
	 * an unexpecting process it spuriously wakes up and does nothing.
	 * Most other signals (e.g. SIGUSR1) cause the process to die if they
	 * are not handled. This is an issue in case the sighandler is not
	 * inherited correctly (or if there is a race in the inheritance
	 * and we send the signal at exactly the wrong time).
	 */
	signal(SIGCONT, sig_handler);
	setpgid(0, 0); /* define a new process group for the tests */

	igt_fork_helper(&signal_helper) {
		setpgid(0, 0); /* Escape from the test process group */

		/* Pass along the test process group identifier,
		 * negative pid => send signal to everyone in the group.
		 */
		signal_helper_process(-getppid());
	}
}

/**
 * igt_stop_signal_helper:
 *
 * Stops the child process spawned with igt_fork_signal_helper() again.
 *
 * In tests with subtests this function can be called outside of failure
 * catching code blocks like #igt_fixture or #igt_subtest.
 */
void igt_stop_signal_helper(void)
{
	if (igt_only_list_subtests())
		return;

	igt_stop_helper(&signal_helper);

	sig_stat = 0;
}

/**
 * igt_suspend_signal_helper:
 *
 * Suspends the child process spawned with igt_fork_signal_helper(). This
 * should be called before a critical section of code that has difficulty to
 * make progress if interrupted frequently, like the clone() syscall called
 * from a largish executable. igt_resume_signal_helper() must be called after
 * the critical section to restart interruptions for the test.
 */
void igt_suspend_signal_helper(void)
{
	int status;

	if (!signal_helper.running)
		return;

	kill(signal_helper.pid, SIGSTOP);
	while (waitpid(signal_helper.pid, &status, WUNTRACED) == -1 &&
	       errno == EINTR)
		;
}

/**
 * igt_resume_signal_helper:
 *
 * Resumes the child process spawned with igt_fork_signal_helper().
 *
 * This should be paired with igt_suspend_signal_helper() and called after the
 * problematic code sensitive to signals.
 */
void igt_resume_signal_helper(void)
{
	if (!signal_helper.running)
		return;

	kill(signal_helper.pid, SIGCONT);
}

static struct igt_helper_process shrink_helper;
__noreturn static void shrink_helper_process(int fd, pid_t pid)
{
	while (1) {
		igt_drop_caches_set(fd, DROP_SHRINK_ALL);
		usleep(1000 * 1000 / 50);
		if (kill(pid, 0)) /* Parent has died, so must we. */
			exit(0);
	}
}

/**
 * igt_fork_shrink_helper:
 *
 * Fork a child process using #igt_fork_helper to force all available objects
 * to be paged out (via i915_gem_shrink()).
 *
 * This is useful to exercise swapping paths, without requiring us to hit swap.
 *
 * This should only be used from an igt_fixture.
 */
void igt_fork_shrink_helper(int drm_fd)
{
	assert(!igt_only_list_subtests());
	igt_require(igt_drop_caches_has(drm_fd, DROP_SHRINK_ALL));
	igt_fork_helper(&shrink_helper)
		shrink_helper_process(drm_fd, getppid());
}

/**
 * igt_stop_shrink_helper:
 *
 * Stops the child process spawned with igt_fork_shrink_helper().
 */
void igt_stop_shrink_helper(void)
{
	igt_stop_helper(&shrink_helper);
}

static void show_kernel_stack(pid_t pid)
{
	char buf[80], *str;
	int dir;

	snprintf(buf, sizeof(buf), "/proc/%d", pid);
	dir = open(buf, O_RDONLY);
	if (dir < 0)
		return;

	str = igt_sysfs_get(dir, "stack");
	if (str) {
		igt_debug("Kernel stack for pid %d:\n%s\n", pid, str);
		free(str);
	}

	close(dir);
}

static struct igt_helper_process hang_detector;
__noreturn static void
hang_detector_process(int fd, pid_t pid, dev_t rdev)
{
	struct udev_monitor *mon =
		udev_monitor_new_from_netlink(udev_new(), "kernel");
	struct pollfd pfd;
	int ret;

	udev_monitor_filter_add_match_subsystem_devtype(mon, "drm", NULL);
	udev_monitor_enable_receiving(mon);

	pfd.fd = udev_monitor_get_fd(mon);
	pfd.events = POLLIN;

	while ((ret = poll(&pfd, 1, 2000)) >= 0) {
		struct udev_device *dev;
		dev_t devnum;

		if (kill(pid, 0)) { /* Parent has died, so must we. */
			igt_warn("Parent died without killing its children (%s)\n",
				 __func__);
			break;
		}

		dev = NULL;
		if (ret > 0)
			dev = udev_monitor_receive_device(mon);
		if (dev == NULL)
			continue;

		devnum = udev_device_get_devnum(dev);
		if (memcmp(&rdev, &devnum, sizeof(dev_t)) == 0) {
			const char *str;

			str = udev_device_get_property_value(dev, "ERROR");
			if (str && atoi(str) == 1) {
				show_kernel_stack(pid);
				kill(pid, SIGIO);
			}
		}

		udev_device_unref(dev);
	}

	exit(0);
}

__noreturn static void sig_abort(int sig)
{
	errno = 0; /* inside a signal, last errno reporting is confusing */
	igt_assert(!"GPU hung");
}

void igt_fork_hang_detector(int fd)
{
	struct stat st;

	igt_assert(fstat(fd, &st) == 0);

	/*
	 * Disable per-engine reset to force an error uevent. We don't
	 * expect to get any hangs whilst the detector is enabled (if we do
	 * they are a test failure!) and so the loss of per-engine reset
	 * functionality is not an issue.
	 */
	igt_assert(igt_params_set(fd, "reset", "%d", 1 /* only global reset */));

	signal(SIGIO, sig_abort);
	igt_fork_helper(&hang_detector)
		hang_detector_process(fd, getppid(), st.st_rdev);
}

void igt_stop_hang_detector(void)
{
	/*
	 * Give the uevent time to arrive. No sleep at all misses about 20% of
	 * hangs (at least, in the i915_hangman/detector test). A sleep of 1ms
	 * seems to miss about 2%, 10ms loses <1%, so 100ms should be safe.
	 */
	usleep(100 * 1000);

	igt_stop_helper(&hang_detector);
}

/**
 * igt_check_boolean_env_var:
 * @env_var: environment variable name
 * @default_value: default value for the environment variable
 *
 * This function should be used to parse boolean environment variable options.
 *
 * Returns:
 * The boolean value of the environment variable @env_var as decoded by atoi()
 * if it is set and @default_value if the variable is not set.
 */
bool igt_check_boolean_env_var(const char *env_var, bool default_value)
{
	char *val;

	val = getenv(env_var);
	if (!val)
		return default_value;

	return atoi(val) != 0;
}

/**
 * igt_aub_dump_enabled:
 *
 * Returns:
 * True if AUB dumping is enabled with IGT_DUMP_AUB=1 in the environment, false
 * otherwise.
 */
bool igt_aub_dump_enabled(void)
{
	static int dump_aub = -1;

	if (dump_aub == -1)
		dump_aub = igt_check_boolean_env_var("IGT_DUMP_AUB", false);

	return dump_aub;
}

/* other helpers */
/**
 * igt_exchange_int:
 * @array: pointer to the array of integers
 * @i: first position
 * @j: second position
 *
 * Exchanges the two values at array indices @i and @j. Useful as an exchange
 * function for igt_permute_array().
 */
void igt_exchange_int(void *array, unsigned i, unsigned j)
{
	int *int_arr, tmp;
	int_arr = array;

	tmp = int_arr[i];
	int_arr[i] = int_arr[j];
	int_arr[j] = tmp;
}

/**
 * igt_exchange_int64:
 * @array: pointer to the array of int64_t
 * @i: first position
 * @j: second position
 *
 * Exchanges the two values at array indices @i and @j. Useful as an exchange
 * function for igt_permute_array().
 */
void igt_exchange_int64(void *array, unsigned i, unsigned j)
{
	int64_t *a = array;

	igt_swap(a[i], a[j]);
}

/**
 * igt_permute_array:
 * @array: pointer to array
 * @size: size of the array
 * @exchange_func: function to exchange array elements
 *
 * This function randomly permutes the array using random() as the PRNG source.
 * The @exchange_func function is called to exchange two elements in the array
 * when needed.
 */
void igt_permute_array(void *array, unsigned size,
                       void (*exchange_func)(void *array,
                                             unsigned i,
                                             unsigned j))
{
	int i;

	for (i = size - 1; i > 0; i--) {
		/* yes, not perfectly uniform, who cares */
		long l = hars_petruska_f54_1_random_unsafe() % (i +1);
		if (i != l)
			exchange_func(array, i, l);
	}
}

__attribute__((format(printf, 1, 2)))
static void igt_interactive_info(const char *format, ...)
{
	va_list args;

	if (!isatty(STDERR_FILENO) || __igt_plain_output) {
		errno = 0; /* otherwise would be either ENOTTY or EBADF */
		return;
	}

	if (igt_log_level > IGT_LOG_INFO)
		return;

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}


/**
 * igt_progress:
 * @header: header string to prepend to the progress indicator
 * @i: work processed thus far
 * @total: total amount of work
 *
 * This function draws a progress indicator, which is useful for running
 * long-winded tests manually on the console. To avoid spamming log files in
 * automated runs the progress indicator is suppressed when not running on a
 * terminal.
 */
void igt_progress(const char *header, uint64_t i, uint64_t total)
{
	int divider = 200;

	if (i+1 >= total) {
		igt_interactive_info("\r%s100%%\n", header);
		return;
	}

	if (total / 200 == 0)
		divider = 1;

	/* only bother updating about every 0.5% */
	if (i % (total / divider) == 0)
		igt_interactive_info("\r%s%3llu%%", header,
				     (long long unsigned)i * 100 / total);
}

/**
 * igt_print_activity:
 *
 * Print a '.' to indicate activity. This is printed without a newline and
 * only if output is to a terminal.
 */
void igt_print_activity(void)
{
	igt_interactive_info(".");
}

static int autoresume_delay;

static const char *suspend_state_name[] = {
	[SUSPEND_STATE_FREEZE] = "freeze",
	[SUSPEND_STATE_STANDBY] = "standby",
	[SUSPEND_STATE_S3] = "mem", /* Forces Suspend-to-Ram (S3) */
	[SUSPEND_STATE_MEM] = "mem", /* Respects system default */
	[SUSPEND_STATE_DISK] = "disk",
};

static const char *suspend_test_name[] = {
	[SUSPEND_TEST_NONE] = "none",
	[SUSPEND_TEST_FREEZER] = "freezer",
	[SUSPEND_TEST_DEVICES] = "devices",
	[SUSPEND_TEST_PLATFORM] = "platform",
	[SUSPEND_TEST_PROCESSORS] = "processors",
	[SUSPEND_TEST_CORE] = "core",
};

static const char *mem_sleep_name[] = {
	[MEM_SLEEP_S2IDLE] = "s2idle",
	[MEM_SLEEP_SHALLOW] = "shallow",
	[MEM_SLEEP_DEEP] = "deep"
};

static enum igt_suspend_test get_suspend_test(int power_dir)
{
	char *test_line;
	char *test_name;
	enum igt_suspend_test test;

	if (faccessat(power_dir, "pm_test", R_OK, 0))
		return SUSPEND_TEST_NONE;

	igt_assert((test_line = igt_sysfs_get(power_dir, "pm_test")));
	for (test_name = strtok(test_line, " "); test_name;
	     test_name = strtok(NULL, " "))
		if (test_name[0] == '[') {
			test_name[strlen(test_name) - 1] = '\0';
			test_name++;
			break;
		}

	if (!test_name) {
	  	free(test_line);
		return SUSPEND_TEST_NONE;
	}

	for (test = SUSPEND_TEST_NONE; test < SUSPEND_TEST_NUM; test++)
		if (strcmp(suspend_test_name[test], test_name) == 0)
			break;

	igt_assert(test < SUSPEND_TEST_NUM);

	free(test_line);
	return test;
}

static void set_suspend_test(int power_dir, enum igt_suspend_test test)
{
	igt_assert(test < SUSPEND_TEST_NUM);

	if (faccessat(power_dir, "pm_test", W_OK, 0)) {
		igt_require(test == SUSPEND_TEST_NONE);
		return;
	}

	igt_assert(igt_sysfs_set(power_dir, "pm_test", suspend_test_name[test]));
}

#define SQUELCH ">/dev/null 2>&1"

static void suspend_via_rtcwake(enum igt_suspend_state state)
{
	char cmd[128];
	int delay, ret;

	igt_assert(state < SUSPEND_STATE_NUM);

	delay = igt_get_autoresume_delay(state);

	/*
	 * Skip if rtcwake would fail for a reason not related to the kernel's
	 * suspend functionality.
	 */
	snprintf(cmd, sizeof(cmd), "rtcwake -n -s %d -m %s " SQUELCH,
		 delay, suspend_state_name[state]);
	ret = igt_system(cmd);
	igt_require_f(ret == 0, "rtcwake test failed with %i\n"
		     "This failure could mean that something is wrong with "
		     "the rtcwake tool or how your distro is set up.\n",
		      ret);

	snprintf(cmd, sizeof(cmd), "rtcwake -s %d -m %s ",
		 delay, suspend_state_name[state]);
	ret = igt_system(cmd);
	if (ret) {
		const char *path = "suspend_stats";
		char *info;
		int dir;

		igt_warn("rtcwake failed with %i\n"
			 "Check dmesg for further details.\n",
			 ret);

		dir = open(igt_debugfs_mount(), O_RDONLY);
		info = igt_sysfs_get(dir, path);
		close(dir);
		if (info) {
			igt_debug("%s:\n%s\n", path, info);
			free(info);
		}
	}
	igt_assert_eq(ret, 0);
}

static void suspend_via_sysfs(int power_dir, enum igt_suspend_state state)
{
	igt_assert(state < SUSPEND_STATE_NUM);
	igt_assert(igt_sysfs_set(power_dir, "state",
				 suspend_state_name[state]));
}

static bool is_state_supported(int power_dir, enum igt_suspend_state state)
{
	const char *str;
	char *states;

	igt_assert((states = igt_sysfs_get(power_dir, "state")));

	str = strstr(states, suspend_state_name[state]);

	if (!str)
		igt_info("State %s not supported.\nSupported States: %s\n",
			 suspend_state_name[state], states);

	free(states);
	return str;
}

static int get_mem_sleep(void)
{
	char *mem_sleep_states;
	char *mem_sleep_state;
	enum igt_mem_sleep mem_sleep;
	int power_dir;

	igt_require((power_dir = open("/sys/power", O_RDONLY)) >= 0);

	if (faccessat(power_dir, "mem_sleep", R_OK, 0))
		return MEM_SLEEP_NONE;

	igt_assert((mem_sleep_states = igt_sysfs_get(power_dir, "mem_sleep")));
	for (mem_sleep_state = strtok(mem_sleep_states, " "); mem_sleep_state;
	     mem_sleep_state = strtok(NULL, " ")) {
		if (mem_sleep_state[0] == '[') {
			mem_sleep_state[strlen(mem_sleep_state) - 1] = '\0';
			mem_sleep_state++;
			break;
		}
	}

	if (!mem_sleep_state) {
		free(mem_sleep_states);
		return MEM_SLEEP_NONE;
	}

	for (mem_sleep = MEM_SLEEP_S2IDLE; mem_sleep < MEM_SLEEP_NUM; mem_sleep++) {
		if (strcmp(mem_sleep_name[mem_sleep], mem_sleep_state) == 0)
			break;
	}

	igt_assert_f(mem_sleep < MEM_SLEEP_NUM, "Invalid mem_sleep state\n");

	free(mem_sleep_states);
	close(power_dir);
	return mem_sleep;
}

static void set_mem_sleep(int power_dir, enum igt_mem_sleep sleep)
{
	igt_assert(sleep < MEM_SLEEP_NUM);

	igt_assert_eq(faccessat(power_dir, "mem_sleep", W_OK, 0), 0);

	igt_assert(igt_sysfs_set(power_dir, "mem_sleep",
				 mem_sleep_name[sleep]));
}

static bool is_mem_sleep_state_supported(int power_dir, enum igt_mem_sleep state)
{
	const char *str;
	char *mem_sleep_states;

	igt_assert((mem_sleep_states = igt_sysfs_get(power_dir, "mem_sleep")));

	str = strstr(mem_sleep_states, mem_sleep_name[state]);

	if (!str)
		igt_info("mem_sleep state %s not supported.\nSupported mem_sleep states: %s\n",
			 mem_sleep_name[state], mem_sleep_states);

	free(mem_sleep_states);
	return str;
}

/**
 * igt_system_suspend_autoresume:
 * @state: an #igt_suspend_state, the target suspend state
 * @test: an #igt_suspend_test, test point at which to complete the suspend
 *	  cycle
 *
 * Execute a system suspend cycle targeting the given @state optionally
 * completing the cycle at the given @test point and automaically wake up
 * again. Waking up is either achieved using the RTC wake-up alarm for a full
 * suspend cycle or a kernel timer for a suspend test cycle. The kernel timer
 * delay for a test cycle can be configured by the suspend.pm_test_delay
 * kernel parameter (5 sec by default).
 *
 * #SUSPEND_TEST_NONE specifies a full suspend cycle.
 * The #SUSPEND_TEST_FREEZER..#SUSPEND_TEST_CORE test points can make it
 * possible to collect error logs in case a full suspend cycle would prevent
 * this by hanging the machine, or they can provide an idea of the faulty
 * component by comparing fail/no-fail results at different test points.
 *
 * This is very handy for implementing any kind of suspend/resume test.
 */
void igt_system_suspend_autoresume(enum igt_suspend_state state,
				   enum igt_suspend_test test)
{
	int power_dir;
	enum igt_suspend_test orig_test;
	enum igt_mem_sleep orig_mem_sleep = MEM_SLEEP_NONE;

	igt_require((power_dir = open("/sys/power", O_RDONLY)) >= 0);
	igt_require(is_state_supported(power_dir, state));
	igt_require(test == SUSPEND_TEST_NONE ||
		    faccessat(power_dir, "pm_test", R_OK | W_OK, 0) == 0);

	igt_skip_on_f(state == SUSPEND_STATE_DISK &&
		      !igt_get_total_swap_mb(),
		      "Suspend to disk requires swap space.\n");

	orig_test = get_suspend_test(power_dir);

	if (state == SUSPEND_STATE_S3) {
		orig_mem_sleep = get_mem_sleep();
		igt_skip_on_f(!is_mem_sleep_state_supported(power_dir, MEM_SLEEP_DEEP),
			      "S3 not supported in this system.\n");
		set_mem_sleep(power_dir, MEM_SLEEP_DEEP);
		igt_skip_on_f(get_mem_sleep() != MEM_SLEEP_DEEP,
			      "S3 not possible in this system.\n");
	}

	set_suspend_test(power_dir, test);

	if (test == SUSPEND_TEST_NONE)
		suspend_via_rtcwake(state);
	else
		suspend_via_sysfs(power_dir, state);

	if (orig_mem_sleep)
		set_mem_sleep(power_dir, orig_mem_sleep);

	set_suspend_test(power_dir, orig_test);
	close(power_dir);
}

static int original_autoresume_delay;

static void igt_restore_autoresume_delay(int sig)
{
	int delay_fd;
	char delay_str[10];

	igt_require((delay_fd = open("/sys/module/suspend/parameters/pm_test_delay",
				    O_WRONLY)) >= 0);

	snprintf(delay_str, sizeof(delay_str), "%d", original_autoresume_delay);
	igt_require(write(delay_fd, delay_str, strlen(delay_str)));

	close(delay_fd);
}

/**
 * igt_set_autoresume_delay:
 * @delay_secs: The delay in seconds before resuming the system
 *
 * Sets how long we wait to resume the system after suspending it, using the
 * suspend.pm_test_delay variable. On exit, the original delay value is
 * restored.
 */
void igt_set_autoresume_delay(int delay_secs)
{
	int delay_fd;
	char delay_str[10];

	delay_fd = open("/sys/module/suspend/parameters/pm_test_delay", O_RDWR);

	if (delay_fd >= 0) {
		if (!original_autoresume_delay) {
			igt_require(read(delay_fd, delay_str,
					 sizeof(delay_str)));
			original_autoresume_delay = atoi(delay_str);
			igt_install_exit_handler(igt_restore_autoresume_delay);
		}

		snprintf(delay_str, sizeof(delay_str), "%d", delay_secs);
		igt_require(write(delay_fd, delay_str, strlen(delay_str)));

		close(delay_fd);
	}

	autoresume_delay = delay_secs;
}

/**
 * igt_get_autoresume_delay:
 * @state: an #igt_suspend_state, the target suspend state
 *
 * Retrieves how long we wait to resume the system after suspending it.
 * This can either be set through igt_set_autoresume_delay or be a default
 * value that depends on the suspend state.
 *
 * Returns: The autoresume delay, in seconds.
 */
int igt_get_autoresume_delay(enum igt_suspend_state state)
{
	int delay;

	if (autoresume_delay)
		delay = autoresume_delay;
	else
		delay = state == SUSPEND_STATE_DISK ? 30 : 15;

	return delay;
}

/**
 * igt_drop_root:
 *
 * Drop root privileges and make sure it actually worked. Useful for tests
 * which need to check security constraints. Note that this should only be
 * called from manually forked processes, since the lack of root privileges
 * will wreak havoc with the automatic cleanup handlers.
 */
void igt_drop_root(void)
{
	igt_assert_eq(getuid(), 0);

	igt_assert_eq(setgroups(0, NULL), 0);
	igt_assert_eq(setgid(2), 0);
	igt_assert_eq(setuid(2), 0);

	igt_assert_eq(getgroups(0, NULL), 0);
	igt_assert_eq(getgid(), 2);
	igt_assert_eq(getuid(), 2);
}

/**
 * igt_debug_wait_for_keypress:
 * @var: var lookup to to enable this wait
 *
 * Waits for a key press when run interactively and when the corresponding debug
 * var is set in the --interactive-debug=$var variable. Multiple keys
 * can be specified as a comma-separated list or alternatively "all" if a wait
 * should happen for all cases. Calling this function with "all" will assert.
 *
 * When not connected to a terminal interactive_debug is ignored
 * and execution immediately continues.
 *
 * This is useful for display tests where under certain situation manual
 * inspection of the display is useful. Or when running a testcase in the
 * background.
 */
void igt_debug_wait_for_keypress(const char *var)
{
	struct termios oldt, newt;

	if (!isatty(STDIN_FILENO)) {
		errno = 0; /* otherwise would be either ENOTTY or EBADF */
		return;
	}

	if (!igt_interactive_debug)
		return;

	if (strstr(var, "all"))
		igt_assert_f(false, "Bug in test: Do not call igt_debug_wait_for_keypress with \"all\"\n");

	if (!strstr(igt_interactive_debug, var) &&
	    !strstr(igt_interactive_debug, "all"))
		return;

	igt_info("Press any key to continue ...\n");

	tcgetattr ( STDIN_FILENO, &oldt );
	newt = oldt;
	newt.c_lflag &= ~( ICANON | ECHO );
	tcsetattr ( STDIN_FILENO, TCSANOW, &newt );
	getchar();
	tcsetattr ( STDIN_FILENO, TCSANOW, &oldt );
}

/**
 * igt_debug_interactive_mode_check:
 * @var: var lookup to to enable this wait
 * @expected: message to be printed as expected behaviour before wait for keys Y/n
 *
 * Waits for a key press when run interactively and when the corresponding debug
 * var is set in the --interactive-debug=$var variable. Multiple vars
 * can be specified as a comma-separated list or alternatively "all" if a wait
 * should happen for all cases.
 *
 * This is useful for display tests where under certain situation manual
 * inspection of the display is useful. Or when running a testcase in the
 * background.
 *
 * When not connected to a terminal interactive_debug is ignored
 * and execution immediately continues. For this reason by default this function
 * returns true. It returns false only when N/n is pressed indicating the
 * user isn't seeing what was expected.
 *
 * Force test fail when N/n is pressed.
 */
void igt_debug_interactive_mode_check(const char *var, const char *expected)
{
	struct termios oldt, newt;
	char key;

	if (!isatty(STDIN_FILENO)) {
		errno = 0; /* otherwise would be either ENOTTY or EBADF */
		return;
	}

	if (!igt_interactive_debug)
		return;

	if (!strstr(igt_interactive_debug, var) &&
	    !strstr(igt_interactive_debug, "all"))
		return;

	igt_info("Is %s [Y/n]", expected);

	tcgetattr ( STDIN_FILENO, &oldt );
	newt = oldt;
	newt.c_lflag &= ~ICANON;
	tcsetattr ( STDIN_FILENO, TCSANOW, &newt );
	key = getchar();
	tcsetattr ( STDIN_FILENO, TCSANOW, &oldt );

	igt_info("\n");

	igt_assert(key != 'n' && key != 'N');
}

/**
 * igt_lock_mem:
 * @size: the amount of memory to lock into RAM, in MB
 *
 * Allocate @size MB of memory and lock it into RAM. This releases any
 * previously locked memory.
 *
 * Use #igt_unlock_mem to release the currently locked memory.
 */
static char *locked_mem;
static size_t locked_size;

void igt_lock_mem(size_t size)
{
	long pagesize = sysconf(_SC_PAGESIZE);
	size_t i;
	int ret;

	if (size == 0) {
		return;
	}

	if (locked_mem) {
		igt_unlock_mem();
		igt_warn("Unlocking previously locked memory.\n");
	}

	locked_size = size * 1024 * 1024;

	locked_mem = malloc(locked_size);
	igt_require_f(locked_mem,
		      "Could not malloc %zdMiB for locking.\n", size);

	/* write into each page to ensure it is allocated */
	for (i = 0; i < locked_size; i += pagesize)
		locked_mem[i] = i;

	ret = mlock(locked_mem, locked_size);
	igt_assert_f(ret == 0, "Could not mlock %zdMiB.\n", size);
}

/**
 * igt_unlock_mem:
 *
 * Release and free the RAM used by #igt_lock_mem.
 */
void igt_unlock_mem(void)
{
	if (!locked_mem)
		return;

	munlock(locked_mem, locked_size);

	free(locked_mem);
	locked_mem = NULL;
}

/**
 * igt_is_process_running:
 * @comm: Name of process in the form found in /proc/pid/comm (limited to 15
 * chars)
 *
 * Returns: true in case the process has been found, false otherwise.
 *
 * This function checks in the process table for an entry with the name @comm.
 */
int igt_is_process_running(const char *comm)
{
	PROCTAB *proc;
	proc_t *proc_info;
	bool found = false;

	proc = openproc(PROC_FILLCOM | PROC_FILLSTAT);
	igt_assert(proc != NULL);

	while ((proc_info = readproc(proc, NULL))) {
		if (!strncasecmp(proc_info->cmd, comm, sizeof(proc_info->cmd))) {
			freeproc(proc_info);
			found = true;
			break;
		}
		freeproc(proc_info);
	}

	closeproc(proc);
	return found;
}

/**
 * igt_terminate_process:
 * @sig: Signal to send
 * @comm: Name of process in the form found in /proc/pid/comm (limited to 15
 * chars)
 *
 * Returns: 0 in case the process is not found running or the signal has been
 * sent successfully or -errno otherwise.
 *
 * This function sends the signal @sig for a process found in process table
 * with name @comm.
 */
int igt_terminate_process(int sig, const char *comm)
{
	PROCTAB *proc;
	proc_t *proc_info;
	int err = 0;

	proc = openproc(PROC_FILLCOM | PROC_FILLSTAT | PROC_FILLARG);
	igt_assert(proc != NULL);

	while ((proc_info = readproc(proc, NULL))) {
		if (!strncasecmp(proc_info->cmd, comm, sizeof(proc_info->cmd))) {

			if (kill(proc_info->tid, sig) < 0)
				err = -errno;

			freeproc(proc_info);
			break;
		}
		freeproc(proc_info);
	}

	closeproc(proc);
	return err;
}

struct pinfo {
	pid_t pid;
	const char *comm;
	const char *fn;
};

static void
__igt_show_stat(struct pinfo *info)
{
	const char *comm, *fn;
	const char *type = "";
	struct stat st;

	pid_t pid = info->pid;
	igt_assert((comm = info->comm));
	igt_assert((fn = info->fn));

	if (lstat(fn, &st) == -1)
		return;

	igt_info("%20.20s ", comm);
	igt_info("%10d ", pid);

	switch (st.st_mode & S_IFMT) {
	case S_IFBLK:
		type = "block";
		break;
	case S_IFCHR:
		type = "character";
		break;
	case S_IFDIR:
		type = "directory";
		break;
	case S_IFIFO:
		type = "FIFO/pipe";
		break;
	case S_IFLNK:
		type = "symlink";
		break;
	case S_IFREG:
		type = "file";
		break;
	case S_IFSOCK:
		type = "socket";
		break;
	default:
		type = "unknown?";
		break;
	}
	igt_info("%20.20s ", type);

	igt_info("%10ld%10ld ", (long) st.st_uid, (long) st.st_gid);

	igt_info("%15lld bytes ", (long long) st.st_size);
	igt_info("%30.30s", fn);
	igt_info("\n");
}


static void
igt_show_stat_header(void)
{
	igt_info("%20.20s%11.11s%21.21s%11.11s%10.10s%22.22s%31.31s\n",
		"COMM", "PID", "Type", "UID", "GID", "Size", "Filename");
}

static void
igt_show_stat(proc_t *info, int *state, const char *fn)
{
	struct pinfo p = { .pid = info->tid, .comm = info->cmd, .fn = fn };

	if (!*state)
		igt_show_stat_header();

	__igt_show_stat(&p);
	++*state;
}

static void
__igt_lsof_fds(proc_t *proc_info, int *state, char *proc_path, const char *dir)
{
	struct dirent *d;
	struct stat st;
	char path[PATH_MAX];
	char *fd_lnk;

	/* default fds or kernel threads */
	const char *default_fds[] = { "/dev/pts", "/dev/null" };

	DIR *dp = opendir(proc_path);
	igt_assert(dp);
again:
	while ((d = readdir(dp))) {
		char *copy_fd_lnk;
		char *dirn;

		unsigned int i;
		ssize_t read;

		if (*d->d_name == '.')
			continue;

		memset(path, 0, sizeof(path));
		snprintf(path, sizeof(path), "%s/%s", proc_path, d->d_name);

		if (lstat(path, &st) == -1)
			continue;

		fd_lnk = malloc(st.st_size + 1);

		igt_assert((read = readlink(path, fd_lnk, st.st_size + 1)));
		fd_lnk[read] = '\0';

		for (i = 0; i < ARRAY_SIZE(default_fds); ++i) {
			if (!strncmp(default_fds[i],
				     fd_lnk,
				     strlen(default_fds[i]))) {
				free(fd_lnk);
				goto again;
			}
		}

		copy_fd_lnk = strdup(fd_lnk);
		dirn = dirname(copy_fd_lnk);

		if (!strncmp(dir, dirn, strlen(dir)))
			igt_show_stat(proc_info, state, fd_lnk);

		free(copy_fd_lnk);
		free(fd_lnk);
	}

	closedir(dp);
}

/*
 * This functions verifies, for each process running on the machine, if the
 * current working directory or the fds matches the one supplied in dir.
 */
static void
__igt_lsof(const char *dir)
{
	PROCTAB *proc;
	proc_t *proc_info;

	char path[30];
	char *name_lnk;
	struct stat st;
	int state = 0;

	proc = openproc(PROC_FILLCOM | PROC_FILLSTAT | PROC_FILLARG);
	igt_assert(proc != NULL);

	while ((proc_info = readproc(proc, NULL))) {
		ssize_t read;

		/* check current working directory */
		memset(path, 0, sizeof(path));
		snprintf(path, sizeof(path), "/proc/%d/cwd", proc_info->tid);

		if (stat(path, &st) == -1)
			continue;

		name_lnk = malloc(st.st_size + 1);

		igt_assert((read = readlink(path, name_lnk, st.st_size + 1)));
		name_lnk[read] = '\0';

		if (!strncmp(dir, name_lnk, strlen(dir)))
			igt_show_stat(proc_info, &state, name_lnk);

		/* check also fd, seems that lsof(8) doesn't look here */
		memset(path, 0, sizeof(path));
		snprintf(path, sizeof(path), "/proc/%d/fd", proc_info->tid);

		__igt_lsof_fds(proc_info, &state, path, dir);

		free(name_lnk);
		freeproc(proc_info);
	}

	closeproc(proc);
}

/**
 * igt_lsof: Lists information about files opened by processes.
 * @dpath: Path to look under. A valid directory is required.
 *
 * This function mimics (a restrictive form of) lsof(8), but also shows
 * information about opened fds.
 */
void
igt_lsof(const char *dpath)
{
	struct stat st;
	size_t len = strlen(dpath);
	char *sanitized;

	if (stat(dpath, &st) == -1)
		return;

	if (!S_ISDIR(st.st_mode)) {
		igt_warn("%s not a directory!\n", dpath);
		return;
	}

	sanitized = strdup(dpath);
	/* remove last '/' so matching is easier */
	if (len > 1 && dpath[len - 1] == '/')
		sanitized[len - 1] = '\0';

	__igt_lsof(sanitized);

	free(sanitized);
}

static void pulseaudio_unload_module(proc_t *proc_info)
{
	struct igt_helper_process pa_proc = {};
	char xdg_dir[PATH_MAX];
	const char *homedir;
	struct passwd *pw;

	igt_fork_helper(&pa_proc) {
		pw = getpwuid(proc_info->euid);
		homedir = pw->pw_dir;
		snprintf(xdg_dir, sizeof(xdg_dir), "/run/user/%d", proc_info->euid);

		igt_info("Request pulseaudio to stop using audio device\n");

		setgid(proc_info->egid);
		setuid(proc_info->euid);
		clearenv();
		setenv("HOME", homedir, 1);
		setenv("XDG_RUNTIME_DIR",xdg_dir, 1);

		system("for i in $(pacmd list-sources|grep module:|cut -d : -f 2); do pactl unload-module $i; done");
	}
	igt_wait_helper(&pa_proc);
}

static int pipewire_pulse_pid = 0;
static int pipewire_pw_reserve_pid = 0;
static struct igt_helper_process pw_reserve_proc = {};

static void pipewire_reserve_wait(void)
{
	char xdg_dir[PATH_MAX];
	const char *homedir;
	struct passwd *pw;
	proc_t *proc_info;
	PROCTAB *proc;

	igt_fork_helper(&pw_reserve_proc) {
		igt_info("Preventing pipewire-pulse to use the audio drivers\n");

		proc = openproc(PROC_FILLCOM | PROC_FILLSTAT | PROC_FILLARG);
		igt_assert(proc != NULL);

		while ((proc_info = readproc(proc, NULL))) {
			if (pipewire_pulse_pid == proc_info->tid)
				break;
			freeproc(proc_info);
		}
		closeproc(proc);

		/* Sanity check: if it can't find the process, it means it has gone */
		if (pipewire_pulse_pid != proc_info->tid)
			exit(0);

		pw = getpwuid(proc_info->euid);
		homedir = pw->pw_dir;
		snprintf(xdg_dir, sizeof(xdg_dir), "/run/user/%d", proc_info->euid);
		setgid(proc_info->egid);
		setuid(proc_info->euid);
		clearenv();
		setenv("HOME", homedir, 1);
		setenv("XDG_RUNTIME_DIR",xdg_dir, 1);
		freeproc(proc_info);

		/*
		 * pw-reserve will run in background. It will only exit when
		 * igt_kill_children() is called later on. So, it shouldn't
		 * call igt_waitchildren(). Instead, just exit with the return
		 * code from pw-reserve.
		 */
		exit(system("pw-reserve -n Audio0 -r"));
	}
}

/* Maximum time waiting for pw-reserve to start running */
#define PIPEWIRE_RESERVE_MAX_TIME 1000 /* milisseconds */

int pipewire_pulse_start_reserve(void)
{
	bool is_pw_reserve_running = false;
	proc_t *proc_info;
	int attempts = 0;
	PROCTAB *proc;

	if (!pipewire_pulse_pid)
		return 0;

	pipewire_reserve_wait();

	/*
	 * Note: using pw-reserve to stop using audio only works with
	 * pipewire version 0.3.50 or upper.
	 */
	for (attempts = 0; attempts < PIPEWIRE_RESERVE_MAX_TIME; attempts++) {
		usleep(1000);
		proc = openproc(PROC_FILLCOM | PROC_FILLSTAT | PROC_FILLARG);
		igt_assert(proc != NULL);

		while ((proc_info = readproc(proc, NULL))) {
			if (!strcmp(proc_info->cmd, "pw-reserve")) {
				is_pw_reserve_running = true;
				pipewire_pw_reserve_pid = proc_info->tid;
				freeproc(proc_info);
				break;
			}
			freeproc(proc_info);
		}
		closeproc(proc);
		if (is_pw_reserve_running)
			break;
	}
	if (!is_pw_reserve_running) {
		igt_warn("Failed to remove audio drivers from pipewire\n");
		return 1;
	}
	/* Let's grant some time for pw_reserve to notify pipewire via D-BUS */
	usleep(50000);

	/*
	 * pw-reserve is running, and should have stopped using the audio
	 * drivers. We can now remove the driver.
	 */

	return 0;
}

void pipewire_pulse_stop_reserve(void)
{
	if (!pipewire_pulse_pid)
		return;

	igt_stop_helper(&pw_reserve_proc);
}

/**
 * __igt_lsof_audio_and_kill_proc() - check if a given process is using an
 *	audio device. If so, stop or prevent them to use such devices.
 *
 * @proc_info: process struct, as returned by readproc()
 * @proc_path: path of the process under procfs
 * @pipewire_pulse_pid: PID of pipewire-pulse process
 *
 * No processes can be using an audio device by the time it gets removed.
 * This function checks if a process is using an audio device from /dev/snd.
 * If so, it will check:
 * 	- if the process is pulseaudio, it can't be killed, as systemd will
 * 	  respawn it. So, instead, send a request for it to stop bind the
 * 	  audio devices.
 *	- if the process is pipewire-pulse, it can't be killed, as systemd will
 *	  respawn it. So, instead, the caller should call pw-reserve, remove
 *	  the kernel driver and then stop pw-reserve. On such case, this
 *	  function returns the PID of pipewire-pulse, but won't touch it.
 * If the check fails, it means that the process can simply be killed.
 */
static int
__igt_lsof_audio_and_kill_proc(proc_t *proc_info, char *proc_path)
{
	const char *audio_dev = "/dev/snd/";
	char path[PATH_MAX * 2];
	struct dirent *d;
	struct stat st;
	char *fd_lnk;
	int fail = 0;
	ssize_t read;
	DIR *dp;

	/*
	 * Terminating pipewire-pulse require an special procedure, which
	 * is only available at version 0.3.50 and upper. Just trying to
	 * kill pipewire will start a race between IGT and systemd. If IGT
	 * wins, the audio driver will be unloaded before systemd tries to
	 * reload it, but if systemd wins, the audio device will be re-opened
	 * and used before IGT has a chance to remove the audio driver.
	 * Pipewire version 0.3.50 should bring a standard way:
	 *
	 * 1) start a thread running:
	 *	 pw-reserve -n Audio0 -r
	 * 2) unload/unbind the the audio driver(s);
	 * 3) stop the pw-reserve thread.
	 */
	if (!strcmp(proc_info->cmd, "pipewire-pulse")) {
		igt_info("process %d (%s) is using audio device. Should be requested to stop using them.\n",
			 proc_info->tid, proc_info->cmd);
		pipewire_pulse_pid = proc_info->tid;
		return 0;
	}
	/*
	 * pipewire-pulse itself doesn't hook into a /dev/snd device. Instead,
	 * the actual binding happens at the Pipewire Session Manager, e.g.
	 * either wireplumber or pipewire media-session.
	 *
	 * Just killing such processes won't produce any effect, as systemd
	 * will respawn them. So, just ignore here, they'll honor pw-reserve,
	 * when the time comes.
	 */
	if (!strcmp(proc_info->cmd, "pipewire-media-session"))
		return 0;
	if (!strcmp(proc_info->cmd, "wireplumber"))
		return 0;

	dp = opendir(proc_path);
	if (!dp && errno == ENOENT)
		return 0;
	igt_assert(dp);

	while ((d = readdir(dp))) {
		if (*d->d_name == '.')
			continue;

		memset(path, 0, sizeof(path));
		snprintf(path, sizeof(path), "%s/%s", proc_path, d->d_name);

		if (lstat(path, &st) == -1)
			continue;

		fd_lnk = malloc(st.st_size + 1);

		igt_assert((read = readlink(path, fd_lnk, st.st_size + 1)));
		fd_lnk[read] = '\0';

		if (strncmp(audio_dev, fd_lnk, strlen(audio_dev))) {
			free(fd_lnk);
			continue;
		}

		free(fd_lnk);

		/*
		 * In order to avoid racing against pa/systemd, ensure that
		 * pulseaudio will close all audio files. This should be
		 * enough to unbind audio modules and won't cause race issues
		 * with systemd trying to reload it.
		 */
		if (!strcmp(proc_info->cmd, "pulseaudio")) {
			pulseaudio_unload_module(proc_info);
			break;
		}

		/* For all other processes, just kill them */
		igt_info("process %d (%s) is using audio device. Should be terminated.\n",
				proc_info->tid, proc_info->cmd);

		if (kill(proc_info->tid, SIGTERM) < 0) {
			igt_info("Fail to terminate %s (pid: %d) with SIGTERM\n",
				proc_info->cmd, proc_info->tid);
			if (kill(proc_info->tid, SIGABRT) < 0) {
				fail++;
				igt_info("Fail to terminate %s (pid: %d) with SIGABRT\n",
					proc_info->cmd, proc_info->tid);
			}
		}

		break;
	}

	closedir(dp);
	return fail;
}

/*
 * This function identifies each process running on the machine that is
 * opening an audio device and tries to stop it.
 *
 * Special care should be taken with pipewire and pipewire-pulse, as those
 * daemons are respanned if they got killed.
 */
int
igt_lsof_kill_audio_processes(void)
{
	char path[PATH_MAX];
	proc_t *proc_info;
	PROCTAB *proc;
	int fail = 0;

	proc = openproc(PROC_FILLCOM | PROC_FILLSTAT | PROC_FILLARG);
	igt_assert(proc != NULL);
	pipewire_pulse_pid = 0;

	while ((proc_info = readproc(proc, NULL))) {
		if (snprintf(path, sizeof(path), "/proc/%d/fd", proc_info->tid) < 1)
			fail++;
		else
			fail += __igt_lsof_audio_and_kill_proc(proc_info, path);

		freeproc(proc_info);
	}
	closeproc(proc);

	return fail;
}

static struct igt_siglatency {
	timer_t timer;
	struct timespec target;
	struct sigaction oldact;
	struct igt_mean mean;

	int sig;
} igt_siglatency;

static long delay(void)
{
	return hars_petruska_f54_1_random_unsafe() % (NSEC_PER_SEC / 1000);
}

static double elapsed(const struct timespec *now, const struct timespec *last)
{
	double nsecs;

	nsecs = now->tv_nsec - last ->tv_nsec;
	nsecs += 1e9*(now->tv_sec - last->tv_sec);

	return nsecs;
}

static void siglatency(int sig, siginfo_t *info, void *arg)
{
	struct itimerspec its;

	clock_gettime(CLOCK_MONOTONIC, &its.it_value);
	if (info)
		igt_mean_add(&igt_siglatency.mean,
			     elapsed(&its.it_value, &igt_siglatency.target));
	igt_siglatency.target = its.it_value;

	its.it_value.tv_nsec += 100 * 1000;
	its.it_value.tv_nsec += delay();
	if (its.it_value.tv_nsec >= NSEC_PER_SEC) {
		its.it_value.tv_nsec -= NSEC_PER_SEC;
		its.it_value.tv_sec += 1;
	}
	its.it_interval.tv_sec = its.it_interval.tv_nsec = 0;
	timer_settime(igt_siglatency.timer, TIMER_ABSTIME, &its, NULL);
}

void igt_start_siglatency(int sig)
{
	struct sigevent sev;
	struct sigaction act;

	if (sig <= 0)
		sig = SIGRTMIN;

	if (igt_siglatency.sig)
		(void)igt_stop_siglatency(NULL);
	igt_assert(igt_siglatency.sig == 0);
	igt_siglatency.sig = sig;

	memset(&sev, 0, sizeof(sev));
	sev.sigev_notify = SIGEV_SIGNAL | SIGEV_THREAD_ID;
	sev.sigev_notify_thread_id = gettid();
	sev.sigev_signo = sig;
	timer_create(CLOCK_MONOTONIC, &sev, &igt_siglatency.timer);

	memset(&act, 0, sizeof(act));
	act.sa_sigaction = siglatency;
	sigaction(sig, &act, &igt_siglatency.oldact);

	siglatency(sig, NULL, NULL);
}

double igt_stop_siglatency(struct igt_mean *result)
{
	double mean = igt_mean_get(&igt_siglatency.mean);

	if (result)
		*result = igt_siglatency.mean;

	sigaction(igt_siglatency.sig, &igt_siglatency.oldact, NULL);
	timer_delete(igt_siglatency.timer);
	memset(&igt_siglatency, 0, sizeof(igt_siglatency));

	return mean;
}

bool igt_allow_unlimited_files(void)
{
	struct rlimit rlim;
	unsigned nofile_rlim = 1024*1024;

	FILE *file = fopen("/proc/sys/fs/nr_open", "r");
	if (file) {
		igt_assert(fscanf(file, "%u", &nofile_rlim) == 1);
		igt_info("System limit for open files is %u\n", nofile_rlim);
		fclose(file);
	}

	if (getrlimit(RLIMIT_NOFILE, &rlim))
		return false;

	rlim.rlim_cur = nofile_rlim;
	rlim.rlim_max = nofile_rlim;
	return setrlimit(RLIMIT_NOFILE, &rlim) == 0;
}

/**
 * vfs_file_max: report maximum number of files
 *
 * Get the global system-wide maximum of open files the kernel allows,
 * by reading /proc/sys/fs/file-max. Fails the current subtest if
 * reading the file fails, and returns a suitable best guess if it
 * cannot be opened.
 *
 * Returns: System-wide maximum of open files, or a best effort guess.
 */
uint64_t vfs_file_max(void)
{
	static long long unsigned max;
	if (max == 0) {
		FILE *file = fopen("/proc/sys/fs/file-max", "r");
		max = 80000;
		if (file) {
			igt_assert(fscanf(file, "%llu", &max) == 1);
			fclose(file);
		}
	}
	return max;
}

void *igt_memdup(const void *ptr, size_t len)
{
	void *dup;

	dup = malloc(len);
	if (dup)
		memcpy(dup, ptr, len);

	return dup;
}
