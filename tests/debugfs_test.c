/*
 * Copyright © 2017 Intel Corporation
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
#include "config.h"

#include "i915/gem.h"
#include "igt.h"
#include "igt_hwmon.h"
#include "igt_sysfs.h"
#include <fcntl.h>
#include <sys/types.h>
#include <dirent.h>

IGT_TEST_DESCRIPTION("Read entries from debugfs, hwmon and sysfs paths.");

static void read_and_discard_sysfs_entries(int path_fd, int indent)
{
	struct dirent *dirent;
	DIR *dir;
	char tabs[8];
	int i;

	igt_assert(indent < sizeof(tabs) - 1);

	for (i = 0; i < indent; i++)
		tabs[i] = '\t';
	tabs[i] = '\0';

	dir = fdopendir(path_fd);
	if (!dir)
		return;

	while ((dirent = readdir(dir))) {
		if (!strcmp(dirent->d_name, ".") ||
		    !strcmp(dirent->d_name, ".."))
			continue;

		if (dirent->d_type == DT_DIR) {
			int sub_fd;

			sub_fd = openat(path_fd, dirent->d_name,
					O_RDONLY | O_DIRECTORY);
			if (sub_fd < 0)
				continue;

			igt_debug("%sEntering subdir %s\n", tabs, dirent->d_name);
			read_and_discard_sysfs_entries(sub_fd, indent + 1);
			close(sub_fd);
		} else if (dirent->d_type == DT_REG) {
			char buf[512];
			int sub_fd;
			ssize_t ret;

			igt_kmsg(KMSG_DEBUG "Reading file \"%s\"\n", dirent->d_name);
			igt_debug("%sReading file \"%s\"\n", tabs, dirent->d_name);
			igt_set_timeout(5, "reading sysfs entry");

			sub_fd = openat(path_fd, dirent->d_name, O_RDONLY | O_NONBLOCK);
			if (sub_fd == -1) {
				igt_debug("%sCould not open file \"%s\" with error: %m\n",
					  tabs, dirent->d_name);
				continue;
			}

			do {
				ret = read(sub_fd, buf, sizeof(buf));
			} while (ret == sizeof(buf));

			if (ret == -1)
				igt_debug("%sCould not read file \"%s\" with error: %m\n",
					  tabs, dirent->d_name);

			igt_reset_timeout();
			close(sub_fd);
		}
	}
	closedir(dir);
}

static void kms_tests(int fd, int debugfs)
{
	igt_display_t display;
	struct igt_fb fb[IGT_MAX_PIPES];
	enum pipe pipe;
	int ret;

	igt_fixture
		igt_display_require(&display, fd);

	igt_subtest("read_all_entries_display_on") {
		/* try to light all pipes */
retry:
		for_each_pipe(&display, pipe) {
			igt_output_t *output;

			for_each_valid_output_on_pipe(&display, pipe, output) {
				igt_plane_t *primary;
				drmModeModeInfo *mode;

				if (output->pending_pipe != PIPE_NONE)
					continue;

				igt_output_set_pipe(output, pipe);
				primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
				mode = igt_output_get_mode(output);
				igt_create_pattern_fb(display.drm_fd,
						      mode->hdisplay, mode->vdisplay,
						      DRM_FORMAT_XRGB8888,
						      DRM_FORMAT_MOD_LINEAR, &fb[pipe]);

				/* Set a valid fb as some debugfs like to inspect it on a active pipe */
				igt_plane_set_fb(primary, &fb[pipe]);
				break;
			}
		}

		if (display.is_atomic)
			ret = igt_display_try_commit_atomic(&display,
					DRM_MODE_ATOMIC_TEST_ONLY |
					DRM_MODE_ATOMIC_ALLOW_MODESET,
					NULL);
		else
			ret = igt_display_try_commit2(&display, COMMIT_LEGACY);

		if (ret) {
			igt_output_t *output;
			bool found = igt_override_all_active_output_modes_to_fit_bw(&display);
			igt_require_f(found, "No valid mode combo found.\n");

			for_each_connected_output(&display, output)
				igt_output_set_pipe(output, PIPE_NONE);

			goto retry;
		}

		igt_display_commit2(&display, display.is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

		read_and_discard_sysfs_entries(debugfs, 0);
	}

	igt_subtest("read_all_entries_display_off") {
		igt_output_t *output;
		igt_plane_t *plane;

		for_each_connected_output(&display, output)
			igt_output_set_pipe(output, PIPE_NONE);

		for_each_pipe(&display, pipe)
			for_each_plane_on_pipe(&display, pipe, plane)
				igt_plane_set_fb(plane, NULL);

		igt_display_commit2(&display, display.is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

		read_and_discard_sysfs_entries(debugfs, 0);
	}

	igt_fixture
		igt_display_fini(&display);
}

igt_main
{
	int fd = -1, debugfs, sysfs, hwmon_fd;

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		igt_require_gem(fd);
		debugfs = igt_debugfs_dir(fd);
		sysfs = igt_sysfs_open(fd);

		kmstest_set_vt_graphics_mode();
	}

	igt_describe("Read all entries from sysfs path.");
	igt_subtest("sysfs")
		read_and_discard_sysfs_entries(sysfs, 0);
	igt_describe("Read all entries from debugfs path.");
	igt_subtest("read_all_entries")
		read_and_discard_sysfs_entries(debugfs, 0);

	igt_describe("Read all entries from hwmon path");
	igt_subtest("basic-hwmon") {
		igt_require_f(gem_has_lmem(fd), "Test applicable only for dgfx\n");
		hwmon_fd = igt_hwmon_open(fd);
		igt_assert(hwmon_fd >= 0);
		read_and_discard_sysfs_entries(hwmon_fd, 0);
		close(hwmon_fd);
	}

	igt_describe("Read all debugfs entries with display on/off.");
	igt_subtest_group
		kms_tests(fd, debugfs);

	igt_fixture {
		close(sysfs);
		close(debugfs);
		close(fd);
	}
}
