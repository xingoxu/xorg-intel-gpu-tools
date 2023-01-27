/*
 * Copyright © 2020 Intel Corporation
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

#include <sys/ioctl.h>
#include <cairo.h>

#include "i915/gem_create.h"
#include "igt.h"
#include "igt_x86.h"
#include "intel_bufops.h"

/**
 * SECTION:intel_bufops
 * @short_description: Buffer operation on tiled surfaces
 * @title: Buffer operations
 * @include: igt.h
 *
 * # Buffer operations
 *
 * Intel GPU devices supports different set of tiled surfaces.
 * Checking each time what tile formats are supports is cumbersome and
 * error prone.
 *
 * Buffer operation (buf_ops) provide a wrapper to conditional code which
 * can be used without worrying of implementation details giving:
 * - copy linear to tiled buffer
 * - copy tiled buffer to linear
 *
 * Following code order should be used (linear is plain memory with some
 * image data):
 *
 * |[<!-- language="c" -->
 * struct buf_ops *bops;
 * struct intel_buf ibuf;
 * ...
 * bops = buf_ops_create(fd);
 * intel_buf_init(bops, &ibuf, 512, 512, 32, 64, I915_TILING_X, false);
 * ...
 * linear_to_intel_buf(bops, &ibuf, linear);
 * ...
 * intel_buf_to_linear(bops, &ibuf, linear);
 * ...
 * intel_buf_close(bops, &ibuf);
 * ...
 * buf_ops_destroy(bops);
 * ]|
 *
 * Calling buf_ops_create(fd) probes hardware capabilities (supported fences,
 * swizzling) and returns opaque pointer to buf_ops. From now on
 * intel_buf_to_linear() and linear_to_intel_buf() will choose appropriate
 * function to do the job.
 *
 * Note: bufops doesn't support SW tiling code yet.
 */

//#define BUFOPS_DEBUGGING
#ifdef BUFOPS_DEBUGGING
#define DEBUG(...) printf(__VA_ARGS__)
#define DEBUGFN() DEBUG("\t -> %s()\n", __FUNCTION__)
#else
#define DEBUG(...)
#define DEBUGFN()
#endif

#define TILE_DEF(x) (1 << (x))
#define TILE_NONE   TILE_DEF(I915_TILING_NONE)
#define TILE_X      TILE_DEF(I915_TILING_X)
#define TILE_Y      TILE_DEF(I915_TILING_Y)
#define TILE_Yf     TILE_DEF(I915_TILING_Yf)
#define TILE_Ys     TILE_DEF(I915_TILING_Ys)
#define TILE_4      TILE_DEF(I915_TILING_4)

#define CCS_OFFSET(buf) (buf->ccs[0].offset)
#define CCS_SIZE(gen, buf) \
	(intel_buf_ccs_width(gen, buf) * intel_buf_ccs_height(gen, buf))

typedef void (*bo_copy)(struct buf_ops *, struct intel_buf *, uint32_t *);

struct buf_ops {
	int fd;
	int gen_start;
	int gen_end;
	unsigned int intel_gen;
	uint32_t supported_tiles;
	uint32_t supported_hw_tiles;
	uint32_t swizzle_x;
	uint32_t swizzle_y;
	uint32_t swizzle_tile4;
	bo_copy linear_to;
	bo_copy linear_to_x;
	bo_copy linear_to_y;
	bo_copy linear_to_yf;
	bo_copy linear_to_ys;
	bo_copy linear_to_tile4;
	bo_copy to_linear;
	bo_copy x_to_linear;
	bo_copy y_to_linear;
	bo_copy yf_to_linear;
	bo_copy ys_to_linear;
	bo_copy tile4_to_linear;
};

static const char *tiling_str(uint32_t tiling)
{
	switch (tiling) {
	case I915_TILING_NONE: return "NONE";
	case I915_TILING_X:    return "X";
	case I915_TILING_Y:    return "Y";
	case I915_TILING_Yf:   return "Yf";
	case I915_TILING_Ys:   return "Ys";
	case I915_TILING_4:    return "4";
	default:               return "UNKNOWN";
	}
}

static const char *bool_str(bool v)
{
	return v ? "yes" : "no";
}

static inline bool is_hw_tiling_supported(struct buf_ops *bops, uint32_t tiling)
{
	return bops->supported_hw_tiles & TILE_DEF(tiling);
}

static inline bool is_tiling_supported(struct buf_ops *bops, uint32_t tiling)
{
	return bops->supported_tiles & TILE_DEF(tiling);
}

static uint32_t get_stride(uint32_t devid, uint32_t tiling)
{
	uint32_t stride = 128;

	if (IS_915G(devid) || IS_915GM(devid) || tiling == I915_TILING_X)
		stride = 512;

	return stride;
}

static int __gem_get_tiling(int fd, struct drm_i915_gem_get_tiling *arg)
{
	int err;

	err = 0;
	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_GET_TILING, arg)) {
		err = -errno;
		igt_assume(err);
	}
	errno = 0;

	return err;
}

static bool __get_tiling(int fd, uint32_t handle, uint32_t *tiling,
			 uint32_t *swizzle, uint32_t *phys_swizzle)
{
	struct drm_i915_gem_get_tiling get_tiling = { .handle = handle };

	if (__gem_get_tiling(fd, &get_tiling) != 0)
		return false;

	*tiling = get_tiling.tiling_mode;
	*swizzle = get_tiling.swizzle_mode;
	*phys_swizzle = get_tiling.phys_swizzle_mode;
	igt_debug("buf tiling: %s, swizzle: %x, phys_swizzle: %x\n",
		  tiling_str(get_tiling.tiling_mode),
		  get_tiling.swizzle_mode,
		  get_tiling.phys_swizzle_mode);

	return true;
}

static int __set_tiling(int fd, uint32_t handle, uint32_t tiling,
			uint32_t stride,
			uint32_t *ret_tiling, uint32_t *ret_swizzle)
{
	struct drm_i915_gem_set_tiling st;

	memset(&st, 0, sizeof(st));
	do {

		int err;

		st.handle = handle;
		st.tiling_mode = tiling;
		st.stride = tiling ? stride : 0;

		err = 0;
		if (ioctl(fd, DRM_IOCTL_I915_GEM_SET_TILING, &st))
			err = -errno;
		errno = 0;
		if (err != -EINTR) {
			if (ret_tiling)
				*ret_tiling = st.tiling_mode;

			if (ret_swizzle)
				*ret_swizzle = st.swizzle_mode;

			return err;
		}
	} while (1);
}

