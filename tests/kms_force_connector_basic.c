/*
 * Copyright © 2014 Intel Corporation
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

IGT_TEST_DESCRIPTION("Check the debugfs force connector/edid features work"
		     " correctly.");

#define CHECK_MODE(m, h, w, r) \
	igt_assert_eq(m.hdisplay, h); igt_assert_eq(m.vdisplay, w); \
	igt_assert_eq(m.vrefresh, r);

static void reset_connectors(void)
{
	drmModeRes *res;
	drmModeConnector *connector = NULL;
	int drm_fd;

	drm_fd = drm_open_driver_master(DRIVER_ANY);
	res = drmModeGetResources(drm_fd);

	for (int i = 0; i < res->count_connectors; i++) {

		connector = drmModeGetConnectorCurrent(drm_fd,
						       res->connectors[i]);

		kmstest_force_connector(drm_fd, connector,
					FORCE_CONNECTOR_UNSPECIFIED);

		kmstest_force_edid(drm_fd, connector, NULL);

		drmModeFreeConnector(connector);
	}

	igt_set_module_param_int(drm_fd, "load_detect_test", 0);

	close(drm_fd);
}

static void force_load_detect(int drm_fd, drmModeConnectorPtr connector, drmModeRes *res)
{
	int i, j, w = 64, h = 64;
	drmModePlaneRes *plane_resources;
	drmModeConnector *temp;
	struct igt_fb xrgb_fb, argb_fb;

	/* No load detect on HDMI. */
	igt_require(connector->connector_type == DRM_MODE_CONNECTOR_VGA);

	igt_create_fb(drm_fd, w, h, DRM_FORMAT_XRGB8888, 0, &xrgb_fb);
	igt_create_fb(drm_fd, w, h, DRM_FORMAT_ARGB8888, 0, &argb_fb);
	igt_assert(drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0);

	/*
	 * Disable all outputs to make sure we have a
	 * free crtc available for load detect.
	 */
	kmstest_unset_all_crtcs(drm_fd, res);

	igt_set_module_param_int(drm_fd, "load_detect_test", 1);

	plane_resources = drmModeGetPlaneResources(drm_fd);
	igt_assert(plane_resources);

	for (i = 0; i < plane_resources->count_planes; i++) {
		drmModePlane *drm_plane;
		bool found = false;
		uint32_t plane_id = plane_resources->planes[i];

		drm_plane = drmModeGetPlane(drm_fd, plane_id);
		igt_assert(drm_plane);

		for (j = 0; j < drm_plane->count_formats; j++) {
			uint32_t format = drm_plane->formats[j];
			uint32_t crtc = ffs(drm_plane->possible_crtcs) - 1;
			uint32_t crtc_id = res->crtcs[crtc];

			if (format == DRM_FORMAT_XRGB8888)
				do_or_die(drmModeSetPlane(drm_fd, plane_id, crtc_id,
						xrgb_fb.fb_id,
						0, 0, 0, w, h,
						0, 0, IGT_FIXED(w, 0), IGT_FIXED(h, 0)));
			else if (format == DRM_FORMAT_ARGB8888)
				do_or_die(drmModeSetPlane(drm_fd, plane_id, crtc_id,
						argb_fb.fb_id,
						0, 0, 0, w, h,
						0, 0, IGT_FIXED(w, 0), IGT_FIXED(h, 0)));
			else
				continue;

			found = true;
			break;
		}
		drmModeFreePlane(drm_plane);
		igt_assert(found);
	}

	/*
	 * This can't use drmModeGetConnectorCurrent
	 * because connector probing is the point of this test.
	 */
	temp = drmModeGetConnector(drm_fd, connector->connector_id);

	igt_set_module_param_int(drm_fd, "load_detect_test", 0);

	igt_assert(temp->connection != DRM_MODE_UNKNOWNCONNECTION);

	drmModeFreeConnector(temp);

	/* Look if planes are unmodified. */
	for (i = 0; i < plane_resources->count_planes; i++) {
		drmModePlane *drm_plane;

		drm_plane = drmModeGetPlane(drm_fd,
					    plane_resources->planes[i]);
		igt_assert(drm_plane);

		igt_assert(drm_plane->crtc_id);
		igt_assert(drm_plane->fb_id);

		if (drm_plane->fb_id != xrgb_fb.fb_id)
			igt_assert_eq(drm_plane->fb_id, argb_fb.fb_id);

		drmModeFreePlane(drm_plane);
	}
}

