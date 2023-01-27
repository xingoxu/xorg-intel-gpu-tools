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

#include <limits.h>
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
#include "i915/gem_vm.h"
#include "igt.h"

#define INTERRUPTIBLE 0x1
#define QUEUE 0x2

static double elapsed(const struct timespec *start, const struct timespec *end)
{
	return ((end->tv_sec - start->tv_sec) +
		(end->tv_nsec - start->tv_nsec)*1e-9);
}

static int measure_qlen(int fd, const intel_ctx_cfg_t *cfg,
			struct drm_i915_gem_execbuffer2 *execbuf,
			const struct intel_engine_data *engines,
			int timeout)
{
	const struct drm_i915_gem_exec_object2 * const obj =
		(struct drm_i915_gem_exec_object2 *)(uintptr_t)execbuf->buffers_ptr;
	const intel_ctx_t *ctx[64];
	int min = INT_MAX, max = 0;

	for (int i = 0; i < ARRAY_SIZE(ctx); i++)
		ctx[i] = intel_ctx_create(fd, cfg);

	for (unsigned int n = 0; n < engines->nengines; n++) {
		uint64_t saved = execbuf->flags;
		struct timespec tv = {};
		int q;

		execbuf->flags |= engines->engines[n].flags;

		for (int i = 0; i < ARRAY_SIZE(ctx); i++) {
			execbuf->rsvd1 = ctx[i]->id;
			gem_execbuf(fd, execbuf);
		}
		gem_sync(fd, obj->handle);

		igt_nsec_elapsed(&tv);
		for (int i = 0; i < ARRAY_SIZE(ctx); i++) {
			execbuf->rsvd1 = ctx[i]->id;
			gem_execbuf(fd, execbuf);
		}
		gem_sync(fd, obj->handle);

		/*
		 * Be conservative and aim not to overshoot timeout, so scale
		 * down by 8 for hopefully a max of 12.5% error.
		 */
		q = ARRAY_SIZE(ctx) * timeout * 1e9 / igt_nsec_elapsed(&tv) /
		    8 + 1;
		if (q < min)
			min = q;
		if (q > max)
			max = q;

		execbuf->flags = saved;
	}

	for (int i = 0; i < ARRAY_SIZE(ctx); i++)
		intel_ctx_destroy(fd, ctx[i]);

	igt_debug("Estimated qlen: {min:%d, max:%d}\n", min, max);
	return min;
}

static void single(int fd, uint32_t handle,
		   const intel_ctx_cfg_t *base_cfg,
		   const struct intel_execution_engine2 *e2,
		   unsigned flags,
		   const int ncpus,
		   int timeout)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_relocation_entry reloc;
	intel_ctx_cfg_t cfg;
	const intel_ctx_t *contexts[64];
	struct {
		double elapsed;
		unsigned long count;
	} *shared;
	int n;

	shared = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(shared != MAP_FAILED);

	cfg = *base_cfg;
	if (flags & QUEUE) {
		cfg.vm = gem_vm_create(fd);
		cfg.flags |= I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE;
	}

	for (n = 0; n < 64; n++)
		contexts[n] = intel_ctx_create(fd, &cfg);

	memset(&obj, 0, sizeof(obj));
	obj.handle = handle;

	if (flags & INTERRUPTIBLE) {
		/* Be tricksy and force a relocation every batch so that
		 * we don't emit the batch but just do MI_SET_CONTEXT
		 */
		memset(&reloc, 0, sizeof(reloc));
		reloc.offset = 1024;
		reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		obj.relocs_ptr = to_user_pointer(&reloc);
		obj.relocation_count = 1;
	}

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(&obj);
	execbuf.buffer_count = 1;
	execbuf.rsvd1 = contexts[0]->id;
	execbuf.flags = e2->flags;
	execbuf.flags |= I915_EXEC_HANDLE_LUT;
	execbuf.flags |= I915_EXEC_NO_RELOC;
	igt_require(__gem_execbuf(fd, &execbuf) == 0);
	if (__gem_execbuf(fd, &execbuf)) {
		execbuf.flags = e2->flags;
		reloc.target_handle = obj.handle;
		gem_execbuf(fd, &execbuf);
	}
	gem_sync(fd, handle);

	igt_fork(child, ncpus) {
		struct timespec start, now;
		unsigned int count = 0;

		/* Warmup to bind all objects into each ctx before we begin */
		for (int i = 0; i < ARRAY_SIZE(contexts); i++) {
			execbuf.rsvd1 = contexts[i]->id;
			gem_execbuf(fd, &execbuf);
		}
		gem_sync(fd, handle);

		clock_gettime(CLOCK_MONOTONIC, &start);
		do {
			igt_while_interruptible(flags & INTERRUPTIBLE) {
				for (int loop = 0; loop < 64; loop++) {
					execbuf.rsvd1 = contexts[loop % 64]->id;
					reloc.presumed_offset = -1;
					gem_execbuf(fd, &execbuf);
				}
				count += 64;
			}
			clock_gettime(CLOCK_MONOTONIC, &now);
		} while (elapsed(&start, &now) < timeout);
		gem_sync(fd, handle);
		clock_gettime(CLOCK_MONOTONIC, &now);

		igt_info("[%d] %s: %'u cycles: %.3fus%s\n",
			 child, e2->name, count,
			 elapsed(&start, &now) * 1e6 / count,
			 flags & INTERRUPTIBLE ? " (interruptible)" : "");

		shared[child].elapsed = elapsed(&start, &now);
		shared[child].count = count;
	}
	igt_waitchildren();

	if (ncpus > 1) {
		unsigned long total = 0;
		double max = 0;

		for (n = 0; n < ncpus; n++) {
			total += shared[n].count;
			if (shared[n].elapsed > max)
				max = shared[n].elapsed;
		}

		igt_info("Total %s: %'lu cycles: %.3fus%s\n",
			 e2->name, total, max*1e6 / total,
			 flags & INTERRUPTIBLE ? " (interruptible)" : "");
	}

	for (n = 0; n < 64; n++)
		intel_ctx_destroy(fd, contexts[n]);

	munmap(shared, 4096);
}