static void set_hw_tiled(struct buf_ops *bops, struct intel_buf *buf)
{
	uint32_t ret_tiling, ret_swizzle;

	if (buf->tiling != I915_TILING_X && buf->tiling != I915_TILING_Y &&
	    buf->tiling != I915_TILING_4)
		return;

	if (!buf_ops_has_hw_fence(bops, buf->tiling)) {
		igt_debug("No HW fence for tiling: %d\n", buf->tiling);
		return;
	}

	igt_assert_eq(__set_tiling(bops->fd,
				   buf->handle, buf->tiling,
				   buf->surface[0].stride,
				   &ret_tiling, &ret_swizzle),
		      0);

	igt_assert(ret_tiling == buf->tiling);
	buf->swizzle_mode = ret_swizzle;
}

static unsigned long swizzle_bit(unsigned int bit, unsigned long offset)
{
	return (offset & (1ul << bit)) >> (bit - 6);
}

static unsigned long swizzle_addr(void *ptr, uint32_t swizzle)
{
	unsigned long addr = to_user_pointer(ptr);

	switch (swizzle) {
	case I915_BIT_6_SWIZZLE_NONE:
		return addr;
	case I915_BIT_6_SWIZZLE_9:
		return addr ^ swizzle_bit(9, addr);
	case I915_BIT_6_SWIZZLE_9_10:
		return addr ^ swizzle_bit(9, addr) ^ swizzle_bit(10, addr);
	case I915_BIT_6_SWIZZLE_9_11:
		return addr ^ swizzle_bit(9, addr) ^ swizzle_bit(11, addr);
	case I915_BIT_6_SWIZZLE_9_10_11:
		return (addr ^
			swizzle_bit(9, addr) ^
			swizzle_bit(10, addr) ^
			swizzle_bit(11, addr));

	case I915_BIT_6_SWIZZLE_UNKNOWN:
	case I915_BIT_6_SWIZZLE_9_17:
	case I915_BIT_6_SWIZZLE_9_10_17:
	default:
		igt_skip("physical swizzling mode impossible to handle in userspace\n");
		return addr;
	}
}

static void *x_ptr(void *ptr,
		   unsigned int x, unsigned int y,
		   unsigned int stride, unsigned int cpp)
{
	const int tile_width = 512;
	const int tile_height = 8;
	const int tile_size = tile_width * tile_height;
	int offset_x, offset_y, pos;
	int tile_x, tile_y;

	x *= cpp;
	tile_x = x / tile_width;
	tile_y = y / tile_height;
	offset_x = tile_x * tile_size;
	offset_y = tile_y * stride * tile_height;

	pos = (offset_y + (y % tile_height * tile_width) +
	       offset_x + (x % tile_width));

	return ptr + pos;
}

static void *y_ptr(void *ptr,
		   unsigned int x, unsigned int y,
		   unsigned int stride, unsigned int cpp)
{
	const int tile_width = 128;
	const int tile_height = 32;
	const int owords = 16;
	const int tile_size = tile_width * tile_height;
	int offset_x, offset_y, pos;
	int shift_x, shift_y;
	int tile_x, tile_y;

	x *= cpp;
	tile_x = x / tile_width;
	tile_y = y / tile_height;
	offset_x = tile_x * tile_size;
	offset_y = tile_y * stride * tile_height;
	shift_x = x % owords + (x % tile_width) / owords * tile_width * cpp;
	shift_y = y % tile_height * owords;

	pos = offset_y + offset_x + shift_x + shift_y;

	return ptr + pos;
}

/*
 * (x,y) to memory location in tiled-4 surface
 *
 * coverted those divisions and multiplications to shifts and masks
 * in hope this wouldn't be so slow.
 */
static void *tile4_ptr(void *ptr,
			unsigned int x, unsigned int y,
			unsigned int stride, unsigned int cpp)
{
	const int tile_width = 128;
	const int tile_height = 32;
	const int subtile_size = 64;
	const int owords = 16;
	int base, _x, _y, subtile, tile_x, tile_y;
	int x_loc = x << __builtin_ctz(cpp);
	int pos;

	/* Pixel in tile via masks */
	tile_x = x_loc & (tile_width - 1);
	tile_y = y & (tile_height - 1);

	/* subtile in 4k tile */
	_x = tile_x >> __builtin_ctz(owords);
	_y = tile_y >> 2;

	/* tile-4 swizzle */
	subtile = ((_y >> 1) << 4) + ((_y & 1) << 2) + (_x & 3) + ((_x & 4) << 1);

	/* memory location */
	base = (y >> __builtin_ctz(tile_height)) *
		(stride << __builtin_ctz(tile_height)) +
		(((x_loc >> __builtin_ctz(tile_width)) << __builtin_ctz(4096)));

	pos = base + (subtile << __builtin_ctz(subtile_size)) +
		((tile_y & 3) << __builtin_ctz(owords)) +
		(tile_x & (owords - 1));
	igt_assert((pos & (cpp - 1)) == 0);
	pos = pos >> __builtin_ctz(cpp);

	return ptr + pos;
}


static void *yf_ptr(void *ptr,
		    unsigned int x, unsigned int y,
		    unsigned int stride, unsigned int cpp)
{
	const int tile_size = 4 * 1024;
	const int tile_width = 128;
	int row_size = stride / tile_width * tile_size;

	x *= cpp; /* convert to Byte offset */

	/*
	 * Within a 4k Yf tile, the byte swizzling pattern is
	 * msb......lsb
	 * xyxyxyyyxxxx
	 * The tiles themselves are laid out in row major order.
	 */
	return ptr +
		((x & 0xf) * 1) + /* 4x1 pixels(32bpp) = 16B */
		((y & 0x3) * 16) + /* 4x4 pixels = 64B */
		(((y & 0x4) >> 2) * 64) + /* 1x2 64B blocks */
		(((x & 0x10) >> 4) * 128) + /* 2x2 64B blocks = 256B block */
		(((y & 0x8) >> 3) * 256) + /* 2x1 256B blocks */
		(((x & 0x20) >> 5) * 512) + /* 2x2 256B blocks */
		(((y & 0x10) >> 4) * 1024) + /* 4x2 256 blocks */
		(((x & 0x40) >> 6) * 2048) + /* 4x4 256B blocks = 4k tile */
		(((x & ~0x7f) >> 7) * tile_size) + /* row of tiles */
		(((y & ~0x1f) >> 5) * row_size);
}

typedef void *(*tile_fn)(void *, unsigned int, unsigned int,
			unsigned int, unsigned int);
static tile_fn __get_tile_fn_ptr(int tiling)
{
	tile_fn fn = NULL;

	switch (tiling) {
	case I915_TILING_X:
		fn = x_ptr;
		break;
	case I915_TILING_Y:
		fn = y_ptr;
		break;
	case I915_TILING_Yf:
		fn = yf_ptr;
		break;
	case I915_TILING_4:
		fn = tile4_ptr;
	case I915_TILING_Ys:
		/* To be implemented */
		break;
	}

	igt_require_f(fn, "Can't find tile function for tiling: %d\n", tiling);
	return fn;
}

static bool is_cache_coherent(int fd, uint32_t handle)
{
	return gem_get_caching(fd, handle) != I915_CACHING_NONE;
}

