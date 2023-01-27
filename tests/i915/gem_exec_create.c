/*
 * Copyright © 2011 Intel Corporation
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

#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>

#include "drm.h"
#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_ring.h"
#include "igt.h"

#include "i915_drm.h"
#include "i915/intel_memory_region.h"

IGT_TEST_DESCRIPTION("This test overloads the driver with transient active objects"
		     " and checks if we don't kill the system under the memory pressure"
		     " some of the symptoms this test look for include mysterious hangs.");

#define ENGINE_FLAGS  (I915_EXEC_RING_MASK | I915_EXEC_BSD_MASK)

static double elapsed(const struct timespec *start, const struct timespec *end)
{
	return ((end->tv_sec - start->tv_sec) +
		(end->tv_nsec - start->tv_nsec)*1e-9);
}

#define ENGINES (1 << 0)
#define LEAK (1 << 1)

static void all(int fd, unsigned flags, int timeout, int ncpus, uint32_t region)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;
	unsigned engines[I915_EXEC_RING_MASK + 1], nengine;
	const intel_ctx_t *ctx;

	nengine = 0;
	if (flags & ENGINES) { /* Modern API to iterate over *all* engines */
		const struct intel_execution_engine2 *e;

		ctx = intel_ctx_create_all_physical(fd);

		for_each_ctx_engine(fd, ctx, e)
			engines[nengine++] = e->flags;

		/* Note: modifies engine map on context 0 */
	} else {
		ctx = intel_ctx_0(fd);

		for_each_physical_ring(e, fd)
			engines[nengine++] = eb_ring(e);
	}
	igt_require(nengine);

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create_in_memory_regions(fd, 4096, region);
	gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.flags |= I915_EXEC_HANDLE_LUT;
	execbuf.flags |= I915_EXEC_NO_RELOC;
	execbuf.rsvd1 = ctx->id;
	if (__gem_execbuf(fd, &execbuf)) {
		execbuf.flags = 0;
		gem_execbuf(fd, &execbuf);
	}
	gem_sync(fd, obj.handle);
	gem_close(fd, obj.handle);

	intel_detect_and_clear_missed_interrupts(fd);
	igt_fork(child, ncpus) {
		struct timespec start, now;
		unsigned long count;
		double time;

		count = 0;
		clock_gettime(CLOCK_MONOTONIC, &start);
		do {
			for (int n = 0; n < nengine; n++) {
				obj.handle = gem_create_in_memory_regions(fd, 4096, region);
				gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));
				execbuf.flags &= ~ENGINE_FLAGS;
				execbuf.flags |= engines[n];
				gem_execbuf(fd, &execbuf);
				if (flags & LEAK)
					gem_madvise(fd, obj.handle, I915_MADV_DONTNEED);
				else
					gem_close(fd, obj.handle);
			}
			count += nengine;
			clock_gettime(CLOCK_MONOTONIC, &now);
		} while (elapsed(&start, &now) < timeout); /* Hang detection ~120s */
		obj.handle = gem_create_in_memory_regions(fd, 4096, region);
		gem_write(fd, obj.handle, 0, &bbe, sizeof(bbe));
		for (int n = 0; n < nengine; n++) {
			execbuf.flags &= ~ENGINE_FLAGS;
			execbuf.flags |= engines[n];
			gem_execbuf(fd, &execbuf);
		}
		gem_sync(fd, obj.handle);
		gem_close(fd, obj.handle);
		clock_gettime(CLOCK_MONOTONIC, &now);

		time = elapsed(&start, &now) / count;
		igt_info("[%d] All (%d engines): %'lu cycles, average %.3fus per cycle\n",
			 child, nengine, count, 1e6*time);
	}
	igt_waitchildren();
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
	intel_ctx_destroy(fd, ctx);
}

igt_main
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	struct drm_i915_query_memory_regions *query_info;
	struct igt_collection *regions, *set;
	uint32_t region;
	int device = -1;

	igt_fixture {
		device = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(device);

		igt_fork_hang_detector(device);

		query_info = gem_get_query_memory_regions(device);
		igt_assert(query_info);

		set = get_memory_region_set(query_info,
					    I915_SYSTEM_MEMORY,
					    I915_DEVICE_MEMORY);
	}

	igt_describe("Check if we kill the system by overloading it with active objects"
		     " iterating over legacy engines.");
	igt_subtest_with_dynamic("legacy")
		for_each_combination(regions, 1, set) {
			char *sub_name = memregion_dynamic_subtest_name(regions);
			region = igt_collection_get_value(regions, 0);

			igt_dynamic_f("%s", sub_name)
				all(device, 0, 2, 1, region);

			free(sub_name);
		}

	igt_describe("Check if we kill system by overloading it with active objects"
		     " iterating over all engines.");
	igt_subtest_with_dynamic("basic")
		for_each_combination(regions, 1, set) {
			char *sub_name = memregion_dynamic_subtest_name(regions);
			region = igt_collection_get_value(regions, 0);

			igt_dynamic_f("%s", sub_name)
				all(device, ENGINES, 2, 1, region);

			free(sub_name);
		}

	igt_describe("Concurrently overloads system with active objects and checks"
		     " if we kill system.");
	igt_subtest_with_dynamic("forked")
		for_each_combination(regions, 1, set) {
			char *sub_name = memregion_dynamic_subtest_name(regions);
			region = igt_collection_get_value(regions, 0);

			igt_dynamic_f("%s", sub_name)
				all(device, ENGINES, 20, ncpus, region);

			free(sub_name);
		}


	igt_describe("This test does a forced reclaim, behaving like a bad application"
		     " leaking its bo cache.");
	igt_subtest_with_dynamic("madvise")
		for_each_combination(regions, 1, set) {
			char *sub_name = memregion_dynamic_subtest_name(regions);
			region = igt_collection_get_value(regions, 0);

			igt_dynamic_f("%s", sub_name)
				all(device, ENGINES | LEAK, 20, 1, region);

			free(sub_name);
		}

	igt_fixture {
		igt_stop_hang_detector();
		close(device);
	}
}