static void all(int fd, uint32_t handle, const intel_ctx_cfg_t *base_cfg,
		unsigned flags, int timeout)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[2];
	struct intel_engine_data engines = { };
	intel_ctx_cfg_t cfg;
	const intel_ctx_t *contexts[65];
	int n, qlen;

	engines = intel_engine_list_for_ctx_cfg(fd, base_cfg);
	igt_require(engines.nengines);

	cfg = *base_cfg;
	if (flags & QUEUE) {
		cfg.vm = gem_vm_create(fd);
		cfg.flags |= I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE;
	}

	for (n = 0; n < ARRAY_SIZE(contexts); n++)
		contexts[n] = intel_ctx_create(fd, &cfg);

	memset(obj, 0, sizeof(obj));
	obj[1].handle = handle;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj + 1);
	execbuf.buffer_count = 1;
	execbuf.rsvd1 = contexts[0]->id;
	execbuf.flags |= I915_EXEC_HANDLE_LUT;
	execbuf.flags |= I915_EXEC_NO_RELOC;
	igt_require(__gem_execbuf(fd, &execbuf) == 0);
	gem_sync(fd, handle);

	qlen = measure_qlen(fd, &cfg, &execbuf, &engines, timeout);
	igt_info("Using timing depth of %d batches\n", qlen);

	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;

	for (int pot = 2; pot <= 64; pot *= 2) {
		for (int nctx = pot - 1; nctx <= pot + 1; nctx++) {
			igt_fork(child, engines.nengines) {
				struct timespec start, now;
				unsigned int count = 0;

				obj[0].handle = gem_create(fd, 4096);
				execbuf.flags |= engines.engines[child].flags;
				for (int loop = 0;
				     loop < ARRAY_SIZE(contexts);
				     loop++) {
					execbuf.rsvd1 = contexts[loop]->id;
					gem_execbuf(fd, &execbuf);
				}
				gem_sync(fd, obj[0].handle);

				clock_gettime(CLOCK_MONOTONIC, &start);
				do {
					for (int loop = 0; loop < qlen; loop++) {
						execbuf.rsvd1 =
							contexts[loop % nctx]->id;
						gem_execbuf(fd, &execbuf);
					}
					count += qlen;
					gem_sync(fd, obj[0].handle);
					clock_gettime(CLOCK_MONOTONIC, &now);
				} while (elapsed(&start, &now) < timeout);
				gem_sync(fd, obj[0].handle);
				clock_gettime(CLOCK_MONOTONIC, &now);
				gem_close(fd, obj[0].handle);

				igt_info("[%d:%d] %s: %'u cycles: %.3fus%s (elapsed: %.3fs)\n",
					 nctx, child,
					 engines.engines[child].name, count,
					 elapsed(&start, &now) * 1e6 / count,
					 flags & INTERRUPTIBLE ?
					 " (interruptible)" : "",
					 elapsed(&start, &now));
			}
			igt_waitchildren();
		}
	}

	for (n = 0; n < ARRAY_SIZE(contexts); n++)
		intel_ctx_destroy(fd, contexts[n]);
}

