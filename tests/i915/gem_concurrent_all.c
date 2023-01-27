/*
 * Copyright © 2009,2012,2013 Intel Corporation
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
 *    Chris Wilson <chris@chris-wilson.co.uk>
 *    Daniel Vetter <daniel.vetter@ffwll.ch>
 *
 */

/** @file gem_concurrent.c
 *
 * This is a test of pread/pwrite/mmap behavior when writing to active
 * buffers.
 *
 * Based on gem_gtt_concurrent_blt.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <drm.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"
#include "igt_vgem.h"

IGT_TEST_DESCRIPTION("Test of pread/pwrite/mmap behavior when writing to active"
		     " buffers.");

int fd, devid, gen;
int vgem_drv = -1;
int all;
int pass;
uint64_t ahnd;

struct create {
	const char *name;
	void (*require)(const struct create *, unsigned);
	struct intel_buf *(*create)(struct buf_ops *bops, uint32_t width,
				    uint32_t height, uint32_t tiling,
				    uint64_t size);
};

struct size {
	const char *name;
	int width, height;
};

struct buffers {
	const char *name;
	const struct create *create;
	const struct access_mode *mode;
	const struct size *size;
	struct buf_ops *bops;
	struct intel_bb *ibb;
	struct intel_buf **src, **dst;
	struct intel_buf *snoop, *spare;
	uint32_t *tmp;
	int width, height, npixels, page_size;
	int count, num_buffers;
};

#define MIN_BUFFERS 3

static void blt_copy_bo(struct buffers *b, struct intel_buf *dst,
			struct intel_buf *src);

static void
nop_release_bo(struct intel_buf *buf)
{
	if (buf->ptr)
		intel_buf_unmap(buf);
	intel_buf_destroy(buf);
}

static void
prw_set_bo(struct buffers *b, struct intel_buf *buf, uint32_t val)
{
	for (int i = 0; i < b->npixels; i++)
		b->tmp[i] = val;
	gem_write(fd, buf->handle, 0, b->tmp, 4*b->npixels);
}

static void
prw_cmp_bo(struct buffers *b, struct intel_buf *buf, uint32_t val)
{
	uint32_t *vaddr;

	vaddr = b->tmp;
	gem_read(fd, buf->handle, 0, vaddr, 4*b->npixels);
	for (int i = 0; i < b->npixels; i++)
		igt_assert_eq_u32(vaddr[i], val);
}

#define pixel(y, width) ((y)*(width) + (((y) + pass)%(width)))

static void
partial_set_bo(struct buffers *b, struct intel_buf *buf, uint32_t val)
{
	for (int y = 0; y < b->height; y++)
		gem_write(fd, buf->handle, 4*pixel(y, b->width), &val, 4);
}

static void
partial_cmp_bo(struct buffers *b, struct intel_buf *buf, uint32_t val)
{
	for (int y = 0; y < b->height; y++) {
		uint32_t tmp;

		gem_read(fd, buf->handle, 4*pixel(y, b->width), &tmp, 4);
		igt_assert_eq_u32(tmp, val);
	}
}

static struct intel_buf *
create_normal_bo(struct buf_ops *bops, uint32_t width,
		 uint32_t height, uint32_t tiling, uint64_t size)
{
	struct intel_buf *buf;
	int bpp = size/height/width * 8;

	buf = intel_buf_create(bops, width, height, bpp, 0, tiling, 0);

	return buf;
}

static void can_create_normal(const struct create *create, unsigned count)
{
}

#if HAVE_CREATE_PRIVATE
static struct intel_buf *
create_private_bo(struct buf_ops *bops, uint32_t width, uint32_t height,
		  uint32_t tiling, uint64_t size)
{
	struct intel_buf *buf;
	uint32_t handle, buf_handle, name;
	int bpp = size/height/width * 8;

	/* XXX gem_create_with_flags(fd, size, I915_CREATE_PRIVATE); */

	handle = gem_create(fd, size);
	name = gem_flink(fd, handle);
	buf_handle = gem_open(fd, name);

	buf = intel_buf_create_using_handle(bops, buf_handle,
					    width, height, bpp, 0, tiling, 0);
	intel_buf_set_ownership(buf, true);

	gem_close(fd, handle);

	return buf;
}

static void can_create_private(const struct create *create, unsigned count)
{
	igt_require(0);
}
#endif

#if HAVE_CREATE_STOLEN
static struct intel_buf *
create_stolen_bo(struct buf_ops *bops, uint32_t width, uint32_t height,
		 uint32_t tiling, uint64_t size)
{
	struct intel_buf *buf;
	uint32_t handle, buf_handle, name;
	int bpp = size/height/width * 8;

	/* XXX gem_create_with_flags(fd, size, I915_CREATE_PRIVATE); */

	handle = gem_create(fd, size);
	name = gem_flink(fd, handle);
	buf_handle = gem_open(fd, name);

	buf = intel_buf_create_using_handle(bops, buf_handle,
					    width, height, bpp, 0, tiling, 0);
	intel_buf_set_ownership(buf, true);

	gem_close(fd, handle);

	return buf;
}

static void can_create_stolen(const struct create *create, unsigned count)
{
	/* XXX check num_buffers against available stolen */
	igt_require(0);
}
#endif

static void create_cpu_require(const struct create *create, unsigned count)
{
#if HAVE_CREATE_STOLEN
	igt_require(create->create != create_stolen_bo);
#endif
}

static struct intel_buf *
create_bo(const struct buffers *b, uint32_t tiling)
{
	return b->create->create(b->bops, b->width, b->height,
				 tiling, 4*b->npixels);
}

static struct intel_buf *
unmapped_create_bo(const struct buffers *b)
{
	return create_bo(b, I915_TILING_NONE);
}

static void create_snoop_require(const struct create *create, unsigned count)
{
	static bool check_llc = true;
	static bool has_snoop;

	create_cpu_require(create, count);
	if (check_llc) {
		has_snoop = !gem_has_llc(fd);
		check_llc = false;
	}

	igt_require(has_snoop);
}

static struct intel_buf *
snoop_create_bo(const struct buffers *b)
{
	struct intel_buf *buf;

	buf = unmapped_create_bo(b);
	__gem_set_caching(fd, buf->handle, I915_CACHING_CACHED);

	return buf;
}

static void create_userptr_require(const struct create *create, unsigned count)
{
	static int has_userptr = -1;
	if (has_userptr < 0) {
		struct drm_i915_gem_userptr arg;

		has_userptr = 0;

		memset(&arg, 0, sizeof(arg));
		arg.user_ptr = -4096ULL;
		arg.user_size = 8192;
		errno = 0;
		drmIoctl(fd, DRM_IOCTL_I915_GEM_USERPTR, &arg);
		if (errno == EFAULT) {
			igt_assert(posix_memalign((void **)&arg.user_ptr,
						  4096, arg.user_size) == 0);
			has_userptr = drmIoctl(fd,
					 DRM_IOCTL_I915_GEM_USERPTR,
					 &arg) == 0;
			free(from_user_pointer(arg.user_ptr));
		}

	}
	igt_require(has_userptr);
}

