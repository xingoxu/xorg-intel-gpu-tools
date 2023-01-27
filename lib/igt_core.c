/*
 * Copyright © 2007, 2011, 2013, 2014 Intel Corporation
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
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <pciaccess.h>
#include <getopt.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#if defined(__linux__) || defined(__FreeBSD__)
#include <sys/syscall.h>
#endif
#include <pthread.h>
#include <sys/utsname.h>
#include <termios.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>
#include <locale.h>
#include <uwildmat/uwildmat.h>
#include <glib.h>

#include "drmtest.h"
#include "i915/gem_create.h"
#include "intel_allocator.h"
#include "intel_batchbuffer.h"
#include "intel_chipset.h"
#include "intel_io.h"
#include "igt_debugfs.h"
#include "igt_dummyload.h"
#include "version.h"
#include "config.h"

#include "igt_core.h"
#include "igt_aux.h"
#include "igt_sysfs.h"
#include "igt_sysrq.h"
#include "igt_rc.h"
#include "igt_list.h"
#include "igt_device_scan.h"
#include "igt_thread.h"
#include "runnercomms.h"

#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <elfutils/libdwfl.h>

#ifdef HAVE_LIBGEN_H
#include <libgen.h>   /* for basename() on Solaris */
#endif

/**
 * SECTION:igt_core
 * @short_description: Core i-g-t testing support
 * @title: Core
 * @include: igt.h
 *
 * This library implements the core of the i-g-t test support infrastructure.
 * Main features are the subtest enumeration, cmdline option parsing helpers for
 * subtest handling and various helpers to structure testcases with subtests and
 * handle subtest test results.
 *
 * Auxiliary code provides exit handlers, support for forked processes with test
 * result propagation. Other generally useful functionality includes optional
 * structure logging infrastructure and some support code for running reduced
 * test set on in simulated hardware environments.
 *
 * When writing tests with subtests it is extremely important that nothing
 * interferes with the subtest enumeration. In i-g-t subtests are enumerated at
 * runtime, which allows powerful testcase enumeration. But it makes subtest
 * enumeration a bit more tricky since the test code needs to be careful to
 * never run any code which might fail (like trying to do privileged operations
 * or opening device driver nodes).
 *
 * To allow this i-g-t provides #igt_fixture code blocks for setup code outside
 * of subtests and automatically skips the subtest code blocks themselves. For
 * special cases igt_only_list_subtests() is also provided. For setup code only
 * shared by a group of subtest encapsulate the #igt_fixture block and all the
 * subtestest in a #igt_subtest_group block.
 *
 * # Magic Control Blocks
 *
 * i-g-t makes heavy use of C macros which serve as magic control blocks. They
 * work fairly well and transparently but since C doesn't have full-blown
 * closures there are caveats:
 *
 * - Asynchronous blocks which are used to spawn children internally use fork().
 *   Which means that nonsensical control flow like jumping out of the control
 *   block is possible, but it will badly confuse the i-g-t library code. And of
 *   course all caveats of a real fork() call apply, namely that file
 *   descriptors are copied, but still point at the original file. This will
 *   terminally upset the libdrm buffer manager if both parent and child keep on
 *   using the same open instance of the drm device. Usually everything related
 *   to interacting with the kernel driver must be reinitialized to avoid such
 *   issues.
 *
 * - Code blocks with magic control flow are implemented with setjmp()
 *   and longjmp(). This applies to #igt_fixture, #igt_subtest,
 *   #igt_subtest_with_dynamic and #igt_dynamic
 *   blocks and all the three variants to finish test: igt_success(),
 *   igt_skip() and igt_fail(). Mostly this is of no concern, except
 *   when such a control block changes stack variables defined in the
 *   same function as the control block resides.  Any store/load
 *   behaviour after a longjmp() is ill-defined for these
 *   variables. Avoid such code.
 *
 *   Quoting the man page for longjmp():
 *
 *   "The values of automatic variables are unspecified after a call to
 *   longjmp() if they meet all the following criteria:"
 *    - "they are local to the function that made the corresponding setjmp() call;
 *    - "their values are changed between the calls to setjmp() and longjmp(); and
 *    - "they are not declared as volatile."
 *
 * # Best Practices for Test Helper Libraries Design
 *
 * Kernel tests itself tend to have fairly complex logic already. It is
 * therefore paramount that helper code, both in libraries and test-private
 * functions, add as little boilerplate code to the main test logic as possible.
 * But then dense code is hard to understand without constantly consulting
 * the documentation and implementation of all the helper functions if it
 * doesn't follow some clear patterns. Hence follow these established best
 * practices:
 *
 * - Make extensive use of the implicit control flow afforded by igt_skip(),
 *   igt_fail and igt_success(). When dealing with optional kernel features
 *   combine igt_skip() with igt_fail() to skip when the kernel support isn't
 *   available but fail when anything else goes awry. void should be the most
 *   common return type in all your functions, except object constructors of
 *   course.
 *
 * - The main test logic should have no explicit control flow for failure
 *   conditions, but instead such assumptions should be written in a declarative
 *   style.  Use one of the many macros which encapsulate i-g-t's implicit
 *   control flow.  Pick the most suitable one to have as much debug output as
 *   possible without polluting the code unnecessarily. For example
 *   igt_assert_cmpint() for comparing integers or do_ioctl() for running ioctls
 *   and checking their results.  Feel free to add new ones to the library or
 *   wrap up a set of checks into a private function to further condense your
 *   test logic.
 *
 * - When adding a new feature test function which uses igt_skip() internally,
 *   use the {prefix}_require_{feature_name} naming scheme. When you
 *   instead add a feature test function which returns a boolean, because your
 *   main test logic must take different actions depending upon the feature's
 *   availability, then instead use the {prefix}_has_{feature_name}.
 *
 * - As already mentioned eschew explicit error handling logic as much as
 *   possible. If your test absolutely has to handle the error of some function
 *   the customary naming pattern is to prefix those variants with __. Try to
 *   restrict explicit error handling to leaf functions. For the main test flow
 *   simply pass the expected error condition down into your helper code, which
 *   results in tidy and declarative test logic.
 *
 * - Make your library functions as simple to use as possible. Automatically
 *   register cleanup handlers through igt_install_exit_handler(). Reduce the
 *   amount of setup boilerplate needed by using implicit singletons and lazy
 *   structure initialization and similar design patterns.
 *
 * - Don't shy away from refactoring common code, even when there are just 2-3
 *   users and even if it's not a net reduction in code. As long as it helps to
 *   remove boilerplate and makes the code more declarative the resulting
 *   clearer test flow is worth it. All i-g-t library code has been organically
 *   extracted from testcases in this fashion.
 *
 * - For general coding style issues please follow the kernel's rules laid out
 *   in
 *   [CodingStyle](https://www.kernel.org/doc/Documentation/CodingStyle).
 *
 * # Interface with Testrunners
 *
 * i-g-t testcase are all executables which should be run as root on an
 * otherwise completely idle system. The test status is reflected in the
 * exitcode. #IGT_EXIT_SUCCESS means "success", #IGT_EXIT_SKIP "skip",
 * #IGT_EXIT_TIMEOUT that some operation "timed out".  All other exit codes
 * encode a failed test result, including any abnormal termination of the test
 * (e.g. by SIGKILL).
 *
 * On top of that tests may report unexpected results and minor issues to
 * stderr. If stderr is non-empty the test result should be treated as "warn".
 *
 * The test lists are generated at build time. Simple testcases are listed in
 * tests/single-tests.txt and tests with subtests are listed in
 * tests/multi-tests.txt. When running tests with subtest from a test runner it
 * is recommend to run each subtest individually, since otherwise the return
 * code will only reflect the overall result.
 *
 * To do that obtain the lists of subtests with "--list-subtests", which can be
 * run as non-root and doesn't require a DRM driver to be loaded (or any GPU to
 * be present). Then individual subtests can be run with "--run-subtest". Usage
 * help for tests with subtests can be obtained with the "--help" command line
 * option.
 *
 * A wildcard expression can be given to --run-subtest to specify a subset of
 * subtests to run. See https://tools.ietf.org/html/rfc3977#section-4 for a
 * description of allowed wildcard expressions.
 * Some examples of allowed wildcard expressions are:
 *
 * - '*basic*' match any subtest containing basic
 * - 'basic-???' match any subtest named basic- with 3 characters after -
 * - 'basic-[0-9]' match any subtest named basic- with a single number after -
 * - 'basic-[^0-9]' match any subtest named basic- with a single non numerical character after -
 * - 'basic*,advanced*' match any subtest starting basic or advanced
 * - '*,!basic*' match any subtest not starting basic
 * - 'basic*,!basic-render*' match any subtest starting basic but not starting basic-render
 *
 * # Configuration
 *
 * Some of IGT's behavior can be configured through a configuration file.
 * By default, this file is expected to exist in ~/.igtrc . The directory for
 * this can be overridden by setting the environment variable %IGT_CONFIG_PATH.
 * An example configuration follows:
 *
 * |[<!-- language="plain" -->
 *	&num; The common configuration section follows.
 *	[Common]
 *	FrameDumpPath=/tmp # The path to dump frames that fail comparison checks
 *
 *	&num; Device selection filter
 *	Device=pci:vendor=8086,card=0;sys:/sys/devices/platform/vgem
 *
 *	&num; The following section is used for configuring the Device Under Test.
 *	&num; It is not mandatory and allows overriding default values.
 *	[DUT]
 *	SuspendResumeDelay=10
 * ]|
 *
 * Some specific configuration options may be used by specific parts of IGT,
 * such as those related to Chamelium support.
 */

jmp_buf igt_subtest_jmpbuf;
jmp_buf igt_dynamic_jmpbuf;

static unsigned int exit_handler_count;
const char *igt_interactive_debug;
bool igt_skip_crc_compare;

/* subtests helpers */
static bool list_subtests = false;
static bool describe_subtests = false;
static char *run_single_subtest = NULL;
static char *run_single_dynamic_subtest = NULL;
static bool run_single_subtest_found = false;
static const char *in_subtest = NULL;
static const char *in_dynamic_subtest = NULL;
static struct timespec subtest_time;
static struct timespec dynamic_subtest_time;
static clockid_t igt_clock = (clockid_t)-1;
static bool in_fixture = false;
static bool test_with_subtests = false;
static bool in_atexit_handler = false;
static bool show_ftrace = false;
static enum {
	CONT = 0, SKIP, FAIL
} skip_subtests_henceforth = CONT;

static char __current_description[512];

struct description_node {
	char desc[sizeof(__current_description)];
	struct igt_list_head link;
};

static struct igt_list_head subgroup_descriptions;


bool __igt_plain_output = false;

/* fork support state */
pid_t *test_children;
int num_test_children;
int test_children_sz;
bool test_child;

/* fork dynamic support state */
pid_t *test_multi_fork_children;
int num_test_multi_fork_children;
int test_multi_fork_children_sz;
bool test_multi_fork_child;

/* For allocator purposes */
pid_t child_pid  = -1;
__thread pid_t child_tid  = -1;

enum {
	/*
	 * Let the first values be used by individual tests so options don't
	 * conflict with core ones
	 */
	OPT_LIST_SUBTESTS = 500,
	OPT_DESCRIBE_SUBTESTS,
	OPT_RUN_SUBTEST,
	OPT_RUN_DYNAMIC_SUBTEST,
	OPT_DESCRIPTION,
	OPT_DEBUG,
	OPT_INTERACTIVE_DEBUG,
	OPT_SKIP_CRC,
	OPT_TRACE_OOPS,
	OPT_DEVICE,
	OPT_VERSION,
	OPT_HELP = 'h'
};

static int igt_exitcode = IGT_EXIT_SUCCESS;
static const char *command_str;

static char* igt_log_domain_filter;
static struct {
	char *entries[256];
	uint8_t start, end;
} log_buffer;
static pthread_mutex_t log_buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOG_PREFIX_SIZE 32
char log_prefix[LOG_PREFIX_SIZE] = { 0 };

