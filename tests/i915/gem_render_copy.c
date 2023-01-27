/*
 * Copyright © 2013 Intel Corporation
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
 *    Damien Lespiau <damien.lespiau@intel.com>
 */

/*
 * This file is a basic test for the render_copy() function, a very simple
 * workload for the 3D engine.
 */

#include <stdbool.h>
#include <unistd.h>
#include <cairo.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <drm.h>

#include "i915/gem.h"
#include "igt.h"
#include "igt_x86.h"
#include "intel_bufops.h"

IGT_TEST_DESCRIPTION("Basic test for the render_copy() function.");

#define WIDTH 512
#define HEIGHT 512

typedef struct {
	int drm_fd;
	uint32_t devid;
	struct buf_ops *bops;
	igt_render_copyfunc_t render_copy;
	igt_vebox_copyfunc_t vebox_copy;
} data_t;
static int opt_dump_png = false;
static int check_all_pixels = false;
static bool dump_compressed_src_buf = false;

static const char *make_filename(const char *filename)
{
	static char buf[64];

	snprintf(buf, sizeof(buf), "%s_%s", igt_subtest_name(), filename);

	return buf;
}

static void *alloc_aligned(uint64_t size)
{
	void *p;

	igt_assert_eq(posix_memalign(&p, 16, size), 0);

	return p;
}

static void
copy_from_linear_buf(data_t *data, struct intel_buf *src, struct intel_buf *dst)
{
	igt_assert(src->tiling == I915_TILING_NONE);

	gem_set_domain(data->drm_fd, src->handle,
		       I915_GEM_DOMAIN_CPU, 0);
	intel_buf_cpu_map(src, false);

	linear_to_intel_buf(data->bops, dst, src->ptr);

	intel_buf_unmap(src);
}

static void *linear_copy_ccs(data_t *data, struct intel_buf *buf)
{
	void *ccs_data, *linear;
	unsigned int gen = intel_gen(data->devid);
	int ccs_size = intel_buf_ccs_width(gen, buf) *
		intel_buf_ccs_height(gen, buf);
	int buf_size = intel_buf_size(buf);

	ccs_data = alloc_aligned(ccs_size);
	linear = alloc_aligned(buf_size);
	memset(linear, 0, buf_size);

	intel_buf_to_linear(data->bops, buf, linear);
	igt_memcpy_from_wc(ccs_data, linear + buf->ccs[0].offset, ccs_size);

	free(linear);

	return ccs_data;
}

static void scratch_buf_draw_pattern(data_t *data, struct intel_buf *buf,
				     int x, int y, int w, int h,
				     int cx, int cy, int cw, int ch,
				     bool use_alternate_colors)
{
	cairo_surface_t *surface;
	cairo_pattern_t *pat;
	cairo_t *cr;
	void *linear;

	linear = alloc_aligned(buf->surface[0].size);

	surface = cairo_image_surface_create_for_data(linear,
						      CAIRO_FORMAT_RGB24,
						      intel_buf_width(buf),
						      intel_buf_height(buf),
						      buf->surface[0].stride);

	cr = cairo_create(surface);

	cairo_rectangle(cr, cx, cy, cw, ch);
	cairo_clip(cr);

	pat = cairo_pattern_create_mesh();
	cairo_mesh_pattern_begin_patch(pat);
	cairo_mesh_pattern_move_to(pat, x,   y);
	cairo_mesh_pattern_line_to(pat, x+w, y);
	cairo_mesh_pattern_line_to(pat, x+w, y+h);
	cairo_mesh_pattern_line_to(pat, x,   y+h);
	if (use_alternate_colors) {
		cairo_mesh_pattern_set_corner_color_rgb(pat, 0, 0.0, 1.0, 1.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 1, 1.0, 0.0, 1.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 2, 1.0, 1.0, 0.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 3, 0.0, 0.0, 0.0);
	} else {
		cairo_mesh_pattern_set_corner_color_rgb(pat, 0, 1.0, 0.0, 0.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 1, 0.0, 1.0, 0.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 2, 0.0, 0.0, 1.0);
		cairo_mesh_pattern_set_corner_color_rgb(pat, 3, 1.0, 1.0, 1.0);
	}
	cairo_mesh_pattern_end_patch(pat);

	cairo_rectangle(cr, x, y, w, h);
	cairo_set_source(cr, pat);
	cairo_fill(cr);
	cairo_pattern_destroy(pat);

	cairo_destroy(cr);

	cairo_surface_destroy(surface);

	linear_to_intel_buf(data->bops, buf, linear);

	free(linear);
}

