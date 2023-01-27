/*
 * Copyright © 2015 Intel Corporation
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

/* This program tests whether the igt_draw library actually works. */

#include "i915/gem.h"
#include "igt.h"

#define MAX_CONNECTORS 32

int drm_fd;
igt_display_t display;
igt_output_t *output;
drmModeModeInfoPtr mode;
struct buf_ops *bops;
igt_pipe_crc_t *pipe_crc;

static const uint32_t formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_XRGB2101010,
};

static const uint64_t modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	I915_FORMAT_MOD_X_TILED,
	I915_FORMAT_MOD_Y_TILED,
	I915_FORMAT_MOD_4_TILED,
};

struct base_crc {
	bool set;
	igt_crc_t crc;
};
struct base_crc base_crcs[ARRAY_SIZE(formats)];

static void find_modeset_params(void)
{
	enum pipe pipe;

	igt_display_reset(&display);
	igt_display_commit(&display);

	for_each_pipe_with_valid_output(&display, pipe, output) {
		igt_output_set_pipe(output, pipe);

		mode = igt_output_get_mode(output);
		if (!mode)
			continue;

		pipe_crc = igt_pipe_crc_new(drm_fd, pipe, IGT_PIPE_CRC_SOURCE_AUTO);
		/*Only one pipe/output is enough*/
		break;
	}
}

static uint32_t get_color(uint32_t drm_format, bool r, bool g, bool b)
{
	uint32_t color = 0;

	switch (drm_format) {
	case DRM_FORMAT_RGB565:
		color |= r ? 0x1F << 11 : 0;
		color |= g ? 0x3F << 5 : 0;
		color |= b ? 0x1F : 0;
		break;
	case DRM_FORMAT_XRGB8888:
		color |= r ? 0xFF << 16 : 0;
		color |= g ? 0xFF << 8 : 0;
		color |= b ? 0xFF : 0;
		break;
	case DRM_FORMAT_XRGB2101010:
		color |= r ? 0x3FF << 20 : 0;
		color |= g ? 0x3FF << 10 : 0;
		color |= b ? 0x3FF : 0;
		break;
	default:
		igt_assert(false);
	}

	return color;
}

