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

#include <sys/poll.h>
#include <signal.h>
#include <time.h>

#include "i915/gem.h"
#include "igt.h"

IGT_TEST_DESCRIPTION("Basic check of KMS ABI with busy framebuffers.");

static bool all_pipes = false;

static igt_output_t *
set_fb_on_crtc(igt_display_t *dpy, int pipe, struct igt_fb *fb)
{
	drmModeModeInfoPtr mode;
	igt_plane_t *primary;
	igt_output_t *output;

	output = igt_get_single_output_for_pipe(dpy, pipe);
	igt_require(output);

	igt_output_set_pipe(output, pipe);
	mode = igt_output_get_mode(output);

	igt_create_pattern_fb(dpy->drm_fd, mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      I915_FORMAT_MOD_X_TILED, fb);

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_plane_set_fb(primary, fb);

	return output;
}

static void do_cleanup_display(igt_display_t *dpy)
{
	enum pipe pipe;
	igt_output_t *output;
	igt_plane_t *plane;

	for_each_pipe(dpy, pipe)
		for_each_plane_on_pipe(dpy, pipe, plane)
			igt_plane_set_fb(plane, NULL);

	for_each_connected_output(dpy, output)
		igt_output_set_pipe(output, PIPE_NONE);

	igt_display_commit2(dpy, dpy->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
}

static void flip_to_fb(igt_display_t *dpy, int pipe,
		       igt_output_t *output,
		       struct igt_fb *fb, int timeout,
		       const char *name, bool modeset)
{
	struct pollfd pfd = { .fd = dpy->drm_fd, .events = POLLIN };
	struct drm_event_vblank ev;
	IGT_CORK_FENCE(cork);
	uint64_t ahnd = get_reloc_ahnd(dpy->drm_fd, 0);
	igt_spin_t *t;
	int fence;

	fence = igt_cork_plug(&cork, dpy->drm_fd);
	t = igt_spin_new(dpy->drm_fd,
			 .ahnd = ahnd,
			 .fence = fence,
			 .dependency = fb->gem_handle,
			 .flags = IGT_SPIN_FENCE_IN);
	close(fence);

	igt_fork(child, 1) {
		igt_assert(gem_bo_busy(dpy->drm_fd, fb->gem_handle));
		if (!modeset)
			do_or_die(drmModePageFlip(dpy->drm_fd,
						  dpy->pipes[pipe].crtc_id, fb->fb_id,
						  DRM_MODE_PAGE_FLIP_EVENT, fb));
		else {
			igt_plane_set_fb(igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY), fb);
			igt_output_set_pipe(output, PIPE_NONE);
			igt_display_commit_atomic(dpy,
						  DRM_MODE_ATOMIC_NONBLOCK |
						  DRM_MODE_PAGE_FLIP_EVENT |
						  DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		}

		igt_assert_f(poll(&pfd, 1, timeout) == 0,
			     "flip completed whilst %s was busy [%d]\n",
			     name, gem_bo_busy(dpy->drm_fd, fb->gem_handle));
		igt_assert(gem_bo_busy(dpy->drm_fd, fb->gem_handle));

		igt_cork_unplug(&cork);
	}

	igt_waitchildren_timeout(5 * timeout,
				 "flip blocked waiting for busy bo\n");
	igt_spin_end(t);
	igt_cork_unplug(&cork);

	igt_assert(read(dpy->drm_fd, &ev, sizeof(ev)) == sizeof(ev));
	igt_assert(poll(&pfd, 1, 0) == 0);

	if (modeset) {
		gem_quiescent_gpu(dpy->drm_fd);

		/* Clear old mode blob. */
		igt_pipe_refresh(dpy, pipe, true);

		igt_output_set_pipe(output, pipe);
		igt_display_commit2(dpy, COMMIT_ATOMIC);
	}

	igt_spin_free(dpy->drm_fd, t);
	put_ahnd(ahnd);
}

static void test_flip(igt_display_t *dpy, int pipe, bool modeset)
{
	struct igt_fb fb[2];
	int warmup[] = { 0, 1, 0, -1 };
	struct timespec tv = {};
	igt_output_t *output;
	int timeout;

	if (modeset)
		igt_require(dpy->is_atomic);

	output = set_fb_on_crtc(dpy, pipe, &fb[0]);
	igt_display_commit2(dpy, COMMIT_LEGACY);

	igt_create_pattern_fb(dpy->drm_fd,
			      fb[0].width, fb[0].height,
			      DRM_FORMAT_XRGB8888,
			      I915_FORMAT_MOD_X_TILED,
			      &fb[1]);

	/* Bind both fb to the display (such that they are ready for future
	 * flips without stalling for the bind) leaving fb[0] as bound.
	 */
	igt_nsec_elapsed(&tv);
	for (int i = 0; warmup[i] != -1; i++) {
		struct drm_event_vblank ev;

		do_or_die(drmModePageFlip(dpy->drm_fd,
					  dpy->pipes[pipe].crtc_id,
					  fb[warmup[i]].fb_id,
					  DRM_MODE_PAGE_FLIP_EVENT,
					  &fb[warmup[i]]));
		igt_assert(read(dpy->drm_fd, &ev, sizeof(ev)) == sizeof(ev));
	}
	timeout = igt_nsec_elapsed(&tv) / 1000 / 1000;
	igt_info("Using timeout of %dms\n", timeout);

	/* Make the frontbuffer busy and try to flip to itself */
	flip_to_fb(dpy, pipe, output, &fb[0], timeout, "fb[0]", modeset);

	/* Repeat for flip to second buffer */
	flip_to_fb(dpy, pipe, output, &fb[1], timeout, "fb[1]", modeset);

	do_cleanup_display(dpy);
	igt_remove_fb(dpy->drm_fd, &fb[1]);
	igt_remove_fb(dpy->drm_fd, &fb[0]);
}

static void test_atomic_commit_hang(igt_display_t *dpy, igt_plane_t *primary,
				    struct igt_fb *busy_fb)
{
	uint64_t ahnd = get_reloc_ahnd(dpy->drm_fd, 0);
	igt_spin_t *t = igt_spin_new(dpy->drm_fd,
				     .ahnd = ahnd,
				     .dependency = busy_fb->gem_handle,
				     .flags = IGT_SPIN_NO_PREEMPTION);
	struct pollfd pfd = { .fd = dpy->drm_fd, .events = POLLIN };
	unsigned flags = 0;
	struct drm_event_vblank ev;

	flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
	flags |= DRM_MODE_ATOMIC_NONBLOCK;
	flags |= DRM_MODE_PAGE_FLIP_EVENT;

	igt_display_commit_atomic(dpy, flags, NULL);

	igt_fork(child, 1) {
		/*
		 * bit of a hack, just set atomic commit to NULL fb to make sure
		 * that we don't wait for the new update to complete.
		 */
		igt_plane_set_fb(primary, NULL);
		igt_display_commit_atomic(dpy, 0, NULL);

		igt_assert_f(poll(&pfd, 1, 1) > 0,
			    "nonblocking update completed whilst fb[%d] was still busy [%d]\n",
			    busy_fb->fb_id, gem_bo_busy(dpy->drm_fd, busy_fb->gem_handle));
	}

	igt_waitchildren();

	igt_assert(read(dpy->drm_fd, &ev, sizeof(ev)) == sizeof(ev));

	igt_spin_end(t);
	put_ahnd(ahnd);
}

static void test_hang(igt_display_t *dpy,
		      enum pipe pipe, bool modeset, bool hang_newfb)
{
	struct igt_fb fb[2];
	igt_output_t *output;
	igt_plane_t *primary;

	output = set_fb_on_crtc(dpy, pipe, &fb[0]);
	igt_display_commit2(dpy, COMMIT_ATOMIC);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_create_pattern_fb(dpy->drm_fd,
			      fb[0].width, fb[0].height,
			      DRM_FORMAT_XRGB8888,
			      I915_FORMAT_MOD_X_TILED,
			      &fb[1]);

	if (modeset) {
		/* Test modeset disable with hang */
		igt_output_set_pipe(output, PIPE_NONE);
		igt_plane_set_fb(primary, &fb[1]);
		test_atomic_commit_hang(dpy, primary, &fb[hang_newfb]);

		/* Test modeset enable with hang */
		igt_plane_set_fb(primary, &fb[0]);
		igt_output_set_pipe(output, pipe);
		test_atomic_commit_hang(dpy, primary, &fb[!hang_newfb]);
	} else {
		/*
		 * Test what happens with a single hanging pageflip.
		 * This always completes early, because we have some
		 * timeouts taking care of it.
		 */
		igt_plane_set_fb(primary, &fb[1]);
		test_atomic_commit_hang(dpy, primary, &fb[hang_newfb]);
	}

	do_cleanup_display(dpy);
	igt_remove_fb(dpy->drm_fd, &fb[1]);
	igt_remove_fb(dpy->drm_fd, &fb[0]);
}

static void test_pageflip_modeset_hang(igt_display_t *dpy, enum pipe pipe)
{
	struct igt_fb fb;
	struct drm_event_vblank ev;
	igt_output_t *output;
	igt_plane_t *primary;
	igt_spin_t *t;
	uint64_t ahnd = get_reloc_ahnd(dpy->drm_fd, 0);

	output = set_fb_on_crtc(dpy, pipe, &fb);
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_display_commit2(dpy, dpy->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	t = igt_spin_new(dpy->drm_fd,
			 .ahnd = ahnd,
			 .dependency = fb.gem_handle,
			 .flags = IGT_SPIN_NO_PREEMPTION);

	do_or_die(drmModePageFlip(dpy->drm_fd, dpy->pipes[pipe].crtc_id, fb.fb_id, DRM_MODE_PAGE_FLIP_EVENT, &fb));

	/* Kill crtc with hung fb */
	igt_plane_set_fb(primary, NULL);
	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit2(dpy, dpy->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_assert(read(dpy->drm_fd, &ev, sizeof(ev)) == sizeof(ev));

	igt_spin_end(t);
	put_ahnd(ahnd);

	igt_remove_fb(dpy->drm_fd, &fb);
}

static int opt_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
		case 'e':
			all_pipes = true;
			break;
		default:
			return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

static void gpu_engines_init_timeouts(int fd, int max_engines,
				      int *num_engines, struct gem_engine_properties *props)
{
	const struct intel_execution_engine2 *e;

	*num_engines = 0;
	for_each_physical_engine(fd, e) {
		igt_assert(*num_engines < max_engines);

		props[*num_engines].engine = e;
		props[*num_engines].preempt_timeout = 0;
		props[*num_engines].heartbeat_interval = 250;

		gem_engine_properties_configure(fd, &props[*num_engines]);

		(*num_engines)++;
	}
}

static void gpu_engines_restore_timeouts(int fd, int num_engines, const struct gem_engine_properties *props)
{
	int i;

	for (i = 0; i < num_engines; i++)
		gem_engine_properties_restore(fd, &props[i]);
}

const char *help_str =
	"  -e \tRun on all pipes. (By default subtests will run on two pipes)\n";

igt_main_args("e", NULL, help_str, opt_handler, NULL)
{
	igt_display_t display = { .drm_fd = -1, .n_pipes = IGT_MAX_PIPES };

	enum pipe active_pipes[IGT_MAX_PIPES];
	uint32_t last_pipe = 0;
	int i;
	struct {
		const char *name;
		bool modeset;
		bool hang_newfb;
		bool reset;
	} tests[] = {
		{ "extended-pageflip-hang-oldfb", false, false, false },
		{ "extended-pageflip-hang-newfb", false, true, false },
		{ "extended-modeset-hang-oldfb", true, false, false },
		{ "extended-modeset-hang-newfb", true, true, false },
		{ "extended-modeset-hang-oldfb-with-reset", true, false, true },
		{ "extended-modeset-hang-newfb-with-reset", true, true, true },
	};
	struct gem_engine_properties saved_gpu_timeouts[GEM_MAX_ENGINES];
	int num_engines;
	int fd;

	igt_fixture {
		enum pipe pipe;

		fd = drm_open_driver_master(DRIVER_INTEL);

		igt_require_gem(fd);
		gem_require_mmap_device_coherent(fd);
		igt_require(gem_has_ring(fd, I915_EXEC_DEFAULT));

		kmstest_set_vt_graphics_mode();
		igt_display_require(&display, fd);
		igt_display_require_output(&display);

		/* Get active pipes. */
		for_each_pipe(&display, pipe)
			active_pipes[last_pipe++] = pipe;
		last_pipe--;

		gpu_engines_init_timeouts(fd, ARRAY_SIZE(saved_gpu_timeouts), &num_engines, saved_gpu_timeouts);
	}

	/* XXX Extend to cover atomic rendering tests to all planes + legacy */

	igt_describe("Test for basic check of KMS ABI with busy framebuffers.");
	igt_subtest_with_dynamic("basic") { /* just run on the first pipe */
		enum pipe pipe;

		for_each_pipe(&display, pipe) {
			igt_dynamic("flip")
				test_flip(&display, pipe, false);
			igt_dynamic("modeset")
				test_flip(&display, pipe, true);
			break;
		}
	}

	igt_subtest_with_dynamic("basic-hang") {
		enum pipe pipe;
		igt_hang_t hang = igt_allow_hang(display.drm_fd, 0, 0);
		errno = 0;

		for_each_pipe(&display, pipe) {
			if (!all_pipes && pipe != active_pipes[0] &&
					  pipe != active_pipes[last_pipe])
				continue;

			igt_dynamic_f("flip-pipe-%s", kmstest_pipe_name(pipe))
				test_flip(&display, pipe, false);
			igt_dynamic_f("modeset-pipe-%s", kmstest_pipe_name(pipe))
				test_flip(&display, pipe, true);
		}

		igt_disallow_hang(display.drm_fd, hang);
	}

	igt_subtest_with_dynamic("extended-pageflip-modeset-hang-oldfb") {
		enum pipe pipe;
		igt_hang_t hang = igt_allow_hang(display.drm_fd, 0, 0);
		errno = 0;

		for_each_pipe(&display, pipe) {
			if (!all_pipes && pipe != active_pipes[0] &&
					  pipe != active_pipes[last_pipe])
				continue;

			igt_dynamic_f("pipe-%s", kmstest_pipe_name(pipe))
				test_pageflip_modeset_hang(&display, pipe);
		}

		igt_disallow_hang(display.drm_fd, hang);
	}

	for (i = 0; i < sizeof(tests) / sizeof (tests[0]); i++) {
		igt_subtest_with_dynamic(tests[i].name) {
			enum pipe pipe;
			igt_hang_t hang;
			errno = 0;

			igt_require(display.is_atomic);
			hang = igt_allow_hang(display.drm_fd, 0, 0);

			for_each_pipe(&display, pipe) {
				if (!all_pipes && pipe != active_pipes[0] &&
						  pipe != active_pipes[last_pipe])
					continue;

				igt_dynamic_f("pipe-%s", kmstest_pipe_name(pipe)) {
					if (tests[i].reset)
						igt_set_module_param_int(display.drm_fd, "force_reset_modeset_test", 1);

					test_hang(&display, pipe, tests[i].modeset, tests[i].hang_newfb);

					if (tests[i].reset)
						igt_set_module_param_int(display.drm_fd, "force_reset_modeset_test", 0);
				}
			}

			igt_disallow_hang(display.drm_fd, hang);
		}
	}

	igt_fixture {
		gpu_engines_restore_timeouts(fd, num_engines, saved_gpu_timeouts);
		igt_display_fini(&display);
		close(display.drm_fd);
	}
}