static struct intel_buf *
userptr_create_bo(const struct buffers *b)
{
	struct drm_i915_gem_userptr userptr;
	struct intel_buf *buf;
	void *ptr;

	memset(&userptr, 0, sizeof(userptr));
	userptr.user_size = b->page_size;

	ptr = mmap(NULL, userptr.user_size,
		   PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
	igt_assert(ptr != (void *)-1);
	userptr.user_ptr = to_user_pointer(ptr);

	do_or_die(drmIoctl(fd, DRM_IOCTL_I915_GEM_USERPTR, &userptr));
	buf = intel_buf_create_using_handle(b->bops, userptr.handle,
					    b->width, b->height, 32, 0,
					    I915_TILING_NONE, 0);
	intel_buf_set_ownership(buf, true);

	buf->ptr = (void *) from_user_pointer(userptr.user_ptr);

	return buf;
}

static void
userptr_set_bo(struct buffers *b, struct intel_buf *buf, uint32_t val)
{
	int size = b->npixels;
	uint32_t *vaddr = buf->ptr;

	gem_set_domain(fd, buf->handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
	while (size--)
		*vaddr++ = val;
}

static void
userptr_cmp_bo(struct buffers *b, struct intel_buf *buf, uint32_t val)
{
	int size =  b->npixels;
	uint32_t *vaddr = buf->ptr;

	gem_set_domain(fd, buf->handle,
		       I915_GEM_DOMAIN_CPU, 0);
	while (size--)
		igt_assert_eq_u32(*vaddr++, val);
}

static void
userptr_release_bo(struct intel_buf *buf)
{
	igt_assert(buf->ptr);

	munmap(buf->ptr, buf->surface[0].size);
	buf->ptr = NULL;

	intel_buf_destroy(buf);
}

static void create_dmabuf_require(const struct create *create, unsigned count)
{
	static int has_dmabuf = -1;
	if (has_dmabuf < 0) {
		struct drm_prime_handle args;
		void *ptr;

		memset(&args, 0, sizeof(args));
		args.handle = gem_create(fd, 4096);
		args.flags = DRM_RDWR;
		args.fd = -1;

		drmIoctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &args);
		gem_close(fd, args.handle);

		has_dmabuf = 0;
		ptr = mmap(NULL, 4096, PROT_READ, MAP_SHARED, args.fd, 0);
		if (ptr != MAP_FAILED) {
			has_dmabuf = 1;
			munmap(ptr, 4096);
		}

		close(args.fd);
	}
	igt_require(has_dmabuf);
	igt_require_files(2*count);
}

struct dmabuf {
	int fd;
	void *map;
};

static struct intel_buf *
dmabuf_create_bo(const struct buffers *b)
{
	struct drm_prime_handle args;
	static struct intel_buf *buf;
	struct dmabuf *dmabuf;
	int size;
	uint32_t handle;

	size = b->page_size;

	memset(&args, 0, sizeof(args));
	args.handle = gem_create(fd, size);
	args.flags = DRM_RDWR;
	args.fd = -1;

	do_ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &args);
	gem_close(fd, args.handle);
	igt_assert(args.fd != -1);

	handle = prime_fd_to_handle(buf_ops_get_fd(b->bops), args.fd);
	buf = intel_buf_create_using_handle(b->bops, handle,
					    b->width, b->height, 32, 0,
					    I915_TILING_NONE, 0);
	intel_buf_set_ownership(buf, true);

	dmabuf = malloc(sizeof(*dmabuf));
	igt_assert(dmabuf);

	dmabuf->fd = args.fd;
	dmabuf->map = mmap(NULL, size,
			   PROT_READ | PROT_WRITE, MAP_SHARED,
			   dmabuf->fd, 0);
	igt_assert(dmabuf->map != (void *)-1);

	buf->ptr = (void *) dmabuf;

	return buf;
}

static void
dmabuf_set_bo(struct buffers *b, struct intel_buf *buf, uint32_t val)
{
	struct dmabuf *dmabuf = (void *) buf->ptr;
	uint32_t *v = dmabuf->map;
	int y;

	prime_sync_start(dmabuf->fd, true);
	for (y = 0; y < b->height; y++)
		v[pixel(y, b->width)] = val;
	prime_sync_end(dmabuf->fd, true);
}

static void
dmabuf_cmp_bo(struct buffers *b, struct intel_buf *buf, uint32_t val)
{
	struct dmabuf *dmabuf = (void *) buf->ptr;
	uint32_t *v = dmabuf->map;
	int y;

	prime_sync_start(dmabuf->fd, false);
	for (y = 0; y < b->height; y++)
		igt_assert_eq_u32(v[pixel(y, b->width)], val);
	prime_sync_end(dmabuf->fd, false);
}

static void
dmabuf_release_bo(struct intel_buf *buf)
{
	struct dmabuf *dmabuf = (void *) buf->ptr;
	igt_assert(dmabuf);
	buf->ptr = NULL;

	munmap(dmabuf->map, buf->surface[0].size);
	close(dmabuf->fd);
	free(dmabuf);

	intel_buf_destroy(buf);
}

static bool has_prime_export(int _fd)
{
	uint64_t value;

	if (drmGetCap(_fd, DRM_CAP_PRIME, &value))
		return false;

	return value & DRM_PRIME_CAP_EXPORT;
}

static void create_vgem_require(const struct create *create, unsigned count)
{
	igt_require(vgem_drv != -1);
	igt_require(has_prime_export(vgem_drv));
	create_dmabuf_require(create, count);
}

static void create_gtt_require(const struct create *create, unsigned count)
{
	gem_require_mappable_ggtt(fd);
}

static struct intel_buf *
vgem_create_bo(const struct buffers *b)
{
	struct drm_prime_handle args;
	struct intel_buf *buf;
	struct vgem_bo vgem;
	struct dmabuf *dmabuf;
	uint32_t handle;

	igt_assert(vgem_drv != -1);

	vgem.width = b->width;
	vgem.height = b->height;
	vgem.bpp = 32;
	vgem_create(vgem_drv, &vgem);

	memset(&args, 0, sizeof(args));
	args.handle = vgem.handle;
	args.flags = DRM_RDWR;
	args.fd = -1;

	do_ioctl(vgem_drv, DRM_IOCTL_PRIME_HANDLE_TO_FD, &args);
	gem_close(vgem_drv, args.handle);
	igt_assert(args.fd != -1);

	handle = prime_fd_to_handle(buf_ops_get_fd(b->bops), args.fd);
	buf = intel_buf_create_using_handle(b->bops, handle,
					    vgem.width, vgem.height, vgem.bpp,
					    0, I915_TILING_NONE, 0);
	intel_buf_set_ownership(buf, true);

	dmabuf = malloc(sizeof(*dmabuf));
	igt_assert(dmabuf);

	dmabuf->fd = args.fd;
	dmabuf->map = mmap(NULL, vgem.size,
			   PROT_READ | PROT_WRITE, MAP_SHARED,
			   dmabuf->fd, 0);
	igt_assert(dmabuf->map != (void *)-1);

	buf->ptr = (void *) dmabuf;

	return buf;
}

