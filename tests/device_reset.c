// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2020 Intel Corporation. All rights reserved.
 */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <signal.h>

#include "i915/gem.h"
#include "igt.h"
#include "igt_device.h"
#include "igt_device_scan.h"
#include "igt_pci.h"
#include "igt_sysfs.h"
#include "igt_kmod.h"

IGT_TEST_DESCRIPTION("Examine behavior of a driver on device sysfs reset");


#define DEV_PATH_LEN 80
#define DEV_BUS_ADDR_LEN 13 /* addr has form 0000:00:00.0 */

enum reset {
	COLD_RESET,
	FLR_RESET
};

/**
 * Helper structure containing file descriptors
 * and bus address related to tested device
 */
struct device_fds {
	struct {
		int dev;
		int dev_dir;
		int drv_dir;
		int slot_dir; /* pci hotplug slots fd */
	} fds;
	char dev_bus_addr[DEV_BUS_ADDR_LEN];
	bool snd_unload;
};

static int __open_sysfs_dir(int fd, const char* path)
{
	int sysfs;

	sysfs = igt_sysfs_open(fd);
	if (sysfs < 0) {
		return -1;
	}

	fd = openat(sysfs, path, O_DIRECTORY);
	close(sysfs);
	return fd;
}

static int open_device_sysfs_dir(int fd)
{
	return __open_sysfs_dir(fd, "device");
}

static int open_driver_sysfs_dir(int fd)
{
	return __open_sysfs_dir(fd, "device/driver");
}

static bool is_pci_power_ctrl_present(struct pci_device *dev)
{
	int offset;
	uint32_t slot_cap;

	offset = find_pci_cap_offset(dev, PCI_EXPRESS_CAP_ID);
	igt_require_f(offset > 0, "PCI Express Capability not found\n");
	igt_assert(!pci_device_cfg_read_u32(dev, &slot_cap, offset + PCI_SLOT_CAP_OFFSET));
	igt_debug("slot cap register 0x%x\n", slot_cap);

	return slot_cap & PCI_SLOT_PWR_CTRL_PRESENT;
}

static bool is_slot_power_ctrl_present(int fd)
{
	struct pci_device *root;

	/*
	 * Card root port Slot Capabilities Register
	 * determines Power Controller Presence.
	 */
	root = igt_device_get_pci_root_port(fd);
	return is_pci_power_ctrl_present(root);
}

static int open_slot_sysfs_dir(int fd)
{
	struct pci_device *pci_dev = NULL;
	int slot_fd = -1, slot;
	char slot_fd_path[PATH_MAX];

	/* Don't search for slot if root port doesn't support power ctrl */
	if (!is_slot_power_ctrl_present(fd))
		return -ENOTSUP;

	pci_dev = igt_device_get_pci_device(fd);
	igt_require(pci_dev);

	while ((pci_dev = pci_device_get_parent_bridge(pci_dev))) {
		slot = igt_pm_get_pcie_acpihp_slot(pci_dev);
		if (slot == -ENOENT) {
			igt_debug("Bridge PCI device %04x:%02x:%02x.%01x does not support acpihp slot\n",
				  pci_dev->domain, pci_dev->bus, pci_dev->dev, pci_dev->func);
			continue;
		}

		/*
		 * Upon getting the valid acpihp slot number break the loop.
		 * It is the desired acpihp slot for gfx card.
		 */
		if (slot > 0) {
			igt_debug("Bridge PCI device %04x:%02x:%02x.%01x associated acpihp slot %d\n",
				  pci_dev->domain, pci_dev->bus, pci_dev->dev, pci_dev->func, slot);
			break;
		}
	}

	if (!pci_dev)
		return -1;

	snprintf(slot_fd_path, PATH_MAX, "/sys/bus/pci/slots/%d", slot);
	slot_fd = open(slot_fd_path, O_RDONLY);
	if (slot_fd < 0)
		return -errno;

	return slot_fd;
}

/**
 * device_sysfs_path:
 * @fd: opened device file descriptor
 * @path: buffer to store sysfs path to device directory
 *
 * Returns:
 * On successfull path resolution sysfs path to device directory,
 * NULL otherwise
 */
static char *device_sysfs_path(int fd, char *path)
{
	char sysfs[DEV_PATH_LEN];

	if (!igt_sysfs_path(fd, sysfs, sizeof(sysfs)))
		return NULL;

	if (DEV_PATH_LEN <= (strlen(sysfs) + strlen("/device")))
		return NULL;

	strcat(sysfs, "/device");

	return realpath(sysfs, path);
}

