/*
 * Copyright © 2009,2011 Intel Corporation
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
 *
 */

/** @file gem_tiled_fence_blits.c
 *
 * This is a test of doing many tiled blits, with a working set
 * larger than the aperture size.
 *
 * The goal is to catch a couple types of failure;
 * - Fence management problems on pre-965.
 * - A17 or L-shaped memory tiling workaround problems in acceleration.
 *
 * The model is to fill a collection of 1MB objects in a way that can't trip
 * over A6 swizzling -- upload data to a non-tiled object, blit to the tiled
 * object.  Then, copy the 1MB objects randomly between each other for a while.
 * Finally, download their data through linear objects again and see what
 * resulted.
 */

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"
#include "igt_x86.h"

IGT_TEST_DESCRIPTION("Tests fence management problems related to tiled blits by performing "
		     "many blits on tiled buffer objects with fences enabled and with working "
		     "set larger than the aperture size.");

enum { width = 512, height = 512 };
static uint32_t linear[width * height];
static const int bo_size = sizeof(linear);

static uint32_t create_bo(int fd, uint32_t start_val)
{
	uint32_t handle;
	uint32_t *ptr;

	handle = gem_create(fd, bo_size);
	gem_set_tiling(fd, handle, I915_TILING_X, width * 4);

	/* Fill the BO with dwords starting at start_val */
	ptr = gem_mmap__gtt(fd, handle, bo_size, PROT_WRITE);
	gem_set_domain(fd, handle, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	for (int i = 0; i < width * height; i++)
		ptr[i] = start_val++;
	munmap(ptr, bo_size);

	return handle;
}

static void check_bo(int fd, uint32_t handle, uint32_t start_val)
{
	uint32_t *ptr;

	ptr = gem_mmap__gtt(fd, handle, bo_size, PROT_READ);
	gem_set_domain(fd, handle, I915_GEM_DOMAIN_GTT, 0);
	igt_memcpy_from_wc(linear, ptr, bo_size);
	munmap(ptr, bo_size);

	for (int i = 0; i < width * height; i++) {
		igt_assert_f(linear[i] == start_val,
			     "Expected 0x%08x, found 0x%08x "
			     "at offset 0x%08x\n",
			     start_val, linear[i], i * 4);
		start_val++;
	}
}

static void
update_batch(int fd, uint32_t bb_handle,
	     struct drm_i915_gem_relocation_entry *reloc,
	     uint64_t dst_offset, uint64_t src_offset)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	const bool has_64b_reloc = gen >= 8;
	uint32_t *batch;
	uint32_t pitch;
	int i = 0;

	batch = gem_mmap__cpu(fd, bb_handle, 0, 4096, PROT_WRITE);

	batch[i] = (XY_SRC_COPY_BLT_CMD |
		    XY_SRC_COPY_BLT_WRITE_ALPHA |
		    XY_SRC_COPY_BLT_WRITE_RGB);
	if (gen >= 4) {
		batch[i] |= (XY_SRC_COPY_BLT_SRC_TILED |
			     XY_SRC_COPY_BLT_DST_TILED);
		pitch = width;
	} else {
		pitch = 4 * width;
	}
	batch[i++] |= 6 + 2 * has_64b_reloc;

	batch[i++] = 3 << 24 | 0xcc << 16 | pitch;
	batch[i++] = 0; /* dst (x1, y1) */
	batch[i++] = height << 16 | width; /* dst (x2 y2) */
	reloc[0].offset = sizeof(*batch) * i;
	reloc[0].read_domains = I915_GEM_DOMAIN_RENDER;
	reloc[0].write_domain = I915_GEM_DOMAIN_RENDER;
	batch[i++] = dst_offset;
	if (has_64b_reloc)
		batch[i++] = dst_offset >> 32;

	batch[i++] = 0; /* src (x1, y1) */
	batch[i++] = pitch;
	reloc[1].offset = sizeof(*batch) * i;
	reloc[1].read_domains = I915_GEM_DOMAIN_RENDER;
	batch[i++] = src_offset;
	if (has_64b_reloc)
		batch[i++] = src_offset >> 32;

	batch[i++] = MI_BATCH_BUFFER_END;
	munmap(batch, 4096);
}

static void xchg_u32(void *array, unsigned i, unsigned j)
{
	uint32_t tmp, *base = array;

	tmp = base[i];
	base[i] = base[j];
	base[j] = tmp;
}

