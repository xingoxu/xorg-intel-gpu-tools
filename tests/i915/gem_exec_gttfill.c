/*
 * Copyright © 2016 Intel Corporation
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
 */

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"
#include "igt_device_scan.h"
#include "igt_rand.h"

IGT_TEST_DESCRIPTION("Fill the GTT with batches.");

#define BATCH_SIZE (4096<<10)

struct batch {
	uint32_t handle;
	void *ptr;
	uint64_t offset;
};

static void xchg_batch(void *array, unsigned int i, unsigned int j)
{
	struct batch *batches = array;
	struct batch tmp;

	tmp = batches[i];
	batches[i] = batches[j];
	batches[j] = tmp;
}

static void submit(int fd, uint64_t ahnd, unsigned int gen,
		   struct drm_i915_gem_execbuffer2 *eb,
		   struct drm_i915_gem_relocation_entry *reloc,
		   struct batch *batches, unsigned int count)
{
	struct drm_i915_gem_exec_object2 obj;
	uint32_t batch[16];
	unsigned n;

	memset(&obj, 0, sizeof(obj));
	obj.relocs_ptr = to_user_pointer(reloc);
	obj.relocation_count = !ahnd ? 2 : 0;

	memset(reloc, 0, 2*sizeof(*reloc));
	reloc[0].offset = eb->batch_start_offset;
	reloc[0].offset += sizeof(uint32_t);
	reloc[0].delta = BATCH_SIZE - eb->batch_start_offset - 8;
	reloc[0].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc[1].offset = eb->batch_start_offset;
	reloc[1].offset += 3*sizeof(uint32_t);
	reloc[1].read_domains = I915_GEM_DOMAIN_INSTRUCTION;

	n = 0;
	batch[n] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[n] |= 1 << 21;
		batch[n]++;
		batch[++n] = reloc[0].delta;/* lower_32_bits(address) */
		batch[++n] = 0; /* upper_32_bits(address) */
	} else if (gen >= 4) {
		batch[++n] = 0;
		batch[++n] = reloc[0].delta;/* lower_32_bits(address) */
		reloc[0].offset += sizeof(uint32_t);
	} else {
		batch[n]--;
		batch[++n] = reloc[0].delta;/* lower_32_bits(address) */
		reloc[1].offset -= sizeof(uint32_t);
	}
	batch[++n] = 0; /* lower_32_bits(value) */
	batch[++n] = 0; /* upper_32_bits(value) / nop */
	batch[++n] = MI_BATCH_BUFFER_END;

	eb->buffers_ptr = to_user_pointer(&obj);
	for (unsigned i = 0; i < count; i++) {
		obj.handle = batches[i].handle;
		reloc[0].target_handle = obj.handle;
		reloc[1].target_handle = obj.handle;

		if (ahnd) {
			uint32_t *delta_ptr = batches[i].ptr + reloc[0].delta;

			*delta_ptr = batches[i].offset;
			batch[1] = batches[i].offset + reloc[0].delta;
			obj.flags = EXEC_OBJECT_PINNED;
			obj.offset = batches[i].offset;
			batch[3] = batches[i].offset;
		} else {
			obj.offset = 0;
		}
		reloc[0].presumed_offset = obj.offset;
		reloc[1].presumed_offset = obj.offset;

		memcpy(batches[i].ptr + eb->batch_start_offset,
		       batch, sizeof(batch));

		gem_execbuf(fd, eb);
	}
	/* As we have been lying about the write_domain, we need to do a sync */
	gem_sync(fd, obj.handle);
}