static void
gtt_set_bo(struct buffers *b, struct intel_buf *buf, uint32_t val)
{
	uint32_t *vaddr = buf->ptr;

	gem_set_domain(fd, buf->handle,
		       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	for (int y = 0; y < b->height; y++)
		vaddr[pixel(y, b->width)] = val;
}

static void
gtt_cmp_bo(struct buffers *b, struct  intel_buf *buf, uint32_t val)
{
	uint32_t *vaddr = buf->ptr;

	/* GTT access is slow. So we just compare a few points */
	gem_set_domain(fd, buf->handle,
		       I915_GEM_DOMAIN_GTT, 0);

	for (int y = 0; y < b->height; y++)
		igt_assert_eq_u32(vaddr[pixel(y, b->width)], val);
}

static struct intel_buf *
map_bo(struct intel_buf *buf)
{
	/* gtt map doesn't have a write parameter, so just keep the mapping
	 * around (to avoid the set_domain with the gtt write domain set) and
	 * manually tell the kernel when we start access the gtt. */
	buf->ptr = gem_mmap__gtt(buf_ops_get_fd(buf->bops), buf->handle,
				 buf->surface[0].size, PROT_READ | PROT_WRITE);

	return buf;
}

static struct intel_buf *
gtt_create_bo(const struct buffers *b)
{
	return map_bo(unmapped_create_bo(b));
}

static struct intel_buf *
gttX_create_bo(const struct buffers *b)
{
	return map_bo(create_bo(b, I915_TILING_X));
}

static void bit17_require(void)
{
	static bool has_tiling2, checked;

#define DRM_IOCTL_I915_GEM_GET_TILING2	DRM_IOWR (DRM_COMMAND_BASE + DRM_I915_GEM_GET_TILING, struct drm_i915_gem_get_tiling2)

	if (!checked) {
		struct drm_i915_gem_get_tiling2 {
			uint32_t handle;
			uint32_t tiling_mode;
			uint32_t swizzle_mode;
			uint32_t phys_swizzle_mode;
		} arg = {};
		int err;

		checked = true;
		arg.handle = gem_create(fd, 4096);
		err = __gem_set_tiling(fd, arg.handle, I915_TILING_X, 512);
		if (!err) {
			igt_ioctl(fd, DRM_IOCTL_I915_GEM_GET_TILING2, &arg);
			if (!errno && arg.phys_swizzle_mode == arg.swizzle_mode)
				has_tiling2 = true;
		}

		errno = 0;
		gem_close(fd, arg.handle);
	}

	igt_require(has_tiling2);
}

static void wc_require(void)
{
	bit17_require();
	gem_require_mmap_wc(fd);
}

static void
wc_create_require(const struct create *create, unsigned count)
{
	wc_require();
}

static struct intel_buf *
wc_create_bo(const struct buffers *b)
{
	static struct intel_buf *buf;

	buf = unmapped_create_bo(b);
	buf->ptr = gem_mmap__wc(fd, buf->handle, 0, buf->surface[0].size,
				PROT_READ | PROT_WRITE);
	return buf;
}

static void
wc_release_bo(struct intel_buf *buf)
{
	igt_assert(buf->ptr);

	munmap(buf->ptr, buf->surface[0].size);
	buf->ptr = 0;

	nop_release_bo(buf);
}

static struct intel_buf *
gpu_create_bo(const struct buffers *b)
{
	return unmapped_create_bo(b);
}

static struct intel_buf *
gpuX_create_bo(const struct buffers *b)
{
	return create_bo(b, I915_TILING_X);
}

static void
cpu_set_bo(struct buffers *b, struct intel_buf *buf, uint32_t val)
{
	int size = b->npixels;
	uint32_t *vaddr;

	intel_buf_device_map(buf, true);

	vaddr = buf->ptr;
	while (size--)
		*vaddr++ = val;
	intel_buf_unmap(buf);
}

static void
cpu_cmp_bo(struct buffers *b, struct intel_buf *buf, uint32_t val)
{
	int size = b->npixels;
	uint32_t *vaddr;

	intel_buf_cpu_map(buf, false);
	vaddr = buf->ptr;
	while (size--)
		igt_assert_eq_u32(*vaddr++, val);
	intel_buf_unmap(buf);
}

static void
gpu_set_bo(struct buffers *buffers, struct intel_buf *buf, uint32_t val)
{
	struct drm_i915_gem_relocation_entry reloc[1];
	struct drm_i915_gem_exec_object2 gem_exec[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t tmp[10], *b;
	uint64_t addr = 0;

	memset(reloc, 0, sizeof(reloc));
	memset(gem_exec, 0, sizeof(gem_exec));
	memset(&execbuf, 0, sizeof(execbuf));

	if (ahnd) {
		addr = buf->addr.offset;
		if (INVALID_ADDR(addr)) {
			addr = intel_allocator_alloc(buffers->ibb->allocator_handle,
						     buf->handle, buf->size, 0);
			buf->addr.offset = addr;
		}
	}

	b = tmp;
	*b++ = XY_COLOR_BLT_CMD_NOLEN |
		((gen >= 8) ? 5 : 4) |
		COLOR_BLT_WRITE_ALPHA | XY_COLOR_BLT_WRITE_RGB;
	if (gen >= 4 && buf->tiling) {
		b[-1] |= XY_COLOR_BLT_TILED;
		*b = buffers->width;
	} else
		*b = buffers->width << 2;
	*b++ |= 0xf0 << 16 | 1 << 25 | 1 << 24;
	*b++ = 0;
	*b++ = buffers->height << 16 | buffers->width;
	reloc[0].offset = (b - tmp) * sizeof(uint32_t);
	reloc[0].target_handle = buf->handle;
	reloc[0].read_domains = I915_GEM_DOMAIN_RENDER;
	reloc[0].write_domain = I915_GEM_DOMAIN_RENDER;
	*b++ = addr;
	if (gen >= 8)
		*b++ = CANONICAL(addr) >> 32;
	*b++ = val;
	*b++ = MI_BATCH_BUFFER_END;
	if ((b - tmp) & 1)
		*b++ = 0;

	gem_exec[0].handle = buf->handle;
	gem_exec[0].flags = EXEC_OBJECT_NEEDS_FENCE;

	gem_exec[1].handle = gem_create(fd, 4096);
	if (!ahnd) {
		gem_exec[1].relocation_count = 1;
		gem_exec[1].relocs_ptr = to_user_pointer(reloc);
	} else {
		gem_exec[1].offset = CANONICAL(intel_allocator_alloc(ahnd,
								     gem_exec[1].handle,
								     4096, 0));
		gem_exec[1].flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

		gem_exec[0].offset = CANONICAL(buf->addr.offset);
		gem_exec[0].flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE |
				     EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	}

	execbuf.buffers_ptr = to_user_pointer(gem_exec);
	execbuf.buffer_count = 2;
	execbuf.batch_len = (b - tmp) * sizeof(tmp[0]);
	if (gen >= 6)
		execbuf.flags = I915_EXEC_BLT;

	gem_write(fd, gem_exec[1].handle, 0, tmp, execbuf.batch_len);
	gem_execbuf(fd, &execbuf);

	gem_close(fd, gem_exec[1].handle);
	put_offset(ahnd, gem_exec[1].handle);
}

static void
gpu_cmp_bo(struct buffers *b, struct intel_buf *buf, uint32_t val)
{
	blt_copy_bo(b, b->snoop, buf);
	cpu_cmp_bo(b, b->snoop, val);
}

struct access_mode {
	const char *name;
	void (*require)(const struct create *, unsigned);
	struct intel_buf *(*create_bo)(const struct buffers *b);
	void (*set_bo)(struct buffers *b, struct intel_buf *buf, uint32_t val);
	void (*cmp_bo)(struct buffers *b, struct intel_buf *buf, uint32_t val);
	void (*release_bo)(struct intel_buf *buf);
};
igt_render_copyfunc_t rendercopy;

static int read_sysctl(const char *path)
{
	FILE *file = fopen(path, "r");
	int max = 0;
	if (file) {
		if (fscanf(file, "%d", &max) != 1)
			max = 0; /* silence! */
		fclose(file);
	}
	return max;
}

static int write_sysctl(const char *path, int value)
{
	FILE *file = fopen(path, "w");
	if (file) {
		fprintf(file, "%d", value);
		fclose(file);
	}
	return read_sysctl(path);
}

static bool set_max_map_count(int num_buffers)
{
	int max = read_sysctl("/proc/sys/vm/max_map_count");
	if (max < num_buffers + 1024)
		max = write_sysctl("/proc/sys/vm/max_map_count",
				   num_buffers + 1024);
	return max > num_buffers;
}

static uint64_t alloc_open(void)
{
	return ahnd ? intel_allocator_open_full(fd, 0, 0, 0, INTEL_ALLOCATOR_SIMPLE,
						ALLOC_STRATEGY_HIGH_TO_LOW, 0) : 0;
}

static struct intel_bb *bb_create(int i915, uint32_t size)
{
	return ahnd ? intel_bb_create_no_relocs(i915, size) :
		      intel_bb_create_with_relocs(i915, size);
}

static void buffers_init(struct buffers *b,
			 const char *name,
			 const struct create *create,
			 const struct access_mode *mode,
			 const struct size *size,
			 int num_buffers,
			 int _fd)
{
	memset(b, 0, sizeof(*b));
	b->name = name;
	b->create = create;
	b->mode = mode;
	b->size = size;
	b->num_buffers = num_buffers;
	b->count = 0;

	b->width = size->width;
	b->height = size->height;
	b->npixels = size->width * size->height;
	b->page_size = 4*b->npixels;
	b->page_size = (b->page_size + 4095) & -4096;
	b->tmp = malloc(b->page_size);
	igt_assert(b->tmp);

	b->bops = buf_ops_create(_fd);

	b->src = malloc(2*sizeof(struct intel_buf *)*num_buffers);
	igt_assert(b->src);
	b->dst = b->src + num_buffers;

	b->ibb = bb_create(_fd, 4096);
}

static void buffers_destroy(struct buffers *b)
{
	int count = b->count;
	if (count == 0)
		return;

	/* Be safe so that we can clean up a partial creation */
	b->count = 0;
	for (int i = 0; i < count; i++) {
		if (b->src[i]) {
			b->mode->release_bo(b->src[i]);
			b->src[i] = NULL;
		} else
			break;

		if (b->dst[i]) {
			b->mode->release_bo(b->dst[i]);
			b->dst[i] = NULL;
		}
	}
	if (b->snoop) {
		nop_release_bo(b->snoop);
		b->snoop = NULL;
	}
	if (b->spare) {
		b->mode->release_bo(b->spare);
		b->spare = NULL;
	}
}

static void bb_destroy(struct buffers *b)
{
	if (b->ibb) {
		intel_bb_destroy(b->ibb);
		b->ibb = NULL;
	}
}

static void __bufs_destroy(struct buffers *b)
{
	buffers_destroy(b);
	if (b->ibb) {
		intel_bb_destroy(b->ibb);
		b->ibb = NULL;
	}
	if (b->bops) {
		buf_ops_destroy(b->bops);
		b->bops = NULL;
	}
}

static void buffers_create(struct buffers *b)
{
	int count = b->num_buffers;
	igt_assert(b->bops);

	buffers_destroy(b);
	igt_assert(b->count == 0);
	b->count = count;

	ahnd = alloc_open();
	for (int i = 0; i < count; i++) {
		b->src[i] = b->mode->create_bo(b);
		b->dst[i] = b->mode->create_bo(b);
	}
	b->spare = b->mode->create_bo(b);
	b->snoop = snoop_create_bo(b);
	if (b->ibb)
		intel_bb_destroy(b->ibb);

	b->ibb = bb_create(fd, 4096);
}

static void buffers_reset(struct buffers *b)
{
	b->bops = buf_ops_create(fd);
	b->ibb = bb_create(fd, 4096);
}

static void __buffers_create(struct buffers *b)
{
	b->bops = buf_ops_create(fd);
	igt_assert(b->bops);
	igt_assert(b->num_buffers > 0);
	igt_assert(b->mode);
	igt_assert(b->mode->create_bo);

	b->count = 0;
	for (int i = 0; i < b->num_buffers; i++) {
		b->src[i] = b->mode->create_bo(b);
		b->dst[i] = b->mode->create_bo(b);
	}
	b->count = b->num_buffers;
	b->spare = b->mode->create_bo(b);
	b->snoop = snoop_create_bo(b);
	ahnd = alloc_open();
	b->ibb = bb_create(fd, 4096);
}

static void buffers_fini(struct buffers *b)
{
	if (b->bops == NULL)
		return;

	buffers_destroy(b);

	free(b->tmp);
	free(b->src);

	if (b->ibb)
		intel_bb_destroy(b->ibb);
	if (b->bops)
		buf_ops_destroy(b->bops);

	memset(b, 0, sizeof(*b));
}

typedef void (*do_copy)(struct buffers *b, struct intel_buf *dst,
			struct intel_buf *src);
typedef igt_hang_t (*do_hang)(void);

static void render_copy_bo(struct buffers *b, struct intel_buf *dst,
			   struct intel_buf *src)
{
	rendercopy(b->ibb,
		   src, 0, 0,
		   b->width, b->height,
		   dst, 0, 0);
	intel_bb_reset(b->ibb, true);
}

static void blt_copy_bo(struct buffers *b, struct intel_buf *dst,
			struct intel_buf *src)
{
	intel_bb_blt_copy(b->ibb,
		       src, 0, 0, 4*b->width,
		       dst, 0, 0, 4*b->width,
		       b->width, b->height, 32);
	intel_bb_reset(b->ibb, true);
}

static void cpu_copy_bo(struct buffers *b, struct intel_buf *dst,
			struct intel_buf *src)
{
	const int size = b->page_size;
	void *d, *s;

	gem_set_domain(fd, src->handle, I915_GEM_DOMAIN_CPU, 0);
	gem_set_domain(fd, dst->handle, I915_GEM_DOMAIN_CPU,
		       I915_GEM_DOMAIN_CPU);
	s = gem_mmap__cpu(fd, src->handle, 0, size, PROT_READ);
	d = gem_mmap__cpu(fd, dst->handle, 0, size, PROT_WRITE);

	memcpy(d, s, size);

	munmap(d, size);
	munmap(s, size);
}

static void gtt_copy_bo(struct buffers *b, struct intel_buf *dst,
			struct intel_buf *src)
{
	const int size = b->page_size;
	void *d, *s;

	gem_set_domain(fd, src->handle, I915_GEM_DOMAIN_GTT, 0);
	gem_set_domain(fd, dst->handle, I915_GEM_DOMAIN_GTT,
		       I915_GEM_DOMAIN_GTT);

	s = gem_mmap__gtt(fd, src->handle, size, PROT_READ);
	d = gem_mmap__gtt(fd, dst->handle, size, PROT_WRITE);

	memcpy(d, s, size);

	munmap(d, size);
	munmap(s, size);
}

static void wc_copy_bo(struct buffers *b, struct intel_buf *dst,
		       struct intel_buf *src)
{
	const int size = b->page_size;
	void *d, *s;

	gem_set_domain(fd, src->handle, I915_GEM_DOMAIN_WC, 0);
	gem_set_domain(fd, dst->handle, I915_GEM_DOMAIN_WC,
		       I915_GEM_DOMAIN_WC);

	s = gem_mmap__wc(fd, src->handle, 0, size, PROT_READ);
	d = gem_mmap__wc(fd, dst->handle, 0, size, PROT_WRITE);

	memcpy(d, s, size);

	munmap(d, size);
	munmap(s, size);
}

static igt_hang_t no_hang(void)
{
	return (igt_hang_t){0, 0};
}

static igt_hang_t bcs_hang(void)
{
	return igt_hang_ring(fd, I915_EXEC_BLT);
}

static igt_hang_t rcs_hang(void)
{
	return igt_hang_ring(fd, I915_EXEC_RENDER);
}

static void do_basic0(struct buffers *buffers,
		      do_copy do_copy_func,
		      do_hang do_hang_func)
{
	buffers->mode->set_bo(buffers, buffers->src[0], 0xdeadbeef);
	for (int i = 0; i < buffers->count; i++) {
		igt_hang_t hang = do_hang_func();

		do_copy_func(buffers, buffers->dst[i], buffers->src[0]);
		buffers->mode->cmp_bo(buffers, buffers->dst[i], 0xdeadbeef);

		igt_post_hang_ring(fd, hang);
	}
}

static void do_basic1(struct buffers *buffers,
		      do_copy do_copy_func,
		      do_hang do_hang_func)
{
	for (int i = 0; i < buffers->count; i++) {
		igt_hang_t hang = do_hang_func();

		buffers->mode->set_bo(buffers, buffers->src[i], i);
		buffers->mode->set_bo(buffers, buffers->dst[i], ~i);

		do_copy_func(buffers, buffers->dst[i], buffers->src[i]);
		usleep(0); /* let someone else claim the mutex */
		buffers->mode->cmp_bo(buffers, buffers->dst[i], i);

		igt_post_hang_ring(fd, hang);
	}
}

static void do_basicN(struct buffers *buffers,
		      do_copy do_copy_func,
		      do_hang do_hang_func)
{
	igt_hang_t hang;

	for (int i = 0; i < buffers->count; i++) {
		buffers->mode->set_bo(buffers, buffers->src[i], i);
		buffers->mode->set_bo(buffers, buffers->dst[i], ~i);
	}

	hang = do_hang_func();

	for (int i = 0; i < buffers->count; i++) {
		do_copy_func(buffers, buffers->dst[i], buffers->src[i]);
		usleep(0); /* let someone else claim the mutex */
	}

	for (int i = 0; i < buffers->count; i++)
		buffers->mode->cmp_bo(buffers, buffers->dst[i], i);

	igt_post_hang_ring(fd, hang);
}

static void do_overwrite_source(struct buffers *buffers,
				do_copy do_copy_func,
				do_hang do_hang_func)
{
	igt_hang_t hang;
	int i;

	for (i = 0; i < buffers->count; i++) {
		buffers->mode->set_bo(buffers, buffers->src[i], i);
		buffers->mode->set_bo(buffers, buffers->dst[i], ~i);
	}
	for (i = 0; i < buffers->count; i++)
		do_copy_func(buffers, buffers->dst[i], buffers->src[i]);
	hang = do_hang_func();
	for (i = buffers->count; i--; )
		buffers->mode->set_bo(buffers, buffers->src[i], 0xdeadbeef);
	for (i = 0; i < buffers->count; i++)
		buffers->mode->cmp_bo(buffers, buffers->dst[i], i);
	igt_post_hang_ring(fd, hang);
}

static void do_overwrite_source_read(struct buffers *buffers,
				     do_copy do_copy_func,
				     do_hang do_hang_func,
				     int do_rcs)
{
	const int half = buffers->count/2;
	igt_hang_t hang;
	int i;

	for (i = 0; i < half; i++) {
		buffers->mode->set_bo(buffers, buffers->src[i], i);
		buffers->mode->set_bo(buffers, buffers->dst[i], ~i);
		buffers->mode->set_bo(buffers, buffers->dst[i+half], ~i);
	}
	for (i = 0; i < half; i++) {
		do_copy_func(buffers, buffers->dst[i], buffers->src[i]);
		if (do_rcs)
			render_copy_bo(buffers, buffers->dst[i+half], buffers->src[i]);
		else
			blt_copy_bo(buffers, buffers->dst[i+half], buffers->src[i]);
	}
	hang = do_hang_func();
	for (i = half; i--; )
		buffers->mode->set_bo(buffers, buffers->src[i], 0xdeadbeef);
	for (i = 0; i < half; i++) {
		buffers->mode->cmp_bo(buffers, buffers->dst[i], i);
		buffers->mode->cmp_bo(buffers, buffers->dst[i+half], i);
	}
	igt_post_hang_ring(fd, hang);
}

static void do_overwrite_source_read_bcs(struct buffers *buffers,
					 do_copy do_copy_func,
					 do_hang do_hang_func)
{
	do_overwrite_source_read(buffers, do_copy_func, do_hang_func, 0);
}

static void do_overwrite_source_read_rcs(struct buffers *buffers,
					 do_copy do_copy_func,
					 do_hang do_hang_func)
{
	do_overwrite_source_read(buffers, do_copy_func, do_hang_func, 1);
}

static void do_overwrite_source__rev(struct buffers *buffers,
				     do_copy do_copy_func,
				     do_hang do_hang_func)
{
	igt_hang_t hang;
	int i;

	for (i = 0; i < buffers->count; i++) {
		buffers->mode->set_bo(buffers, buffers->src[i], i);
		buffers->mode->set_bo(buffers, buffers->dst[i], ~i);
	}
	for (i = 0; i < buffers->count; i++)
		do_copy_func(buffers, buffers->dst[i], buffers->src[i]);
	hang = do_hang_func();
	for (i = 0; i < buffers->count; i++)
		buffers->mode->set_bo(buffers, buffers->src[i], 0xdeadbeef);
	for (i = buffers->count; i--; )
		buffers->mode->cmp_bo(buffers, buffers->dst[i], i);
	igt_post_hang_ring(fd, hang);
}

static void do_overwrite_source__one(struct buffers *buffers,
				     do_copy do_copy_func,
				     do_hang do_hang_func)
{
	igt_hang_t hang;

	buffers->mode->set_bo(buffers, buffers->src[0], 0);
	buffers->mode->set_bo(buffers, buffers->dst[0], ~0);
	do_copy_func(buffers, buffers->dst[0], buffers->src[0]);
	hang = do_hang_func();
	buffers->mode->set_bo(buffers, buffers->src[0], 0xdeadbeef);
	buffers->mode->cmp_bo(buffers, buffers->dst[0], 0);
	igt_post_hang_ring(fd, hang);
}

static void do_intermix(struct buffers *buffers,
			do_copy do_copy_func,
			do_hang do_hang_func,
			int do_rcs)
{
	const int half = buffers->count/2;
	igt_hang_t hang;
	int i;

	for (i = 0; i < buffers->count; i++) {
		buffers->mode->set_bo(buffers, buffers->src[i], 0xdeadbeef^~i);
		buffers->mode->set_bo(buffers, buffers->dst[i], i);
	}
	for (i = 0; i < half; i++) {
		if (do_rcs == 1 || (do_rcs == -1 && i & 1))
			render_copy_bo(buffers, buffers->dst[i], buffers->src[i]);
		else
			blt_copy_bo(buffers, buffers->dst[i], buffers->src[i]);

		do_copy_func(buffers, buffers->dst[i+half], buffers->src[i]);

		if (do_rcs == 1 || (do_rcs == -1 && (i & 1) == 0))
			render_copy_bo(buffers, buffers->dst[i], buffers->dst[i+half]);
		else
			blt_copy_bo(buffers, buffers->dst[i], buffers->dst[i+half]);

		do_copy_func(buffers, buffers->dst[i+half], buffers->src[i+half]);
	}
	hang = do_hang_func();
	for (i = 0; i < 2*half; i++)
		buffers->mode->cmp_bo(buffers, buffers->dst[i], 0xdeadbeef^~i);
	igt_post_hang_ring(fd, hang);
}

static void do_intermix_rcs(struct buffers *buffers,
			    do_copy do_copy_func,
			    do_hang do_hang_func)
{
	do_intermix(buffers, do_copy_func, do_hang_func, 1);
}

static void do_intermix_bcs(struct buffers *buffers,
			    do_copy do_copy_func,
			    do_hang do_hang_func)
{
	do_intermix(buffers, do_copy_func, do_hang_func, 0);
}

static void do_intermix_both(struct buffers *buffers,
			     do_copy do_copy_func,
			     do_hang do_hang_func)
{
	do_intermix(buffers, do_copy_func, do_hang_func, -1);
}

static void do_early_read(struct buffers *buffers,
			  do_copy do_copy_func,
			  do_hang do_hang_func)
{
	igt_hang_t hang;
	int i;

	for (i = buffers->count; i--; )
		buffers->mode->set_bo(buffers, buffers->src[i], 0xdeadbeef);
	for (i = 0; i < buffers->count; i++)
		do_copy_func(buffers, buffers->dst[i], buffers->src[i]);
	hang = do_hang_func();
	for (i = buffers->count; i--; )
		buffers->mode->cmp_bo(buffers, buffers->dst[i], 0xdeadbeef);
	igt_post_hang_ring(fd, hang);
}

static void do_read_read_bcs(struct buffers *buffers,
			     do_copy do_copy_func,
			     do_hang do_hang_func)
{
	igt_hang_t hang;
	int i;

	for (i = buffers->count; i--; )
		buffers->mode->set_bo(buffers, buffers->src[i], 0xdeadbeef ^ i);
	for (i = 0; i < buffers->count; i++) {
		do_copy_func(buffers, buffers->dst[i], buffers->src[i]);
		blt_copy_bo(buffers, buffers->spare, buffers->src[i]);
	}
	buffers->mode->cmp_bo(buffers, buffers->spare, 0xdeadbeef^(buffers->count-1));
	hang = do_hang_func();
	for (i = buffers->count; i--; )
		buffers->mode->cmp_bo(buffers, buffers->dst[i], 0xdeadbeef ^ i);
	igt_post_hang_ring(fd, hang);
}

static void do_write_read_bcs(struct buffers *buffers,
			      do_copy do_copy_func,
			      do_hang do_hang_func)
{
	igt_hang_t hang;
	int i;

	for (i = buffers->count; i--; )
		buffers->mode->set_bo(buffers, buffers->src[i], 0xdeadbeef ^ i);
	for (i = 0; i < buffers->count; i++) {
		blt_copy_bo(buffers, buffers->spare, buffers->src[i]);
		do_copy_func(buffers, buffers->dst[i], buffers->spare);
	}
	hang = do_hang_func();
	for (i = buffers->count; i--; )
		buffers->mode->cmp_bo(buffers, buffers->dst[i], 0xdeadbeef ^ i);
	igt_post_hang_ring(fd, hang);
}

static void do_read_read_rcs(struct buffers *buffers,
			     do_copy do_copy_func,
			     do_hang do_hang_func)
{
	igt_hang_t hang;
	int i;

	for (i = buffers->count; i--; )
		buffers->mode->set_bo(buffers, buffers->src[i], 0xdeadbeef ^ i);
	for (i = 0; i < buffers->count; i++) {
		do_copy_func(buffers, buffers->dst[i], buffers->src[i]);
		render_copy_bo(buffers, buffers->spare, buffers->src[i]);
	}
	buffers->mode->cmp_bo(buffers, buffers->spare, 0xdeadbeef^(buffers->count-1));
	hang = do_hang_func();
	for (i = buffers->count; i--; )
		buffers->mode->cmp_bo(buffers, buffers->dst[i], 0xdeadbeef ^ i);
	igt_post_hang_ring(fd, hang);
}

static void do_write_read_rcs(struct buffers *buffers,
			      do_copy do_copy_func,
			      do_hang do_hang_func)
{
	igt_hang_t hang;
	int i;

	for (i = buffers->count; i--; )
		buffers->mode->set_bo(buffers, buffers->src[i], 0xdeadbeef ^ i);
	for (i = 0; i < buffers->count; i++) {
		render_copy_bo(buffers, buffers->spare, buffers->src[i]);
		do_copy_func(buffers, buffers->dst[i], buffers->spare);
	}
	hang = do_hang_func();
	for (i = buffers->count; i--; )
		buffers->mode->cmp_bo(buffers, buffers->dst[i], 0xdeadbeef ^ i);
	igt_post_hang_ring(fd, hang);
}

static void do_gpu_read_after_write(struct buffers *buffers,
				    do_copy do_copy_func,
				    do_hang do_hang_func)
{
	igt_hang_t hang;
	int i;

	for (i = buffers->count; i--; )
		buffers->mode->set_bo(buffers, buffers->src[i], 0xabcdabcd);
	for (i = 0; i < buffers->count; i++)
		do_copy_func(buffers, buffers->dst[i], buffers->src[i]);
	for (i = buffers->count; i--; )
		do_copy_func(buffers, buffers->spare, buffers->dst[i]);
	hang = do_hang_func();
	for (i = buffers->count; i--; )
		buffers->mode->cmp_bo(buffers, buffers->dst[i], 0xabcdabcd);
	igt_post_hang_ring(fd, hang);
}

typedef void (*do_test)(struct buffers *buffers,
			do_copy do_copy_func,
			do_hang do_hang_func);

typedef void (*run_wrap)(struct buffers *buffers,
			 do_test do_test_func,
			 do_copy do_copy_func,
			 do_hang do_hang_func);

static void run_single(struct buffers *buffers,
		       do_test do_test_func,
		       do_copy do_copy_func,
		       do_hang do_hang_func)
{
	pass = 0;
	bb_destroy(buffers);
	buffers->ibb = bb_create(fd, 4096);
	do_test_func(buffers, do_copy_func, do_hang_func);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void run_interruptible(struct buffers *buffers,
			      do_test do_test_func,
			      do_copy do_copy_func,
			      do_hang do_hang_func)
{
	pass = 0;
	bb_destroy(buffers);
	buffers->ibb = bb_create(fd, 4096);
	igt_while_interruptible(true)
		do_test_func(buffers, do_copy_func, do_hang_func);
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
}

static void run_child(struct buffers *buffers,
		      do_test do_test_func,
		      do_copy do_copy_func,
		      do_hang do_hang_func)

{
	/* We inherit the buffers from the parent, but the bops/intel_bb
	 * needs to be local as the cache of reusable itself will be COWed,
	 * leading to the child closing an object without the parent knowing.
	 */
	pass = 0;
	__bufs_destroy(buffers);

	igt_fork(child, 1) {
		/* recreate process local variables */
		intel_allocator_init();
		__buffers_create(buffers);
		do_test_func(buffers, do_copy_func, do_hang_func);
	}
	igt_waitchildren();
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);
	buffers_reset(buffers);
}

static void __run_forked(struct buffers *buffers,
			 int num_children, int loops, bool interrupt,
			 do_test do_test_func,
			 do_copy do_copy_func,
			 do_hang do_hang_func)

{
	/* purge the caches before cloing the process */
	__bufs_destroy(buffers);

	igt_fork(child, num_children) {
		int num_buffers;

		/* recreate process local variables */
		fd = gem_reopen_driver(fd);

		intel_allocator_init(); /* detach from thread */
		num_buffers = buffers->num_buffers / num_children;
		num_buffers += MIN_BUFFERS;
		if (num_buffers < buffers->num_buffers)
			buffers->num_buffers = num_buffers;

		__buffers_create(buffers);

		igt_while_interruptible(interrupt) {
			for (pass = 0; pass < loops; pass++)
				do_test_func(buffers,
					     do_copy_func,
					     do_hang_func);
		}
	}
	igt_waitchildren();
	igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);

	buffers_reset(buffers);
}

static void run_forked(struct buffers *buffers,
		       do_test do_test_func,
		       do_copy do_copy_func,
		       do_hang do_hang_func)
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	__run_forked(buffers, ncpus, ncpus, false,
		     do_test_func, do_copy_func, do_hang_func);
}

