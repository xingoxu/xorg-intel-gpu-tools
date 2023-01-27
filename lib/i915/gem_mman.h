/*
 * Copyright © 2007, 2011, 2013, 2014, 2019 Intel Corporation
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

#ifndef GEM_MMAN_H
#define GEM_MMAN_H

#include <stdint.h>

void *gem_mmap__gtt(int fd, uint32_t handle, uint64_t size, unsigned prot);
void *gem_mmap__cpu(int fd, uint32_t handle, uint64_t offset, uint64_t size, unsigned prot);
void *gem_mmap_offset__cpu(int fd, uint32_t handle, uint64_t offset,
			   uint64_t size, unsigned prot);

bool gem_mmap__has_wc(int fd);
bool gem_mmap_offset__has_wc(int fd);
void *gem_mmap__wc(int fd, uint32_t handle, uint64_t offset, uint64_t size, unsigned prot);
void *gem_mmap_offset__wc(int fd, uint32_t handle, uint64_t offset,
			  uint64_t size, unsigned prot);
void *gem_mmap_offset__fixed(int fd, uint32_t handle, uint64_t offset,
			     uint64_t size, unsigned prot);
void *gem_mmap__device_coherent(int fd, uint32_t handle, uint64_t offset,
				uint64_t size, unsigned prot);
bool gem_mmap__has_device_coherent(int fd);
void *gem_mmap__cpu_coherent(int fd, uint32_t handle, uint64_t offset,
			     uint64_t size, unsigned prot);

bool gem_has_mappable_ggtt(int i915);
void gem_require_mappable_ggtt(int i915);
bool gem_has_mmap_offset(int fd);
bool gem_has_legacy_mmap(int fd);

void *__gem_mmap__gtt(int fd, uint32_t handle, uint64_t size, unsigned prot);
void *__gem_mmap__cpu(int fd, uint32_t handle, uint64_t offset, uint64_t size, unsigned prot);
void *__gem_mmap_offset__cpu(int fd, uint32_t handle, uint64_t offset,
			     uint64_t size, unsigned prot);
void *__gem_mmap__wc(int fd, uint32_t handle, uint64_t offset, uint64_t size, unsigned prot);
void *__gem_mmap_offset__wc(int fd, uint32_t handle, uint64_t offset,
			    uint64_t size, unsigned prot);
void *__gem_mmap_offset__fixed(int fd, uint32_t handle, uint64_t offset,
			       uint64_t size, unsigned prot);
void *__gem_mmap__device_coherent(int fd, uint32_t handle, uint64_t offset,
				  uint64_t size, unsigned prot);
void *__gem_mmap_offset(int fd, uint32_t handle, uint64_t offset, uint64_t size,
			unsigned int prot, uint64_t flags);
void *__gem_mmap__cpu_coherent(int fd, uint32_t handle, uint64_t offset,
			       uint64_t size, unsigned prot);

int gem_munmap(void *ptr, uint64_t size);

/**
 * gem_require_mmap_offset:
 * @fd: open i915 drm file descriptor
 *
 * Feature test macro to query whether is possible to map memory using mmap
 * offset interface. Automatically skips through igt_require() if not.
 */
#define gem_require_mmap_offset(fd) igt_require(gem_has_mmap_offset(fd))

/**
 * gem_require_mmap_wc:
 * @fd: open i915 drm file descriptor
 *
 * Feature test macro to query whether direct (i.e. cpu access path, bypassing
 * the gtt) write-combine memory mappings are available. Automatically skips
 * through igt_require() if not.
 */
#define gem_require_mmap_wc(fd) igt_require(gem_mmap__has_wc(fd))

/**
 * gem_require_mmap_offset_wc:
 * @fd: open i915 drm file descriptor
 *
 * Feature test macro to query whether direct (i.e. cpu access path, bypassing
 * the gtt) write-combine memory mappings are available. Automatically skips
 * through igt_require() if not.
 */
#define gem_require_mmap_offset_wc(fd) igt_require(gem_mmap_offset__has_wc(fd))

/**
 * gem_require_mmap_offset_device_coherent:
 * @fd: open i915 drm file descriptor
 *
 * Feature test macro to query whether direct (i.e. cpu access path, bypassing
 * the gtt) write-combine memory mappings are available, or fixed mapping for
 * discrete. Automatically skips through igt_require() if not.
 */
#define gem_require_mmap_device_coherent(fd) igt_require(gem_mmap__has_device_coherent(fd))

extern const struct mmap_offset {
	const char *name;
	unsigned int type;
	unsigned int domain;
} mmap_offset_types[];

bool gem_has_mmap_offset_type(int fd, const struct mmap_offset *t);

#define for_each_mmap_offset_type(fd, __t) \
	for (const struct mmap_offset *__t = mmap_offset_types; \
	     (__t)->name; \
	     (__t)++) \
		for_each_if(gem_has_mmap_offset_type((fd), (__t)))

uint64_t gem_available_aperture_size(int fd);
uint64_t gem_aperture_size(int fd);
uint64_t gem_global_aperture_size(int fd);
uint64_t gem_mappable_aperture_size(int fd);
int gem_available_fences(int fd);

#endif /* GEM_MMAN_H */