static void
scratch_buf_copy(data_t *data,
		 struct intel_buf *src, int sx, int sy, int w, int h,
		 struct intel_buf *dst, int dx, int dy)
{
	int width = intel_buf_width(dst);
	int height = intel_buf_height(dst);
	uint32_t *linear_dst;
	uint32_t *linear_src;

	igt_assert_eq(intel_buf_width(dst), intel_buf_width(src));
	igt_assert_eq(intel_buf_height(dst), intel_buf_height(src));
	igt_assert_eq(intel_buf_size(dst), intel_buf_size(src));
	igt_assert_eq(dst->bpp, src->bpp);

	w = min(w, width - sx);
	w = min(w, width - dx);

	h = min(h, height - sy);
	h = min(h, height - dy);

	linear_dst = alloc_aligned(intel_buf_size(dst));
	linear_src = alloc_aligned(intel_buf_size(src));
	intel_buf_to_linear(data->bops, src, linear_src);
	intel_buf_to_linear(data->bops, dst, linear_dst);

	for (int y = 0; y < h; y++) {
		memcpy(&linear_dst[(dy+y) * width + dx],
				&linear_src[(sy+y) * width + sx],
				w * (src->bpp / 8));
	}
	free(linear_src);

	linear_to_intel_buf(data->bops, dst, linear_dst);
	free(linear_dst);
}

static void scratch_buf_init(data_t *data, struct intel_buf *buf,
			     int width, int height,
			     uint32_t req_tiling,
			     enum i915_compression compression,
			     uint32_t region)
{
	int bpp = 32;

	intel_buf_init_in_region(data->bops, buf, width, height, bpp, 0,
				 req_tiling, compression, region);

	igt_assert(intel_buf_width(buf) == width);
	igt_assert(intel_buf_height(buf) == height);
}

static void scratch_buf_fini(struct intel_buf *buf)
{
	intel_buf_close(buf->bops, buf);
}

static void
scratch_buf_check(data_t *data,
		  struct intel_buf *buf,
		  struct intel_buf *ref,
		  int x, int y)
{
	int width = intel_buf_width(buf);
	uint32_t buf_val, ref_val;
	uint32_t *linear;

	igt_assert_eq(intel_buf_width(buf), intel_buf_width(ref));
	igt_assert_eq(intel_buf_height(buf), intel_buf_height(ref));
	igt_assert_eq(buf->surface[0].size, ref->surface[0].size);

	linear = alloc_aligned(buf->surface[0].size);
	intel_buf_to_linear(data->bops, buf, linear);
	buf_val = linear[y * width + x];
	free(linear);

	linear = alloc_aligned(ref->surface[0].size);
	intel_buf_to_linear(data->bops, buf, linear);
	ref_val = linear[y * width + x];
	free(linear);

	igt_assert_f(buf_val == ref_val,
		     "Expected 0x%08x, found 0x%08x at (%d,%d)\n",
		     ref_val, buf_val, x, y);
}

static void
scratch_buf_check_all(data_t *data,
		      struct intel_buf *buf,
		      struct intel_buf *ref)
{
	int width = intel_buf_width(buf);
	int height = intel_buf_height(buf);
	uint32_t *linear_buf, *linear_ref;

	igt_assert_eq(intel_buf_width(buf), intel_buf_width(ref));
	igt_assert_eq(intel_buf_height(buf), intel_buf_height(ref));
	igt_assert_eq(buf->surface[0].size, ref->surface[0].size);