static void get_method_crc(enum igt_draw_method method, uint32_t drm_format,
			   uint64_t modifier, igt_crc_t *crc)
{
	struct igt_fb fb;
	int rc;
	igt_plane_t *primary;

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_create_fb(drm_fd, mode->hdisplay, mode->vdisplay,
		      drm_format, modifier, &fb);
	igt_plane_set_fb(primary, &fb);

	igt_draw_rect_fb(drm_fd, bops, 0, &fb, method,
			 0, 0, fb.width, fb.height,
			 get_color(drm_format, 0, 0, 1));

	igt_draw_rect_fb(drm_fd, bops, 0, &fb, method,
			 fb.width / 4, fb.height / 4,
			 fb.width / 2, fb.height / 2,
			 get_color(drm_format, 0, 1, 0));
	igt_draw_rect_fb(drm_fd, bops, 0, &fb, method,
			 fb.width / 8, fb.height / 8,
			 fb.width / 4, fb.height / 4,
			 get_color(drm_format, 1, 0, 0));
	igt_draw_rect_fb(drm_fd, bops, 0, &fb, method,
			 fb.width / 2, fb.height / 2,
			 fb.width / 3, fb.height / 3,
			 get_color(drm_format, 1, 0, 1));
	igt_draw_rect_fb(drm_fd, bops, 0, &fb, method, 1, 1, 15, 15,
			 get_color(drm_format, 0, 1, 1));

	rc = igt_display_commit2(&display, display.is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
	igt_assert_eq(rc, 0);

	igt_pipe_crc_collect_crc(pipe_crc, crc);

	igt_remove_fb(drm_fd, &fb);
}

static void draw_method_subtest(enum igt_draw_method method,
				uint32_t format_index, uint64_t modifier)
{
	igt_crc_t crc;

	/* Use IGT_DRAW_MMAP_GTT/WC on an untiled buffer as the parameter for
	 * comparison. Cache the value so we don't recompute it for every single
	 * subtest. */
	if (!base_crcs[format_index].set) {
		get_method_crc(gem_has_mappable_ggtt(drm_fd) ? IGT_DRAW_MMAP_GTT :
							       IGT_DRAW_MMAP_WC,
			       formats[format_index],
			       DRM_FORMAT_MOD_LINEAR,
			       &base_crcs[format_index].crc);
		base_crcs[format_index].set = true;
	}

	get_method_crc(method, formats[format_index], modifier, &crc);
	igt_assert_crc_equal(&crc, &base_crcs[format_index].crc);
}

static void get_fill_crc(uint64_t modifier, igt_crc_t *crc)
{
	struct igt_fb fb;
	int rc;

	igt_create_fb(drm_fd, mode->hdisplay, mode->vdisplay,
		      DRM_FORMAT_XRGB8888, modifier, &fb);

	igt_draw_fill_fb(drm_fd, &fb, 0xFF);

	rc = igt_display_commit2(&display, display.is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
	igt_assert_eq(rc, 0);

	igt_pipe_crc_collect_crc(pipe_crc, crc);

	igt_remove_fb(drm_fd, &fb);
}

static void fill_fb_subtest(void)
{
	int rc;
	struct igt_fb fb;
	igt_crc_t base_crc, crc;
	igt_plane_t *primary;
	bool has_4tile = intel_get_device_info(intel_get_drm_devid(drm_fd))->has_4tile;

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_create_fb(drm_fd, mode->hdisplay, mode->vdisplay,
		      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, &fb);

	igt_plane_set_fb(primary, &fb);

	igt_draw_rect_fb(drm_fd, bops, 0, &fb,
			 gem_has_mappable_ggtt(drm_fd) ? IGT_DRAW_MMAP_GTT :
							 IGT_DRAW_MMAP_WC,
			 0, 0, fb.width, fb.height, 0xFF);

	rc = igt_display_commit2(&display, display.is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
	igt_assert_eq(rc, 0);

	igt_pipe_crc_collect_crc(pipe_crc, &base_crc);

	get_fill_crc(DRM_FORMAT_MOD_LINEAR, &crc);
	igt_assert_crc_equal(&crc, &base_crc);

	get_fill_crc(I915_FORMAT_MOD_X_TILED, &crc);
	igt_assert_crc_equal(&crc, &base_crc);

	if (intel_display_ver(intel_get_drm_devid(drm_fd)) >= 9) {
		get_fill_crc(has_4tile ?
			     I915_FORMAT_MOD_4_TILED : I915_FORMAT_MOD_Y_TILED,
			     &crc);
		igt_assert_crc_equal(&crc, &base_crc);
	}

	igt_remove_fb(drm_fd, &fb);
}

static void setup_environment(void)
{
	drm_fd = drm_open_driver_master(DRIVER_INTEL);
	igt_require(drm_fd >= 0);
	igt_display_require(&display, drm_fd);
	igt_display_require_output(&display);

	kmstest_set_vt_graphics_mode();

	bops = buf_ops_create(drm_fd);

	find_modeset_params();
}

static void teardown_environment(void)
{
	igt_display_fini(&display);

	igt_pipe_crc_free(pipe_crc);

	buf_ops_destroy(bops);

	close(drm_fd);
}

static const char *format_str(int format_index)
{
	switch (formats[format_index]) {
	case DRM_FORMAT_RGB565:
		return "rgb565";
	case DRM_FORMAT_XRGB8888:
		return "xrgb8888";
	case DRM_FORMAT_XRGB2101010:
		return "xrgb2101010";
	default:
		igt_assert(false);
	}
}

static const char *modifier_str(int modifier_index)
{
	switch (modifiers[modifier_index]) {
	case DRM_FORMAT_MOD_LINEAR :
		return "untiled";
	case I915_FORMAT_MOD_X_TILED:
		return "xtiled";
	case I915_FORMAT_MOD_Y_TILED:
		return "ytiled";
	case I915_FORMAT_MOD_4_TILED:
		return "4tiled";
	default:
		igt_assert(false);
	}
}

igt_main
{
	enum igt_draw_method method;
	int format_idx, modifier_idx;
	uint64_t modifier;

	igt_fixture
		setup_environment();

	igt_describe("This subtest verfies igt_draw library works "
		     "with different modifiers, DRM_FORMATS, DRAW_METHODS.");
	igt_subtest_with_dynamic("draw-method") {
		for (format_idx = 0; format_idx < ARRAY_SIZE(formats); format_idx++) {
			for (method = 0; method < IGT_DRAW_METHOD_COUNT; method++) {
				for (modifier_idx = 0; modifier_idx < ARRAY_SIZE(modifiers); modifier_idx++) {
					modifier = modifiers[modifier_idx];

					if (method == IGT_DRAW_MMAP_WC && !gem_mmap__has_wc(drm_fd))
						continue;

					if (method == IGT_DRAW_MMAP_GTT &&
					    !gem_has_mappable_ggtt(drm_fd))
						continue;

					if (!igt_display_has_format_mod(&display, formats[format_idx], modifier))
						continue;

					igt_dynamic_f("%s-%s-%s",
						      format_str(format_idx),
						      igt_draw_get_method_name(method),
						      modifier_str(modifier_idx))
						draw_method_subtest(method, format_idx,
								    modifier);
				}
			}
		}
	}

	igt_describe("This subtest verifies CRC after filling fb with x-tiling "
		     "or none.");
	igt_subtest("fill-fb")
		fill_fb_subtest();

	igt_fixture
		teardown_environment();
}