static void force_connector_state(int drm_fd, drmModeConnectorPtr connector)
{
	igt_display_t display;
	drmModeConnector *temp;

	igt_display_require(&display, drm_fd);

	/* Reset display before attempt to use it. */
	igt_display_reset(&display);
	igt_display_commit(&display);

	/* Force the connector on and check the reported values. */
	kmstest_force_connector(drm_fd, connector, FORCE_CONNECTOR_ON);
	temp = drmModeGetConnectorCurrent(drm_fd,
					  connector->connector_id);
	igt_assert_eq(temp->connection, DRM_MODE_CONNECTED);
	igt_assert_lt(0, temp->count_modes);
	drmModeFreeConnector(temp);

	/* Force the connector off. */
	kmstest_force_connector(drm_fd, connector,
				FORCE_CONNECTOR_OFF);
	temp = drmModeGetConnectorCurrent(drm_fd,
					  connector->connector_id);
	igt_assert_eq(temp->connection, DRM_MODE_DISCONNECTED);
	igt_assert_eq(0, temp->count_modes);
	drmModeFreeConnector(temp);

	/* Check that the previous state is restored. */
	kmstest_force_connector(drm_fd, connector,
				FORCE_CONNECTOR_UNSPECIFIED);
	temp = drmModeGetConnectorCurrent(drm_fd,
					  connector->connector_id);
	igt_assert_eq(temp->connection, connector->connection);
	drmModeFreeConnector(temp);

	igt_display_fini(&display);
}

static void force_edid(int drm_fd, drmModeConnectorPtr connector)
{
	drmModeConnector *temp;

	kmstest_force_connector(drm_fd, connector,
				FORCE_CONNECTOR_ON);
	temp = drmModeGetConnectorCurrent(drm_fd,
					  connector->connector_id);
	drmModeFreeConnector(temp);

	/* Test edid forcing. */
	kmstest_force_edid(drm_fd, connector,
			   igt_kms_get_base_edid());
	temp = drmModeGetConnectorCurrent(drm_fd,
					  connector->connector_id);

	igt_debug("num_conn %i\n", temp->count_modes);

	CHECK_MODE(temp->modes[0], 1920, 1080, 60);
	/*
	 * Don't check non-preferred modes to avoid to tight coupling
	 * with the in-kernel EDID parser.
	 */

	drmModeFreeConnector(temp);

	/* Remove edid. */
	kmstest_force_edid(drm_fd, connector, NULL);
	kmstest_force_connector(drm_fd, connector,
				FORCE_CONNECTOR_UNSPECIFIED);
	temp = drmModeGetConnectorCurrent(drm_fd,
					  connector->connector_id);
	/*
	 * The connector should now have the same number of modes that
	 * it started with.
	 */
	igt_assert_eq(temp->count_modes, connector->count_modes);
	drmModeFreeConnector(temp);

}