static void init_device_fds(struct device_fds *dev)
{
	char dev_path[PATH_MAX];
	char *addr_pos;
	uint32_t devid;

	igt_debug("open device\n");
	/**
	 * As subtests must be able to close examined devices
	 * completely, don't use drm_open_driver() as it keeps
	 * a device file descriptor open for exit handler use.
	 */
	dev->fds.dev = __drm_open_driver(DRIVER_ANY);
	igt_assert_fd(dev->fds.dev);
	if (is_i915_device(dev->fds.dev)) {
		igt_require_gem(dev->fds.dev);

		devid = intel_get_drm_devid(dev->fds.dev);
		if ((IS_HASWELL(devid) || IS_BROADWELL(devid) ||
		     IS_DG1(devid)) &&
		     (igt_kmod_is_loaded("snd_hda_intel"))) {
			igt_debug("Enable WA to unload snd driver\n");
			dev->snd_unload = true;
		}
	}

	igt_assert(device_sysfs_path(dev->fds.dev, dev_path));
	addr_pos = strrchr(dev_path, '/');
	igt_assert(addr_pos);
	igt_assert_eq(sizeof(dev->dev_bus_addr) - 1,
		      snprintf(dev->dev_bus_addr, sizeof(dev->dev_bus_addr),
			       "%s", addr_pos + 1));

	dev->fds.dev_dir = open_device_sysfs_dir(dev->fds.dev);
	igt_assert_fd(dev->fds.dev_dir);

	dev->fds.drv_dir = open_driver_sysfs_dir(dev->fds.dev);
	igt_assert_fd(dev->fds.drv_dir);

	dev->fds.slot_dir = open_slot_sysfs_dir(dev->fds.dev);
}

static int close_if_opened(int *fd)
{
	int rc = 0;

	if (fd && *fd != -1) {
		rc = close(*fd);
		*fd = -1;
	}
	return rc;
}

static void cleanup_device_fds(struct device_fds *dev)
{
	igt_ignore_warn(close_if_opened(&dev->fds.dev));
	igt_ignore_warn(close_if_opened(&dev->fds.dev_dir));
	igt_ignore_warn(close_if_opened(&dev->fds.drv_dir));
	igt_ignore_warn(close_if_opened(&dev->fds.slot_dir));
}

/**
 * is_sysfs_reset_supported:
 * @fd: opened device file descriptor
 *
 * Check if device supports reset based on sysfs file presence.
 *
 * Returns:
 * True if device supports reset, false otherwise.
 */
static bool is_sysfs_reset_supported(int fd)
{
	struct stat st;
	int rc;
	int sysfs;
	int reset_fd = -1;

	sysfs = igt_sysfs_open(fd);

	if (sysfs >= 0) {
		reset_fd = openat(sysfs, "device/reset", O_WRONLY);
		close(sysfs);
	}

	if (reset_fd < 0)
		return false;

	rc = fstat(reset_fd, &st);
	close(reset_fd);

	if (rc || !S_ISREG(st.st_mode))
		return false;

	return true;
}

/**
 * is_sysfs_cold_reset_supported:
 * @fd: opened device file descriptor
 *
 * Check if device supports cold reset based on sysfs file presence.
 *
 * Returns:
 * True if device supports reset, false otherwise.
 */
static bool is_sysfs_cold_reset_supported(int slot_fd)
{
	struct stat st;
	int rc;
	int cold_reset_fd = -1;

	cold_reset_fd = openat(slot_fd, "power", O_WRONLY);

	if (cold_reset_fd < 0)
		return false;

	rc = fstat(cold_reset_fd, &st);
	close(cold_reset_fd);

	if (rc || !S_ISREG(st.st_mode))
		return false;

	return true;
}

/* Unbind the driver from the device */
static void driver_unbind(struct device_fds *dev)
{
	/**
	 * FIXME: Unbinding the i915 driver on affected platforms with
	 * audio results in a kernel WARN on "i915 raw-wakerefs=1
	 * wakelocks=1 on cleanup". The below CI friendly user level
	 * workaround to unload and de-couple audio from IGT testing,
	 * prevents the warning from appearing. Drop this hack as soon
	 * as this is fixed in the kernel. unbind/re-bind validation
	 * on audio side is not robust and we could have potential
	 * failures blocking display CI, currently this seems to the
	 * safest and easiest way out.
	 */
	if (dev->snd_unload) {
		igt_terminate_process(SIGTERM, "alsactl");

		/* unbind snd_hda_intel */
		kick_snd_hda_intel();

		if (igt_kmod_unload("snd_hda_intel", 0)) {
			dev->snd_unload = false;
			igt_warn("Could not unload snd_hda_intel\n");
			igt_kmod_list_loaded();
			igt_lsof("/dev/snd");
			igt_skip("Audio is in use, skipping\n");
		} else {
			igt_info("Preventively unloaded snd_hda_intel\n");
		}
	}

	igt_debug("unbind the driver from the device\n");
	igt_assert(igt_sysfs_set(dev->fds.drv_dir, "unbind",
		   dev->dev_bus_addr));
}