GKeyFile *igt_key_file;

char *igt_frame_dump_path;
char *igt_rc_device;

static bool stderr_needs_sentinel = false;

static int _igt_dynamic_tests_executed = -1;

static void print_backtrace(void)
{
	unw_cursor_t cursor;
	unw_context_t uc;
	int stack_num = 0;

	Dwfl_Callbacks cbs = {
		.find_elf = dwfl_linux_proc_find_elf,
		.find_debuginfo = dwfl_standard_find_debuginfo,
	};

	Dwfl *dwfl = dwfl_begin(&cbs);

	if (dwfl_linux_proc_report(dwfl, getpid())) {
		dwfl_end(dwfl);
		dwfl = NULL;
	} else
		dwfl_report_end(dwfl, NULL, NULL);

	igt_info("Stack trace:\n");

	unw_getcontext(&uc);
	unw_init_local(&cursor, &uc);
	while (unw_step(&cursor) > 0) {
		char name[255];
		unw_word_t off, ip;
		Dwfl_Module *mod = NULL;

		unw_get_reg(&cursor, UNW_REG_IP, &ip);

		if (dwfl)
			mod = dwfl_addrmodule(dwfl, ip);

		if (mod) {
			const char *src, *dwfl_name;
			Dwfl_Line *line;
			int lineno;
			GElf_Sym sym;

			line = dwfl_module_getsrc(mod, ip);
			dwfl_name = dwfl_module_addrsym(mod, ip, &sym, NULL);

			if (line && dwfl_name) {
				src = dwfl_lineinfo(line, NULL, &lineno, NULL, NULL, NULL);
				igt_info("  #%d %s:%d %s()\n", stack_num++, src, lineno, dwfl_name);
				continue;
			}
		}

		if (unw_get_proc_name(&cursor, name, 255, &off) < 0)
			igt_info("  #%d [<unknown>+0x%x]\n", stack_num++,
				 (unsigned int) ip);
		else
			igt_info("  #%d [%s+0x%x]\n", stack_num++, name,
				 (unsigned int) off);
	}

	if (dwfl)
		dwfl_end(dwfl);
}

__attribute__((format(printf, 2, 3)))
static void internal_assert(bool cond, const char *format, ...)
{
	if (!cond) {
		va_list ap;

		va_start(ap, format);
		vfprintf(stderr, format, ap);
		va_end(ap);
		fprintf(stderr, "please refer to lib/igt_core documentation\n");

		print_backtrace();

		assert(0);
	}
}

const char *igt_test_name(void)
{
	return command_str;
}

static void _igt_log_buffer_append(char *line)
{
	pthread_mutex_lock(&log_buffer_mutex);

	free(log_buffer.entries[log_buffer.end]);
	log_buffer.entries[log_buffer.end] = line;
	log_buffer.end++;
	if (log_buffer.end == log_buffer.start)
		log_buffer.start++;

	pthread_mutex_unlock(&log_buffer_mutex);
}

static void _igt_log_buffer_reset(void)
{
	pthread_mutex_lock(&log_buffer_mutex);

	log_buffer.start = log_buffer.end = 0;

	pthread_mutex_unlock(&log_buffer_mutex);
}

static void _log_to_runner_split(int stream, const char *str)
{
	size_t limit = 4096;
	size_t len;
	char *buf = NULL;

	len = strlen(str);

	while (len > limit) {
		if (!buf)
			buf = malloc(limit + 1);

		strncpy(buf, str, limit);
		buf[limit] = '\0';

		send_to_runner(runnerpacket_log(stream, buf));

		str += limit;
		len -= limit;
	}

	send_to_runner(runnerpacket_log(stream, str));
	free(buf);
}

__attribute__((format(printf, 2, 3)))
static void _log_line_fprintf(FILE* stream, const char *format, ...)
{
	va_list ap;
	char *str;

	va_start(ap, format);

	if (runner_connected()) {
		vasprintf(&str, format, ap);
		_log_to_runner_split(fileno(stream), str);
		free(str);
	} else {
		vfprintf(stream, format, ap);
	}
}

enum _subtest_type {
      _SUBTEST_TYPE_NORMAL,
      _SUBTEST_TYPE_DYNAMIC,
};

static void _subtest_result_message(enum _subtest_type subtest_type,
				    const char *name,
				    const char *result,
				    double timeelapsed)
{
	char timestr[32];

	snprintf(timestr, sizeof(timestr), "%.3f", timeelapsed);

	if (runner_connected()) {
		if (subtest_type == _SUBTEST_TYPE_NORMAL)
			send_to_runner(runnerpacket_subtest_result(name, result, timestr, NULL));
		else
			send_to_runner(runnerpacket_dynamic_subtest_result(name, result, timestr, NULL));

		return;
	}

	printf("%s%s %s: %s (%ss)%s\n",
	       (!__igt_plain_output) ? "\x1b[1m" : "",
	       subtest_type == _SUBTEST_TYPE_NORMAL ? "Subtest" : "Dynamic subtest",
	       name,
	       result,
	       timestr,
	       (!__igt_plain_output) ? "\x1b[0m" : "");
	fflush(stdout);
	if (stderr_needs_sentinel)
		fprintf(stderr, "%s %s: %s (%ss)\n",
			subtest_type == _SUBTEST_TYPE_NORMAL ? "Subtest" : "Dynamic subtest",
			name,
			result,
			timestr);
}

static void _subtest_starting_message(enum _subtest_type subtest_type,
				      const char *name)
{
	if (runner_connected()) {
		if (subtest_type == _SUBTEST_TYPE_NORMAL)
			send_to_runner(runnerpacket_subtest_start(name));
		else
			send_to_runner(runnerpacket_dynamic_subtest_start(name));

		return;
	}

	igt_info("Starting %s: %s\n",
		 subtest_type == _SUBTEST_TYPE_NORMAL ? "subtest" : "dynamic subtest",
		 name);
	fflush(stdout);
	if (stderr_needs_sentinel)
		fprintf(stderr, "Starting %s: %s\n",
			subtest_type == _SUBTEST_TYPE_NORMAL ? "subtest" : "dynamic subtest",
			name);
}

static void _igt_log_buffer_dump(void)
{
	uint8_t i;

	if (in_subtest && !in_dynamic_subtest && _igt_dynamic_tests_executed >= 0) {
		/*
		 * We're exiting a subtest with dynamic subparts and
		 * we're reaching this function because a dynamic
		 * subpart failed which automatically translates to
		 * the subtest failing. There cannot be anything in
		 * the log buffer that is of any use to anyone; The
		 * dynamic subpart that failed has already printed out
		 * the real reason for the failure, and dumping the
		 * buffer at this point will only cause the last
		 * executed dynamic part be incorrectly marked as
		 * 'WARN' by igt_runner.
		 */
		_igt_log_buffer_reset();
		return;
	}

	if (in_dynamic_subtest)
		_log_line_fprintf(stderr, "Dynamic subtest %s failed.\n", in_dynamic_subtest);
	else if (in_subtest)
		_log_line_fprintf(stderr, "Subtest %s failed.\n", in_subtest);
	else
		_log_line_fprintf(stderr, "Test %s failed.\n", command_str);

	if (log_buffer.start == log_buffer.end) {
		_log_line_fprintf(stderr, "No log.\n");
		return;
	}

	pthread_mutex_lock(&log_buffer_mutex);
	_log_line_fprintf(stderr, "**** DEBUG ****\n");

	i = log_buffer.start;
	do {
		char *last_line = log_buffer.entries[i];
		_log_line_fprintf(stderr, "%s", last_line);
		i++;
	} while (i != log_buffer.start && i != log_buffer.end);

	/* reset the buffer */
	log_buffer.start = log_buffer.end = 0;

	_log_line_fprintf(stderr, "****  END  ****\n");
	pthread_mutex_unlock(&log_buffer_mutex);
}

/**
 * igt_log_buffer_inspect:
 *
 * Provides a way to replay the internal igt log buffer for inspection.
 * @check: A user-specified handler that gets invoked for each line of
 *         the log buffer. The handler should return true to stop
 *         inspecting the rest of the buffer.
 * @data: passed as a user argument to the inspection function.
 */
void igt_log_buffer_inspect(igt_buffer_log_handler_t check, void *data)
{
	uint8_t i;
	pthread_mutex_lock(&log_buffer_mutex);

	i = log_buffer.start;
	do {
		if (check(log_buffer.entries[i], data))
			break;
		i++;
	} while (i != log_buffer.start && i != log_buffer.end);

	pthread_mutex_unlock(&log_buffer_mutex);
}

void igt_kmsg(const char *format, ...)
{
	va_list ap;
	FILE *file;

	file = fopen("/dev/kmsg", "w");
	if (file == NULL)
		return;

	va_start(ap, format);
	vfprintf(file, format, ap);
	va_end(ap);

	fclose(file);
}

void igt_trace(const char *format, ...)
{
	char path[128];
	va_list ap;
	FILE *file;

	snprintf(path, sizeof(path), "%s/tracing/trace_marker",
		 igt_debugfs_mount());

	file = fopen(path, "w");
	if (file == NULL)
		return;

	va_start(ap, format);
	vfprintf(file, format, ap);
	va_end(ap);

	fclose(file);
}

#define time_valid(ts) ((ts)->tv_sec || (ts)->tv_nsec)

double igt_time_elapsed(struct timespec *then,
			struct timespec *now)
{
	double elapsed = -1.;

	if (time_valid(then) && time_valid(now)) {
		elapsed = now->tv_sec - then->tv_sec;
		elapsed += (now->tv_nsec - then->tv_nsec) * 1e-9;
	}

	return elapsed;
}

int igt_gettime(struct timespec *ts)
{
	memset(ts, 0, sizeof(*ts));
	errno = 0;

	/* Stay on the same clock for consistency. */
	if (igt_clock != (clockid_t)-1) {
		if (clock_gettime(igt_clock, ts))
			goto error;
		return 0;
	}

#ifdef CLOCK_MONOTONIC_RAW
	if (!clock_gettime(igt_clock = CLOCK_MONOTONIC_RAW, ts))
		return 0;
#endif
#ifdef CLOCK_MONOTONIC_COARSE
	if (!clock_gettime(igt_clock = CLOCK_MONOTONIC_COARSE, ts))
		return 0;
#endif
	if (!clock_gettime(igt_clock = CLOCK_MONOTONIC, ts))
		return 0;
error:
	igt_warn("Could not read monotonic time: %s\n",
		 strerror(errno));

	return -errno;
}

uint64_t igt_nsec_elapsed(struct timespec *start)
{
	struct timespec now;

	igt_gettime(&now);
	if ((start->tv_sec | start->tv_nsec) == 0) {
		*start = now;
		return 0;
	}

	return ((now.tv_nsec - start->tv_nsec) +
		(uint64_t)NSEC_PER_SEC*(now.tv_sec - start->tv_sec));
}

void __igt_assert_in_outer_scope(void)
{
	internal_assert(!in_subtest,
			"must only be called outside of a subtest\n");
}

bool __igt_fixture(void)
{
	internal_assert(!in_fixture,
			"nesting multiple igt_fixtures is invalid\n");
	internal_assert(!in_subtest,
			"nesting igt_fixture in igt_subtest is invalid\n");
	internal_assert(test_with_subtests,
			"igt_fixture in igt_simple_main is invalid\n");

	if (igt_only_list_subtests())
		return false;

	if (skip_subtests_henceforth)
		return false;

	in_fixture = true;
	return true;
}

void __igt_fixture_complete(void)
{
	assert(in_fixture);

	in_fixture = false;
}

void __igt_fixture_end(void)
{
	assert(in_fixture);

	in_fixture = false;
	siglongjmp(igt_subtest_jmpbuf, 1);
}

