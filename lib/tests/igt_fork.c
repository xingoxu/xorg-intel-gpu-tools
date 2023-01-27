/*
 * Copyright © 2019 Intel Corporation
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
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "igt_core.h"
#include "drmtest.h"

#include "igt_tests_common.h"

char test[] = "test";
char *fake_argv[] = { test };
int fake_argc = ARRAY_SIZE(fake_argv);
int fork_type_dyn;

__noreturn static void igt_fork_vs_skip(void)
{
	igt_simple_init(fake_argc, fake_argv);

	if (fork_type_dyn) {
		igt_multi_fork(i, 1) {
			igt_skip("skipping multi-fork");
		}
	} else {
		igt_fork(i, 1) {
			igt_skip("skipping fork");
		}
	}

	igt_waitchildren();

	igt_exit();
}

__noreturn static void igt_fork_vs_assert(void)
{
	igt_simple_init(fake_argc, fake_argv);

	if (fork_type_dyn) {
		igt_multi_fork(i, 1) {
			igt_assert(0);
		}
	} else {
		igt_fork(i, 1) {
			igt_assert(0);
		}
	}

	igt_waitchildren();

	igt_exit();
}

__noreturn static void igt_fork_leak(void)
{
	igt_simple_init(fake_argc, fake_argv);

	if (fork_type_dyn) {
		igt_multi_fork(i, 1) {
			sleep(10);
		}
	} else {
		igt_fork(i, 1) {
			sleep(10);
		}
	}

	igt_exit();
}

__noreturn static void plain_fork_leak(void)
{
	int pid;

	igt_simple_init(fake_argc, fake_argv);

	switch (pid = fork()) {
	case -1:
		internal_assert(0);
	case 0:
		sleep(1);
	default:
		exit(0);
	}

	igt_exit();
}

__noreturn static void igt_fork_timeout_leak(void)
{
	igt_simple_init(fake_argc, fake_argv);

	if (fork_type_dyn) {
		igt_multi_fork(i, 1) {
			sleep(10);
		}
	} else {
		igt_fork(i, 1) {
			sleep(10);
		}
	}

	igt_waitchildren_timeout(1, "library test");

	igt_exit();
}

__noreturn static void subtest_leak(void)
{
	pid_t *children =
		mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	const int num_children = 4096 / sizeof(*children);

	igt_subtest_init(fake_argc, fake_argv);

	igt_subtest("fork-leak") {
		if (fork_type_dyn) {
			igt_multi_fork(child, num_children)
				children[child] = getpid();
		} else {
			igt_fork(child, num_children)
				children[child] = getpid();
		}

		/* leak the children */
		igt_assert(0);
	}

	/* We expect the exit_subtest to cleanup after the igt_fork and igt_multi_fork */
	for (int i = 0; i < num_children; i++) {
		if (children[i] > 0)
			assert(kill(children[i], 0) == -1 && errno == ESRCH);
	}

	munmap(children, 4096);

	igt_exit();
}

int main(int argc, char **argv)
{
	int ret;

	for (fork_type_dyn = 0;	fork_type_dyn <= 1; ++fork_type_dyn) {
		printf("Checking %sfork ...\n", fork_type_dyn ? "multi-" : "");

		printf("\ncheck that igt_assert is forwarded\n");
		ret = do_fork(igt_fork_vs_assert);
		internal_assert_wexited(ret, IGT_EXIT_FAILURE);

		printf("\ncheck that igt_skip within a fork blows up\n");
		ret = do_fork(igt_fork_vs_skip);
		internal_assert_wexited(ret, SIGABRT + 128);

		printf("\ncheck that failure to clean up fails\n");
		ret = do_fork(igt_fork_leak);
		internal_assert_wsignaled(ret, SIGABRT);

		printf("\ncheck that igt_waitchildren_timeout cleans up\n");
		ret = do_fork(igt_fork_timeout_leak);
		internal_assert_wexited(ret, SIGKILL + 128);

		printf("\ncheck that any other process leaks are caught\n");
		ret = do_fork(plain_fork_leak);
		internal_assert_wsignaled(ret, SIGABRT);

		printf("\ncheck subtest leak %d\n", fork_type_dyn);
		ret = do_fork(subtest_leak);
		internal_assert_wexited(ret, IGT_EXIT_FAILURE); /* not asserted! */
	}

	printf("SUCCESS all tests passed\n");
}