/* Re-bind the driver to the device */
static void driver_bind(struct device_fds *dev)
{
	igt_debug("rebind the driver to the device\n");
	igt_abort_on_f(!igt_sysfs_set(dev->fds.drv_dir, "bind",
		       dev->dev_bus_addr), "driver rebind failed");

	if (dev->snd_unload)
		igt_kmod_load("snd_hda_intel", NULL);
}

/* Initiate device reset */
static void initiate_device_reset(struct device_fds *dev, enum reset type)
{
	igt_debug("reset device\n");

	if (type == FLR_RESET) {
		igt_assert(igt_sysfs_set(dev->fds.dev_dir, "reset", "1"));
	} else if (type == COLD_RESET) {
		igt_assert(igt_sysfs_set(dev->fds.slot_dir, "power", "0"));
		igt_assert(!igt_sysfs_get_boolean(dev->fds.slot_dir, "power"));
		igt_assert(igt_sysfs_set(dev->fds.slot_dir, "power", "1"));
	}

}

static bool is_i915_wedged(int i915)
{
	int err = 0;

	if (ioctl(i915, DRM_IOCTL_I915_GEM_THROTTLE))
		err = -errno;
	return err == -EIO;
}

/**
 * healthcheck:
 * @dev: structure with device descriptor, if descriptor equals -1
 * 	 the device is reopened
 */
static void healthcheck(struct device_fds *dev)
{
	if (dev->fds.dev == -1) {
		/* refresh device list */
		igt_devices_scan(true);
		igt_debug("reopen the device\n");
		dev->fds.dev = __drm_open_driver(DRIVER_ANY);
	}
	igt_assert_fd(dev->fds.dev);

	if (is_i915_device(dev->fds.dev))
		igt_assert(!is_i915_wedged(dev->fds.dev));
}

/**
 * set_device_filter:
 *
 * Sets device filter to ensure subtests always reopen the same device
 *
 * @dev_path: path to device under tests
 */
static void set_device_filter(const char* dev_path)
{
#define FILTER_PREFIX_LEN 4
	char filter[PATH_MAX + FILTER_PREFIX_LEN];

	igt_assert_lt(FILTER_PREFIX_LEN, snprintf(filter, sizeof(filter),
						  "sys:%s", dev_path));
	igt_device_filter_free_all();
	igt_assert_eq(igt_device_filter_add(filter), 1);
}

static void unbind_reset_rebind(struct device_fds *dev, enum reset type)
{
	igt_debug("close the device\n");
	close_if_opened(&dev->fds.dev);

	driver_unbind(dev);

	initiate_device_reset(dev, type);

	driver_bind(dev);
}

igt_main
{
	struct device_fds dev = { .fds = {-1, -1, -1}, .dev_bus_addr = {0}, };

	igt_fixture {
		char dev_path[PATH_MAX];

		igt_debug("opening device\n");
		init_device_fds(&dev);

		/* Make sure subtests always reopen the same device */
		igt_assert(device_sysfs_path(dev.fds.dev, dev_path));
		set_device_filter(dev_path);

		igt_skip_on(!is_sysfs_reset_supported(dev.fds.dev));
	}

	igt_describe("Unbinds driver from device, initiates reset"
		     " then rebinds driver to device");
	igt_subtest("unbind-reset-rebind") {
		unbind_reset_rebind(&dev, FLR_RESET);
		healthcheck(&dev);
	}

	igt_describe("Resets device with bound driver");
	igt_subtest("reset-bound") {
		initiate_device_reset(&dev, FLR_RESET);
		healthcheck(&dev);
	}

	igt_subtest_group {
		igt_fixture {
			igt_skip_on_f(dev.fds.slot_dir < 0, "Gfx Card does not support any "
				      "pcie slot for cold reset\n");
			igt_skip_on(!is_sysfs_cold_reset_supported(dev.fds.slot_dir));
		}

		igt_describe("Unbinds driver from device, initiates cold reset"
			     " then rebinds driver to device");
		igt_subtest("unbind-cold-reset-rebind") {
			unbind_reset_rebind(&dev, COLD_RESET);
			healthcheck(&dev);
		}

		igt_describe("Cold Resets device with bound driver");
		igt_subtest("cold-reset-bound") {
			initiate_device_reset(&dev, COLD_RESET);
			/*
			 * Cold reset will initiate card boot sequence again,
			 * therefore let healthcheck() re-epen the dev fd.
			 */
			dev.fds.dev = -1;
			healthcheck(&dev);
		}
	}

	igt_fixture {
		cleanup_device_fds(&dev);
	}
}