/*
 * If the test takes out the machine, in addition to the usual dmesg
 * spam, the kernel may also emit a "death rattle" -- extra debug
 * information that is overkill for normal successful tests, but
 * vital for post-mortem analysis.
 */
static void ftrace_dump_on_oops(bool enable)
{
	int fd;

	fd = open("/proc/sys/kernel/ftrace_dump_on_oops", O_WRONLY);
	if (fd < 0)
		return;

	/*
	 * If we fail, we do not get the death rattle we wish, but we
	 * still want to run the tests anyway.
	 */
	igt_ignore_warn(write(fd, enable ? "1\n" : "0\n", 2));
	close(fd);
}

bool igt_exit_called;
bool igt_is_aborting;
static void common_exit_handler(int sig)
{
	if (!igt_only_list_subtests()) {
		bind_fbcon(true);
	}

	/* When not killed by a signal check that igt_exit() has been properly
	 * called. */
	assert(sig != 0 || igt_exit_called || igt_is_aborting);
}

static void print_line_wrapping(const char *indent, const char *text)
{
	char *copy, *curr, *next_space;
	int current_line_length = 0;
	bool done = false;

	const int total_line_length = 80;
	const int line_length = total_line_length - strlen(indent);

	copy = malloc(strlen(text) + 1);
	memcpy(copy, text, strlen(text) + 1);

	curr = copy;

	printf("%s", indent);

	while (!done) {
		next_space = strchr(curr, ' ');

		if (!next_space) { /* no more spaces, print everything that is left */
			done = true;
			next_space = strchr(curr, '\0');
		}

		*next_space = '\0';

		if ((next_space - curr) + current_line_length > line_length && curr != copy) {
			printf("\n%s", indent);
			current_line_length = 0;
		}

		if (current_line_length == 0)
			printf("%s", curr); /* first word in a line, don't space out */
		else
			printf(" %s", curr);

		current_line_length += next_space - curr;
		curr = next_space + 1;
	}

	printf("\n");

	free(copy);
}


static void print_test_description(void)
{
	if (&__igt_test_description) {
		print_line_wrapping("", __igt_test_description);
		if (describe_subtests)
			printf("\n");
	}
}

static void print_version(void)
{
	struct utsname uts;

	if (list_subtests)
		return;

	uname(&uts);

	if (runner_connected()) {
		char versionstr[256];

		snprintf(versionstr, sizeof(versionstr),
			 "IGT-Version: %s-%s (%s) (%s: %s %s)\n", PACKAGE_VERSION,
			 IGT_GIT_SHA1, TARGET_CPU_PLATFORM,
			 uts.sysname, uts.release, uts.machine);
		send_to_runner(runnerpacket_versionstring(versionstr));
	} else {
		igt_info("IGT-Version: %s-%s (%s) (%s: %s %s)\n", PACKAGE_VERSION,
			 IGT_GIT_SHA1, TARGET_CPU_PLATFORM,
			 uts.sysname, uts.release, uts.machine);
	}
}

static void print_usage(const char *help_str, bool output_on_stderr)
{
	FILE *f = output_on_stderr ? stderr : stdout;

	fprintf(f, "Usage: %s [OPTIONS]\n", command_str);
	fprintf(f, "  --list-subtests\n"
		   "  --run-subtest <pattern>\n"
		   "  --dynamic-subtest <pattern>\n"
		   "  --debug[=log-domain]\n"
		   "  --interactive-debug[=domain]\n"
		   "  --skip-crc-compare\n"
		   "  --trace-on-oops\n"
		   "  --help-description\n"
		   "  --describe\n"
		   "  --device filters\n"
		   "  --version\n"
		   "  --help|-h\n");
	if (help_str)
		fprintf(f, "%s\n", help_str);
}


static void oom_adjust_for_doom(void)
{
	int fd;
	const char always_kill[] = "1000";

	fd = open("/proc/self/oom_score_adj", O_WRONLY);
	igt_assert(fd != -1);
	igt_assert(write(fd, always_kill, sizeof(always_kill)) == sizeof(always_kill));
	close(fd);

}

/**
 * load_igtrc:
 *
 * Load .igtrc from the path pointed to by #IGT_CONFIG_PATH or from
 * home directory if that is not set. The returned keyfile needs to be
 * deallocated using g_key_file_free().
 *
 * Returns: Pointer to the keyfile, NULL on error.
 */
GKeyFile *igt_load_igtrc(void)
{
	char *key_file_env = NULL;
	char *key_file_loc = NULL;
	GError *error = NULL;
	GKeyFile *file;
	int ret;

	/* Determine igt config path */
	key_file_env = getenv("IGT_CONFIG_PATH");
	if (key_file_env) {
		key_file_loc = key_file_env;
	} else {
		key_file_loc = malloc(100);
		snprintf(key_file_loc, 100, "%s/.igtrc", g_get_home_dir());
	}

	/* Load igt config file */
	file = g_key_file_new();
	ret = g_key_file_load_from_file(file, key_file_loc,
					G_KEY_FILE_NONE, &error);
	if (!ret) {
		g_error_free(error);
		g_key_file_free(file);
		file = NULL;

		goto out;
	}

	g_clear_error(&error);

 out:
	if (!key_file_env && key_file_loc)
		free(key_file_loc);

	return file;
}

static void common_init_config(void)
{
	GError *error = NULL;
	int ret = 0;

	igt_key_file = igt_load_igtrc();

	if (igt_key_file && !igt_frame_dump_path)
		igt_frame_dump_path =
			g_key_file_get_string(igt_key_file, "Common",
					      "FrameDumpPath", &error);

	g_clear_error(&error);

	if (igt_key_file)
		ret = g_key_file_get_integer(igt_key_file, "DUT", "SuspendResumeDelay",
					     &error);
	assert(!error || error->code != G_KEY_FILE_ERROR_INVALID_VALUE);

	g_clear_error(&error);

	if (ret != 0)
		igt_set_autoresume_delay(ret);

	/* Adding filters, order .igtrc, IGT_DEVICE, --device filter */
	if (igt_device_filter_count() > 0)
		igt_debug("Notice: using --device filters:\n");
	else {
		if (igt_rc_device) {
			igt_debug("Notice: using IGT_DEVICE env:\n");
		} else {
			if (igt_key_file)
				igt_rc_device =	g_key_file_get_string(igt_key_file,
								      "Common",
								      "Device", &error);
			g_clear_error(&error);
			if (igt_rc_device)
				igt_debug("Notice: using .igtrc "
					  "Common::Device:\n");
		}
		if (igt_rc_device) {
			igt_device_filter_add(igt_rc_device);
			free(igt_rc_device);
			igt_rc_device = NULL;
		}
	}

	for (int i = 0; i < igt_device_filter_count(); i++)
		igt_debug("[%s]\n", igt_device_filter_get(i));
}

static void common_init_env(void)
{
	const char *env;

	if (!isatty(STDOUT_FILENO) || getenv("IGT_PLAIN_OUTPUT"))
		__igt_plain_output = true;

	errno = 0; /* otherwise may be either ENOTTY or EBADF because of isatty */

	if (!__igt_plain_output)
		setlocale(LC_ALL, "");

	env = getenv("IGT_LOG_LEVEL");
	if (env) {
		if (strcmp(env, "debug") == 0)
			igt_log_level = IGT_LOG_DEBUG;
		else if (strcmp(env, "info") == 0)
			igt_log_level = IGT_LOG_INFO;
		else if (strcmp(env, "warn") == 0)
			igt_log_level = IGT_LOG_WARN;
		else if (strcmp(env, "none") == 0)
			igt_log_level = IGT_LOG_NONE;
	}

	igt_frame_dump_path = getenv("IGT_FRAME_DUMP_PATH");

	stderr_needs_sentinel = getenv("IGT_SENTINEL_ON_STDERR") != NULL;

	env = getenv("IGT_FORCE_DRIVER");
	if (env) {
		__set_forced_driver(env);
	}

	env = getenv("IGT_DEVICE");
	if (env) {
		igt_rc_device = strdup(env);
	}

	env = getenv("IGT_RUNNER_SOCKET_FD");
	if (env) {
		set_runner_socket(atoi(env));
	}
}

static int common_init(int *argc, char **argv,
		       const char *extra_short_opts,
		       const struct option *extra_long_opts,
		       const char *help_str,
		       igt_opt_handler_t extra_opt_handler,
		       void *handler_data)
{
	int c, option_index = 0, i, x;
	static struct option long_options[] = {
		{"list-subtests",     no_argument,       NULL, OPT_LIST_SUBTESTS},
		{"describe",          optional_argument, NULL, OPT_DESCRIBE_SUBTESTS},
		{"run-subtest",       required_argument, NULL, OPT_RUN_SUBTEST},
		{"dynamic-subtest",   required_argument, NULL, OPT_RUN_DYNAMIC_SUBTEST},
		{"help-description",  no_argument,       NULL, OPT_DESCRIPTION},
		{"debug",             optional_argument, NULL, OPT_DEBUG},
		{"interactive-debug", optional_argument, NULL, OPT_INTERACTIVE_DEBUG},
		{"skip-crc-compare",  no_argument,       NULL, OPT_SKIP_CRC},
		{"trace-on-oops",     no_argument,       NULL, OPT_TRACE_OOPS},
		{"device",            required_argument, NULL, OPT_DEVICE},
		{"version",           no_argument,       NULL, OPT_VERSION},
		{"help",              no_argument,       NULL, OPT_HELP},
		{0, 0, 0, 0}
	};
	char *short_opts;
	const char *std_short_opts = "h";
	size_t std_short_opts_len = strlen(std_short_opts);
	struct option *combined_opts;
	int extra_opt_count;
	int all_opt_count;
	int ret = 0;

	common_init_env();
	IGT_INIT_LIST_HEAD(&subgroup_descriptions);

	command_str = argv[0];
	if (strrchr(command_str, '/'))
		command_str = strrchr(command_str, '/') + 1;

	/* Check for conflicts and calculate space for passed-in extra long options */
	for  (extra_opt_count = 0; extra_long_opts && extra_long_opts[extra_opt_count].name; extra_opt_count++) {
		char *conflicting_char;

		/* check for conflicts with standard long option values */
		for (i = 0; long_options[i].name; i++) {
			if (0 == strcmp(extra_long_opts[extra_opt_count].name, long_options[i].name)) {
				igt_critical("Conflicting extra long option defined --%s\n", long_options[i].name);
				assert(0);

			}

			if (extra_long_opts[extra_opt_count].val == long_options[i].val) {
				igt_critical("Conflicting long option 'val' representation between --%s and --%s\n",
					     extra_long_opts[extra_opt_count].name,
					     long_options[i].name);
				assert(0);
			}
		}

		/* check for conflicts with standard short options */
		if (extra_long_opts[extra_opt_count].val != ':'
		    && (conflicting_char = memchr(std_short_opts, extra_long_opts[extra_opt_count].val, std_short_opts_len))) {
			igt_critical("Conflicting long and short option 'val' representation between --%s and -%c\n",
				     extra_long_opts[extra_opt_count].name,
				     *conflicting_char);
			assert(0);
		}
	}

	/* check for conflicts in extra short options*/
	for (i = 0; extra_short_opts && extra_short_opts[i]; i++) {
		if (extra_short_opts[i] == ':')
			continue;

		/* check for conflicts with standard short options */
		if (memchr(std_short_opts, extra_short_opts[i], std_short_opts_len)) {
			igt_critical("Conflicting short option: -%c\n", std_short_opts[i]);
			assert(0);
		}

		/* check for conflicts with standard long option values */
		for (x = 0; long_options[x].name; x++) {
			if (long_options[x].val == extra_short_opts[i]) {
				igt_critical("Conflicting short option and long option 'val' representation: --%s and -%c\n",
					     long_options[x].name, extra_short_opts[i]);
				assert(0);
			}
		}
	}

	all_opt_count = extra_opt_count + ARRAY_SIZE(long_options);

	combined_opts = malloc(all_opt_count * sizeof(*combined_opts));
	if (extra_opt_count > 0)
		memcpy(combined_opts, extra_long_opts,
		       extra_opt_count * sizeof(*combined_opts));

	/* Copy the subtest long options (and the final NULL entry) */
	memcpy(&combined_opts[extra_opt_count], long_options,
		ARRAY_SIZE(long_options) * sizeof(*combined_opts));

	ret = asprintf(&short_opts, "%s%s",
		       extra_short_opts ? extra_short_opts : "",
		       std_short_opts);
	assert(ret >= 0);

	while ((c = getopt_long(*argc, argv, short_opts, combined_opts,
			       &option_index)) != -1) {
		switch(c) {
		case OPT_INTERACTIVE_DEBUG:
			if (optarg && strlen(optarg) > 0)
				igt_interactive_debug = strdup(optarg);
			else
				igt_interactive_debug = "all";
			break;
		case OPT_DEBUG:
			igt_log_level = IGT_LOG_DEBUG;
			if (optarg && strlen(optarg) > 0)
				igt_log_domain_filter = strdup(optarg);
			break;
		case OPT_LIST_SUBTESTS:
			if (!run_single_subtest)
				list_subtests = true;
			break;
		case OPT_DESCRIBE_SUBTESTS:
			if (optarg)
				run_single_subtest = strdup(optarg);
			list_subtests = true;
			describe_subtests = true;
			print_test_description();
			break;
		case OPT_RUN_SUBTEST:
			assert(optarg);
			if (!list_subtests)
				run_single_subtest = strdup(optarg);
			break;
		case OPT_RUN_DYNAMIC_SUBTEST:
			assert(optarg);
			if (!list_subtests)
				run_single_dynamic_subtest = strdup(optarg);
			break;
		case OPT_DESCRIPTION:
			print_test_description();
			ret = -1;
			goto out;
		case OPT_SKIP_CRC:
			igt_skip_crc_compare = true;
			break;
		case OPT_TRACE_OOPS:
			show_ftrace = true;
			break;
		case OPT_DEVICE:
			assert(optarg);
			/* if set by env IGT_DEVICE we need to free it */
			if (igt_rc_device) {
				free(igt_rc_device);
				igt_rc_device = NULL;
			}
			igt_device_filter_add(optarg);
			break;
		case OPT_VERSION:
			print_version();
			ret = -1;
			goto out;
		case OPT_HELP:
			print_usage(help_str, false);
			ret = -1;
			goto out;
		case '?':
			print_usage(help_str, true);
			ret = -2;
			goto out;
		default:
			ret = extra_opt_handler(c, option_index, handler_data);
			if (ret)
				goto out;
		}
	}

	common_init_config();

out:
	free(short_opts);
	free(combined_opts);

	/* exit immediately if this test has no subtests and a subtest or the
	 * list of subtests has been requested */
	if (!test_with_subtests) {
		if (run_single_subtest) {
			igt_warn("Unknown subtest: %s\n", run_single_subtest);
			exit(IGT_EXIT_INVALID);
		}
		if (list_subtests)
			exit(IGT_EXIT_INVALID);
	}

	if (ret < 0)
		/* exit with no error for -h/--help */
		exit(ret == -1 ? 0 : IGT_EXIT_INVALID);

	if (!list_subtests) {
		bind_fbcon(false);
		igt_kmsg(KMSG_INFO "%s: executing\n", command_str);
		print_version();

		sync();
		oom_adjust_for_doom();
		ftrace_dump_on_oops(show_ftrace);
	}

	/* install exit handler, to ensure we clean up */
	igt_install_exit_handler(common_exit_handler);

	if (!test_with_subtests)
		igt_gettime(&subtest_time);

	for (i = 0; (optind + i) < *argc; i++)
		argv[i + 1] = argv[optind + i];

	*argc = *argc - optind + 1;

	return ret;
}


