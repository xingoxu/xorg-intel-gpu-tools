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

/** @file gem_render_tiled_blits.c
 *
 * This is a test of doing many blits, with a working set
 * larger than the aperture size.
 *
 * The goal is to simply ensure the basics work.
 */

#include <stdlib.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>

#include <drm.h>

#include "i915/gem.h"
#include "igt.h"

IGT_TEST_DESCRIPTION("Tests performs cyclic forward, backward and random blits on tiled buffer "
		      "objects using render engine with various working set sizes and compares "
		      "outputs with expected ones.");

#define WIDTH 512
#define STRIDE (WIDTH*4)
#define HEIGHT 512
#define SIZE (HEIGHT*STRIDE)

static igt_render_copyfunc_t render_copy;
static struct intel_buf linear;
static uint32_t data[WIDTH*HEIGHT];
static int snoop;

static void
check_buf(struct intel_bb *ibb, struct intel_buf *buf, uint32_t val)
{
	int i915 = buf_ops_get_fd(linear.bops);
	uint32_t *ptr;
	int i;

	render_copy(ibb, buf, 0, 0, WIDTH, HEIGHT, &linear, 0, 0);

	if (snoop) {
		ptr = gem_mmap__cpu_coherent(i915, linear.handle, 0,
					     linear.surface[0].size, PROT_READ);
		gem_set_domain(i915, linear.handle, I915_GEM_DOMAIN_CPU, 0);
	} else {
		gem_read(i915, linear.handle, 0, data, sizeof(data));
		ptr = data;
	}
	for (i = 0; i < WIDTH*HEIGHT; i++) {
		igt_assert_f(ptr[i] == val,
			"Expected 0x%08x, found 0x%08x "
			"at offset 0x%08x\n",
			val, ptr[i], i * 4);
		val++;
	}
	if (ptr != data)
		munmap(ptr, linear.surface[0].size);
}

static void run_test (int fd, int count)
{
	struct buf_ops *bops;
	struct intel_bb *ibb;
	uint32_t *start_val;
	struct intel_buf *bufs;
	uint32_t start = 0;
	int i, j;
	uint32_t devid;

	devid = intel_get_drm_devid(fd);

	render_copy = igt_get_render_copyfunc(devid);
	igt_require(render_copy);

	snoop = 1;
	if (IS_GEN2(devid)) /* chipset only handles cached -> uncached */
		snoop = 0;
	if (IS_BROADWATER(devid) || IS_CRESTLINE(devid)) /* snafu */
		snoop = 0;

	bops = buf_ops_create(fd);
	ibb = intel_bb_create(fd, 4096);

	intel_buf_init(bops, &linear, WIDTH, HEIGHT, 32, 0,
		       I915_TILING_NONE, I915_COMPRESSION_NONE);
	if (snoop) {
		gem_set_caching(fd, linear.handle, 1);
		igt_info("Using a snoop linear buffer for comparisons\n");
	}

	bufs = calloc(sizeof(*bufs), count);
	start_val = malloc(sizeof(*start_val)*count);

	for (i = 0; i < count; i++) {
		uint32_t tiling = I915_TILING_X + (random() & 1);
		uint32_t *ptr;

		intel_buf_init(bops, &bufs[i], WIDTH, HEIGHT, 32, 0,
			       tiling, I915_COMPRESSION_NONE);
		start_val[i] = start;

		ptr = gem_mmap__gtt(fd, bufs[i].handle,
				    bufs[i].surface[0].size, PROT_WRITE);
		gem_set_domain(fd, bufs[i].handle,
			       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
		for (j = 0; j < WIDTH*HEIGHT; j++)
			ptr[j] = start++;

		munmap(ptr, bufs[i].surface[0].size);
	}

	igt_info("Verifying initialisation...\n");
	for (i = 0; i < count; i++)
		check_buf(ibb, &bufs[i], start_val[i]);

	igt_info("Cyclic blits, forward...\n");
	for (i = 0; i < count * 4; i++) {
		int src = i % count;
		int dst = (i + 1) % count;

		render_copy(ibb, &bufs[src], 0, 0, WIDTH, HEIGHT,
			    &bufs[dst], 0, 0);
		start_val[dst] = start_val[src];
	}

	for (i = 0; i < count; i++)
		check_buf(ibb, &bufs[i], start_val[i]);

	igt_info("Cyclic blits, backward...\n");
	for (i = 0; i < count * 4; i++) {
		int src = (i + 1) % count;
		int dst = i % count;

		render_copy(ibb, &bufs[src], 0, 0, WIDTH, HEIGHT,
			    &bufs[dst], 0, 0);
		start_val[dst] = start_val[src];
	}
	for (i = 0; i < count; i++)
		check_buf(ibb, &bufs[i], start_val[i]);

	igt_info("Random blits...\n");
	for (i = 0; i < count * 4; i++) {
		int src = random() % count;
		int dst = random() % count;

		if (src == dst)
			continue;

		render_copy(ibb, &bufs[src], 0, 0, WIDTH, HEIGHT,
			    &bufs[dst], 0, 0);
		start_val[dst] = start_val[src];
	}

	for (i = 0; i < count; i++)
		check_buf(ibb, &bufs[i], start_val[i]);

	/* release resources */
	intel_buf_close(bops, &linear);
	for (i = 0; i < count; i++) {
		intel_buf_close(bops, &bufs[i]);
	}
	intel_bb_destroy(ibb);
	buf_ops_destroy(bops);
}


igt_main
{
	int fd = 0;
	int count = 0;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);
		igt_require(gem_available_fences(fd) > 0);
	}

	igt_describe("Check basic functionality.");
	igt_subtest("basic") {
		run_test(fd, 2);
	}

	igt_describe("Check with working set size larger than aperture size.");
	igt_subtest("aperture-thrash") {
		count = 3 * gem_aperture_size(fd) / SIZE / 2;
		igt_require_memory(count, SIZE, CHECK_RAM);
		run_test(fd, count);
	}

	igt_describe("Check with working set size larger than aperture size and "
		     "a helper process to shrink buffer object caches.");
	igt_subtest("aperture-shrink") {
		igt_fork_shrink_helper(fd);

		count = 3 * gem_aperture_size(fd) / SIZE / 2;
		igt_require_memory(count, SIZE, CHECK_RAM);
		run_test(fd, count);

		igt_stop_shrink_helper();
	}

	igt_describe("Check with working set size larger than system memory size "
		     "resulting in usage and thrashing of swap space.");
	igt_subtest("swap-thrash") {
		uint64_t swap_mb = igt_get_total_swap_mb();
		igt_require(swap_mb > 0);
		count = ((igt_get_avail_ram_mb() + (swap_mb / 2)) * 1024*1024) / SIZE;
		igt_require_memory(count, SIZE, CHECK_RAM | CHECK_SWAP);
		run_test(fd, count);
	}
}