static void run_bomb(struct buffers *buffers,
		     do_test do_test_func,
		     do_copy do_copy_func,
		     do_hang do_hang_func)
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	__run_forked(buffers, 8*ncpus, 2, true,
		     do_test_func, do_copy_func, do_hang_func);
}

static void cpu_require(void)
{
	bit17_require();
}

static void gtt_require(void)
{
	gem_require_mappable_ggtt(fd);
}

static void bcs_require(void)
{
}

static void rcs_require(void)
{
	igt_require(rendercopy);
}

static void
run_mode(const char *prefix,
	 const struct create *create,
	 const struct access_mode *mode,
	 const struct size *size,
	 const int num_buffers,
	 const char *suffix,
	 run_wrap run_wrap_func)
{
	const struct {
		const char *prefix;
		do_copy copy;
		void (*require)(void);
	} pipelines[] = {
		{ "cpu", cpu_copy_bo, cpu_require },
		{ "gtt", gtt_copy_bo, gtt_require },
		{ "wc", wc_copy_bo, wc_require },
		{ "blt", blt_copy_bo, bcs_require },
		{ "render", render_copy_bo, rcs_require },
		{ NULL, NULL }
	}, *pskip = pipelines + 3, *p;
	const struct {
		const char *suffix;
		do_hang hang;
	} hangs[] = {
		{ "", no_hang },
		{ "-hang-blt", bcs_hang },
		{ "-hang-render", rcs_hang },
		{ NULL, NULL },
	}, *h;
	struct buffers buffers;

	igt_fixture
		buffers_init(&buffers, prefix, create, mode,
			     size, num_buffers, fd);

	for (h = hangs; h->suffix; h++) {
		if (!all && *h->suffix)
			continue;

		if (!*h->suffix)
			igt_fixture
				igt_fork_hang_detector(fd);

		for (p = all ? pipelines : pskip; p->prefix; p++) {
			igt_subtest_group  {
				igt_fixture p->require();

				igt_subtest_f("%s-%s-%s-sanitycheck0%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
					buffers_create(&buffers);
					run_wrap_func(&buffers, do_basic0,
							p->copy, h->hang);
				}

				igt_subtest_f("%s-%s-%s-sanitycheck1%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
					buffers_create(&buffers);
					run_wrap_func(&buffers, do_basic1,
							p->copy, h->hang);
				}

				igt_subtest_f("%s-%s-%s-sanitycheckN%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
					buffers_create(&buffers);
					run_wrap_func(&buffers, do_basicN,
							p->copy, h->hang);
				}

				/* try to overwrite the source values */
				igt_subtest_f("%s-%s-%s-overwrite-source-one%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
					buffers_create(&buffers);
					run_wrap_func(&buffers,
							do_overwrite_source__one,
							p->copy, h->hang);
				}

				igt_subtest_f("%s-%s-%s-overwrite-source%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
					buffers_create(&buffers);
					run_wrap_func(&buffers,
							do_overwrite_source,
							p->copy, h->hang);
				}

				igt_subtest_f("%s-%s-%s-overwrite-source-read-bcs%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
					buffers_create(&buffers);
					run_wrap_func(&buffers,
							do_overwrite_source_read_bcs,
							p->copy, h->hang);
				}

				igt_subtest_f("%s-%s-%s-overwrite-source-read-rcs%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
					igt_require(rendercopy);
					buffers_create(&buffers);
					run_wrap_func(&buffers,
							do_overwrite_source_read_rcs,
							p->copy, h->hang);
				}

				igt_subtest_f("%s-%s-%s-overwrite-source-rev%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
					buffers_create(&buffers);
					run_wrap_func(&buffers,
							do_overwrite_source__rev,
							p->copy, h->hang);
				}

				/* try to intermix copies with GPU copies*/
				igt_subtest_f("%s-%s-%s-intermix-rcs%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
					igt_require(rendercopy);
					buffers_create(&buffers);
					run_wrap_func(&buffers,
							do_intermix_rcs,
							p->copy, h->hang);
				}
				igt_subtest_f("%s-%s-%s-intermix-bcs%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
					igt_require(rendercopy);
					buffers_create(&buffers);
					run_wrap_func(&buffers,
							do_intermix_bcs,
							p->copy, h->hang);
				}
				igt_subtest_f("%s-%s-%s-intermix-both%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
					igt_require(rendercopy);
					buffers_create(&buffers);
					run_wrap_func(&buffers,
							do_intermix_both,
							p->copy, h->hang);
				}

				/* try to read the results before the copy completes */
				igt_subtest_f("%s-%s-%s-early-read%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
					buffers_create(&buffers);
					run_wrap_func(&buffers,
							do_early_read,
							p->copy, h->hang);
				}

				/* concurrent reads */
				igt_subtest_f("%s-%s-%s-read-read-bcs%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
					buffers_create(&buffers);
					run_wrap_func(&buffers,
							do_read_read_bcs,
							p->copy, h->hang);
				}
				igt_subtest_f("%s-%s-%s-read-read-rcs%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
					igt_require(rendercopy);
					buffers_create(&buffers);
					run_wrap_func(&buffers,
							do_read_read_rcs,
							p->copy, h->hang);
				}

				/* split copying between rings */
				igt_subtest_f("%s-%s-%s-write-read-bcs%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
					buffers_create(&buffers);
					run_wrap_func(&buffers,
							do_write_read_bcs,
							p->copy, h->hang);
				}
				igt_subtest_f("%s-%s-%s-write-read-rcs%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
					igt_require(rendercopy);
					buffers_create(&buffers);
					run_wrap_func(&buffers,
							do_write_read_rcs,
							p->copy, h->hang);
				}

				/* and finally try to trick the kernel into loosing the pending write */
				igt_subtest_f("%s-%s-%s-gpu-read-after-write%s%s", prefix, mode->name, p->prefix, suffix, h->suffix) {
					buffers_create(&buffers);
					run_wrap_func(&buffers,
							do_gpu_read_after_write,
							p->copy, h->hang);
				}
			}
		}

		if (!*h->suffix)
			igt_fixture
				igt_stop_hang_detector();
	}

	igt_fixture
		buffers_fini(&buffers);
}