enum ccs_copy_direction {
	CCS_LINEAR_TO_BUF,
	CCS_BUF_TO_LINEAR,
};

static void __copy_ccs(struct buf_ops *bops, struct intel_buf *buf,
		       uint32_t *linear, enum ccs_copy_direction dir)
{
	uint64_t size, offset, ccs_size;
	unsigned int gen;
	void *map;

	if (!buf->compression || HAS_FLATCCS(intel_get_drm_devid(bops->fd)))
		return;

	gen = bops->intel_gen;
	offset = CCS_OFFSET(buf);
	ccs_size = CCS_SIZE(gen, buf);
	size = offset + ccs_size;

	if (gem_has_lmem(bops->fd)) {
		map = gem_mmap__device_coherent(bops->fd, buf->handle, 0, size,
						PROT_READ | PROT_WRITE);
	} else {
		map = __gem_mmap_offset__wc(bops->fd, buf->handle, 0, size,
					    PROT_READ | PROT_WRITE);
		if (!map)
			map = gem_mmap__wc(bops->fd, buf->handle, 0, size,
					   PROT_READ | PROT_WRITE);
	}

	switch (dir) {
	case CCS_LINEAR_TO_BUF:
		gem_set_domain(bops->fd, buf->handle,
			       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);
		igt_memcpy_from_wc(map + offset, (uint8_t *) linear + offset,
				   ccs_size);
	case CCS_BUF_TO_LINEAR:
		gem_set_domain(bops->fd, buf->handle, I915_GEM_DOMAIN_WC, 0);
		igt_memcpy_from_wc((uint8_t *) linear + offset, map + offset,
				   ccs_size);
	}

	munmap(map, size);
}

static void *mmap_write(int fd, struct intel_buf *buf)
{
	void *map = NULL;

	if (gem_has_lmem(fd)) {
		/*
		 * set/get_caching and set_domain are no longer supported on
		 * discrete, also the only mmap mode supportd is FIXED.
		 */
		map = gem_mmap_offset__fixed(fd, buf->handle, 0,
					     buf->surface[0].size,
					     PROT_READ | PROT_WRITE);
		igt_assert_eq(gem_wait(fd, buf->handle, 0), 0);
	}

	if (!map && is_cache_coherent(fd, buf->handle)) {
		map = __gem_mmap_offset__cpu(fd, buf->handle, 0, buf->surface[0].size,
					     PROT_READ | PROT_WRITE);
		if (!map)
			map = __gem_mmap__cpu(fd, buf->handle, 0, buf->surface[0].size,
					      PROT_READ | PROT_WRITE);

		if (map)
			gem_set_domain(fd, buf->handle,
				       I915_GEM_DOMAIN_CPU,
				       I915_GEM_DOMAIN_CPU);
	}

	if (!map) {
		map = __gem_mmap_offset__wc(fd, buf->handle, 0, buf->surface[0].size,
					    PROT_READ | PROT_WRITE);
		if (!map)
			map = gem_mmap__wc(fd, buf->handle, 0, buf->surface[0].size,
					   PROT_READ | PROT_WRITE);

		gem_set_domain(fd, buf->handle,
			       I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);
	}

	return map;
}

static void *mmap_read(int fd, struct intel_buf *buf)
{
	void *map = NULL;

	if (gem_has_lmem(fd)) {
		/*
		 * set/get_caching and set_domain are no longer supported on
		 * discrete, also the only supported mmap mode is FIXED.
		 */
		map = gem_mmap_offset__fixed(fd, buf->handle, 0,
					     buf->surface[0].size, PROT_READ);
		igt_assert_eq(gem_wait(fd, buf->handle, 0), 0);
	}

	if (!map && (gem_has_llc(fd) || is_cache_coherent(fd, buf->handle))) {
		map = __gem_mmap_offset__cpu(fd, buf->handle, 0,
					     buf->surface[0].size, PROT_READ);
		if (!map)
			map = __gem_mmap__cpu(fd, buf->handle, 0, buf->surface[0].size,
					      PROT_READ);

		if (map)
			gem_set_domain(fd, buf->handle, I915_GEM_DOMAIN_CPU, 0);
	}

	if (!map) {
		map = __gem_mmap_offset__wc(fd, buf->handle, 0, buf->surface[0].size,
					    PROT_READ);
		if (!map)
			map = gem_mmap__wc(fd, buf->handle, 0, buf->surface[0].size,
					   PROT_READ);

		gem_set_domain(fd, buf->handle, I915_GEM_DOMAIN_WC, 0);
	}

	return map;
}

static void __copy_linear_to(int fd, struct intel_buf *buf,
			     const uint32_t *linear,
			     int tiling, uint32_t swizzle)
{
	const tile_fn fn = __get_tile_fn_ptr(tiling);
	int height = intel_buf_height(buf);
	int width = intel_buf_width(buf);
	void *map = mmap_write(fd, buf);

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			uint32_t *ptr = fn(map, x, y, buf->surface[0].stride, buf->bpp/8);

			if (swizzle)
				ptr = from_user_pointer(swizzle_addr(ptr,
								     swizzle));
			*ptr = linear[y * width + x];
		}
	}

	munmap(map, buf->surface[0].size);
}

static void copy_linear_to_x(struct buf_ops *bops, struct intel_buf *buf,
			     uint32_t *linear)
{
	DEBUGFN();
	__copy_linear_to(bops->fd, buf, linear, I915_TILING_X, bops->swizzle_x);
}

static void copy_linear_to_y(struct buf_ops *bops, struct intel_buf *buf,
			     uint32_t *linear)
{
	DEBUGFN();
	__copy_linear_to(bops->fd, buf, linear, I915_TILING_Y, bops->swizzle_y);
}

static void copy_linear_to_yf(struct buf_ops *bops, struct intel_buf *buf,
			      uint32_t *linear)
{
	DEBUGFN();
	__copy_linear_to(bops->fd, buf, linear, I915_TILING_Yf, 0);
}

static void copy_linear_to_ys(struct buf_ops *bops, struct intel_buf *buf,
			      uint32_t *linear)
{
	DEBUGFN();
	__copy_linear_to(bops->fd, buf, linear, I915_TILING_Ys, 0);
}

static void copy_linear_to_tile4(struct buf_ops *bops, struct intel_buf *buf,
				 uint32_t *linear)
{
	DEBUGFN();
	__copy_linear_to(bops->fd, buf, linear, I915_TILING_4, bops->swizzle_tile4);
}

static void __copy_to_linear(int fd, struct intel_buf *buf,
			     uint32_t *linear, int tiling, uint32_t swizzle)
{
	const tile_fn fn = __get_tile_fn_ptr(tiling);
	int height = intel_buf_height(buf);
	int width = intel_buf_width(buf);
	void *map = mmap_write(fd, buf);

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			uint32_t *ptr = fn(map, x, y, buf->surface[0].stride, buf->bpp/8);

			if (swizzle)
				ptr = from_user_pointer(swizzle_addr(ptr,
								     swizzle));
			linear[y * width + x] = *ptr;
		}
	}

	munmap(map, buf->surface[0].size);
}