/**
 * igt_subtest_init_parse_opts:
 * @argc: argc from the test's main()
 * @argv: argv from the test's main()
 * @extra_short_opts: getopt_long() compliant list with additional short options
 * @extra_long_opts: getopt_long() compliant list with additional long options
 * @help_str: help string for the additional options
 * @extra_opt_handler: handler for the additional options
 * @handler_data: user data given to @extra_opt_handler when invoked
 *
 * This function handles the subtest related command line options and allows an
 * arbitrary set of additional options. This is useful for tests which have
 * additional knobs to tune when run manually like the number of rounds execute
 * or the size of the allocated buffer objects.
 *
 * Tests should use #igt_main_args instead of their own main()
 * function and calling this function.
 *
 * The @help_str parameter is printed directly after the help text of
 * standard arguments. The formatting of the string should be:
 * - One line per option
 * - Two spaces, option flag, tab character, help text, newline character
 *
 * Example: "  -s\tBuffer size\n"
 *
 * The opt handler function must return #IGT_OPT_HANDLER_SUCCESS on
 * successful handling, #IGT_OPT_HANDLER_ERROR on errors.
 *
 * Returns: Forwards any option parsing errors from getopt_long.
 */
int igt_subtest_init_parse_opts(int *argc, char **argv,
				const char *extra_short_opts,
				const struct option *extra_long_opts,
				const char *help_str,
				igt_opt_handler_t extra_opt_handler,
				void *handler_data)
{
	int ret;

	test_with_subtests = true;
	ret = common_init(argc, argv, extra_short_opts, extra_long_opts,
			  help_str, extra_opt_handler, handler_data);

	return ret;
}

enum igt_log_level igt_log_level = IGT_LOG_INFO;

/**
 * igt_simple_init_parse_opts:
 * @argc: argc from the test's main()
 * @argv: argv from the test's main()
 * @extra_short_opts: getopt_long() compliant list with additional short options
 * @extra_long_opts: getopt_long() compliant list with additional long options
 * @help_str: help string for the additional options
 * @extra_opt_handler: handler for the additional options
 * @handler_data: user data given to @extra_opt_handler when invoked
 *
 * This initializes a simple test without any support for subtests and allows
 * an arbitrary set of additional options. This is useful for tests which have
 * additional knobs to tune when run manually like the number of rounds execute
 * or the size of the allocated buffer objects.
 *
 * Tests should use #igt_simple_main_args instead of their own main()
 * function and calling this function.
 *
 * The @help_str parameter is printed directly after the help text of
 * standard arguments. The formatting of the string should be:
 * - One line per option
 * - Two spaces, option flag, tab character, help text, newline character
 *
 * Example: "  -s\tBuffer size\n"
 *
 * The opt handler function must return #IGT_OPT_HANDLER_SUCCESS on
 * successful handling, #IGT_OPT_HANDLER_ERROR on errors.
 */
void igt_simple_init_parse_opts(int *argc, char **argv,
				const char *extra_short_opts,
				const struct option *extra_long_opts,
				const char *help_str,
				igt_opt_handler_t extra_opt_handler,
				void *handler_data)
{
	common_init(argc, argv, extra_short_opts, extra_long_opts, help_str,
		    extra_opt_handler, handler_data);
}

static void _clear_current_description(void) {
	__current_description[0] = '\0';
}

static void __igt_print_description(const char *subtest_name, const char *file, int line)
{
	struct description_node *desc;
	const char indent[] = "  ";
	bool has_doc = false;


	printf("SUB %s %s:%d:\n", subtest_name, file, line);

	igt_list_for_each_entry(desc, &subgroup_descriptions, link) {
		print_line_wrapping(indent, desc->desc);
		printf("\n");
		has_doc = true;
	}

	if (__current_description[0] != '\0') {
		print_line_wrapping(indent, __current_description);
		printf("\n");
		has_doc = true;
	}

	if (!has_doc)
		printf("%sNO DOCUMENTATION!\n\n", indent);
}

static bool valid_name_for_subtest(const char *subtest_name)
{
	int i;

	/* check the subtest name only contains a-z, A-Z, 0-9, '-' and '_' */
	for (i = 0; subtest_name[i] != '\0'; i++)
		if (subtest_name[i] != '_' && subtest_name[i] != '-'
		    && !isalnum(subtest_name[i]))
			return false;

	return true;
}

/*
 * Note: Testcases which use these helpers MUST NOT output anything to stdout
 * outside of places protected by igt_run_subtest checks - the piglit
 * runner adds every line to the subtest list.
 */
bool __igt_run_subtest(const char *subtest_name, const char *file, const int line)
{
	internal_assert(!igt_can_fail(),
			"igt_subtest can be nested only in igt_main"
			" or igt_subtest_group\n");

	if (!valid_name_for_subtest(subtest_name)) {
		igt_critical("Invalid subtest name \"%s\".\n",
			     subtest_name);
		igt_exit();
	}

	if (run_single_subtest) {
		if (uwildmat(subtest_name, run_single_subtest) == 0) {
			_clear_current_description();
			return false;
		} else {
			run_single_subtest_found = true;
		}
	}

	if (describe_subtests) {
		__igt_print_description(subtest_name, file, line);
		_clear_current_description();
		return false;
	} else if (list_subtests) {
		printf("%s\n", subtest_name);
		return false;
	}


	if (skip_subtests_henceforth) {
		_subtest_result_message(_SUBTEST_TYPE_NORMAL, subtest_name,
					skip_subtests_henceforth == SKIP ? "SKIP" : "FAIL",
					0.0);
		return false;
	}

	igt_kmsg(KMSG_INFO "%s: starting subtest %s\n",
		 command_str, subtest_name);
	_subtest_starting_message(_SUBTEST_TYPE_NORMAL, subtest_name);

	_igt_log_buffer_reset();
	igt_thread_clear_fail_state();

	igt_gettime(&subtest_time);
	return (in_subtest = subtest_name);
}

bool __igt_run_dynamic_subtest(const char *dynamic_subtest_name)
{
	internal_assert(in_subtest && _igt_dynamic_tests_executed >= 0,
			"igt_dynamic is allowed only inside igt_subtest_with_dynamic\n");
	internal_assert(!in_dynamic_subtest,
			"igt_dynamic is not allowed to be nested in another igt_dynamic\n");

	if (!valid_name_for_subtest(dynamic_subtest_name)) {
			igt_critical("Invalid dynamic subtest name \"%s\".\n",
				     dynamic_subtest_name);
			igt_exit();
	}

	if (run_single_dynamic_subtest &&
	    uwildmat(dynamic_subtest_name, run_single_dynamic_subtest) == 0)
		return false;

	igt_kmsg(KMSG_INFO "%s: starting dynamic subtest %s\n",
		 command_str, dynamic_subtest_name);
	_subtest_starting_message(_SUBTEST_TYPE_DYNAMIC, dynamic_subtest_name);

	_igt_log_buffer_reset();
	igt_thread_clear_fail_state();

	_igt_dynamic_tests_executed++;

	igt_gettime(&dynamic_subtest_time);
	return (in_dynamic_subtest = dynamic_subtest_name);
}

/**
 * igt_subtest_name:
 *
 * Returns: The name of the currently executed subtest or NULL if called from
 * outside a subtest block.
 */
const char *igt_subtest_name(void)
{
	return in_subtest;
}

/**
 * igt_only_list_subtests:
 *
 * Returns: Returns true if only subtest should be listed and any setup code
 * must be skipped, false otherwise.
 */
bool igt_only_list_subtests(void)
{
	return list_subtests;
}



void __igt_subtest_group_save(int *save, int *desc)
{
	internal_assert(test_with_subtests,
			"igt_subtest_group is not allowed in igt_simple_main\n");

	if (__current_description[0] != '\0') {
		struct description_node *new = calloc(1, sizeof(*new));
		memcpy(new->desc, __current_description, sizeof(__current_description));
		igt_list_add_tail(&new->link, &subgroup_descriptions);
		_clear_current_description();
		*desc = true;
	}

	*save = skip_subtests_henceforth;
}