	linear_buf = alloc_aligned(buf->surface[0].size);
	linear_ref = alloc_aligned(ref->surface[0].size);
	intel_buf_to_linear(data->bops, buf, linear_buf);
	intel_buf_to_linear(data->bops, ref, linear_ref);

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			uint32_t buf_val = linear_buf[y * width + x];
			uint32_t ref_val = linear_ref[y * width + x];

			igt_assert_f(buf_val == ref_val,
				     "Expected 0x%08x, found 0x%08x at (%d,%d)\n",
				     ref_val, buf_val, x, y);
		}
	}

	free(linear_ref);
	free(linear_buf);
}

static void scratch_buf_ccs_check(data_t *data,
				  struct intel_buf *buf)
{
	unsigned int gen = intel_gen(data->devid);
	int ccs_size = intel_buf_ccs_width(gen, buf) *
		intel_buf_ccs_height(gen, buf);
	uint8_t *linear;
	int i;

	linear = linear_copy_ccs(data, buf);

	for (i = 0; i < ccs_size; i++) {
		if (linear[i])
			break;
	}

	free(linear);

	igt_assert_f(i < ccs_size,
		     "Ccs surface indicates that nothing was compressed\n");
}

static void
dump_intel_buf_to_file(data_t *data, struct intel_buf *buf, const char *filename)
{
	FILE *out;
	void *ptr;
	uint32_t size = intel_buf_size(buf);

	gem_set_domain(data->drm_fd, buf->handle,
		       I915_GEM_DOMAIN_CPU, 0);
	ptr = gem_mmap__cpu_coherent(data->drm_fd, buf->handle, 0, size, PROT_READ);

	out = fopen(filename, "wb");
	igt_assert(out);
	fwrite(ptr, size, 1, out);
	fclose(out);

	munmap(ptr, size);
}

#define SOURCE_MIXED_TILED	1
#define FORCE_VEBOX_DST_COPY	2

static void test(data_t *data, uint32_t src_tiling, uint32_t dst_tiling,
		 enum i915_compression src_compression,
		 enum i915_compression dst_compression,
		 int flags,
		 struct igt_collection *memregion_set)
{
	struct intel_bb *ibb;
	struct intel_buf ref, src_tiled, src_ccs, dst_ccs, dst;
	struct {
		struct intel_buf buf;
		const char *filename;
		uint32_t tiling;
		int x, y;
	} src[] = {
		{
			.filename = "source-linear.png",
			.tiling = I915_TILING_NONE,
			.x = 1, .y = HEIGHT/2+1,
		},
		{
			.filename = "source-x-tiled.png",
			.tiling = I915_TILING_X,
			.x = WIDTH/2+1, .y = HEIGHT/2+1,
		},
		{
			.filename = "source-y-tiled.png",
			.tiling = I915_TILING_Y,
			.x = WIDTH/2+1, .y = 1,
		},
		{
			.filename = "source-yf-tiled.png",
			.tiling = I915_TILING_Yf,
			.x = 1, .y = 1,
		},
	};
	int num_src = ARRAY_SIZE(src);
	uint32_t region = igt_collection_get_value(memregion_set, 0);
	const bool src_mixed_tiled = flags & SOURCE_MIXED_TILED;
	const bool src_compressed = src_compression != I915_COMPRESSION_NONE;
	const bool dst_compressed = dst_compression != I915_COMPRESSION_NONE;
	const bool force_vebox_dst_copy = flags & FORCE_VEBOX_DST_COPY;

	/*
	 * The source tilings for mixed source tiling test cases are determined
	 * by the tiling of the src[] buffers above.
	 */
	igt_assert(src_tiling == I915_TILING_NONE || !src_mixed_tiled);

	/*
	 * The vebox engine can produce only a media compressed or
	 * uncompressed surface.
	 */
	igt_assert(!force_vebox_dst_copy ||
		   dst_compression == I915_COMPRESSION_MEDIA ||
		   dst_compression == I915_COMPRESSION_NONE);