static void run_test(int fd, int count, uint64_t end)
{
	struct drm_i915_gem_relocation_entry reloc[2];
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_execbuffer2 eb;
	uint32_t *src_order, *dst_order;
	uint32_t *bo, *bo_start_val;
	uint32_t start = 0;
	uint64_t ahnd = 0;

	if (!gem_has_relocations(fd))
		ahnd = intel_allocator_open_full(fd, 0, 0, end,
						 INTEL_ALLOCATOR_RELOC,
						 ALLOC_STRATEGY_LOW_TO_HIGH, 0);
	memset(reloc, 0, sizeof(reloc));
	memset(obj, 0, sizeof(obj));
	obj[0].flags = EXEC_OBJECT_NEEDS_FENCE;
	obj[1].flags = EXEC_OBJECT_NEEDS_FENCE;
	obj[2].handle = gem_create(fd, 4096);
	obj[2].offset = get_offset(ahnd, obj[2].handle, 4096, 0);
	if (ahnd) {
		obj[0].flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE;
		obj[1].flags |= EXEC_OBJECT_PINNED;
		obj[2].flags |= EXEC_OBJECT_PINNED;
	}
	obj[2].relocs_ptr = to_user_pointer(reloc);
	obj[2].relocation_count = !ahnd ? ARRAY_SIZE(reloc) : 0;
	update_batch(fd, obj[2].handle, reloc,
		     obj[0].offset, obj[1].offset);

	memset(&eb, 0, sizeof(eb));
	eb.buffers_ptr = to_user_pointer(obj);
	eb.buffer_count = ARRAY_SIZE(obj);
	if (intel_gen(intel_get_drm_devid(fd)) >= 6)
		eb.flags = I915_EXEC_BLT;

	bo = calloc(count,
		    sizeof(*bo) + sizeof(*bo_start_val) +
		    sizeof(*src_order) + sizeof(*dst_order));
	igt_assert(bo);
	bo_start_val = bo + count;
	src_order = bo_start_val + count;
	dst_order = src_order + count;

	for (int i = 0; i < count; i++) {
		bo[i] = create_bo(fd, start);
		bo_start_val[i] = start;
		start += width * height;
		src_order[i] = i;
		dst_order[i] = i;
	}

	/* Twice should be enough to thrash (cause eviction and reload)... */
	for (int pass = 0; pass < 3; pass++) {
		igt_permute_array(src_order, count, xchg_u32);
		igt_permute_array(dst_order, count, xchg_u32);

		for (int i = 0; i < count; i++) {
			int src = src_order[i];
			int dst = dst_order[i];

			if (src == dst)
				continue;

			reloc[0].target_handle = obj[0].handle = bo[dst];
			reloc[1].target_handle = obj[1].handle = bo[src];

			if (ahnd) {
				obj[0].offset = get_offset(ahnd, obj[0].handle,
						sizeof(linear), 0);
				obj[1].offset = get_offset(ahnd, obj[1].handle,
						sizeof(linear), 0);
				obj[2].offset = get_offset(ahnd, obj[2].handle,
						4096, 0);
				update_batch(fd, obj[2].handle, reloc,
					     obj[0].offset, obj[1].offset);
			}

			gem_execbuf(fd, &eb);
			if (ahnd) {
				gem_close(fd, obj[2].handle);
				obj[2].handle = gem_create(fd, 4096);
			}

			bo_start_val[dst] = bo_start_val[src];
		}
	}

	for (int i = 0; i < count; i++) {
		check_bo(fd, bo[i], bo_start_val[i]);
		gem_close(fd, bo[i]);
	}
	free(bo);

	gem_close(fd, obj[2].handle);
	put_ahnd(ahnd);
}

#define MAX_32b ((1ull << 32) - 4096)

igt_main
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	uint64_t count = 0, end;
	int fd;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);
		gem_require_blitter(fd);
		gem_require_mappable_ggtt(fd);

		count = gem_mappable_aperture_size(fd); /* thrash fences! */
		if (count >> 32)
			count = MAX_32b;
		end = count;
		count = 3 + count / (1024 * 1024);
		igt_require(count > 1);
		igt_require_memory(count, 1024 * 1024 , CHECK_RAM);

		igt_debug("Using %'"PRIu64" 1MiB buffers\n", count);
		count = (count + ncpus - 1) / ncpus;
	}

	igt_describe("Check basic functionality.");
	igt_subtest("basic")
		run_test(fd, 2, end);

	igt_describe("Check with parallel execution.");
	igt_subtest("normal") {
		intel_allocator_multiprocess_start();
		igt_fork(child, ncpus)
			run_test(fd, count, end);
		igt_waitchildren();
		intel_allocator_multiprocess_stop();
	}

	igt_fixture
		close(fd);
}