void __igt_subtest_group_restore(int save, int desc)
{
	if (desc) {
		struct description_node *last =
			igt_list_last_entry(&subgroup_descriptions, last, link);
		igt_list_del(&last->link);
		free(last);
	}

	skip_subtests_henceforth = save;
}

static bool skipped_one = false;
static bool succeeded_one = false;
static bool failed_one = false;
static bool dynamic_failed_one = false;

bool __igt_enter_dynamic_container(void)
{
	_igt_dynamic_tests_executed = 0;
	dynamic_failed_one = false;

	return true;
}

static void kill_and_wait(pid_t *pids, int size, int signum)
{
	for (int c = 0; c < size; c++) {
		if (pids[c] > 0) {
			kill(pids[c], signum);
			waitpid(pids[c], NULL, 0); /* don't leave zombies! */
		}
	}
}

__noreturn static void exit_subtest(const char *result)
{
	struct timespec now;
	const char **subtest_name = in_dynamic_subtest ? &in_dynamic_subtest : &in_subtest;
	struct timespec *thentime = in_dynamic_subtest ? &dynamic_subtest_time : &subtest_time;
	jmp_buf *jmptarget = in_dynamic_subtest ? &igt_dynamic_jmpbuf : &igt_subtest_jmpbuf;

	igt_gettime(&now);

	if (test_multi_fork_child)
		__igt_plain_output = true;

	_subtest_result_message(in_dynamic_subtest ? _SUBTEST_TYPE_DYNAMIC : _SUBTEST_TYPE_NORMAL,
				*subtest_name,
				result,
				igt_time_elapsed(thentime, &now));
	igt_terminate_spins();

	/* If the subtest aborted, it may have left children behind */
	for (int c = 0; c < num_test_children; c++) {
		if (test_children[c] > 0) {
			kill(test_children[c], SIGKILL);
			waitpid(test_children[c], NULL, 0); /* don't leave zombies! */
		}
	}
	num_test_children = 0;
	if (!test_multi_fork_child && num_test_multi_fork_children > 0)
		kill_and_wait(test_multi_fork_children, num_test_multi_fork_children, SIGKILL);

	num_test_multi_fork_children = 0;

	/*
	 * When test completes - mostly in fail state it can leave allocated
	 * objects. An allocator is not an exception as it is global IGT
	 * entity and when test will allocate some ranges and then it will
	 * fail no free/close likely will be called (controling potential
	 * fails and clearing before assertions in IGT is not common).
	 *
	 * We call intel_allocator_init() then to prepare the allocator
	 * infrastructure from scratch for each test. Init also removes
	 * remnants from previous allocator run (if any).
	 */
	intel_allocator_init();
	intel_bb_reinit_allocator();
	gem_pool_init();

	if (!in_dynamic_subtest)
		_igt_dynamic_tests_executed = -1;

	/*
	 * Don't keep the above text in the log if exiting a dynamic
	 * subsubtest, the subtest would print it again otherwise.
	 * Also don't keep it if called from multi_fork.
	 */
	if (in_dynamic_subtest || test_multi_fork_child)
		_igt_log_buffer_reset();

	*subtest_name = NULL;

	siglongjmp(*jmptarget, 1);
}

/**
 * igt_skip:
 * @f: format string
 * @...: optional arguments used in the format string
 *
 * Subtest aware test skipping. The format string is printed to stderr as the
 * reason why the test skipped.
 *
 * For tests with subtests this will either bail out of the current subtest or
 * mark all subsequent subtests as SKIP (presuming some global setup code
 * failed).
 *
 * For normal tests without subtest it will directly exit.
 */
void igt_skip(const char *f, ...)
{
	va_list args;
	skipped_one = true;

	internal_assert(!test_child,
			"skips are not allowed in forks\n");
	internal_assert(!test_multi_fork_child,
			"skips are not allowed in multi_fork\n");

	if (!igt_only_list_subtests()) {
		va_start(args, f);
		if (runner_connected()) {
			char *str;

			vasprintf(&str, f, args);
			send_to_runner(runnerpacket_log(STDOUT_FILENO, str));
			free(str);
		} else {
			vprintf(f, args);
		}
		va_end(args);
	}

	if (in_subtest) {
		if (in_dynamic_subtest) {
			/*
			 * Don't count skipping dynamic subtests, for
			 * the purposes of getting the result of the
			 * containing subtest.
			 */
			_igt_dynamic_tests_executed--;
		}
		exit_subtest("SKIP");
	} else if (test_with_subtests) {
		skip_subtests_henceforth = SKIP;
		internal_assert(in_fixture,
			"skipping is allowed only in fixtures, subtests"
			" or igt_simple_main\n");
		__igt_fixture_end();
	} else {
		igt_exitcode = IGT_EXIT_SKIP;
		igt_exit();
	}
}

void __igt_skip_check(const char *file, const int line,
		      const char *func, const char *check,
		      const char *f, ...)
{
	va_list args;
	int err = errno;
	char *err_str = NULL;

	if (!igt_thread_is_main())
		assert(!"igt_require/skip allowed only in the main thread!");

	if (err)
		igt_assert_neq(asprintf(&err_str, "Last errno: %i, %s\n", err, strerror(err)),
			       -1);

	if (f) {
		static char *buf;

		/* igt_skip never returns, so try to not leak too badly. */
		if (buf)
			free(buf);

		va_start(args, f);
		igt_assert_neq(vasprintf(&buf, f, args), -1);
		va_end(args);

		igt_skip("Test requirement not met in function %s, file %s:%i:\n"
			 "Test requirement: %s\n%s"
			 "%s",
			 func, file, line, check, buf, err_str ?: "");
	} else {
		igt_skip("Test requirement not met in function %s, file %s:%i:\n"
			 "Test requirement: %s\n"
			 "%s",
			 func, file, line, check, err_str ?: "");
	}
}

/**
 * igt_success:
 *
 * Complete a (subtest) as successful
 *
 * This bails out of a subtests and marks it as successful. For global tests it
 * it won't bail out of anything.
 */
void igt_success(void)
{
	igt_thread_assert_no_failures();

	if (in_subtest && !in_dynamic_subtest && _igt_dynamic_tests_executed >= 0) {
		/*
		 * We're exiting a dynamic container, yield a result
		 * according to the dynamic tests that got
		 * executed.
		 */
		if (dynamic_failed_one)
			igt_fail(IGT_EXIT_FAILURE);

		if (_igt_dynamic_tests_executed == 0)
			igt_skip("No dynamic tests executed.\n");
	}

	if (!in_dynamic_subtest)
		succeeded_one = true;

	if (in_subtest)
		exit_subtest("SUCCESS");
}

/**
 * igt_fail:
 * @exitcode: exitcode
 *
 * Fail a testcase. The exitcode is used as the exit code of the test process.
 * It may not be 0 (which indicates success) or 77 (which indicates a skipped
 * test).
 *
 * For tests with subtests this will either bail out of the current subtest or
 * mark all subsequent subtests as FAIL (presuming some global setup code
 * failed).
 *
 * For normal tests without subtest it will directly exit with the given
 * exitcode.
 */
void igt_fail(int exitcode)
{
	assert(exitcode != IGT_EXIT_SUCCESS && exitcode != IGT_EXIT_SKIP);

	if (!igt_thread_is_main()) {
		igt_thread_fail();
		pthread_exit(NULL);
	}

	igt_debug_wait_for_keypress("failure");

	/* Exit immediately if the test is already exiting and igt_fail is
	 * called. This can happen if an igt_assert fails in an exit handler */
	if (in_atexit_handler)
		_exit(IGT_EXIT_FAILURE);

	if (in_dynamic_subtest) {
		dynamic_failed_one = true;
	} else {
		/* Dynamic subtest containers must not fail explicitly */
		assert(_igt_dynamic_tests_executed < 0 || dynamic_failed_one);

		if (!failed_one)
			igt_exitcode = exitcode;

		failed_one = true;
	}

	/* Silent exit, parent will do the yelling. */
	if (test_child)
		exit(exitcode);

	_igt_log_buffer_dump();

	if (test_multi_fork_child)
		exit(exitcode);

	if (in_subtest) {
		exit_subtest("FAIL");
	} else {
		internal_assert(igt_can_fail(), "failing test is only allowed"
				" in fixtures, subtests and igt_simple_main\n");

		if (in_fixture) {
			skip_subtests_henceforth = FAIL;
			__igt_fixture_end();
		}

		igt_exit();
	}
}

/**
 * igt_fatal_error: Stop test execution on fatal errors
 *
 * Stop test execution or optionally, if the IGT_REBOOT_ON_FATAL_ERROR
 * environment variable is set, reboot the machine.
 *
 * Since out test runner (piglit) does support fatal test exit codes, we
 * implement the default behaviour by waiting endlessly.
 */
void igt_fatal_error(void)
{
	if (igt_check_boolean_env_var("IGT_REBOOT_ON_FATAL_ERROR", false)) {
		igt_warn("FATAL ERROR - REBOOTING\n");
		igt_sysrq_reboot();
	} else {
		igt_warn("FATAL ERROR\n");
		for (;;)
			pause();
	}
}


/**
 * igt_can_fail:
 *
 * Returns true if called from either an #igt_fixture, #igt_subtest or a
 * testcase without subtests, i.e. #igt_simple_main. Returns false otherwise. In
 * other words, it checks whether it's legal to call igt_fail(), igt_skip_on()
 * and all the convenience macros build around those.
 *
 * This is useful to make sure that library code is called from the right
 * places.
 */
bool igt_can_fail(void)
{
	return !test_with_subtests || in_fixture || in_subtest;
}

/**
 * igt_describe_f:
 * @fmt: format string containing description
 * @...: argument used by the format string
 *
 * Attach a description to the following #igt_subtest or #igt_subtest_group
 * block.
 *
 * Check #igt_describe for more details.
 *
 */
void igt_describe_f(const char *fmt, ...)
{
	int ret;
	va_list args;

	internal_assert(!in_subtest || _igt_dynamic_tests_executed < 0,
			"documenting dynamic subsubtests is impossible,"
			" document the subtest instead.\n");

	if (!describe_subtests)
		return;

	va_start(args, fmt);

	ret = vsnprintf(__current_description, sizeof(__current_description), fmt, args);

	va_end(args);

	assert(ret < sizeof(__current_description));
}

static bool is_gdb(pid_t pid)
{
	char pathname[30], buf[1024];
	ssize_t len;

	sprintf(pathname, "/proc/%d/exe", pid);
	len = readlink(pathname, buf, sizeof(buf) - 1);
	if (len < 0)
		return false;

	buf[len] = '\0';

	return strncmp(basename(buf), "gdb", 3) == 0;
}

static pid_t tracer_pid(void)
{
	char pathname[30];
	pid_t pid = 0;
	FILE *f;

	sprintf(pathname, "/proc/%d/status", getpid());

	f = fopen(pathname, "r");
	if (!f)
		return getppid();

	for (;;) {
		char buf[32];
		char *s;

		s = fgets(buf, sizeof(buf), f);
		if (!s)
			break;

		if (sscanf(s, "TracerPid: %d", &pid) == 1)
			break;
	}

	fclose(f);

	return pid ?: getppid();
}

/*
 * By default gdb will only track a single process. To make
 * it track all of them, and let them all run simultaneously
 * one needs the following incantations:
 *   set detach-on-fork off
 *   set schedule-multiple on
 */
static bool running_under_gdb(void)
{
	return is_gdb(tracer_pid());
}

static void __write_stderr(const char *str, size_t len)
{
	if (runner_connected())
		log_to_runner_sig_safe(str, len);
	else
		igt_ignore_warn(write(STDERR_FILENO, str, len));
}

static void write_stderr(const char *str)
{
	__write_stderr(str, strlen(str));
}

static const char hex[] = "0123456789abcdef";

static void
xputch(int c)
{
	if (runner_connected())
		log_to_runner_sig_safe((const void *) &c, 1);
	else
		igt_ignore_warn(write(STDERR_FILENO, (const void *) &c, 1));
}