	/* no Yf before gen9 */
	if (intel_gen(data->devid) < 9)
		num_src--;

	if (src_tiling == I915_TILING_Yf || dst_tiling == I915_TILING_Yf ||
	    src_compressed || dst_compressed)
		igt_require(intel_gen(data->devid) >= 9);

	ibb = intel_bb_create(data->drm_fd, 4096);

	for (int i = 0; i < num_src; i++)
		scratch_buf_init(data, &src[i].buf, WIDTH, HEIGHT, src[i].tiling,
				 I915_COMPRESSION_NONE, region);
	if (!src_mixed_tiled)
		scratch_buf_init(data, &src_tiled, WIDTH, HEIGHT, src_tiling,
				 I915_COMPRESSION_NONE, region);
	scratch_buf_init(data, &dst, WIDTH, HEIGHT, dst_tiling,
			 I915_COMPRESSION_NONE, region);

	if (src_compressed)
		scratch_buf_init(data, &src_ccs, WIDTH, HEIGHT,
				 src_tiling, src_compression, region);
	if (dst_compressed)
		scratch_buf_init(data, &dst_ccs, WIDTH, HEIGHT,
				 dst_tiling, dst_compression, region);
	scratch_buf_init(data, &ref, WIDTH, HEIGHT, I915_TILING_NONE,
			 I915_COMPRESSION_NONE, region);

	for (int i = 0; i < num_src; i++)
		scratch_buf_draw_pattern(data, &src[i].buf,
					 0, 0, WIDTH, HEIGHT,
					 0, 0, WIDTH, HEIGHT, (i % 2));

	scratch_buf_draw_pattern(data, &dst,
				 0, 0, WIDTH, HEIGHT,
				 0, 0, WIDTH, HEIGHT, false);

	scratch_buf_copy(data,
			 &dst, 0, 0, WIDTH, HEIGHT,
			 &ref, 0, 0);
	for (int i = 0; i < num_src; i++)
		scratch_buf_copy(data,
				 &src[i].buf, WIDTH/4, HEIGHT/4, WIDTH/2-2, HEIGHT/2-2,
				 &ref, src[i].x, src[i].y);

	if (!src_mixed_tiled)
		copy_from_linear_buf(data, &ref, &src_tiled);

	if (opt_dump_png) {
		for (int i = 0; i < num_src; i++)
			intel_buf_write_to_png(&src[i].buf,
					       make_filename(src[i].filename));
		if (!src_mixed_tiled)
			intel_buf_write_to_png(&src_tiled,
					       make_filename("source-tiled.png"));
		intel_buf_write_to_png(&dst, make_filename("destination.png"));
		intel_buf_write_to_png(&ref, make_filename("reference.png"));
	}

