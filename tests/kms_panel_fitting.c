/*
 * Copyright © 2013,2014 Intel Corporation
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

#include "igt.h"
#include <math.h>
#include <sys/stat.h>

IGT_TEST_DESCRIPTION("Test display panel fitting");

enum test_type {
	TEST_LEGACY,
	TEST_ATOMIC,
};

typedef struct {
	int drm_fd;
	igt_display_t display;

	struct igt_fb fb1;
	struct igt_fb fb2;

	igt_plane_t *plane1;
	igt_plane_t *plane2;
} data_t;

static void cleanup_crtc(data_t *data)
{
	igt_display_reset(&data->display);
	igt_remove_fb(data->drm_fd, &data->fb1);
	igt_remove_fb(data->drm_fd, &data->fb2);
}

static void prepare_crtc(data_t *data, igt_output_t *output, enum pipe pipe,
			igt_plane_t *plane, drmModeModeInfo *mode, enum igt_commit_style s)
{
	igt_display_t *display = &data->display;

	igt_output_override_mode(output, mode);
	igt_output_set_pipe(output, pipe);

	/* before allocating, free if any older fb */
	igt_remove_fb(data->drm_fd, &data->fb1);

	/* allocate fb for plane 1 */
	igt_create_pattern_fb(data->drm_fd,
			      mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      DRM_FORMAT_MOD_LINEAR,
			      &data->fb1);

	/*
	 * We always set the primary plane to actually enable the pipe as
	 * there's no way (that works) to light up a pipe with only a sprite
	 * plane enabled at the moment.
	 */
	if (plane->type != DRM_PLANE_TYPE_PRIMARY) {
		igt_plane_t *primary;

		primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
		igt_plane_set_fb(primary, &data->fb1);
	}

	igt_plane_set_fb(plane, &data->fb1);
	igt_display_commit2(display, s);
}

static void
test_panel_fitting_legacy(data_t *d, igt_display_t *display,
			  const enum pipe pipe, igt_output_t *output)
{
	drmModeModeInfo *mode, native_mode;
	bool is_plane_scaling_active = true;

	igt_output_set_pipe(output, pipe);

	mode = igt_output_get_mode(output);
	native_mode = *mode;

	/* allocate fb2 with image */
	igt_create_pattern_fb(d->drm_fd, mode->hdisplay / 2, mode->vdisplay / 2,
			      DRM_FORMAT_XRGB8888,
			      DRM_FORMAT_MOD_LINEAR, &d->fb2);

	/* set up display to enable panel fitting */
	if (is_amdgpu_device(display->drm_fd)) {
		mode->hdisplay = 800;
		mode->vdisplay = 600;
	} else {
		mode->hdisplay = 640;
		mode->vdisplay = 480;
	}
	d->plane1 = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	prepare_crtc(d, output, pipe, d->plane1, mode, COMMIT_LEGACY);

	/* disable panel fitting */
	prepare_crtc(d, output, pipe, d->plane1, &native_mode, COMMIT_LEGACY);

	/* enable panel fitting */
	mode->hdisplay = 800;
	mode->vdisplay = 600;
	prepare_crtc(d, output, pipe, d->plane1, mode, COMMIT_LEGACY);

	/* disable panel fitting */
	prepare_crtc(d, output, pipe, d->plane1, &native_mode, COMMIT_LEGACY);

	/* set up fb2->plane2 mapping. */
	d->plane2 = igt_output_get_plane_type(output, DRM_PLANE_TYPE_OVERLAY);
	igt_plane_set_fb(d->plane2, &d->fb2);

	/* enable sprite plane */
	igt_fb_set_position(&d->fb2, d->plane2, 100, 100);
	igt_fb_set_size(&d->fb2, d->plane2, d->fb2.width-200, d->fb2.height-200);
	igt_plane_set_position(d->plane2, 100, 100);

	if (is_i915_device(display->drm_fd)) {
		uint32_t devid = intel_get_drm_devid(display->drm_fd);
		/*
		 * Most of gen7 and all of gen8 doesn't support plane scaling
		 * at all.
		 *
		 * gen9 pipe C has only 1 scaler shared with the crtc, which
		 * means pipe scaling can't work simultaneously with panel
		 * fitting.
		 *
		 * Since this is the legacy path, userspace has to know about
		 * the HW limitations, whereas atomic can ask.
		 */
		if (IS_GEN8(devid) ||
			(IS_GEN7(devid) && !IS_IVYBRIDGE(devid)) ||
			(IS_GEN9(devid) && pipe == PIPE_C)) {
			is_plane_scaling_active = false;
		}
	}
	if (is_plane_scaling_active) {
		/*
		 * different than visible area of fb => plane scaling
		 * active
		 */
		igt_plane_set_size(d->plane2,
				   mode->hdisplay-200,
				   mode->vdisplay-200);
	} else {
		/* same as visible area of fb => no scaling */
		igt_plane_set_size(d->plane2,
			d->fb2.width - 200,
			d->fb2.height - 200);
	}

	/* Plane scaling active (if possible), pfit off */
	igt_display_commit2(display, COMMIT_UNIVERSAL);

	/* enable panel fitting along with sprite scaling */
	mode->hdisplay = 1024;
	mode->vdisplay = 768;
	prepare_crtc(d, output, pipe, d->plane1, mode, COMMIT_LEGACY);
}