static int
xpow(int base, int pow)
{
	int i, r = 1;

	for (i = 0; i < pow; i++)
		r *= base;

	return r;
}

static void
printnum(unsigned long long num, unsigned base)
{
	int i = 0;
	unsigned long long __num = num;

	/* determine from where we should start dividing */
	do {
		__num /= base;
		i++;
	} while (__num);

	while (i--)
		xputch(hex[num / xpow(base, i) % base]);
}

static size_t
xstrlcpy(char *dst, const char *src, size_t size)
{
	char *dst_in;

	dst_in = dst;
	if (size > 0) {
		while (--size > 0 && *src != '\0')
			*dst++ = *src++;
		*dst = '\0';
	}

	return dst - dst_in;
}

static void
xprintfmt(const char *fmt, va_list ap)
{
	const char *p;
	int ch, base;
	unsigned long long num;

	while (1) {
		while ((ch = *(const unsigned char *) fmt++) != '%') {
			if (ch == '\0') {
				return;
			}
			xputch(ch);
		}

		ch = *(const unsigned char *) fmt++;
		switch (ch) {
		/* character */
		case 'c':
			xputch(va_arg(ap, int));
			break;
		/* string */
		case 's':
			if ((p = va_arg(ap, char *)) == NULL) {
				p = "(null)";
			}

			for (; (ch = *p++) != '\0';) {
				if (ch < ' ' || ch > '~') {
					xputch('?');
				} else {
					xputch(ch);
				}
			}
			break;
		/* (signed) decimal */
		case 'd':
			num = va_arg(ap, int);
			if ((long long) num < 0) {
				xputch('-');
				num = -(long long) num;
			}
			base = 10;
			goto number;
		/* unsigned decimal */
		case 'u':
			num = va_arg(ap, unsigned int);
			base = 10;
			goto number;
		/* (unsigned) hexadecimal */
		case 'x':
			num = va_arg(ap, unsigned int);
			base = 16;
number:
			printnum(num, base);
			break;

		/* The following are not implemented */

		/* width field */
		case '1': case '2':
		case '3': case '4':
		case '5': case '6':
		case '7': case '8':
		case '9':
		case '.': case '#':
		/* long */
		case 'l':
		/* octal */
		case 'o':
		/* pointer */
		case 'p':
		/* float */
		case 'f':
			abort();
		/* escaped '%' character */
		case '%':
			xputch(ch);
			break;
		/* unrecognized escape sequence - just print it literally */
		default:
			xputch('%');
			for (fmt--; fmt[-1] != '%'; fmt--)
				; /* do nothing */
			break;
		}
	}
}

/* async-safe printf */
static void
xprintf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	xprintfmt(fmt, ap);
	va_end(ap);
}

static void print_backtrace_sig_safe(void)
{
	unw_cursor_t cursor;
	unw_context_t uc;
	int stack_num = 0;

	write_stderr("Stack trace: \n");

	unw_getcontext(&uc);
	unw_init_local(&cursor, &uc);
	while (unw_step(&cursor) > 0) {
		char name[255];
		unw_word_t off;

		if (unw_get_proc_name(&cursor, name, 255, &off) < 0)
			xstrlcpy(name, "<unknown>", 10);

		xprintf(" #%d [%s+0x%x]\n", stack_num++, name,
				(unsigned int) off);

	}
}

void __igt_fail_assert(const char *domain, const char *file, const int line,
		       const char *func, const char *assertion,
		       const char *f, ...)
{
	va_list args;
	int err = errno;

	igt_log(domain, IGT_LOG_CRITICAL,
		"Test assertion failure function %s, file %s:%i:\n", func, file,
		line);
	igt_log(domain, IGT_LOG_CRITICAL, "Failed assertion: %s\n", assertion);
	if (err)
		igt_log(domain, IGT_LOG_CRITICAL, "Last errno: %i, %s\n", err,
			strerror(err));

	if (f) {
		va_start(args, f);
		igt_vlog(domain, IGT_LOG_CRITICAL, f, args);
		va_end(args);
	}

	print_backtrace();

	if (running_under_gdb())
		abort();
	igt_fail(IGT_EXIT_FAILURE);
}

void igt_kill_children(int signal)
{
	for (int c = 0; c < num_test_children; c++) {
		if (test_children[c] > 0)
			kill(test_children[c], signal);
	}

	for (int c = 0; c < num_test_multi_fork_children; c++) {
		if (test_multi_fork_children[c] > 0)
			kill(test_multi_fork_children[c], signal);
	}
}

void __igt_abort(const char *domain, const char *file, const int line,
		 const char *func, const char *expression,
		 const char *f, ...)
{
	va_list args;
	int err = errno;

	igt_is_aborting = true;

	igt_log(domain, IGT_LOG_CRITICAL,
		"Test abort in function %s, file %s:%i:\n", func, file,
		line);
	igt_log(domain, IGT_LOG_CRITICAL, "abort condition: %s\n", expression);
	if (err)
		igt_log(domain, IGT_LOG_CRITICAL, "Last errno: %i, %s\n", err,
			strerror(err));

	if (f) {
		va_start(args, f);
		igt_vlog(domain, IGT_LOG_CRITICAL, f, args);
		va_end(args);
	}

	/* just try our best, we are aborting the execution anyway */
	igt_kill_children(SIGKILL);

	print_backtrace();

	if (running_under_gdb())
		abort();

	_igt_log_buffer_dump();

	exit(IGT_EXIT_ABORT);
}

/**
 * igt_exit:
 *
 * exit() for both types (simple and with subtests) of i-g-t tests.
 *
 * This will exit the test with the right exit code when subtests have been
 * skipped. For normal tests it exits with a successful exit code, presuming
 * everything has worked out. For subtests it also checks that at least one
 * subtest has been run (save when only listing subtests.
 *
 * It is an error to normally exit a test calling igt_exit() - without it the
 * result reporting will be wrong. To avoid such issues it is highly recommended
 * to use #igt_main or #igt_simple_main instead of a hand-rolled main() function.
 */
void igt_exit(void)
{
	int tmp;

	if (!test_with_subtests)
		igt_thread_assert_no_failures();

	igt_exit_called = true;

	if (igt_key_file)
		g_key_file_free(igt_key_file);

	if (run_single_subtest && !run_single_subtest_found) {
		igt_critical("Unknown subtest: %s\n", run_single_subtest);
		exit(IGT_EXIT_INVALID);
	}

	if (igt_only_list_subtests())
		exit(IGT_EXIT_SUCCESS);

	/* Calling this without calling one of the above is a failure */
	assert(!test_with_subtests ||
	       skipped_one ||
	       succeeded_one ||
	       failed_one);

	if (test_with_subtests && !failed_one) {
		if (succeeded_one)
			igt_exitcode = IGT_EXIT_SUCCESS;
		else
			igt_exitcode = IGT_EXIT_SKIP;
	}

	if (!test_multi_fork_child) {
		/* parent will do the yelling */
		if (command_str)
			igt_kmsg(KMSG_INFO "%s: exiting, ret=%d\n",
				 command_str, igt_exitcode);
		igt_debug("Exiting with status code %d\n", igt_exitcode);
	}

	igt_kill_children(SIGKILL);
	assert(!num_test_children);
	assert(!num_test_multi_fork_children);

	assert(waitpid(-1, &tmp, WNOHANG) == -1 && errno == ECHILD);

	if (!test_with_subtests) {
		struct timespec now;
		const char *result;

		igt_gettime(&now);

		switch (igt_exitcode) {
			case IGT_EXIT_SUCCESS:
				result = "SUCCESS";
				break;
			case IGT_EXIT_SKIP:
				result = "SKIP";
				break;
			default:
				result = "FAIL";
		}

		if (test_multi_fork_child) /* parent will do the yelling */
			_log_line_fprintf(stdout, "dyn_child pid:%d (%.3fs) ends with err=%d\n",
					  getpid(), igt_time_elapsed(&subtest_time, &now),
					  igt_exitcode);
		else
			_log_line_fprintf(stdout, "%s (%.3fs)\n",
					  result, igt_time_elapsed(&subtest_time, &now));
	}

	exit(igt_exitcode);
}

/* fork support code */
static int helper_process_count;
static pid_t helper_process_pids[] =
{ -1, -1, -1, -1};

static void reset_helper_process_list(void)
{
	for (int i = 0; i < ARRAY_SIZE(helper_process_pids); i++)
		helper_process_pids[i] = -1;
	helper_process_count = 0;
}

static int __waitpid(pid_t pid)
{
	int status = -1;
	while (waitpid(pid, &status, 0) == -1 &&
	       errno == EINTR)
		;

	return status;
}

static void fork_helper_exit_handler(int sig)
{
	/* Inside a signal handler, play safe */
	for (int i = 0; i < ARRAY_SIZE(helper_process_pids); i++) {
		pid_t pid = helper_process_pids[i];
		if (pid != -1) {
			kill(pid, SIGTERM);
			__waitpid(pid);
			helper_process_count--;
		}
	}

	assert(helper_process_count == 0);
}

bool __igt_fork_helper(struct igt_helper_process *proc)
{
	pid_t pid;
	int id;
	int tmp_count;

	assert(!proc->running);
	assert(helper_process_count < ARRAY_SIZE(helper_process_pids));

	for (id = 0; helper_process_pids[id] != -1; id++)
		;

	igt_install_exit_handler(fork_helper_exit_handler);

	/*
	 * Avoid races when the parent stops the child before the setup code
	 * had a chance to run. This happens e.g. when skipping tests wrapped in
	 * the signal helper.
	 */
	tmp_count = exit_handler_count;
	exit_handler_count = 0;

	/* ensure any buffers are flushed before fork */
	fflush(NULL);

	switch (pid = fork()) {
	case -1:
		exit_handler_count = tmp_count;
		igt_assert(0);
	case 0:
		reset_helper_process_list();
		oom_adjust_for_doom();

		return true;
	default:
		exit_handler_count = tmp_count;
		proc->running = true;
		proc->pid = pid;
		proc->id = id;
		helper_process_pids[id] = pid;
		helper_process_count++;

		return false;
	}

}

/**
 * igt_wait_helper:
 * @proc: #igt_helper_process structure
 *
 * Joins a helper process. It is an error to call this on a helper process which
 * hasn't been spawned yet.
 */
int igt_wait_helper(struct igt_helper_process *proc)
{
	int status;

	assert(proc->running);

	status = __waitpid(proc->pid);

	proc->running = false;

	helper_process_pids[proc->id] = -1;
	helper_process_count--;

	return status;
}

static bool helper_was_alive(struct igt_helper_process *proc,
			     int status)
{
	return (WIFSIGNALED(status) &&
		WTERMSIG(status) == (proc->use_SIGKILL ? SIGKILL : SIGTERM));
}

/**
 * igt_stop_helper:
 * @proc: #igt_helper_process structure
 *
 * Terminates a helper process. It is legal to call this on a helper process
 * which hasn't been spawned yet, e.g. if the helper was skipped due to
 * HW restrictions.
 */
void igt_stop_helper(struct igt_helper_process *proc)
{
	int status;

	if (!proc->running) /* never even started */
		return;

	/* failure here means the pid is already dead and so waiting is safe */
	kill(proc->pid, proc->use_SIGKILL ? SIGKILL : SIGTERM);

	status = igt_wait_helper(proc);
	if (!helper_was_alive(proc, status))
		igt_debug("Helper died too early with status=%d\n", status);
	assert(helper_was_alive(proc, status));
}

static void children_exit_handler(int sig)
{
	int status;

	/* The exit handler can be called from a fatal signal, so play safe */
	while (num_test_children-- && wait(&status))
		;
}

static pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