static void
run_modes(const char *style,
	  const struct create *create,
	  const struct access_mode *mode,
	  const struct size *size,
	  const int num)
{
	const struct wrap {
		const char *suffix;
		run_wrap func;
	} wrappers[] = {
		{ "", run_single },
		{ "-child", run_child },
		{ "-forked", run_forked },
		{ "-interruptible", run_interruptible },
		{ "-bomb", run_bomb },
		{ NULL },
	};

	while (mode->name) {
		igt_subtest_group {
			igt_fixture {
				if (mode->require)
					mode->require(create, num);
			}

			for (const struct wrap *w = wrappers; w->suffix; w++) {
				run_mode(style, create, mode, size, num,
					 w->suffix, w->func);
			}
		}

		mode++;
	}
}

static unsigned
num_buffers(uint64_t max,
	    const struct size *s,
	    const struct create *c,
	    unsigned allow_mem)
{
	unsigned size = 4*s->width*s->height;
	uint64_t n;

	igt_assert(size);
	n = max / (2*size);
	n += MIN_BUFFERS;

	igt_require(n < INT32_MAX);
	igt_require(set_max_map_count(2*n));

	if (c->require)
		c->require(c, n);

	igt_require_memory(2*n, size, allow_mem);

	return n;
}

igt_main
{
	const struct access_mode modes[] = {
		{
			.name = "prw",
			.create_bo = unmapped_create_bo,
			.set_bo = prw_set_bo,
			.cmp_bo = prw_cmp_bo,
			.release_bo = nop_release_bo,
		},
		{
			.name = "partial",
			.create_bo = unmapped_create_bo,
			.set_bo = partial_set_bo,
			.cmp_bo = partial_cmp_bo,
			.release_bo = nop_release_bo,
		},
		{
			.name = "cpu",
			.create_bo = unmapped_create_bo,
			.require = create_cpu_require,
			.set_bo = cpu_set_bo,
			.cmp_bo = cpu_cmp_bo,
			.release_bo = nop_release_bo,
		},
		{
			.name = "snoop",
			.create_bo = snoop_create_bo,
			.require = create_snoop_require,
			.set_bo = cpu_set_bo,
			.cmp_bo = cpu_cmp_bo,
			.release_bo = nop_release_bo,
		},
		{
			.name = "userptr",
			.create_bo = userptr_create_bo,
			.require = create_userptr_require,
			.set_bo = userptr_set_bo,
			.cmp_bo = userptr_cmp_bo,
			.release_bo = userptr_release_bo,
		},
		{
			.name = "dmabuf",
			.create_bo = dmabuf_create_bo,
			.require = create_dmabuf_require,
			.set_bo = dmabuf_set_bo,
			.cmp_bo = dmabuf_cmp_bo,
			.release_bo = dmabuf_release_bo,
		},
		{
			.name = "vgem",
			.create_bo = vgem_create_bo,
			.require = create_vgem_require,
			.set_bo = dmabuf_set_bo,
			.cmp_bo = dmabuf_cmp_bo,
			.release_bo = dmabuf_release_bo,
		},
		{
			.name = "gtt",
			.create_bo = gtt_create_bo,
			.require = create_gtt_require,
			.set_bo = gtt_set_bo,
			.cmp_bo = gtt_cmp_bo,
			.release_bo = nop_release_bo,
		},
		{
			.name = "gttX",
			.create_bo = gttX_create_bo,
			.require = create_gtt_require,
			.set_bo = gtt_set_bo,
			.cmp_bo = gtt_cmp_bo,
			.release_bo = nop_release_bo,
		},
		{
			.name = "wc",
			.require = wc_create_require,
			.create_bo = wc_create_bo,
			.set_bo = gtt_set_bo,
			.cmp_bo = gtt_cmp_bo,
			.release_bo = wc_release_bo,
		},
		{
			.name = "gpu",
			.create_bo = gpu_create_bo,
			.set_bo = gpu_set_bo,
			.cmp_bo = gpu_cmp_bo,
			.release_bo = nop_release_bo,
		},
		{
			.name = "gpuX",
			.create_bo = gpuX_create_bo,
			.set_bo = gpu_set_bo,
			.cmp_bo = gpu_cmp_bo,
			.release_bo = nop_release_bo,
		},
		{ NULL },
	};
	const struct create create[] = {
		{ "", can_create_normal, create_normal_bo},
#if HAVE_CREATE_PRIVATE
		{ "private-", can_create_private, create_private_bo},
#endif
#if HAVE_CREATE_STOLEN
		{ "stolen-", can_create_stolen, create_stolen_bo},
#endif
		{ NULL, NULL }
	};
	const struct size sizes[] = {
		{ "4KiB", 128, 8 },
		{ "256KiB", 128, 128 },
		{ "1MiB", 512, 512 },
		{ "16MiB", 2048, 2048 },
		{ NULL}
	};
	uint64_t pin_sz = 0;
	void *pinned = NULL;
	char name[80];
	int count = 0;

	if (strstr(igt_test_name(), "all"))
		all = true;

	igt_fixture {
		igt_allow_unlimited_files();

		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);
		intel_detect_and_clear_missed_interrupts(fd);
		devid = intel_get_drm_devid(fd);
		gen = intel_gen(devid);
		rendercopy = igt_get_render_copyfunc(devid);

		vgem_drv = __drm_open_driver(DRIVER_VGEM);

		ahnd = get_simple_h2l_ahnd(fd, 0);
		put_ahnd(ahnd);
		if (ahnd)
			intel_bb_track(true);
	}

	for (const struct create *c = create; c->name; c++) {
		for (const struct size *s = sizes; s->name; s++) {
			/* Minimum test set */
			snprintf(name, sizeof(name), "%s%s-%s",
				 c->name, s->name, "tiny");
			igt_subtest_group {
				igt_fixture {
					count = num_buffers(0, s, c, CHECK_RAM);
				}
				run_modes(name, c, modes, s, count);
			}

			/* "Average" test set */
			snprintf(name, sizeof(name), "%s%s-%s",
				 c->name, s->name, "small");
			igt_subtest_group {
				igt_fixture {
					count = num_buffers(gem_mappable_aperture_size(fd)/4,
							    s, c, CHECK_RAM);
				}
				run_modes(name, c, modes, s, count);
			}

			/* Use the entire mappable aperture */
			snprintf(name, sizeof(name), "%s%s-%s",
				 c->name, s->name, "thrash");
			igt_subtest_group {
				igt_fixture {
					count = num_buffers(gem_mappable_aperture_size(fd),
							    s, c, CHECK_RAM);
				}
				run_modes(name, c, modes, s, count);
			}

			/* Use the entire global GTT */
			snprintf(name, sizeof(name), "%s%s-%s",
				 c->name, s->name, "global");
			igt_subtest_group {
				igt_fixture {
					count = num_buffers(gem_global_aperture_size(fd),
							    s, c, CHECK_RAM);
				}
				run_modes(name, c, modes, s, count);
			}

			/* Use the entire per-process GTT */
			snprintf(name, sizeof(name), "%s%s-%s",
				 c->name, s->name, "full");
			igt_subtest_group {
				igt_fixture {
					count = num_buffers(gem_aperture_size(fd),
							    s, c, CHECK_RAM);
				}
				run_modes(name, c, modes, s, count);
			}

			snprintf(name, sizeof(name), "%s%s-%s",
				 c->name, s->name, "shrink");
			igt_subtest_group {
				igt_fixture {
					count = num_buffers(gem_mappable_aperture_size(fd),
							    s, c, CHECK_RAM);

					igt_fork_shrink_helper(fd);
				}
				run_modes(name, c, modes, s, count);

				igt_fixture
					igt_stop_shrink_helper();
			}

			/* Use the entire mappable aperture, force swapping */
			snprintf(name, sizeof(name), "%s%s-%s",
				 c->name, s->name, "swap");
			igt_subtest_group {
				igt_fixture {
					if (igt_get_avail_ram_mb() > gem_mappable_aperture_size(fd)/(1024*1024)) {
						pin_sz = igt_get_avail_ram_mb() - gem_mappable_aperture_size(fd)/(1024*1024);

						igt_debug("Pinning %lld MiB\n", (long long)pin_sz);
						pin_sz *= 1024 * 1024;

						if (posix_memalign(&pinned, 4096, pin_sz) ||
						    mlock(pinned, pin_sz) ||
						    madvise(pinned, pin_sz, MADV_DONTFORK)) {
							munlock(pinned, pin_sz);
							free(pinned);
							pinned = NULL;
						}
						igt_require(pinned);
					}

					count = num_buffers(gem_mappable_aperture_size(fd),
							    s, c, CHECK_RAM | CHECK_SWAP);
				}
				run_modes(name, c, modes, s, count);

				if (pinned) {
					munlock(pinned, pin_sz);
					free(pinned);
					pinned = NULL;
				}
			}
		}
	}
}