static void fillgtt(int fd, const intel_ctx_t *ctx, unsigned ring, int timeout)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_relocation_entry reloc[2];
	unsigned engines[I915_EXEC_RING_MASK + 1];
	volatile uint64_t *shared;
	struct timespec tv = {};
	struct batch *batches;
	unsigned nengine;
	unsigned count;
	uint64_t size;
	uint64_t ahnd;

	shared = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(shared != MAP_FAILED);

	nengine = 0;
	if (ring == ALL_ENGINES) {
		struct intel_execution_engine2 *e;

		for_each_ctx_engine(fd, ctx, e) {
			if (!gem_class_can_store_dword(fd, e->class))
				continue;

			engines[nengine++] = e->flags;
		}
	} else {
		engines[nengine++] = ring;
	}
	igt_require(nengine);
	igt_assert(nengine * 64 <= BATCH_SIZE);

	size = gem_aperture_size(fd);
	if (size > 1ull<<32) /* Limit to 4GiB as we do not use allow-48b */
		size = 1ull << 32;
	igt_require(size < (1ull<<32) * BATCH_SIZE);

	count = size / BATCH_SIZE + 1;
	igt_debug("Using %'d batches to fill %'llu aperture on %d engines\n",
		  count, (long long)size, nengine);
	igt_require_memory(count, BATCH_SIZE, CHECK_RAM);
	intel_detect_and_clear_missed_interrupts(fd);

	igt_nsec_elapsed(&tv);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffer_count = 1;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;
	execbuf.rsvd1 = ctx->id;

	ahnd = get_reloc_ahnd(fd, ctx->id);
	batches = calloc(count, sizeof(*batches));
	igt_assert(batches);
	for (unsigned i = 0; i < count; i++) {
		batches[i].handle = gem_create(fd, BATCH_SIZE);
		batches[i].ptr =
			gem_mmap__device_coherent(fd, batches[i].handle,
						  0, BATCH_SIZE, PROT_WRITE);
		batches[i].offset = get_offset(ahnd, batches[i].handle, BATCH_SIZE, 0);
		batches[i].offset %= (1ull << 32) - BATCH_SIZE;
	}

	/* Flush all memory before we start the timer */
	submit(fd, ahnd, gen, &execbuf, reloc, batches, count);

	igt_info("Setup %u batches in %.2fms\n",
		 count, 1e-6 * igt_nsec_elapsed(&tv));

	igt_fork(child, nengine) {
		uint64_t cycles = 0;
		hars_petruska_f54_1_random_perturb(child);
		igt_permute_array(batches, count, xchg_batch);
		execbuf.batch_start_offset = child*64;
		execbuf.flags |= engines[child];
		igt_until_timeout(timeout) {
			submit(fd, ahnd, gen, &execbuf, reloc, batches, count);
			for (unsigned i = 0; i < count; i++) {
				uint64_t offset, delta;

				offset = *(uint64_t *)(batches[i].ptr + reloc[1].offset);
				delta = *(uint64_t *)(batches[i].ptr + reloc[0].delta);
				igt_assert_eq_u64(offset, delta);
			}
			cycles++;
		}
		shared[child] = cycles;
		igt_info("engine[%d]: %llu cycles\n", child, (long long)cycles);
	}
	igt_waitchildren();

	for (unsigned i = 0; i < count; i++) {
		munmap(batches[i].ptr, BATCH_SIZE);
		gem_close(fd, batches[i].handle);
	}

	shared[nengine] = 0;
	for (unsigned i = 0; i < nengine; i++)
		shared[nengine] += shared[i];
	igt_info("Total: %llu cycles\n", (long long)shared[nengine]);

	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

igt_main
{
	const struct intel_execution_engine2 *e;
	const intel_ctx_t *ctx;
	int i915 = -1, gpu_count;

	igt_fixture {
		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);
		ctx = intel_ctx_create_all_physical(i915);
		igt_fork_hang_detector(i915);
		intel_allocator_multiprocess_start();
	}

	igt_describe("Checks if it can handle enough batches to fill gtt");
	igt_subtest("basic") /* just enough to run a single pass */
		fillgtt(i915, ctx, ALL_ENGINES, 1);

	igt_describe("Checks the correctness of handling enough batches "
		     "to fill gtt for each engine");
	igt_subtest_with_dynamic("engines") {
		for_each_ctx_engine(i915, ctx, e) {
			if (!gem_class_can_store_dword(i915, e->class))
				continue;

			igt_dynamic_f("%s", e->name)
				fillgtt(i915, ctx, e->flags, 20);
		}
	}

	igt_describe("Stress test check behaviour/correctness of handling"
		     " batches to fill gtt");
	igt_subtest("all")
		fillgtt(i915, ctx, ALL_ENGINES, 20);

	igt_fixture {
		igt_stop_hang_detector();
		intel_ctx_destroy(i915, ctx);
		// prepare multigpu tests
		gpu_count = igt_device_filter_count();
	}

	igt_subtest("multigpu-basic") { /* run on two or more discrete cards */
		igt_require(gpu_count > 1);
		igt_multi_fork(child, gpu_count) {
			int g_fd;
			// prepare
			g_fd = __drm_open_driver_another(child, DRIVER_INTEL);
			igt_assert(g_fd >= 0);
			ctx = intel_ctx_create_all_physical(g_fd);
			igt_fork_hang_detector(g_fd);
			// subtest
			fillgtt(g_fd, ctx, ALL_ENGINES, 1);
			// release resources
			igt_stop_hang_detector();
			intel_ctx_destroy(g_fd, ctx);
			close(g_fd);
		}

		igt_waitchildren();
	}

	igt_fixture {
		intel_allocator_multiprocess_stop();
		close(i915);
	}
}