bool __igt_fork(void)
{
	internal_assert(!test_with_subtests || in_subtest,
			"forking is only allowed in subtests or igt_simple_main\n");
	internal_assert(!test_child,
			"forking is not allowed from already forked children\n");

	igt_install_exit_handler(children_exit_handler);

	if (num_test_children >= test_children_sz) {
		if (!test_children_sz)
			test_children_sz = 4;
		else
			test_children_sz *= 2;

		test_children = realloc(test_children,
					sizeof(pid_t)*test_children_sz);
		igt_assert(test_children);
	}

	/* ensure any buffers are flushed before fork */
	fflush(NULL);

	switch (test_children[num_test_children++] = fork()) {
	case -1:
		num_test_children--; /* so we won't kill(-1) during cleanup */
		igt_assert(0);
	case 0:
		test_child = true;
		pthread_mutex_init(&print_mutex, NULL);
		child_pid = getpid();
		child_tid = -1;
		exit_handler_count = 0;
		reset_helper_process_list();
		oom_adjust_for_doom();
		igt_unshare_spins();

		return true;
	default:
		return false;
	}

}

static void dyn_children_exit_handler(int sig)
{
	int status;

	/* The exit handler can be called from a fatal signal, so play safe */
	while (num_test_multi_fork_children-- && wait(&status))
		;
}

bool __igt_multi_fork(void)
{
	internal_assert(!test_with_subtests || in_subtest,
			"multi-forking is only allowed in subtests or igt_simple_main\n");
	internal_assert(!test_child,
			"multi-forking is not allowed from already forked children\n");
	internal_assert(!test_multi_fork_child,
			"multi-forking is not allowed from already multi-forked children\n");

	if (!num_test_multi_fork_children)
		igt_install_exit_handler(dyn_children_exit_handler);

	if (num_test_multi_fork_children >= test_multi_fork_children_sz) {
		if (!test_multi_fork_children_sz)
			test_multi_fork_children_sz = 4;
		else
			test_multi_fork_children_sz *= 2;

		test_multi_fork_children = realloc(test_multi_fork_children,
					sizeof(pid_t)*test_multi_fork_children_sz);
		igt_assert(test_multi_fork_children);
	}

	/* ensure any buffers are flushed before fork */
	fflush(NULL);

	switch (test_multi_fork_children[num_test_multi_fork_children++] = fork()) {
	case -1:
		num_test_multi_fork_children--; /* so we won't kill(-1) during cleanup */
		igt_assert(0);
	case 0:
		test_multi_fork_child = true;
		snprintf(log_prefix, LOG_PREFIX_SIZE, "<g:%d> ", num_test_multi_fork_children - 1);
		num_test_multi_fork_children = 0; /* only parent should care */
		pthread_mutex_init(&print_mutex, NULL);
		child_pid = getpid(); /* for allocator */
		child_tid = -1; /* for allocator */
		exit_handler_count = 0;
		reset_helper_process_list();
		oom_adjust_for_doom();
		igt_unshare_spins();

		return true;
	default:
		return false;
	}

}

int __igt_waitchildren(void)
{
	int err = 0;
	int count;

	assert(!test_child);

	count = 0;
	while (count < num_test_children) {
		int status = -1;
		pid_t pid;
		int c;

		pid = wait(&status);
		if (pid == -1) {
			if (errno == EINTR)
				continue;

			printf("wait(num_children:%d) failed with %m\n",
			       num_test_children - count);
			return IGT_EXIT_FAILURE;
		}

		for (c = 0; c < num_test_children; c++)
			if (pid == test_children[c])
				break;
		if (c == num_test_children)
			continue;

		if (err == 0 && status != 0) {
			if (WIFEXITED(status)) {
				printf("child %i failed with exit status %i\n",
				       c, WEXITSTATUS(status));
				err = WEXITSTATUS(status);
			} else if (WIFSIGNALED(status)) {
				printf("child %i died with signal %i, %s\n",
				       c, WTERMSIG(status),
				       strsignal(WTERMSIG(status)));
				err = 128 + WTERMSIG(status);
			} else {
				printf("Unhandled failure [%d] in child %i\n", status, c);
				err = 256;
			}

			igt_kill_children(SIGKILL);
		}

		count++;
	}

	num_test_children = 0;
	return err;
}

/**
 * igt_waitchildren:
 *
 * Wait for all children forked with igt_fork.
 *
 * The magic here is that exit codes from children will be correctly propagated
 * to the main thread, including the relevant exit code if a child thread failed.
 * Of course if multiple children failed with different exit codes the resulting
 * exit code will be non-deterministic.
 *
 * Note that igt_skip() will not be forwarded, feature tests need to be done
 * before spawning threads with igt_fork().
 */
void igt_waitchildren(void)
{
	int err;

	if (num_test_multi_fork_children)
		err = __igt_multi_wait();
	else
		err = __igt_waitchildren();

	if (err)
		igt_fail(err);
}

int __igt_multi_wait(void)
{
	int err = 0;
	int count;
	bool was_killed = false;

	assert(!test_multi_fork_child);
	count = 0;
	while (count < num_test_multi_fork_children) {
		int status = -1;
		int last = 0;
		pid_t pid;
		int c;

		pid = wait(&status);
		if (pid == -1) {
			if (errno == EINTR)
				continue;

			igt_debug("wait(multi_fork children running:%d) failed with %m\n",
				  num_test_multi_fork_children - count);
			return IGT_EXIT_FAILURE;
		}

		for (c = 0; c < num_test_multi_fork_children; c++)
			if (pid == test_multi_fork_children[c])
				break;
		if (c == num_test_multi_fork_children)
			continue;

		if (status != 0) {
			if (WIFEXITED(status)) {
				printf("dynamic child %i pid:%d failed with exit status %i\n",
				       c, pid, WEXITSTATUS(status));
				last = WEXITSTATUS(status);
				test_multi_fork_children[c] = -1;
			} else if (WIFSIGNALED(status)) {
				printf("dynamic child %i pid:%d died with signal %i, %s\n",
				       c, pid, WTERMSIG(status),
				       strsignal(WTERMSIG(status)));
				last = 128 + WTERMSIG(status);
				test_multi_fork_children[c] = -1;
			} else {
				printf("Unhandled failure [%d] in dynamic child %i pid:%d\n",
				       status, c, pid);
				last = 256;
			}

			/* we don't want to overwrite error with skip */
			if (err == 0 || err == IGT_EXIT_SKIP)
				err = last;
			if (err && err != IGT_EXIT_SKIP && !was_killed) {
				igt_kill_children(SIGKILL); // if non-skip happen
				was_killed = true;
			}
		}

		count++;
	}

	num_test_multi_fork_children = 0;

	return err;
}

static void igt_alarm_killchildren(int signal)
{
	igt_info("Timed out waiting for children\n");

	igt_kill_children(SIGKILL);
}

/**
 * igt_waitchildren_timeout:
 * @seconds: timeout in seconds to wait
 * @reason: debug string explaining what timedout
 *
 * Wait for all children forked with igt_fork, for a maximum of @seconds. If the
 * timeout expires, kills all children, cleans them up, and then fails by
 * calling igt_fail().
 */
void igt_waitchildren_timeout(int seconds, const char *reason)
{
	struct sigaction sa;
	int ret;

	sa.sa_handler = igt_alarm_killchildren;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	sigaction(SIGALRM, &sa, NULL);

	alarm(seconds);

	if (num_test_multi_fork_children)
		ret = __igt_multi_wait();
	else
		ret = __igt_waitchildren();
	igt_reset_timeout();
	if (ret)
		igt_fail(ret);
}

/* exit handler code */
#define MAX_SIGNALS		32
#define MAX_EXIT_HANDLERS	10

#ifndef HAVE_SIGHANDLER_T
typedef void (*sighandler_t)(int);
#endif

static struct {
	sighandler_t handler;
	bool installed;
} orig_sig[MAX_SIGNALS];

static igt_exit_handler_t exit_handler_fn[MAX_EXIT_HANDLERS];
static bool exit_handler_disabled;
static const struct {
	int number;
	const char *name;
	size_t name_len;
} handled_signals[] = {
#define SIGDEF(x) { x, #x, sizeof(#x) - 1 }
#define SILENT(x) { x, NULL, 0 }

	SILENT(SIGINT),
	SILENT(SIGHUP),
	SILENT(SIGPIPE),
	SILENT(SIGTERM),

	SIGDEF(SIGQUIT), /* used by igt_runner for its external timeout */

	SIGDEF(SIGABRT),
	SIGDEF(SIGSEGV),
	SIGDEF(SIGBUS),
	SIGDEF(SIGFPE)

#undef SILENT
#undef SIGDEF
};

static int install_sig_handler(int sig_num, sighandler_t handler)
{
	orig_sig[sig_num].handler = signal(sig_num, handler);

	if (orig_sig[sig_num].handler == SIG_ERR)
		return -1;

	orig_sig[sig_num].installed = true;

	return 0;
}

static void restore_sig_handler(int sig_num)
{
	/* Just restore the default so that we properly fall over. */
	signal(sig_num, SIG_DFL);
}

static void restore_all_sig_handler(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(orig_sig); i++)
		restore_sig_handler(i);
}

static void call_exit_handlers(int sig)
{
	int i;

	igt_terminate_spins();

	if (!exit_handler_count) {
		return;
	}

	for (i = exit_handler_count - 1; i >= 0; i--)
		exit_handler_fn[i](sig);

	/* ensure we don't get called twice */
	exit_handler_count = 0;
}

static void igt_atexit_handler(void)
{
	in_atexit_handler = true;

	restore_all_sig_handler();

	if (!exit_handler_disabled)
		call_exit_handlers(0);
}

static bool crash_signal(int sig)
{
	switch (sig) {
	case SIGILL:
	case SIGBUS:
	case SIGFPE:
	case SIGSEGV:
		return true;
	default:
		return false;
	}
}

static void fatal_sig_handler(int sig)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(handled_signals); i++) {
		if (handled_signals[i].number != sig)
			continue;

		if (handled_signals[i].name_len) {
			write_stderr("Received signal ");
			__write_stderr(handled_signals[i].name,
				       handled_signals[i].name_len);
			write_stderr(".\n");

			print_backtrace_sig_safe();
		}

		if (crash_signal(sig)) {
			/* Linux standard to return exit code as 128 + signal */
			if (!failed_one)
				igt_exitcode = 128 + sig;
			failed_one = true;

			if (in_subtest)
				exit_subtest("CRASH");
		}
		break;
	}

	restore_all_sig_handler();

	/*
	 * exit_handler_disabled is always false here, since when we set it
	 * we also block signals.
	 */
	call_exit_handlers(sig);

	{
#ifdef __linux__
	/* Workaround cached PID and TID races on glibc and Bionic libc. */
		pid_t pid = syscall(SYS_getpid);
		pid_t tid = gettid();

		syscall(SYS_tgkill, pid, tid, sig);
#else
		pthread_t tid = pthread_self();
		union sigval value = { .sival_ptr = NULL };

		pthread_sigqueue(tid, sig, value);
#endif
        }
}

/**
 * igt_install_exit_handler:
 * @fn: exit handler function
 *
 * Set a handler that will be called either when the process calls exit() or
 * <!-- -->returns from the main function, or one of the signals in
 * 'handled_signals' is raised. MAX_EXIT_HANDLERS handlers can be installed,
 * each of which will be called only once, even if a subsequent signal is
 * raised. If the exit handlers are called due to a signal, the signal will be
 * re-raised with the original signal disposition after all handlers returned.
 *
 * The handler will be passed the signal number if called due to a signal, or
 * 0 otherwise. Exit handlers can also be used from test children spawned with
 * igt_fork(), but not from within helper processes spawned with
 * igt_fork_helper(). The list of exit handlers is reset when forking to
 * avoid issues with children cleanup up the parent's state too early.
 */