	/* This will copy the src to the mid point of the dst buffer. Presumably
	 * the out of bounds accesses will get clipped.
	 * Resulting buffer should look like:
	 *	  _______
	 *	 |dst|dst|
	 *	 |dst|src|
	 *	  -------
	 */
	if (src_mixed_tiled) {
		if (dst_compressed)
			data->render_copy(ibb,
					  &dst, 0, 0, WIDTH, HEIGHT,
					  &dst_ccs, 0, 0);

		for (int i = 0; i < num_src; i++) {
			data->render_copy(ibb,
					  &src[i].buf,
					  WIDTH/4, HEIGHT/4, WIDTH/2-2, HEIGHT/2-2,
					  dst_compressed ? &dst_ccs : &dst,
					  src[i].x, src[i].y);
		}

		if (dst_compressed)
			data->render_copy(ibb,
					  &dst_ccs, 0, 0, WIDTH, HEIGHT,
					  &dst, 0, 0);

	} else {
		if (src_compression == I915_COMPRESSION_RENDER) {
			data->render_copy(ibb,
					  &src_tiled, 0, 0, WIDTH, HEIGHT,
					  &src_ccs,
					  0, 0);
			if (dump_compressed_src_buf) {
				dump_intel_buf_to_file(data, &src_tiled,
						       "render-src_tiled.bin");
				dump_intel_buf_to_file(data, &src_ccs,
						       "render-src_ccs.bin");
			}
		} else if (src_compression == I915_COMPRESSION_MEDIA) {
			data->vebox_copy(ibb,
					 &src_tiled, WIDTH, HEIGHT,
					 &src_ccs);
			if (dump_compressed_src_buf) {
				dump_intel_buf_to_file(data, &src_tiled,
						       "vebox-src_tiled.bin");
				dump_intel_buf_to_file(data, &src_ccs,
						       "vebox-src_ccs.bin");
			}
		}

		if (dst_compression == I915_COMPRESSION_RENDER) {
			data->render_copy(ibb,
					  src_compressed ? &src_ccs : &src_tiled,
					  0, 0, WIDTH, HEIGHT,
					  &dst_ccs,
					  0, 0);

			data->render_copy(ibb,
					  &dst_ccs,
					  0, 0, WIDTH, HEIGHT,
					  &dst,
					  0, 0);
		} else if (dst_compression == I915_COMPRESSION_MEDIA) {
			data->vebox_copy(ibb,
					 src_compressed ? &src_ccs : &src_tiled,
					 WIDTH, HEIGHT,
					 &dst_ccs);

			data->vebox_copy(ibb,
					 &dst_ccs,
					 WIDTH, HEIGHT,
					 &dst);
		} else if (force_vebox_dst_copy) {
			data->vebox_copy(ibb,
					 src_compressed ? &src_ccs : &src_tiled,
					 WIDTH, HEIGHT,
					 &dst);
		} else {
			data->render_copy(ibb,
					  src_compressed ? &src_ccs : &src_tiled,
					  0, 0, WIDTH, HEIGHT,
					  &dst,
					  0, 0);
		}
	}

	if (opt_dump_png){
		intel_buf_write_to_png(&dst, make_filename("result.png"));
		if (src_compressed) {
			intel_buf_write_to_png(&src_ccs,
					       make_filename("compressed-src.png"));
			intel_buf_write_aux_to_png(&src_ccs,
						   "compressed-src-ccs.png");
		}
		if (dst_compressed) {
			intel_buf_write_to_png(&dst_ccs,
					       make_filename("compressed-dst.png"));
			intel_buf_write_aux_to_png(&dst_ccs,
						   "compressed-dst-ccs.png");
		}
	}

	if (check_all_pixels) {
		scratch_buf_check_all(data, &dst, &ref);
	} else {
		scratch_buf_check(data, &dst, &ref, 10, 10);
		scratch_buf_check(data, &dst, &ref, WIDTH - 10, HEIGHT - 10);
	}

	if (src_compressed)
		scratch_buf_ccs_check(data, &src_ccs);
	if (dst_compressed)
		scratch_buf_ccs_check(data, &dst_ccs);

	scratch_buf_fini(&ref);
	if (!src_mixed_tiled)
		scratch_buf_fini(&src_tiled);
	if (dst_compressed)
		scratch_buf_fini(&dst_ccs);
	if (src_compressed)
		scratch_buf_fini(&src_ccs);
	scratch_buf_fini(&dst);
	for (int i = 0; i < num_src; i++)
		scratch_buf_fini(&src[i].buf);

	intel_bb_destroy(ibb);
}

static int opt_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'd':
		opt_dump_png = true;
		break;
	case 'a':
		check_all_pixels = true;
		break;
	case 'c':
		dump_compressed_src_buf = true;
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =
	"  -d\tDump PNG\n"
	"  -a\tCheck all pixels\n"
	"  -c\tDump compressed src surface\n"
	;