static void copy_x_to_linear(struct buf_ops *bops, struct intel_buf *buf,
			     uint32_t *linear)
{
	DEBUGFN();
	__copy_to_linear(bops->fd, buf, linear, I915_TILING_X, bops->swizzle_x);
}

static void copy_y_to_linear(struct buf_ops *bops, struct intel_buf *buf,
			     uint32_t *linear)
{
	DEBUGFN();
	__copy_to_linear(bops->fd, buf, linear, I915_TILING_Y, bops->swizzle_y);
}

static void copy_yf_to_linear(struct buf_ops *bops, struct intel_buf *buf,
			      uint32_t *linear)
{
	DEBUGFN();
	__copy_to_linear(bops->fd, buf, linear, I915_TILING_Yf, 0);
}

static void copy_ys_to_linear(struct buf_ops *bops, struct intel_buf *buf,
			      uint32_t *linear)
{
	DEBUGFN();
	__copy_to_linear(bops->fd, buf, linear, I915_TILING_Ys, 0);
}

static void copy_tile4_to_linear(struct buf_ops *bops, struct intel_buf *buf,
				 uint32_t *linear)
{
	DEBUGFN();
	__copy_to_linear(bops->fd, buf, linear, I915_TILING_4, 0);
}

static void copy_linear_to_gtt(struct buf_ops *bops, struct intel_buf *buf,
			       uint32_t *linear)
{
	void *map;

	DEBUGFN();

	map = gem_mmap__gtt(bops->fd, buf->handle,
			    buf->surface[0].size, PROT_READ | PROT_WRITE);

	gem_set_domain(bops->fd, buf->handle,
		       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	memcpy(map, linear, buf->surface[0].size);

	munmap(map, buf->surface[0].size);
}

static void copy_gtt_to_linear(struct buf_ops *bops, struct intel_buf *buf,
			       uint32_t *linear)
{
	void *map;

	DEBUGFN();

	map = gem_mmap__gtt(bops->fd, buf->handle,
			    buf->surface[0].size, PROT_READ);

	gem_set_domain(bops->fd, buf->handle,
		       I915_GEM_DOMAIN_GTT, 0);

	igt_memcpy_from_wc(linear, map, buf->surface[0].size);

	munmap(map, buf->surface[0].size);
}

static void copy_linear_to_wc(struct buf_ops *bops, struct intel_buf *buf,
			      uint32_t *linear)
{
	void *map;

	DEBUGFN();

	map = mmap_write(bops->fd, buf);
	memcpy(map, linear, buf->surface[0].size);
	munmap(map, buf->surface[0].size);
}

static void copy_wc_to_linear(struct buf_ops *bops, struct intel_buf *buf,
			      uint32_t *linear)
{
	void *map;

	DEBUGFN();

	map = mmap_read(bops->fd, buf);
	igt_memcpy_from_wc(linear, map, buf->surface[0].size);
	munmap(map, buf->surface[0].size);
}

void intel_buf_to_linear(struct buf_ops *bops, struct intel_buf *buf,
			 uint32_t *linear)
{
	igt_assert(bops);

	switch (buf->tiling) {
	case I915_TILING_NONE:
		igt_assert(bops->to_linear);
		bops->to_linear(bops, buf, linear);
		break;
	case I915_TILING_X:
		igt_assert(bops->x_to_linear);
		bops->x_to_linear(bops, buf, linear);
		break;
	case I915_TILING_Y:
		igt_assert(bops->y_to_linear);
		bops->y_to_linear(bops, buf, linear);
		break;
	case I915_TILING_Yf:
		igt_assert(bops->yf_to_linear);
		bops->yf_to_linear(bops, buf, linear);
		break;
	case I915_TILING_Ys:
		igt_assert(bops->ys_to_linear);
		bops->ys_to_linear(bops, buf, linear);
		break;
	}

	if (buf->compression)
		__copy_ccs(bops, buf, linear, CCS_BUF_TO_LINEAR);
}

void linear_to_intel_buf(struct buf_ops *bops, struct intel_buf *buf,
			 uint32_t *linear)
{
	igt_assert(bops);

	switch (buf->tiling) {
	case I915_TILING_NONE:
		igt_assert(bops->linear_to);
		bops->linear_to(bops, buf, linear);
		break;
	case I915_TILING_X:
		igt_assert(bops->linear_to_x);
		bops->linear_to_x(bops, buf, linear);
		break;
	case I915_TILING_Y:
		igt_assert(bops->linear_to_y);
		bops->linear_to_y(bops, buf, linear);
		break;
	case I915_TILING_Yf:
		igt_assert(bops->linear_to_yf);
		bops->linear_to_yf(bops, buf, linear);
		break;
	case I915_TILING_Ys:
		igt_assert(bops->linear_to_ys);
		bops->linear_to_ys(bops, buf, linear);
		break;
	}

	if (buf->compression)
		__copy_ccs(bops, buf, linear, CCS_LINEAR_TO_BUF);
}

static void __intel_buf_init(struct buf_ops *bops,
			     uint32_t handle,
			     struct intel_buf *buf,
			     int width, int height, int bpp, int alignment,
			     uint32_t req_tiling, uint32_t compression,
			     uint64_t bo_size, int bo_stride,
			     uint32_t region)
{
	uint32_t tiling = req_tiling;
	uint64_t size;
	uint32_t devid;
	int tile_width;
	int align_h = 1;

	igt_assert(bops);
	igt_assert(buf);
	igt_assert(width > 0 && height > 0);
	igt_assert(bpp == 8 || bpp == 16 || bpp == 32 || bpp == 64);
	igt_assert(alignment >= 0);

	memset(buf, 0, sizeof(*buf));

	buf->bops = bops;
	buf->addr.offset = INTEL_BUF_INVALID_ADDRESS;
	IGT_INIT_LIST_HEAD(&buf->link);

	if (compression) {
		igt_require(bops->intel_gen >= 9);
		igt_assert(req_tiling == I915_TILING_Y ||
			   req_tiling == I915_TILING_Yf ||
			   req_tiling == I915_TILING_4);
		/*
		 * On GEN12+ we align the main surface to 4 * 4 main surface
		 * tiles, which is 64kB. These 16 tiles are mapped by 4 AUX
		 * CCS units, that is 4 * 64 bytes. These 4 CCS units are in
		 * turn mapped by one L1 AUX page table entry.
		 */
		if (bo_stride)
			buf->surface[0].stride = bo_stride;
		else if (bops->intel_gen >= 12)
			buf->surface[0].stride = ALIGN(width * (bpp / 8), 128 * 4);
		else
			buf->surface[0].stride = ALIGN(width * (bpp / 8), 128);

		if (bops->intel_gen >= 12)
			height = ALIGN(height, 32);

		buf->surface[0].size = buf->surface[0].stride * height;
		buf->tiling = tiling;
		buf->bpp = bpp;
		buf->compression = compression;

		if (!HAS_FLATCCS(intel_get_drm_devid(bops->fd))) {
			int aux_width, aux_height;

			aux_width = intel_buf_ccs_width(bops->intel_gen, buf);
			aux_height = intel_buf_ccs_height(bops->intel_gen, buf);

			buf->ccs[0].offset = buf->surface[0].stride * ALIGN(height, 32);
			buf->ccs[0].stride = aux_width;
			size = buf->ccs[0].offset + aux_width * aux_height;
		} else {
			size = buf->ccs[0].offset;
		}
	} else {
		if (tiling) {
			devid =  intel_get_drm_devid(bops->fd);
			tile_width = get_stride(devid, tiling);
			if (bo_stride)
				buf->surface[0].stride = bo_stride;
			else
				buf->surface[0].stride = ALIGN(width * (bpp / 8), tile_width);
			align_h = tiling == I915_TILING_X ? 8 : 32;
		} else {
			if (bo_stride)
				buf->surface[0].stride = bo_stride;
			else
				buf->surface[0].stride = ALIGN(width * (bpp / 8), alignment ?: 1);
		}

		buf->surface[0].size = buf->surface[0].stride * height;
		buf->tiling = tiling;
		buf->bpp = bpp;

		size = buf->surface[0].stride * ALIGN(height, align_h);
	}

	if (bo_size > 0) {
		igt_assert(bo_size >= size);
		size = bo_size;
	}

	/* Store buffer size to avoid mistakes in calculating it again */
	buf->size = size;
	buf->handle = handle;

	if (!handle)
		if (__gem_create_in_memory_regions(bops->fd, &buf->handle, &size, region))
			igt_assert_eq(__gem_create(bops->fd, &size, &buf->handle), 0);

	/* Store gem bo size */
	buf->bo_size = size;

	set_hw_tiled(bops, buf);
}

/**
 * intel_buf_init
 * @bops: pointer to buf_ops
 * @buf: pointer to intel_buf structure to be filled
 * @width: surface width
 * @height: surface height
 * @bpp: bits-per-pixel (8 / 16 / 32 / 64)
 * @alignment: alignment of the stride for linear surfaces
 * @tiling: surface tiling
 * @compression: surface compression type
 *
 * Function creates new BO within intel_buf structure and fills all
 * structure fields. Takes bo handle ownership.
 *
 * Note. For X / Y if GPU supports fences HW tiling is configured.
 */
void intel_buf_init(struct buf_ops *bops,
		    struct intel_buf *buf,
		    int width, int height, int bpp, int alignment,
		    uint32_t tiling, uint32_t compression)
{
	__intel_buf_init(bops, 0, buf, width, height, bpp, alignment,
			 tiling, compression, 0, 0, I915_SYSTEM_MEMORY);

	intel_buf_set_ownership(buf, true);
}

/**
 * intel_buf_init_in_region
 *
 * Same as intel_buf_init with the additional region argument
 */
void intel_buf_init_in_region(struct buf_ops *bops,
			      struct intel_buf *buf,
			      int width, int height, int bpp, int alignment,
			      uint32_t tiling, uint32_t compression,
			      uint32_t region)
{
	__intel_buf_init(bops, 0, buf, width, height, bpp, alignment,
			 tiling, compression, 0, 0, region);

	intel_buf_set_ownership(buf, true);
}

/**
 * intel_buf_close
 * @bops: pointer to buf_ops
 * @buf: pointer to intel_buf structure
 *
 * Function closes gem BO inside intel_buf if bo is owned by intel_buf.
 * For handle passed from the caller intel_buf doesn't take ownership and
 * doesn't close it in close()/destroy() paths. When intel_buf was previously
 * added to intel_bb (intel_bb_add_intel_buf() call) it is tracked there and
 * must be removed from its internal structures.
 */
void intel_buf_close(struct buf_ops *bops, struct intel_buf *buf)
{
	igt_assert(bops);
	igt_assert(buf);

	/* If buf is tracked by some intel_bb ensure it will be removed there */
	if (buf->ibb) {
		intel_bb_remove_intel_buf(buf->ibb, buf);
		buf->addr.offset = INTEL_BUF_INVALID_ADDRESS;
		buf->ibb = NULL;
		IGT_INIT_LIST_HEAD(&buf->link);
	}

	if (buf->is_owner)
		gem_close(bops->fd, buf->handle);
}

/**
 * intel_buf_init_using_handle
 * @bops: pointer to buf_ops
 * @handle: BO handle created by the caller
 * @buf: pointer to intel_buf structure to be filled
 * @width: surface width
 * @height: surface height
 * @bpp: bits-per-pixel (8 / 16 / 32 / 64)
 * @alignment: alignment of the stride for linear surfaces
 * @tiling: surface tiling
 * @compression: surface compression type
 *
 * Function configures BO handle within intel_buf structure passed by the caller
 * (with all its metadata - width, height, ...). Useful if BO was created
 * outside.
 *
 * Note: intel_buf_close() can be used because intel_buf is aware it is not
 * buffer owner so it won't close it underneath.
 */
void intel_buf_init_using_handle(struct buf_ops *bops,
				 uint32_t handle,
				 struct intel_buf *buf,
				 int width, int height, int bpp, int alignment,
				 uint32_t req_tiling, uint32_t compression)
{
	__intel_buf_init(bops, handle, buf, width, height, bpp, alignment,
			 req_tiling, compression, 0, 0, -1);
}

/**
 * intel_buf_create
 * @bops: pointer to buf_ops
 * @width: surface width
 * @height: surface height
 * @bpp: bits-per-pixel (8 / 16 / 32 / 64)
 * @alignment: alignment of the stride for linear surfaces
 * @tiling: surface tiling
 * @compression: surface compression type
 *
 * Function creates intel_buf with created BO handle. Takes ownership of the
 * buffer.
 */
struct intel_buf *intel_buf_create(struct buf_ops *bops,
				   int width, int height,
				   int bpp, int alignment,
				   uint32_t req_tiling, uint32_t compression)
{
	struct intel_buf *buf;

	igt_assert(bops);

	buf = calloc(1, sizeof(*buf));
	igt_assert(buf);

	intel_buf_init(bops, buf, width, height, bpp, alignment,
		       req_tiling, compression);

	return buf;
}

/**
 * intel_buf_create_using_handle
 * @bops: pointer to buf_ops
 * @handle: BO handle created by the caller
 * @width: surface width
 * @height: surface height
 * @bpp: bits-per-pixel (8 / 16 / 32 / 64)
 * @alignment: alignment of the stride for linear surfaces
 * @tiling: surface tiling
 * @compression: surface compression type
 *
 * Function creates intel_buf with passed BO handle from the caller. Doesn't
 * take ownership of the buffer. close()/destroy() paths doesn't close
 * passed handle unless buffer will take ownership using set_ownership().
 */
struct intel_buf *intel_buf_create_using_handle(struct buf_ops *bops,
						uint32_t handle,
						int width, int height,
						int bpp, int alignment,
						uint32_t req_tiling,
						uint32_t compression)
{
	struct intel_buf *buf;

	igt_assert(bops);

	buf = calloc(1, sizeof(*buf));
	igt_assert(buf);

	intel_buf_init_using_handle(bops, handle, buf, width, height, bpp,
				    alignment, req_tiling, compression);

	return buf;
}

struct intel_buf *intel_buf_create_using_handle_and_size(struct buf_ops *bops,
							 uint32_t handle,
							 int width, int height,
							 int bpp, int alignment,
							 uint32_t req_tiling,
							 uint32_t compression,
							 uint64_t size,
							 int stride)
{
	struct intel_buf *buf;

	igt_assert(bops);

	buf = calloc(1, sizeof(*buf));
	igt_assert(buf);

	__intel_buf_init(bops, handle, buf, width, height, bpp, alignment,
			 req_tiling, compression, size, stride, -1);

	return buf;
}


/**
 * intel_buf_destroy
 * @buf: intel_buf
 *
 * Function frees intel_buf memory. It closes bo handle if intel_buf has
 * buffer ownership.
 */
void intel_buf_destroy(struct intel_buf *buf)
{
	igt_assert(buf);
	igt_assert(buf->ptr == NULL);

	intel_buf_close(buf->bops, buf);

	free(buf);
}

void *intel_buf_cpu_map(struct intel_buf *buf, bool write)
{
	int i915 = buf_ops_get_fd(buf->bops);

	igt_assert(buf);
	igt_assert(buf->ptr == NULL); /* already mapped */

	buf->cpu_write = write;
	buf->ptr = gem_mmap__cpu_coherent(i915, buf->handle, 0,
					  buf->surface[0].size,
					  write ? PROT_WRITE : PROT_READ);

	gem_set_domain(i915, buf->handle,
		       I915_GEM_DOMAIN_CPU,
		       write ? I915_GEM_DOMAIN_CPU : 0);

	return buf->ptr;
}

void *intel_buf_device_map(struct intel_buf *buf, bool write)
{
	int i915 = buf_ops_get_fd(buf->bops);

	igt_assert(buf);
	igt_assert(buf->ptr == NULL); /* already mapped */

	buf->ptr = gem_mmap__device_coherent(i915, buf->handle, 0,
					     buf->surface[0].size,
					     write ? PROT_WRITE : PROT_READ);

	gem_set_domain(i915, buf->handle,
		       I915_GEM_DOMAIN_WC,
		       write ? I915_GEM_DOMAIN_WC : 0);

	return buf->ptr;
}

void intel_buf_unmap(struct intel_buf *buf)
{
	igt_assert(buf);
	igt_assert(buf->ptr);

	munmap(buf->ptr, buf->surface[0].size);
	buf->ptr = NULL;
}

void intel_buf_flush_and_unmap(struct intel_buf *buf)
{
	igt_assert(buf);
	igt_assert(buf->ptr);

	if (buf->cpu_write)
		gem_sw_finish(buf_ops_get_fd(buf->bops), buf->handle);

	intel_buf_unmap(buf);
}

void intel_buf_print(const struct intel_buf *buf)
{
	igt_info("[name: %s]\n", buf->name);
	igt_info("[%u]: w: %u, h: %u, stride: %u, size: %" PRIx64
		 ", buf-size: %" PRIx64 ", bo-size: %" PRIx64
		 ", bpp: %u, tiling: %u, compress: %u\n",
		 buf->handle, intel_buf_width(buf), intel_buf_height(buf),
		 buf->surface[0].stride, buf->surface[0].size,
		 intel_buf_size(buf), intel_buf_bo_size(buf), buf->bpp,
		 buf->tiling, buf->compression);
	igt_info(" ccs <offset: %u, stride: %u, w: %u, h: %u> cc <offset: %u>\n",
		 buf->ccs[0].offset,
		 intel_buf_ccs_width(buf->bops->intel_gen, buf),
		 intel_buf_ccs_height(buf->bops->intel_gen, buf),
		 buf->ccs[0].stride, buf->cc.offset);
	igt_info(" addr <offset: %p, ctx: %u>\n",
		 from_user_pointer(buf->addr.offset), buf->addr.ctx);
}

void intel_buf_dump(const struct intel_buf *buf, const char *filename)
{
	int i915 = buf_ops_get_fd(buf->bops);
	uint64_t size = intel_buf_size(buf);
	FILE *out;
	void *ptr;

	ptr = gem_mmap__device_coherent(i915, buf->handle, 0, size, PROT_READ);
	out = fopen(filename, "wb");
	igt_assert(out);
	fwrite(ptr, size, 1, out);
	fclose(out);
	munmap(ptr, size);
}

const char *intel_buf_set_name(struct intel_buf *buf, const char *name)
{
	return strncpy(buf->name, name, INTEL_BUF_NAME_MAXSIZE);
}

static void __intel_buf_write_to_png(struct buf_ops *bops,
				     struct intel_buf *buf,
				     const char *filename,
				     bool write_ccs)
{
	cairo_surface_t *surface;
	cairo_status_t ret;
	void *linear;
	int format, width, height, stride, offset;
	unsigned int gen = bops->intel_gen;

	igt_assert_eq(posix_memalign(&linear, 16, intel_buf_size(buf)), 0);

	format = write_ccs ? CAIRO_FORMAT_A8 : CAIRO_FORMAT_RGB24;
	width = write_ccs ? intel_buf_ccs_width(gen, buf) : intel_buf_width(buf);
	height = write_ccs ? intel_buf_ccs_height(gen, buf) : intel_buf_height(buf);
	stride = write_ccs ? buf->ccs[0].stride : buf->surface[0].stride;
	offset = write_ccs ? buf->ccs[0].offset : 0;

	intel_buf_to_linear(bops, buf, linear);

	surface = cairo_image_surface_create_for_data((uint8_t *) linear + offset,
						      format, width, height,
						      stride);
	ret = cairo_surface_write_to_png(surface, filename);
	igt_assert(ret == CAIRO_STATUS_SUCCESS);
	cairo_surface_destroy(surface);

	free(linear);
}

void intel_buf_write_to_png(struct intel_buf *buf, const char *filename)
{
	__intel_buf_write_to_png(buf->bops, buf, filename, false);
}

void intel_buf_write_aux_to_png(struct intel_buf *buf, const char *filename)
{
	igt_assert(buf->compression);

	__intel_buf_write_to_png(buf->bops, buf, filename, true);
}

#define DEFAULT_BUFOPS(__gen_start, __gen_end) \
	.gen_start          = __gen_start, \
	.gen_end            = __gen_end, \
	.supported_hw_tiles = TILE_X | TILE_Y | TILE_4, \
	.linear_to          = copy_linear_to_wc, \
	.linear_to_x        = copy_linear_to_gtt, \
	.linear_to_y        = copy_linear_to_gtt, \
	.linear_to_yf       = copy_linear_to_yf, \
	.linear_to_ys       = copy_linear_to_ys, \
	.linear_to_tile4    = copy_linear_to_tile4, \
	.to_linear          = copy_wc_to_linear, \
	.x_to_linear        = copy_gtt_to_linear, \
	.y_to_linear        = copy_gtt_to_linear, \
	.yf_to_linear       = copy_yf_to_linear, \
	.ys_to_linear       = copy_ys_to_linear, \
	.tile4_to_linear    = copy_tile4_to_linear

static const struct buf_ops buf_ops_arr[] = {
	{
		DEFAULT_BUFOPS(2, 8),
		.supported_tiles    = TILE_NONE | TILE_X | TILE_Y,
	},

	{
		DEFAULT_BUFOPS(9, 11),
		.supported_tiles    = TILE_NONE | TILE_X | TILE_Y | TILE_Yf,
	},

	{
		DEFAULT_BUFOPS(12, ~0U),
		.supported_tiles   = TILE_NONE | TILE_X | TILE_Y | TILE_Yf | TILE_Ys | TILE_4,
	},
};

static bool probe_hw_tiling(struct buf_ops *bops, uint32_t tiling,
			    bool *swizzling_supported)
{
	uint64_t size = 256 * 256;
	uint32_t handle, buf_tiling, buf_swizzle, phys_swizzle;
	uint32_t devid, stride;
	int ret;
	bool is_set = false;

	devid =  intel_get_drm_devid(bops->fd);
	stride = get_stride(devid, tiling);
	handle = gem_create(bops->fd, size);

	/* Single shot, if no fences are available we fail immediately */
	ret = __set_tiling(bops->fd, handle, tiling, stride, NULL, NULL);
	if (ret)
		goto end;

	is_set = __get_tiling(bops->fd, handle, &buf_tiling, &buf_swizzle,
			      &phys_swizzle);
	if (is_set) {
		if (tiling == I915_TILING_X)
			bops->swizzle_x = buf_swizzle;
		else if (tiling == I915_TILING_Y)
			bops->swizzle_y = buf_swizzle;
		else if (tiling == I915_TILING_4)
			bops->swizzle_tile4 = buf_swizzle;

		*swizzling_supported = buf_swizzle == phys_swizzle;
	}
end:
	gem_close(bops->fd, handle);

	return is_set;
}

static void *alloc_aligned(uint64_t size)
{
	void *p;

	igt_assert_eq(posix_memalign(&p, 16, size), 0);

	return p;
}

/*
 * Simple idempotency test between HW -> SW and SW -> HW BO.
 */
static void idempotency_selftest(struct buf_ops *bops, uint32_t tiling)
{
	struct intel_buf buf;
	uint8_t *linear_in, *linear_out, *map;
	int i;
	const int width = 512;
	const int height = 512;
	const int bpp = 32;
	const int size = width * height * bpp / 8;
	bool software_tiling = false;

	if (!is_hw_tiling_supported(bops, tiling))
		return;

	linear_in = alloc_aligned(size);
	linear_out = alloc_aligned(size);

	for (i = 0; i < size; i++)
		linear_in[i] = i % 253; /* prime chosen intentionally */

	do {
		igt_debug("Checking idempotency, SW: %s, HW: %s, tiling: %s\n",
			  bool_str(software_tiling),
			  bool_str(!software_tiling),
			  tiling_str(tiling));
		intel_buf_init(bops, &buf, width, height, bpp, 0, tiling, false);
		buf_ops_set_software_tiling(bops, tiling, software_tiling);

		linear_to_intel_buf(bops, &buf, (uint32_t *) linear_in);
		map = __gem_mmap_offset__cpu(bops->fd, buf.handle, 0,
					     buf.surface[0].size, PROT_READ);
		if (!map)
			map = gem_mmap__cpu(bops->fd, buf.handle, 0,
					    buf.surface[0].size, PROT_READ);
		gem_set_domain(bops->fd, buf.handle, I915_GEM_DOMAIN_CPU, 0);
		igt_assert(memcmp(linear_in, map, size));
		munmap(map, size);

		buf_ops_set_software_tiling(bops, tiling, !software_tiling);
		intel_buf_to_linear(bops, &buf, (uint32_t *) linear_out);
		igt_assert(!memcmp(linear_in, linear_out, size));

		intel_buf_close(bops, &buf);

		software_tiling = !software_tiling;
	} while (software_tiling);

	igt_debug("Idempotency for %s tiling OK\n", tiling_str(tiling));
	buf_ops_set_software_tiling(bops, tiling, false);
}

uint64_t intel_buf_size(const struct intel_buf *buf)
{
	return buf->size;
}

uint64_t intel_buf_bo_size(const struct intel_buf *buf)
{
	return buf->bo_size;
}

static struct buf_ops *__buf_ops_create(int fd, bool check_idempotency)
{
	struct buf_ops *bops = calloc(1, sizeof(*bops));
	unsigned int generation;
	uint32_t devid;

	igt_assert(bops);

	devid = intel_get_drm_devid(fd);
	generation = intel_gen(devid);

	/* Predefined settings: see intel_device_info? */
	for (int i = 0; i < ARRAY_SIZE(buf_ops_arr); i++) {
		if (generation >= buf_ops_arr[i].gen_start &&
		    generation <= buf_ops_arr[i].gen_end) {
			memcpy(bops, &buf_ops_arr[i], sizeof(*bops));
			break;
		}
	}

	bops->fd = fd;
	bops->intel_gen = generation;
	igt_debug("generation: %d, supported tiles: 0x%02x\n",
		  bops->intel_gen, bops->supported_tiles);

	/*
	 * Warning!
	 *
	 * Gen2 software tiling/detiling is not supported! (yet).
	 *
	 * If you are brave hero with an access to Gen2 you can save the world.
	 * Until then we're doomed to use only hardware (de)tiling.
	 *
	 * Ok, you have been warned.
	 */
	if (bops->intel_gen == 2) {
		igt_warn("Gen2 detected. HW (de)tiling support only.");
		return bops;
	}

	/* Let's probe X and Y hw tiling support */
	if (is_hw_tiling_supported(bops, I915_TILING_X)) {
		bool swizzling_supported;
		bool supported = probe_hw_tiling(bops, I915_TILING_X,
						 &swizzling_supported);

		if (!swizzling_supported) {
			igt_debug("Swizzling for X is not supported\n");
			bops->supported_tiles &= ~TILE_X;
		}

		igt_debug("X fence support: %s\n", bool_str(supported));
		if (!supported) {
			bops->supported_hw_tiles &= ~TILE_X;
			bops->linear_to_x = copy_linear_to_x;
			bops->x_to_linear = copy_x_to_linear;
		}
	}

	if (is_hw_tiling_supported(bops, I915_TILING_Y)) {
		bool swizzling_supported;
		bool supported = probe_hw_tiling(bops, I915_TILING_Y,
						 &swizzling_supported);

		if (!swizzling_supported) {
			igt_debug("Swizzling for Y is not supported\n");
			bops->supported_tiles &= ~TILE_Y;
		}

		igt_debug("Y fence support: %s\n", bool_str(supported));
		if (!supported) {
			bops->supported_hw_tiles &= ~TILE_Y;
			bops->linear_to_y = copy_linear_to_y;
			bops->y_to_linear = copy_y_to_linear;
		}
	}

	if (is_hw_tiling_supported(bops, I915_TILING_4)) {
		bool swizzling_supported;
		bool supported = probe_hw_tiling(bops, I915_TILING_4,
						 &swizzling_supported);

		if (!swizzling_supported) {
			igt_debug("Swizzling for 4 is not supported\n");
			bops->supported_tiles &= ~TILE_4;
		}

		igt_debug("4 fence support: %s\n", bool_str(supported));
		if (!supported) {
			bops->supported_hw_tiles &= ~TILE_4;
			bops->linear_to_tile4 = copy_linear_to_tile4;
			bops->tile4_to_linear = copy_tile4_to_linear;
		}
	}

	/* Disable other tiling format functions if not supported */
	if (!is_tiling_supported(bops, I915_TILING_Yf)) {
		igt_debug("Yf format not supported\n");
		bops->linear_to_yf = NULL;
		bops->yf_to_linear = NULL;
	}

	if (!is_tiling_supported(bops, I915_TILING_Ys)) {
		igt_debug("Ys format not supported\n");
		bops->linear_to_ys = NULL;
		bops->ys_to_linear = NULL;
	}

	if (check_idempotency) {
		idempotency_selftest(bops, I915_TILING_X);
		idempotency_selftest(bops, I915_TILING_Y);
	}

	return bops;
}

/**
 * buf_ops_create
 * @fd: device filedescriptor
 *
 * Create buf_ops structure depending on fd-device capabilities.
 *
 * Returns: opaque pointer to buf_ops.
 *
 */
struct buf_ops *buf_ops_create(int fd)
{
	return __buf_ops_create(fd, false);
}

/**
 * buf_ops_create_with_selftest
 * @fd: device filedescriptor
 *
 * Create buf_ops structure depending on fd-device capabilities.
 * Runs with idempotency selftest to verify software tiling gives same
 * result like hardware tiling (gens with mappable gtt).
 *
 * Returns: opaque pointer to buf_ops.
 *
 */
struct buf_ops *buf_ops_create_with_selftest(int fd)
{
	return __buf_ops_create(fd, true);
}

/**
 * buf_ops_destroy
 * @bops: pointer to buf_ops
 *
 * Function frees buf_ops structure.
 */
void buf_ops_destroy(struct buf_ops *bops)
{
	igt_assert(bops);
	free(bops);
}

/**
 * buf_ops_get_fd
 * @bops: pointer to buf_ops
 *
 * Returns: drm fd
 */
int buf_ops_get_fd(struct buf_ops *bops)
{
	igt_assert(bops);

	return bops->fd;
}

/**
 * buf_ops_set_software_tiling
 * @bops: pointer to buf_ops
 * @tiling: surface tiling
 * @use_software_tiling: if true use software copying methods, otherwise
 * use hardware (via gtt)
 *
 * Function allows switch X / Y surfaces to software / hardware copying methods
 * which honors tiling and swizzling.
 *
 * Returns:
 * false - switch wasn't possible.
 * true - switch to software / hardware method succeed.
 */
bool buf_ops_set_software_tiling(struct buf_ops *bops,
				 uint32_t tiling,
				 bool use_software_tiling)
{
	bool was_changed = true;

	igt_assert(bops);

	/* Until appropriate code is added we don't support SW tiling on Gen2 */
	if (bops->intel_gen == 2) {
		igt_warn("Change to software tiling on Gen2 is not supported!");
		return false;
	}

	switch (tiling) {
	case I915_TILING_X:
		if (use_software_tiling) {
			bool supported = buf_ops_has_tiling_support(bops, tiling);

			igt_assert_f(supported, "Cannot switch to X software tiling\n");
			igt_debug("-> change X to SW\n");
			bops->linear_to_x = copy_linear_to_x;
			bops->x_to_linear = copy_x_to_linear;
		} else {
			if (is_hw_tiling_supported(bops, I915_TILING_X)) {
				igt_debug("-> change X to HW\n");
				bops->linear_to_x = copy_linear_to_gtt;
				bops->x_to_linear = copy_gtt_to_linear;
			} else {
				igt_debug("-> X cannot be changed to HW\n");
				was_changed = false;
			}
		}
		break;

	case I915_TILING_Y:
		if (use_software_tiling) {
			bool supported = buf_ops_has_tiling_support(bops, tiling);

			igt_assert_f(supported, "Cannot switch to Y software tiling\n");
			igt_debug("-> change Y to SW\n");
			bops->linear_to_y = copy_linear_to_y;
			bops->y_to_linear = copy_y_to_linear;
		} else {
			if (is_hw_tiling_supported(bops, I915_TILING_Y)) {
				igt_debug("-> change Y to HW\n");
				bops->linear_to_y = copy_linear_to_gtt;
				bops->y_to_linear = copy_gtt_to_linear;
			} else {
				igt_debug("-> Y cannot be changed to HW\n");
				was_changed = false;
			}
		}
		break;

	default:
		igt_warn("Invalid tiling: %d\n", tiling);
		was_changed = false;
	}

	return was_changed;
}

/**
 * buf_ops_has_hw_fence
 * @bops: pointer to buf_ops
 * @tiling: surface tiling
 *
 * Function checks if surface with tiling has HW fences which can be used
 * to copy it via gtt.
 *
 * Returns:
 * false - fence for tiling is not supported.
 * true - fence for tiling is supported.
 */
bool buf_ops_has_hw_fence(struct buf_ops *bops, uint32_t tiling)
{
	uint32_t tile_mask = TILE_DEF(tiling);

	igt_assert(bops);

	if (tile_mask & bops->supported_hw_tiles)
		return true;

	return false;
}

/**
 * buf_ops_has_tiling_support
 * @bops: pointer to buf_ops
 * @tiling: surface tiling
 *
 * Function checks capabilities to handle surfaces with tiling in GPU.
 *
 * Returns:
 * false - GPU does not support tiling.
 * true - GPU supports tiling.
 */
bool buf_ops_has_tiling_support(struct buf_ops *bops, uint32_t tiling)
{
	uint32_t tile_mask = TILE_DEF(tiling);

	igt_assert(bops);

	if (tile_mask & bops->supported_tiles)
		return true;

	return false;
}
