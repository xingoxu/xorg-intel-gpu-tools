/*
 * Copyright © 2012 Intel Corporation
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
 *    Ben Widawsky <ben@bwidawsk.net>
 *
 */

#include <sys/poll.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"
#include "igt_vgem.h"

IGT_TEST_DESCRIPTION("Tests the GEM_WAIT ioctl");

static int __gem_wait(int fd, struct drm_i915_gem_wait *w)
{
	int err;

	err = 0;
	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_WAIT, w))
		err = -errno;

	return err;
}

static void invalid_flags(int fd)
{
	struct drm_i915_gem_wait wait;

	memset(&wait, 0, sizeof(wait));
	wait.bo_handle = gem_create(fd, 4096);
	wait.timeout_ns = 1;
	/* NOTE: This test intentionally tests for just the next available flag.
	 * Don't "fix" this testcase without the ABI testcases for new flags
	 * first. */
	wait.flags = 1;

	igt_assert_eq(__gem_wait(fd, &wait), -EINVAL);

	gem_close(fd, wait.bo_handle);
}

static void invalid_buf(int fd)
{
	struct drm_i915_gem_wait wait;

	memset(&wait, 0, sizeof(wait));
	igt_assert_eq(__gem_wait(fd, &wait), -ENOENT);
}

#define BUSY 1
#define HANG 2
#define AWAIT 4
#define WRITE 8

#define timespec_isset(x) ((x)->tv_sec | (x)->tv_nsec)

static void basic(int fd, const intel_ctx_t *ctx, unsigned engine,
		  unsigned flags)
{
	uint64_t ahnd = get_reloc_ahnd(fd, ctx->id);
	IGT_CORK_HANDLE(cork);
	uint32_t plug =
		flags & (WRITE | AWAIT) ? igt_cork_plug(&cork, fd) : 0;
	igt_spin_t *spin =
		igt_spin_new(fd,
			     .ahnd = ahnd,
			     .ctx = ctx,
			     .engine = engine,
			     .dependency = plug,
			     .flags = (flags & HANG) ? IGT_SPIN_NO_PREEMPTION : 0);
	struct drm_i915_gem_wait wait = {
		flags & WRITE ? plug : spin->handle
	};

	igt_assert_eq(__gem_wait(fd, &wait), -ETIME);

	if (flags & BUSY) {
		struct timespec tv = {};
		int timeout;

		timeout = 120;
		if ((flags & HANG) == 0) {
			igt_spin_set_timeout(spin, NSEC_PER_SEC/2);
			timeout = 1;
		}

		if (flags & (WRITE | AWAIT))
			igt_cork_unplug(&cork);

		igt_assert_eq(__gem_wait(fd, &wait), -ETIME);

		while (__gem_wait(fd, &wait) == -ETIME &&
		       igt_seconds_elapsed(&tv) < timeout)
			;

		if ((flags & HANG) == 0 && !timespec_isset(&spin->last_signal))
			igt_warn("spinner not terminated, expired? %d!\n",
				 poll(&(struct pollfd){ spin->timerfd, POLLIN }, 1, 0));

		igt_assert_eq(__gem_wait(fd, &wait), 0);
	} else {
		wait.timeout_ns = NSEC_PER_SEC / 2; /* 0.5s */
		igt_assert_eq(__gem_wait(fd, &wait), -ETIME);
		igt_assert_eq_s64(wait.timeout_ns, 0);

		if (flags & (WRITE | AWAIT))
			igt_cork_unplug(&cork);

		wait.timeout_ns = 0;
		igt_assert_eq(__gem_wait(fd, &wait), -ETIME);

		if ((flags & HANG) == 0) {
			igt_spin_set_timeout(spin, NSEC_PER_SEC/2);
			wait.timeout_ns = NSEC_PER_SEC; /* 1.0s */
			igt_assert_eq(__gem_wait(fd, &wait), 0);
			igt_assert(wait.timeout_ns >= 0);
		} else {
			wait.timeout_ns = -1;
			igt_assert_eq(__gem_wait(fd, &wait), 0);
			igt_assert(wait.timeout_ns == -1);
		}

		wait.timeout_ns = 0;
		igt_assert_eq(__gem_wait(fd, &wait), 0);
		igt_assert(wait.timeout_ns == 0);
	}

	if (plug)
		gem_close(fd, plug);
	igt_spin_free(fd, spin);
	put_ahnd(ahnd);
}

static void test_all_engines(const char *name, int i915, const intel_ctx_t *ctx,
			     unsigned int test)
{
	const struct intel_execution_engine2 *e;

	igt_subtest_with_dynamic(name) {
		igt_dynamic("all") {
			gem_quiescent_gpu(i915);
			basic(i915, ctx, ALL_ENGINES, test);
			gem_quiescent_gpu(i915);
		}

		for_each_ctx_engine(i915, ctx, e) {
			igt_dynamic_f("%s", e->name) {
				gem_quiescent_gpu(i915);
				basic(i915, ctx, e->flags, test);
				gem_quiescent_gpu(i915);
			}
		}
	}
}

igt_main
{
	const intel_ctx_t *ctx;
	int fd = -1;

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		igt_require_gem(fd);
		ctx = intel_ctx_create_all_physical(fd);
	}

	igt_describe("Verify that GEM_WAIT called with invalid flag will fail.");
	igt_subtest("invalid-flags")
		invalid_flags(fd);

	igt_describe("Verify that GEM_WAIT called with invalid buffer object will fail.");
	igt_subtest("invalid-buf")
		invalid_buf(fd);

	igt_subtest_group {
		static const struct {
			const char *name;
			unsigned int flags;
		} tests[] = {
			{ "busy", BUSY },
			{ "wait", 0 },
			{ "await", AWAIT },
			{ "write-busy", BUSY | WRITE },
			{ "write-wait", WRITE },
			{ }
		};

		igt_fixture {
			igt_fork_hang_detector(fd);
			igt_fork_signal_helper();
		}

		for (const typeof(*tests) *t = tests; t->name; t++) {
			igt_describe_f("Verify GEM_WAIT functionality in"
				       " %s mode.", t->name);
			test_all_engines(t->name, fd, ctx, t->flags);
		}

		igt_fixture {
			igt_stop_signal_helper();
			igt_stop_hang_detector();
		}
	}

	igt_subtest_group {
		static const struct {
			const char *name;
			unsigned int flags;
		} tests[] = {
			{ "hang-busy", HANG | BUSY },
			{ "hang-wait", HANG },
			{ "hang-busy-write", HANG | WRITE  | BUSY},
			{ "hang-wait-write", HANG | WRITE },
			{ }
		};
		igt_hang_t hang;

		igt_fixture {
			hang = igt_allow_hang(fd, ctx->id, 0);
			igt_fork_signal_helper();
		}

		for (const typeof(*tests) *t = tests; t->name; t++) {
			igt_describe_f("Verify GEM_WAIT functionality in %s mode,"
				       " when hang is allowed.", (t->name+5));
			test_all_engines(t->name, fd, ctx, t->flags);
		}

		igt_fixture {
			igt_stop_signal_helper();
			igt_disallow_hang(fd, hang);
		}
	}

	igt_fixture {
		intel_ctx_destroy(fd, ctx);
		close(fd);
	}
}