static void
test_panel_fitting_fastset(igt_display_t *display, const enum pipe pipe, igt_output_t *output)
{
	igt_plane_t *primary, *sprite;
	drmModeModeInfo mode;
	struct igt_fb red, green, blue;

	mode = *igt_output_get_mode(output);

	igt_output_set_pipe(output, pipe);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	sprite = igt_output_get_plane_type(output, DRM_PLANE_TYPE_OVERLAY);

	igt_create_color_fb(display->drm_fd, mode.hdisplay, mode.vdisplay,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			    0.f, 0.f, 1.f, &blue);

	igt_create_color_fb(display->drm_fd, 640, 480,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			    1.f, 0.f, 0.f, &red);

	igt_create_color_fb(display->drm_fd, 800, 600,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			    0.f, 1.f, 0.f, &green);

	igt_plane_set_fb(primary, &blue);
	igt_plane_set_fb(sprite, &red);

	igt_display_commit2(display, COMMIT_ATOMIC);

	mode.hdisplay = 640;
	mode.vdisplay = 480;
	igt_output_override_mode(output, &mode);

	igt_plane_set_fb(sprite, NULL);
	igt_plane_set_fb(primary, &red);

	/* Don't pass ALLOW_MODESET with overridden mode, force fastset. */
	igt_display_commit_atomic(display, 0, NULL);

	/* Test with different scaled mode */
	mode.hdisplay = 800;
	mode.vdisplay = 600;
	igt_output_override_mode(output, &mode);
	igt_plane_set_fb(primary, &green);
	igt_display_commit_atomic(display, 0, NULL);
}

static void test_panel_fitting(data_t *data, enum test_type type)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	enum pipe pipe;
	struct stat sb;

	if (type == TEST_ATOMIC) {
		igt_require_f(is_i915_device(display->drm_fd), "not valid for non-i915 devices\n");

		/* Until this is force enabled, force modeset evasion. */
		if (stat("/sys/module/i915/parameters/fastboot", &sb) == 0)
			igt_set_module_param_int(data->drm_fd, "fastboot", 1);

		igt_require(intel_display_ver(intel_get_drm_devid(display->drm_fd)) >= 5);

	}

	for_each_pipe_with_valid_output(display, pipe, output) {
		/* Check that the "scaling mode" property has been set. */
		if (!igt_output_has_prop(output, IGT_CONNECTOR_SCALING_MODE))
			continue;

		cleanup_crtc(data);

		igt_dynamic_f("pipe-%s-%s", kmstest_pipe_name(pipe), output->name) {
			if (type == TEST_ATOMIC)
				test_panel_fitting_fastset(display, pipe, output);
			if (type == TEST_LEGACY)
				test_panel_fitting_legacy(data, display, pipe, output);
		}
	}
}

igt_main
{
	data_t data = {};

	igt_fixture {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		igt_display_require(&data.display, data.drm_fd);
		igt_display_require_output(&data.display);
	}

	igt_describe("Tests panel fitting usages with legacy style commit.");
	igt_subtest_with_dynamic("legacy")
		test_panel_fitting(&data, TEST_LEGACY);

	igt_fixture
		igt_require(&data.display.is_atomic);

	igt_describe("Tests panel fitting usages with atomic fastset.");
	igt_subtest_with_dynamic("atomic-fastset")
		test_panel_fitting(&data, TEST_ATOMIC);

	igt_fixture {
		igt_display_fini(&data.display);
		close(data.drm_fd);
	}
}