static bool
has_queues(int fd)
{
	return gem_has_vm(fd) && gem_context_has_single_timeline(fd);
}

igt_main
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	const struct intel_execution_engine2 *e2;
	const struct intel_execution_ring *e;
	const intel_ctx_cfg_t legacy_cfg = {};
	intel_ctx_cfg_t engines_cfg;
	static const struct {
		const char *name;
		unsigned int flags;
		bool (*require)(int fd);
	} phases[] = {
		{ "", 0, NULL },
		{ "-interruptible", INTERRUPTIBLE, NULL },
		{ "-queue", QUEUE, has_queues },
		{ "-queue-interruptible", QUEUE | INTERRUPTIBLE, has_queues },
		{ }
	};
	uint32_t light = 0, heavy;
	int fd = -1;

	igt_fixture {
		const uint32_t bbe = MI_BATCH_BUFFER_END;

		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);

		gem_require_contexts(fd);

		engines_cfg = intel_ctx_cfg_all_physical(fd);

		light = gem_create(fd, 4096);
		gem_write(fd, light, 0, &bbe, sizeof(bbe));

		heavy = gem_create(fd, 4096*1024);
		gem_write(fd, heavy, 4096*1024-sizeof(bbe), &bbe, sizeof(bbe));

		igt_fork_hang_detector(fd);
	}

	/* Legacy testing must be first. */
	for (e = intel_execution_rings; e->name; e++) {
		struct intel_execution_engine2 e2__;

		e2__ = gem_eb_flags_to_engine(eb_ring(e));
		if (e2__.flags == -1)
			continue; /* I915_EXEC_BSD with no ring selectors */

		e2 = &e2__;

		for (typeof(*phases) *p = phases; p->name; p++) {
			igt_subtest_group {
				igt_fixture {
					gem_require_ring(fd, e2->flags);
					if (p->require)
						igt_require(p->require(fd));
				}

				igt_subtest_f("legacy-%s%s", e->name, p->name)
					single(fd, light, &legacy_cfg, e2,
					       p->flags, 1, 2);
				igt_subtest_f("legacy-%s-heavy%s",
					      e->name, p->name)
					single(fd, heavy, &legacy_cfg, e2,
					       p->flags, 1, 2);
				igt_subtest_f("legacy-%s-forked%s",
					      e->name, p->name)
					single(fd, light, &legacy_cfg, e2,
					       p->flags, ncpus, 20);
				igt_subtest_f("legacy-%s-forked-heavy%s",
					      e->name, p->name)
					single(fd, heavy, &legacy_cfg, e2,
					       p->flags, ncpus, 20);
			}
		}
	}

	/* Must come after legacy subtests. */
	for_each_ctx_cfg_engine(fd, &engines_cfg, e2) {
		for (typeof(*phases) *p = phases; p->name; p++) {
			igt_subtest_group {
				igt_fixture {
					if (p->require)
						igt_require(p->require(fd));
				}

				igt_subtest_f("%s%s", e2->name, p->name)
					single(fd, light, &engines_cfg, e2,
					       p->flags, 1, 2);
				igt_subtest_f("%s-heavy%s", e2->name, p->name)
					single(fd, heavy, &engines_cfg, e2,
					       p->flags, 1, 2);
				igt_subtest_f("%s-forked%s", e2->name, p->name)
					single(fd, light, &engines_cfg, e2,
					       p->flags, ncpus, 20);
				igt_subtest_f("%s-forked-heavy%s",
					      e2->name, p->name)
					single(fd, heavy, &engines_cfg, e2,
					       p->flags, ncpus, 20);
			}
		}
	}

	igt_subtest("all-light")
		all(fd, light, &engines_cfg, 0, 2);
	igt_subtest("all-heavy")
		all(fd, heavy, &engines_cfg, 0, 2);

	igt_subtest_group {
		igt_fixture {
			gem_require_vm(fd);
		}
		igt_subtest("queue-light")
			all(fd, light, &engines_cfg, QUEUE, 2);
		igt_subtest("queue-heavy")
			all(fd, heavy, &engines_cfg, QUEUE, 2);
	}

	igt_fixture {
		igt_stop_hang_detector();
		gem_close(fd, heavy);
		gem_close(fd, light);
		close(fd);
	}
}
