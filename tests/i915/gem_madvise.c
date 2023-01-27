/*
 * Copyright © 2014 Intel Corporation
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
 *    Chris Wilson <chris@chris-wilson.co.uk>
 *
 */

#include "igt.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>

#include "drm.h"
#include "i915/gem_create.h"

IGT_TEST_DESCRIPTION("Checks that the kernel reports EFAULT when trying to use"
		     " purged bo.");

#define OBJECT_SIZE (1024*1024)

/*
 * Testcase: checks that the kernel reports EFAULT when trying to use purged bo
 *
 */

static jmp_buf jmp;

__noreturn static void sigtrap(int sig)
{
	siglongjmp(jmp, sig);
}

static void
dontneed_before_mmap(void)
{
	uint32_t handle;
	char *ptr;
	int fd;

	fd = drm_open_driver(DRIVER_INTEL);

	for_each_mmap_offset_type(fd, t) {
		sighandler_t old_sigsegv, old_sigbus;

		igt_debug("Mapping mode: %s\n", t->name);

		handle = gem_create(fd, OBJECT_SIZE);
		gem_madvise(fd, handle, I915_MADV_DONTNEED);

		ptr = __gem_mmap_offset(fd, handle, 0, OBJECT_SIZE,
					PROT_READ | PROT_WRITE,
					t->type);

		close(fd);
		if (!ptr)
			continue;

		old_sigsegv = signal(SIGSEGV, sigtrap);
		old_sigbus = signal(SIGBUS, sigtrap);
		switch (sigsetjmp(jmp, SIGBUS | SIGSEGV)) {
		case SIGBUS:
			break;
		case 0:
			*ptr = 0;
		default:
			igt_assert(!"reached");
			break;
		}
		munmap(ptr, OBJECT_SIZE);
		signal(SIGBUS, old_sigsegv);
		signal(SIGSEGV, old_sigbus);

		fd = drm_open_driver(DRIVER_INTEL);
	}

	close(fd);
}

static void
dontneed_after_mmap(void)
{
	uint32_t handle;
	char *ptr;
	int fd;

	fd = drm_open_driver(DRIVER_INTEL);

	for_each_mmap_offset_type(fd, t) {
		sighandler_t old_sigsegv, old_sigbus;

		igt_debug("Mapping mode: %s\n", t->name);

		handle = gem_create(fd, OBJECT_SIZE);

		ptr = __gem_mmap_offset(fd, handle, 0, OBJECT_SIZE,
					PROT_READ | PROT_WRITE,
					t->type);

		gem_madvise(fd, handle, I915_MADV_DONTNEED);
		close(fd);
		if (!ptr)
			continue;

		old_sigsegv = signal(SIGSEGV, sigtrap);
		old_sigbus = signal(SIGBUS, sigtrap);
		switch (sigsetjmp(jmp, SIGBUS | SIGSEGV)) {
		case SIGBUS:
			break;
		case 0:
			*ptr = 0;
		default:
			igt_assert(!"reached");
			break;
		}
		munmap(ptr, OBJECT_SIZE);
		signal(SIGBUS, old_sigbus);
		signal(SIGSEGV, old_sigsegv);

		fd = drm_open_driver(DRIVER_INTEL);
	}

	close(fd);
}

static void
dontneed_before_pwrite(void)
{
	int fd = drm_open_driver(DRIVER_INTEL);
	uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle;

	gem_require_pread_pwrite(fd);
	handle = gem_create(fd, OBJECT_SIZE);
	gem_madvise(fd, handle, I915_MADV_DONTNEED);

	igt_assert_eq(__gem_write(fd, handle, 0, &bbe, sizeof(bbe)), -EFAULT);

	close(fd);
}

static void
dontneed_before_exec(void)
{
	int fd = drm_open_driver(DRIVER_INTEL);
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 exec;
	uint32_t buf[] = { MI_BATCH_BUFFER_END, 0 };

	gem_require_pread_pwrite(fd);
	memset(&execbuf, 0, sizeof(execbuf));
	memset(&exec, 0, sizeof(exec));

	exec.handle = gem_create(fd, OBJECT_SIZE);
	gem_write(fd, exec.handle, 0, buf, sizeof(buf));
	gem_madvise(fd, exec.handle, I915_MADV_DONTNEED);

	execbuf.buffers_ptr = to_user_pointer(&exec);
	execbuf.buffer_count = 1;
	execbuf.batch_len = sizeof(buf);
	igt_assert_eq(__gem_execbuf(fd, &execbuf), -EFAULT);

	close(fd);
}

igt_main
{
	igt_describe("Check signal for Segmentation Fault and bus error before"
		     " obtaining a purgeable object and calling for sighandler.");
	igt_subtest("dontneed-before-mmap")
		dontneed_before_mmap();

	igt_describe("Check signal for Segmentation Fault and bus error after"
		     " obtaining a purgeable object and calling for sighandler.");
	igt_subtest("dontneed-after-mmap")
		dontneed_after_mmap();

	igt_describe("Check if PWRITE reports EFAULT when trying to use purged bo"
		     " for write operation.");
	igt_subtest("dontneed-before-pwrite")
		dontneed_before_pwrite();

	igt_describe("Check if EXECBUFFER2 reports EFAULT when trying to submit"
		     " purged bo for GPU.");
	igt_subtest("dontneed-before-exec")
		dontneed_before_exec();
}