static void prune_stale_modes(int drm_fd, drmModeConnectorPtr connector)
{
	int i;
	drmModeConnector *temp;

	kmstest_force_connector(drm_fd, connector,
				FORCE_CONNECTOR_ON);

	/* Test pruning of stale modes. */
	kmstest_force_edid(drm_fd, connector,
			   igt_kms_get_alt_edid());
	temp = drmModeGetConnectorCurrent(drm_fd,
					  connector->connector_id);

	for (i = 0; i < temp->count_modes; i++) {
		if (temp->modes[i].hdisplay == 1400 &&
		    temp->modes[i].vdisplay == 1050)
			break;
	}
	igt_require_f(i != temp->count_modes, "1400x1050 not on mode list\n");
	drmModeFreeConnector(temp);

	kmstest_force_edid(drm_fd, connector,
			   igt_kms_get_base_edid());
	temp = drmModeGetConnectorCurrent(drm_fd,
					  connector->connector_id);

	for (i = 0; i < temp->count_modes; i++) {
		if (temp->modes[i].hdisplay == 1400 &&
		    temp->modes[i].vdisplay == 1050)
			break;
	}
	igt_assert_f(i == temp->count_modes, "1400x1050 not pruned from mode list\n");

	drmModeFreeConnector(temp);

	kmstest_force_edid(drm_fd, connector, NULL);
	kmstest_force_connector(drm_fd, connector,
				FORCE_CONNECTOR_UNSPECIFIED);
}

static int opt_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'r':
		reset_connectors();
		exit(0);
		break;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

struct option long_opts[] = {
	{"reset", 0, 0, 'r'},
	{0, 0, 0, 0}
};
const char *help_str =
	"  --reset\t\tReset all connector force states and edid.\n";

igt_main_args("", long_opts, help_str, opt_handler, NULL)
{
	/* Force the VGA output and test that it worked. */
	int i, drm_fd = 0;
	drmModeRes *res;
	drmModeConnector *connector = NULL;
	struct {
		const char *name;
		const char *desc;
		void (*func)(int, drmModeConnector *);
	} tests[] = {
		{ "force-connector-state",
		  "Test to check the forced connector state.",
		  force_connector_state },
		{ "force-edid",
		  "Test to check the values after forcing edid.",
		  force_edid },
		{ "prune-stale-modes",
		  "Tests pruning of stale modes.",
		  prune_stale_modes }
	};

	igt_fixture {
		unsigned connector_id = 0;

		drm_fd = drm_open_driver_master(DRIVER_ANY);
		kmstest_set_vt_graphics_mode();

		res = drmModeGetResources(drm_fd);
		igt_require(res);

		/* Find a vga connector. */
		for (i = 0; i < res->count_connectors; i++) {
			connector = drmModeGetConnectorCurrent(drm_fd,
							       res->connectors[i]);

			if (connector->connector_type == DRM_MODE_CONNECTOR_VGA) {
				/* Ensure that no override was left in place. */
				kmstest_force_connector(drm_fd,
							connector,
							FORCE_CONNECTOR_UNSPECIFIED);

				/* Only use the first VGA connector. */
				if (!connector_id)
					connector_id = res->connectors[i];
			}

			drmModeFreeConnector(connector);
		}

		if (connector_id)
			goto end;

		/* Find a hdmi connector if we didn't find vga. */
		for (i = 0; i < res->count_connectors; i++) {
			connector = drmModeGetConnectorCurrent(drm_fd,
							       res->connectors[i]);

			if (connector->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
			    connector->connector_type == DRM_MODE_CONNECTOR_HDMIB) {
				/* Ensure that no override was left in place. */
				kmstest_force_connector(drm_fd,
							connector,
							FORCE_CONNECTOR_UNSPECIFIED);

				/* Use the the first HDMI connector. */
				if (!connector_id)
					connector_id = res->connectors[i];
			}

			drmModeFreeConnector(connector);
		}

		igt_require(connector_id);

end:
		/* Reacquire status after clearing any previous overrides. */
		connector = drmModeGetConnector(drm_fd, connector_id);
	}

	igt_describe("Test to detect forced load.");
	igt_subtest("force-load-detect")
		force_load_detect(drm_fd, connector, res);

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		igt_describe_f("%s", tests[i].desc);
		igt_subtest_f("%s", tests[i].name)
			tests[i].func(drm_fd, connector);
	}

	igt_fixture {
		drmModeFreeConnector(connector);
		close(drm_fd);

		reset_connectors();
	}
}