static void buf_mode_to_str(uint32_t tiling, bool mixed_tiled,
			    enum i915_compression compression,
			    char *buf, int buf_size)
{
	const char *compression_str;
	const char *tiling_str;

	if (mixed_tiled)
		tiling_str = "mixed-tiled";
	else switch (tiling) {
	case I915_TILING_NONE:
		tiling_str = "linear";
		break;
	case I915_TILING_X:
		tiling_str = "x-tiled";
		break;
	case I915_TILING_Y:
		tiling_str = "y-tiled";
		break;
	case I915_TILING_Yf:
		tiling_str = "yf-tiled";
		break;
	default:
		igt_assert(0);
	}

	switch (compression) {
	case I915_COMPRESSION_NONE:
		compression_str = "";
		break;
	case I915_COMPRESSION_RENDER:
		compression_str = "ccs";
		break;
	case I915_COMPRESSION_MEDIA:
		compression_str = "mc-ccs";
		break;
	default:
		igt_assert(0);
	}

	snprintf(buf, buf_size, "%s%s%s",
		 tiling_str, compression_str[0] ? "-" : "", compression_str);
}

igt_main_args("dac", NULL, help_str, opt_handler, NULL)
{
	static const struct test_desc {
		int src_tiling;
		int dst_tiling;
		enum i915_compression src_compression;
		enum i915_compression dst_compression;
		int flags;
	} tests[] = {
		{ I915_TILING_NONE,		I915_TILING_NONE,
		  I915_COMPRESSION_NONE,	I915_COMPRESSION_NONE,
		  SOURCE_MIXED_TILED, },
		{ I915_TILING_NONE,		I915_TILING_X,
		  I915_COMPRESSION_NONE,	I915_COMPRESSION_NONE,
		  SOURCE_MIXED_TILED, },
		{ I915_TILING_NONE,		I915_TILING_Y,
		  I915_COMPRESSION_NONE,	I915_COMPRESSION_NONE,
		  SOURCE_MIXED_TILED, },
		{ I915_TILING_NONE,		I915_TILING_Yf,
		  I915_COMPRESSION_NONE,	I915_COMPRESSION_NONE,
		  SOURCE_MIXED_TILED, },

		{ I915_TILING_NONE,		I915_TILING_Y,
		  I915_COMPRESSION_NONE,	I915_COMPRESSION_RENDER,
		  SOURCE_MIXED_TILED },
		{ I915_TILING_NONE,		I915_TILING_Yf,
		  I915_COMPRESSION_NONE,	I915_COMPRESSION_RENDER,
		  SOURCE_MIXED_TILED },

		{ I915_TILING_Y,		I915_TILING_NONE,
		  I915_COMPRESSION_RENDER,	I915_COMPRESSION_NONE,
		  0, },
		{ I915_TILING_Y,		I915_TILING_X,
		  I915_COMPRESSION_RENDER,	I915_COMPRESSION_NONE,
		  0, },
		{ I915_TILING_Y,		I915_TILING_Y,
		  I915_COMPRESSION_RENDER,	I915_COMPRESSION_NONE,
		  0, },
		{ I915_TILING_Y,		I915_TILING_Yf,
		  I915_COMPRESSION_RENDER,	I915_COMPRESSION_NONE,
		  0, },

		{ I915_TILING_Yf,		I915_TILING_NONE,
		  I915_COMPRESSION_RENDER,	I915_COMPRESSION_NONE,
		  0, },
		{ I915_TILING_Yf,		I915_TILING_X,
		  I915_COMPRESSION_RENDER,	I915_COMPRESSION_NONE,
		  0, },
		{ I915_TILING_Yf,		I915_TILING_Y,
		  I915_COMPRESSION_RENDER,	I915_COMPRESSION_NONE,
		  0, },
		{ I915_TILING_Yf,		I915_TILING_Yf,
		  I915_COMPRESSION_RENDER,	I915_COMPRESSION_NONE,
		  0, },

		{ I915_TILING_Y,		I915_TILING_Y,
		  I915_COMPRESSION_RENDER,	I915_COMPRESSION_RENDER,
		  0, },
		{ I915_TILING_Yf,		I915_TILING_Yf,
		  I915_COMPRESSION_RENDER,	I915_COMPRESSION_RENDER,
		  0, },
		{ I915_TILING_Y,		I915_TILING_Yf,
		  I915_COMPRESSION_RENDER,	I915_COMPRESSION_RENDER,
		  0, },
		{ I915_TILING_Yf,		I915_TILING_Y,
		  I915_COMPRESSION_RENDER,	I915_COMPRESSION_RENDER,
		  0, },

		{ I915_TILING_NONE,		I915_TILING_Yf,
		  I915_COMPRESSION_NONE,	I915_COMPRESSION_NONE,
		  FORCE_VEBOX_DST_COPY, },
		{ I915_TILING_NONE,		I915_TILING_Y,
		  I915_COMPRESSION_NONE,	I915_COMPRESSION_NONE,
		  FORCE_VEBOX_DST_COPY, },

		{ I915_TILING_X,		I915_TILING_Yf,
		  I915_COMPRESSION_NONE,	I915_COMPRESSION_NONE,
		  FORCE_VEBOX_DST_COPY, },
		{ I915_TILING_X,		I915_TILING_Y,
		  I915_COMPRESSION_NONE,	I915_COMPRESSION_NONE,
		  FORCE_VEBOX_DST_COPY, },

		{ I915_TILING_Y,		I915_TILING_NONE,
		  I915_COMPRESSION_NONE,	I915_COMPRESSION_NONE,
		  FORCE_VEBOX_DST_COPY, },
		{ I915_TILING_Y,		I915_TILING_X,
		  I915_COMPRESSION_NONE,	I915_COMPRESSION_NONE,
		  FORCE_VEBOX_DST_COPY, },
		{ I915_TILING_Y,		I915_TILING_Y,
		  I915_COMPRESSION_NONE,	I915_COMPRESSION_NONE,
		  FORCE_VEBOX_DST_COPY, },
		{ I915_TILING_Y,		I915_TILING_Yf,
		  I915_COMPRESSION_NONE,	I915_COMPRESSION_NONE,
		  FORCE_VEBOX_DST_COPY, },

		{ I915_TILING_Yf,		I915_TILING_NONE,
		  I915_COMPRESSION_NONE,	I915_COMPRESSION_NONE,
		  FORCE_VEBOX_DST_COPY, },
		{ I915_TILING_Yf,		I915_TILING_X,
		  I915_COMPRESSION_NONE,	I915_COMPRESSION_NONE,
		  FORCE_VEBOX_DST_COPY, },
		{ I915_TILING_Yf,		I915_TILING_Yf,
		  I915_COMPRESSION_NONE,	I915_COMPRESSION_NONE,
		  FORCE_VEBOX_DST_COPY, },
		{ I915_TILING_Yf,		I915_TILING_Y,
		  I915_COMPRESSION_NONE,	I915_COMPRESSION_NONE,
		  FORCE_VEBOX_DST_COPY, },

		{ I915_TILING_Y,		I915_TILING_Y,
		  I915_COMPRESSION_MEDIA,	I915_COMPRESSION_NONE,
		  FORCE_VEBOX_DST_COPY, },
		{ I915_TILING_Yf,		I915_TILING_Yf,
		  I915_COMPRESSION_MEDIA,	I915_COMPRESSION_NONE,
		  FORCE_VEBOX_DST_COPY, },
		{ I915_TILING_Y,		I915_TILING_Yf,
		  I915_COMPRESSION_MEDIA,	I915_COMPRESSION_NONE,
		  FORCE_VEBOX_DST_COPY, },
		{ I915_TILING_Yf,		I915_TILING_Y,
		  I915_COMPRESSION_MEDIA,	I915_COMPRESSION_NONE,
		  FORCE_VEBOX_DST_COPY, },

		{ I915_TILING_Y,		I915_TILING_Y,
		  I915_COMPRESSION_MEDIA,	I915_COMPRESSION_RENDER,
		  0, },
		{ I915_TILING_Y,		I915_TILING_Yf,
		  I915_COMPRESSION_MEDIA,	I915_COMPRESSION_RENDER,
		  0, },

		{ I915_TILING_Y,		I915_TILING_Y,
		  I915_COMPRESSION_RENDER,	I915_COMPRESSION_MEDIA,
		  0, },
		{ I915_TILING_Y,		I915_TILING_Yf,
		  I915_COMPRESSION_RENDER,	I915_COMPRESSION_MEDIA,
		  0, },
	};
	int i;
	struct drm_i915_query_memory_regions *regions;
	struct igt_collection *set, *region_set;

	data_t data = {0, };

	igt_fixture {
		data.drm_fd = drm_open_driver_render(DRIVER_INTEL);
		data.devid = intel_get_drm_devid(data.drm_fd);
		igt_require_gem(data.drm_fd);

		data.render_copy = igt_get_render_copyfunc(data.devid);
		igt_require_f(data.render_copy,
			      "no render-copy function\n");

		data.vebox_copy = igt_get_vebox_copyfunc(data.devid);

		data.bops = buf_ops_create(data.drm_fd);

		regions = gem_get_query_memory_regions(data.drm_fd);
		igt_assert(regions);

		set = get_memory_region_set(regions,
					    I915_SYSTEM_MEMORY,
					    I915_DEVICE_MEMORY);

		igt_fork_hang_detector(data.drm_fd);
	}

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		const struct test_desc *t = &tests[i];
		char name[128];
		char src_mode[32];
		char dst_mode[32];
		const bool src_mixed_tiled = t->flags & SOURCE_MIXED_TILED;
		const bool force_vebox_dst_copy = t->flags & FORCE_VEBOX_DST_COPY;
		const bool vebox_copy_used =
			t->src_compression == I915_COMPRESSION_MEDIA ||
			t->dst_compression == I915_COMPRESSION_MEDIA ||
			force_vebox_dst_copy;
		const bool render_copy_used =
			!vebox_copy_used ||
			t->src_compression == I915_COMPRESSION_RENDER ||
			t->dst_compression == I915_COMPRESSION_RENDER;

		buf_mode_to_str(t->src_tiling, src_mixed_tiled,
				t->src_compression, src_mode, sizeof(src_mode));
		buf_mode_to_str(t->dst_tiling, false,
				t->dst_compression, dst_mode, sizeof(dst_mode));

		igt_describe_f("Test %s%s%s from a %s to a %s buffer.",
			       render_copy_used ? "render_copy()" : "",
			       render_copy_used && vebox_copy_used ? " and " : "",
			       vebox_copy_used ? "vebox_copy()" : "",
			       src_mode, dst_mode);

		/* Preserve original test names */
		if (src_mixed_tiled &&
		    t->dst_compression == I915_COMPRESSION_NONE)
			src_mode[0] = '\0';

		snprintf(name, sizeof(name),
			 "%s%s%s%s",
			 src_mode,
			 src_mode[0] ? "-to-" : "",
			 force_vebox_dst_copy ? "vebox-" : "",
			 dst_mode);
		igt_subtest_with_dynamic(name) {
			igt_skip_on(IS_DG2(data.devid) &&
				    ((t->src_tiling == I915_TILING_Y) ||
				     (t->src_tiling == I915_TILING_Yf) ||
				     (t->dst_tiling == I915_TILING_Y) ||
				     (t->dst_tiling == I915_TILING_Yf)));

			igt_require_f(data.vebox_copy || !vebox_copy_used,
				      "no vebox-copy function\n");
			for_each_combination(region_set, 1, set) {
				char *sub_name = memregion_dynamic_subtest_name(region_set);

				igt_dynamic_f("%s", sub_name)
					test(&data,
					     t->src_tiling, t->dst_tiling,
					     t->src_compression, t->dst_compression,
					     t->flags,
					     region_set);

				free(sub_name);
			}
		}
	}

	igt_fixture {
		igt_stop_hang_detector();
		buf_ops_destroy(data.bops);
		igt_collection_destroy(set);
		close(data.drm_fd);
	}
}