void igt_install_exit_handler(igt_exit_handler_t fn)
{
	int i;

	for (i = 0; i < exit_handler_count; i++)
		if (exit_handler_fn[i] == fn)
			return;

	igt_assert(exit_handler_count < MAX_EXIT_HANDLERS);

	exit_handler_fn[exit_handler_count] = fn;
	exit_handler_count++;

	if (exit_handler_count > 1)
		return;

	for (i = 0; i < ARRAY_SIZE(handled_signals); i++) {
		if (install_sig_handler(handled_signals[i].number,
					fatal_sig_handler))
			goto err;
	}

	if (atexit(igt_atexit_handler))
		goto err;

	return;
err:
	restore_all_sig_handler();
	exit_handler_count--;

	igt_assert_f(0, "failed to install the signal handler\n");
}

/* simulation enviroment support */

/**
 * igt_run_in_simulation:
 *
 * This function can be used to select a reduced test set when running in
 * simulation environments. This i-g-t mode is selected by setting the
 * INTEL_SIMULATION environment variable to 1.
 *
 * Returns: True when run in simulation mode, false otherwise.
 */
bool igt_run_in_simulation(void)
{
	static int simulation = -1;

	if (simulation == -1)
		simulation = igt_check_boolean_env_var("INTEL_SIMULATION", false);

	return simulation;
}

/**
 * igt_skip_on_simulation:
 *
 * Skip tests when INTEL_SIMULATION environment variable is set. It uses
 * igt_skip() internally and hence is fully subtest aware.
 *
 * Note that in contrast to all other functions which use igt_skip() internally
 * it is allowed to use this outside of an #igt_fixture block in a test with
 * subtests. This is because in contrast to most other test requirements,
 * checking for simulation mode doesn't depend upon the present hardware and it
 * so makes a lot of sense to have this check in the outermost #igt_main block.
 */
void igt_skip_on_simulation(void)
{
	if (igt_only_list_subtests())
		return;

	if (!igt_can_fail()) {
		igt_fixture
			igt_require(!igt_run_in_simulation());
	} else
		igt_require(!igt_run_in_simulation());
}

/* structured logging */

/**
 * igt_log:
 * @domain: the log domain, or NULL for no domain
 * @level: #igt_log_level
 * @format: format string
 * @...: optional arguments used in the format string
 *
 * This is the generic structured logging helper function. i-g-t testcase should
 * output all normal message to stdout. Warning level message should be printed
 * to stderr and the test runner should treat this as an intermediate result
 * between SUCCESS and FAILURE.
 *
 * The log level can be set through the IGT_LOG_LEVEL environment variable with
 * values "debug", "info", "warn", "critical" and "none". By default verbose
 * debug message are disabled. "none" completely disables all output and is not
 * recommended since crucial issues only reported at the IGT_LOG_WARN level are
 * ignored.
 */
void igt_log(const char *domain, enum igt_log_level level, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	igt_vlog(domain, level, format, args);
	va_end(args);
}

static pthread_key_t __vlog_line_continuation;

igt_constructor {
	pthread_key_create(&__vlog_line_continuation, NULL);
}

/**
 * igt_vlog:
 * @domain: the log domain, or NULL for no domain
 * @level: #igt_log_level
 * @format: format string
 * @args: variable arguments lists
 *
 * This is the generic logging helper function using an explicit varargs
 * structure and hence useful to implement domain-specific logging
 * functions.
 *
 * If there is no need to wrap up a vararg list in the caller it is simpler to
 * just use igt_log().
 */
void igt_vlog(const char *domain, enum igt_log_level level, const char *format, va_list args)
{
	FILE *file;
	char *line, *formatted_line;
	char *thread_id;
	const char *program_name;
	const char *igt_log_level_str[] = {
		"DEBUG",
		"INFO",
		"WARNING",
		"CRITICAL",
		"NONE"
	};

	assert(format);

#ifdef __GLIBC__
	program_name = program_invocation_short_name;
#else
	program_name = command_str;
#endif

	if (igt_thread_is_main()) {
		thread_id = strdup(log_prefix);
	} else {
		if (asprintf(&thread_id, "%s[thread:%d] ", log_prefix, gettid()) == -1)
			thread_id = NULL;
	}

	if (!thread_id)
		return;

	if (list_subtests && level <= IGT_LOG_WARN)
		return;

	if (vasprintf(&line, format, args) == -1)
		return;

	if (pthread_getspecific(__vlog_line_continuation)) {
		formatted_line = strdup(line);
		if (!formatted_line)
			goto out;
	} else if (asprintf(&formatted_line, "(%s:%d) %s%s%s%s: %s", program_name,
		     getpid(), thread_id, (domain) ? domain : "", (domain) ? "-" : "",
		     igt_log_level_str[level], line) == -1) {
		goto out;
	}

	if (line[strlen(line) - 1] == '\n')
		pthread_setspecific(__vlog_line_continuation, (void*) false);
	else
		pthread_setspecific(__vlog_line_continuation, (void*) true);

	/* append log buffer */
	_igt_log_buffer_append(formatted_line);

	/* check print log level */
	if (igt_log_level > level)
		goto out;

	/* check domain filter */
	if (igt_log_domain_filter) {
		/* if null domain and filter is not "application", return */
		if (!domain && strcmp(igt_log_domain_filter, "application"))
			goto out;
		/* else if domain and filter do not match, return */
		else if (domain && strcmp(igt_log_domain_filter, domain))
			goto out;
	}

	pthread_mutex_lock(&print_mutex);

	/* use stderr for warning messages and above */
	if (level >= IGT_LOG_WARN) {
		file = stderr;
		fflush(stdout);
	}
	else
		file = stdout;

	/* prepend all except information messages with process, domain and log
	 * level information */
	if (level != IGT_LOG_INFO) {
		_log_line_fprintf(file, "%s", formatted_line);
	} else {
		_log_line_fprintf(file, "%s%s", thread_id, line);
	}

	pthread_mutex_unlock(&print_mutex);

out:
	free(line);
	free(thread_id);
}

static const char *timeout_op;
__noreturn static void igt_alarm_handler(int signal)
{
	if (timeout_op)
		igt_info("Timed out: %s\n", timeout_op);
	else
		igt_info("Timed out\n");

	/* exit with failure status */
	igt_fail(IGT_EXIT_FAILURE);
}

/**
 * igt_set_timeout:
 * @seconds: number of seconds before timeout
 * @op: Optional string to explain what operation has timed out in the debug log
 *
 * Fail a test and exit with #IGT_EXIT_FAILURE status after the specified
 * number of seconds have elapsed. If the current test has subtests and the
 * timeout occurs outside a subtest, subsequent subtests will be skipped and
 * marked as failed.
 *
 * Any previous timer is cancelled and no timeout is scheduled if @seconds is
 * zero. But for clarity the timeout set with this function should be cleared
 * with igt_reset_timeout().
 */
void igt_set_timeout(unsigned int seconds,
		     const char *op)
{
	struct sigaction sa;

	sa.sa_handler = igt_alarm_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	timeout_op = op;

	if (seconds == 0)
		sigaction(SIGALRM, NULL, NULL);
	else
		sigaction(SIGALRM, &sa, NULL);

	alarm(seconds);
}

/**
 * igt_reset_timeout:
 *
 * This function resets a timeout set by igt_set_timeout() and disables any
 * timer set up by the former function.
 */
void igt_reset_timeout(void)
{
	igt_set_timeout(0, NULL);
}

FILE *__igt_fopen_data(const char* igt_srcdir, const char* igt_datadir,
		       const char* filename)
{
	char path[PATH_MAX];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/%s", igt_datadir, filename);
	fp = fopen(path, "r");
	if (!fp) {
		snprintf(path, sizeof(path), "%s/%s", igt_srcdir, filename);
		fp = fopen(path, "r");
	}
	if (!fp) {
		snprintf(path, sizeof(path), "./%s", filename);
		fp = fopen(path, "r");
	}

	if (!fp)
		igt_critical("Could not open data file \"%s\": %m\n", filename);

	return fp;
}

static void log_output(int *fd, enum igt_log_level level)
{
	ssize_t len;
	char buf[PIPE_BUF];

	if (*fd < 0)
		return;

	memset(buf, 0, sizeof(buf));
	len = read(*fd, buf, sizeof(buf));
	if (len <= 0) {
		close(*fd);
		*fd = -1;
		return;
	}

	igt_log(IGT_LOG_DOMAIN, level, "[cmd] %s", buf);
}

/**
 * igt_system:
 *
 * An improved replacement of the system() call.
 *
 * Executes the shell command specified in @command with the added feature of
 * concurrently capturing its stdout and stderr to igt_log and igt_warn
 * respectively.
 *
 * Returns: The exit status of the executed process. -1 for failure.
 */
int igt_system(const char *command)
{
	int outpipe[2] = { -1, -1 };
	int errpipe[2] = { -1, -1 };
	int status;
	struct igt_helper_process process = {};

	if (pipe(outpipe) < 0)
		goto err;
	if (pipe(errpipe) < 0)
		goto err;

	/*
	 * The clone() system call called from a largish executable has
	 * difficulty to make progress if interrupted too frequently, so
	 * suspend the signal helper for the time of the syscall.
	 */
	igt_suspend_signal_helper();

	igt_fork_helper(&process) {
		close(outpipe[0]);
		close(errpipe[0]);

		if (dup2(outpipe[1], STDOUT_FILENO) < 0)
			goto child_err;
		if (dup2(errpipe[1], STDERR_FILENO) < 0)
			goto child_err;

		execl("/bin/sh", "sh", "-c", command,
		      (char *) NULL);

	child_err:
		exit(EXIT_FAILURE);
	}

	igt_resume_signal_helper();

	close(outpipe[1]);
	close(errpipe[1]);

	while (outpipe[0] >= 0 || errpipe[0] >= 0) {
		log_output(&outpipe[0], IGT_LOG_INFO);
		log_output(&errpipe[0], IGT_LOG_WARN);
	}

	status = igt_wait_helper(&process);

	return WEXITSTATUS(status);
err:
	close(outpipe[0]);
	close(outpipe[1]);
	close(errpipe[0]);
	close(errpipe[1]);
	return -1;
}

/**
 * igt_system_quiet:
 * Similar to igt_system(), except redirect output to /dev/null
 *
 * Returns: The exit status of the executed process. -1 for failure.
 */
int igt_system_quiet(const char *command)
{
	int stderr_fd_copy = -1, stdout_fd_copy = -1, status, nullfd = -1;

	/* redirect */
	if ((nullfd = open("/dev/null", O_WRONLY)) == -1)
		goto err;
	if ((stdout_fd_copy = dup(STDOUT_FILENO)) == -1)
		goto err;
	if ((stderr_fd_copy = dup(STDERR_FILENO)) == -1)
		goto err;

	if (dup2(nullfd, STDOUT_FILENO) == -1)
		goto err;
	if (dup2(nullfd, STDERR_FILENO) == -1)
		goto err;

	/* See igt_system() for the reason for suspending the signal helper. */
	igt_suspend_signal_helper();

	if ((status = system(command)) == -1)
		goto err;

	igt_resume_signal_helper();

	/* restore */
	if (dup2(stdout_fd_copy, STDOUT_FILENO) == -1)
		goto err;
	if (dup2(stderr_fd_copy, STDERR_FILENO) == -1)
		goto err;

	close(stdout_fd_copy);
	close(stderr_fd_copy);
	close(nullfd);

	return WEXITSTATUS(status);
err:
	igt_resume_signal_helper();

	close(stderr_fd_copy);
	close(stdout_fd_copy);
	close(nullfd);

	return -1;
}

/* IGT wrappers around libpciaccess init/cleanup functions */

static void pci_system_exit_handler(int sig)
{
	pci_system_cleanup();
}

static void __pci_system_init(void)
{
	if (!igt_warn_on_f(pci_system_init(), "Could not initialize libpciaccess global data\n"))
		igt_install_exit_handler(pci_system_exit_handler);
}

int igt_pci_system_init(void)
{
	static pthread_once_t once_control = PTHREAD_ONCE_INIT;

	return pthread_once(&once_control, __pci_system_init);
}
