/**************************************************************************
 *
 * Copyright 2006 Tungsten Graphics, Inc., Cedar Park, Texas.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include <inttypes.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <search.h>

#include "drm.h"
#include "drmtest.h"
#include "i915/gem_create.h"
#include "intel_batchbuffer.h"
#include "intel_bufmgr.h"
#include "intel_bufops.h"
#include "intel_chipset.h"
#include "intel_reg.h"
#include "veboxcopy.h"
#include "rendercopy.h"
#include "media_fill.h"
#include "ioctl_wrappers.h"
#include "sw_sync.h"
#include "i915/gem_mman.h"
#include "media_spin.h"
#include "gpgpu_fill.h"
#include "igt_aux.h"
#include "i830_reg.h"
#include "huc_copy.h"
#include <glib.h>

#include <i915_drm.h>

#define BCS_SWCTRL 0x22200
#define BCS_SRC_Y (1 << 0)
#define BCS_DST_Y (1 << 1)

/**
 * SECTION:intel_batchbuffer
 * @short_description: Batchbuffer and blitter support
 * @title: Batch Buffer
 * @include: igt.h
 *
 * This library provides some basic support for batchbuffers and using the
 * blitter engine based upon libdrm. A new batchbuffer is allocated with
 * intel_batchbuffer_alloc() and for simple blitter commands submitted with
 * intel_batchbuffer_flush().
 *
 * It also provides some convenient macros to easily emit commands into
 * batchbuffers. All those macros presume that a pointer to a #intel_batchbuffer
 * structure called batch is in scope. The basic macros are #BEGIN_BATCH,
 * #OUT_BATCH, #OUT_RELOC and #ADVANCE_BATCH.
 *
 * Note that this library's header pulls in the [i-g-t core](igt-gpu-tools-i-g-t-core.html)
 * library as a dependency.
 */

static bool intel_bb_do_tracking;
static IGT_LIST_HEAD(intel_bb_list);
static pthread_mutex_t intel_bb_list_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * intel_batchbuffer_align:
 * @batch: batchbuffer object
 * @align: value in bytes to which we want to align
 *
 * Aligns the current in-batch offset to the given value.
 *
 * Returns: Batchbuffer offset aligned to the given value.
 */
uint32_t
intel_batchbuffer_align(struct intel_batchbuffer *batch, uint32_t align)
{
	uint32_t offset = batch->ptr - batch->buffer;

	offset = ALIGN(offset, align);
	batch->ptr = batch->buffer + offset;
	return offset;
}

/**
 * intel_batchbuffer_subdata_alloc:
 * @batch: batchbuffer object
 * @size: amount of bytes need to allocate
 * @align: value in bytes to which we want to align
 *
 * Verify if sufficient @size within @batch is available to deny overflow.
 * Then allocate @size bytes within @batch.
 *
 * Returns: Offset within @batch between allocated subdata and base of @batch.
 */
void *
intel_batchbuffer_subdata_alloc(struct intel_batchbuffer *batch, uint32_t size,
				uint32_t align)
{
	uint32_t offset = intel_batchbuffer_align(batch, align);

	igt_assert(size <= intel_batchbuffer_space(batch));

	batch->ptr += size;
	return memset(batch->buffer + offset, 0, size);
}

/**
 * intel_batchbuffer_subdata_offset:
 * @batch: batchbuffer object
 * @ptr: pointer to given data
 *
 * Returns: Offset within @batch between @ptr and base of @batch.
 */
uint32_t
intel_batchbuffer_subdata_offset(struct intel_batchbuffer *batch, void *ptr)
{
	return (uint8_t *)ptr - batch->buffer;
}

/**
 * intel_batchbuffer_reset:
 * @batch: batchbuffer object
 *
 * Resets @batch by allocating a new gem buffer object as backing storage.
 */
void
intel_batchbuffer_reset(struct intel_batchbuffer *batch)
{
	if (batch->bo != NULL) {
		drm_intel_bo_unreference(batch->bo);
		batch->bo = NULL;
	}

	batch->bo = drm_intel_bo_alloc(batch->bufmgr, "batchbuffer",
				       BATCH_SZ, 4096);

	memset(batch->buffer, 0, sizeof(batch->buffer));
	batch->ctx = NULL;

	batch->ptr = batch->buffer;
	batch->end = NULL;
}

/**
 * intel_batchbuffer_alloc:
 * @bufmgr: libdrm buffer manager
 * @devid: pci device id of the drm device
 *
 * Allocates a new batchbuffer object. @devid must be supplied since libdrm
 * doesn't expose it directly.
 *
 * Returns: The allocated and initialized batchbuffer object.
 */
struct intel_batchbuffer *
intel_batchbuffer_alloc(drm_intel_bufmgr *bufmgr, uint32_t devid)
{
	struct intel_batchbuffer *batch = calloc(sizeof(*batch), 1);

	batch->bufmgr = bufmgr;
	batch->devid = devid;
	batch->gen = intel_gen(devid);
	intel_batchbuffer_reset(batch);

	return batch;
}

/**
 * intel_batchbuffer_free:
 * @batch: batchbuffer object
 *
 * Releases all resource of the batchbuffer object @batch.
 */
void
intel_batchbuffer_free(struct intel_batchbuffer *batch)
{
	drm_intel_bo_unreference(batch->bo);
	batch->bo = NULL;
	free(batch);
}

#define CMD_POLY_STIPPLE_OFFSET       0x7906

static unsigned int
flush_on_ring_common(struct intel_batchbuffer *batch, int ring)
{
	unsigned int used = batch->ptr - batch->buffer;

	if (used == 0)
		return 0;

	if (IS_GEN5(batch->devid)) {
		/* emit gen5 w/a without batch space checks - we reserve that
		 * already. */
		*(uint32_t *) (batch->ptr) = CMD_POLY_STIPPLE_OFFSET << 16;
		batch->ptr += 4;
		*(uint32_t *) (batch->ptr) = 0;
		batch->ptr += 4;
	}

	/* Round batchbuffer usage to 2 DWORDs. */
	if ((used & 4) == 0) {
		*(uint32_t *) (batch->ptr) = 0; /* noop */
		batch->ptr += 4;
	}

	/* Mark the end of the buffer. */
	*(uint32_t *)(batch->ptr) = MI_BATCH_BUFFER_END; /* noop */
	batch->ptr += 4;
	return batch->ptr - batch->buffer;
}

/**
 * intel_batchbuffer_flush_on_ring:
 * @batch: batchbuffer object
 * @ring: execbuf ring flag
 *
 * Submits the batch for execution on @ring.
 */
void
intel_batchbuffer_flush_on_ring(struct intel_batchbuffer *batch, int ring)
{
	unsigned int used = flush_on_ring_common(batch, ring);
	drm_intel_context *ctx;

	if (used == 0)
		return;

	do_or_die(drm_intel_bo_subdata(batch->bo, 0, used, batch->buffer));

	batch->ptr = NULL;

	/* XXX bad kernel API */
	ctx = batch->ctx;
	if (ring != I915_EXEC_RENDER)
		ctx = NULL;
	do_or_die(drm_intel_gem_bo_context_exec(batch->bo, ctx, used, ring));

	intel_batchbuffer_reset(batch);
}

void
intel_batchbuffer_set_context(struct intel_batchbuffer *batch,
				     drm_intel_context *context)
{
	batch->ctx = context;
}

/**
 * intel_batchbuffer_flush_with_context:
 * @batch: batchbuffer object
 * @context: libdrm hardware context object
 *
 * Submits the batch for execution on the render engine with the supplied
 * hardware context.
 */
void
intel_batchbuffer_flush_with_context(struct intel_batchbuffer *batch,
				     drm_intel_context *context)
{
	int ret;
	unsigned int used = flush_on_ring_common(batch, I915_EXEC_RENDER);

	if (used == 0)
		return;

	ret = drm_intel_bo_subdata(batch->bo, 0, used, batch->buffer);
	igt_assert(ret == 0);

	batch->ptr = NULL;

	ret = drm_intel_gem_bo_context_exec(batch->bo, context, used,
					    I915_EXEC_RENDER);
	igt_assert(ret == 0);

	intel_batchbuffer_reset(batch);
}

/**
 * intel_batchbuffer_flush:
 * @batch: batchbuffer object
 *
 * Submits the batch for execution on the blitter engine, selecting the right
 * ring depending upon the hardware platform.
 */
void
intel_batchbuffer_flush(struct intel_batchbuffer *batch)
{
	int ring = 0;
	if (HAS_BLT_RING(batch->devid))
		ring = I915_EXEC_BLT;
	intel_batchbuffer_flush_on_ring(batch, ring);
}


/**
 * intel_batchbuffer_emit_reloc:
 * @batch: batchbuffer object
 * @buffer: relocation target libdrm buffer object
 * @delta: delta value to add to @buffer's gpu address
 * @read_domains: gem domain bits for the relocation
 * @write_domain: gem domain bit for the relocation
 * @fenced: whether this gpu access requires fences
 *
 * Emits both a libdrm relocation entry pointing at @buffer and the pre-computed
 * DWORD of @batch's presumed gpu address plus the supplied @delta into @batch.
 *
 * Note that @fenced is only relevant if @buffer is actually tiled.
 *
 * This is the only way buffers get added to the validate list.
 */
void
intel_batchbuffer_emit_reloc(struct intel_batchbuffer *batch,
                             drm_intel_bo *buffer, uint64_t delta,
			     uint32_t read_domains, uint32_t write_domain,
			     int fenced)
{
	uint64_t offset;
	int ret;

	if (batch->ptr - batch->buffer > BATCH_SZ)
		igt_info("bad relocation ptr %p map %p offset %d size %d\n",
			 batch->ptr, batch->buffer,
			 (int)(batch->ptr - batch->buffer), BATCH_SZ);

	if (fenced)
		ret = drm_intel_bo_emit_reloc_fence(batch->bo, batch->ptr - batch->buffer,
						    buffer, delta,
						    read_domains, write_domain);
	else
		ret = drm_intel_bo_emit_reloc(batch->bo, batch->ptr - batch->buffer,
					      buffer, delta,
					      read_domains, write_domain);

	offset = buffer->offset64;
	offset += delta;
	intel_batchbuffer_emit_dword(batch, offset);
	if (batch->gen >= 8)
		intel_batchbuffer_emit_dword(batch, offset >> 32);
	igt_assert(ret == 0);
}

/**
 * intel_batchbuffer_copy_data:
 * @batch: batchbuffer object
 * @data: pointer to the data to write into the batchbuffer
 * @bytes: number of bytes to write into the batchbuffer
 * @align: value in bytes to which we want to align
 *
 * This transfers the given @data into the batchbuffer. Note that the length
 * must be DWORD aligned, i.e. multiples of 32bits. The caller must
 * confirm that there is enough space in the batch for the data to be
 * copied.
 *
 * Returns: Offset of copied data.
 */
uint32_t
intel_batchbuffer_copy_data(struct intel_batchbuffer *batch,
			    const void *data, unsigned int bytes,
			    uint32_t align)
{
	uint32_t *subdata;

	igt_assert((bytes & 3) == 0);
	subdata = intel_batchbuffer_subdata_alloc(batch, bytes, align);
	memcpy(subdata, data, bytes);

	return intel_batchbuffer_subdata_offset(batch, subdata);
}

#define CHECK_RANGE(x)	do { \
	igt_assert_lte(0, (x)); \
	igt_assert_lt((x), (1 << 15)); \
} while (0)

/**
 * intel_blt_copy:
 * @batch: batchbuffer object
 * @src_bo: source libdrm buffer object
 * @src_x1: source pixel x-coordination
 * @src_y1: source pixel y-coordination
 * @src_pitch: @src_bo's pitch in bytes
 * @dst_bo: destination libdrm buffer object
 * @dst_x1: destination pixel x-coordination
 * @dst_y1: destination pixel y-coordination
 * @dst_pitch: @dst_bo's pitch in bytes
 * @width: width of the copied rectangle
 * @height: height of the copied rectangle
 * @bpp: bits per pixel
 *
 * This emits a 2D copy operation using blitter commands into the supplied batch
 * buffer object.
 */
void
intel_blt_copy(struct intel_batchbuffer *batch,
	       drm_intel_bo *src_bo, int src_x1, int src_y1, int src_pitch,
	       drm_intel_bo *dst_bo, int dst_x1, int dst_y1, int dst_pitch,
	       int width, int height, int bpp)
{
	const unsigned int gen = batch->gen;
	uint32_t src_tiling, dst_tiling, swizzle;
	uint32_t cmd_bits = 0;
	uint32_t br13_bits;

	igt_assert(bpp*(src_x1 + width) <= 8*src_pitch);
	igt_assert(bpp*(dst_x1 + width) <= 8*dst_pitch);
	igt_assert(src_pitch * (src_y1 + height) <= src_bo->size);
	igt_assert(dst_pitch * (dst_y1 + height) <= dst_bo->size);

	drm_intel_bo_get_tiling(src_bo, &src_tiling, &swizzle);
	drm_intel_bo_get_tiling(dst_bo, &dst_tiling, &swizzle);

	if (gen >= 4 && src_tiling != I915_TILING_NONE) {
		src_pitch /= 4;
		cmd_bits |= XY_SRC_COPY_BLT_SRC_TILED;
	}

	if (gen >= 4 && dst_tiling != I915_TILING_NONE) {
		dst_pitch /= 4;
		cmd_bits |= XY_SRC_COPY_BLT_DST_TILED;
	}

	CHECK_RANGE(src_x1); CHECK_RANGE(src_y1);
	CHECK_RANGE(dst_x1); CHECK_RANGE(dst_y1);
	CHECK_RANGE(width); CHECK_RANGE(height);
	CHECK_RANGE(src_x1 + width); CHECK_RANGE(src_y1 + height);
	CHECK_RANGE(dst_x1 + width); CHECK_RANGE(dst_y1 + height);
	CHECK_RANGE(src_pitch); CHECK_RANGE(dst_pitch);

	br13_bits = 0;
	switch (bpp) {
	case 8:
		break;
	case 16:		/* supporting only RGB565, not ARGB1555 */
		br13_bits |= 1 << 24;
		break;
	case 32:
		br13_bits |= 3 << 24;
		cmd_bits |= XY_SRC_COPY_BLT_WRITE_ALPHA |
			    XY_SRC_COPY_BLT_WRITE_RGB;
		break;
	default:
		igt_fail(IGT_EXIT_FAILURE);
	}

	BLIT_COPY_BATCH_START(cmd_bits);
	OUT_BATCH((br13_bits) |
		  (0xcc << 16) | /* copy ROP */
		  dst_pitch);
	OUT_BATCH((dst_y1 << 16) | dst_x1); /* dst x1,y1 */
	OUT_BATCH(((dst_y1 + height) << 16) | (dst_x1 + width)); /* dst x2,y2 */
	OUT_RELOC_FENCED(dst_bo, I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER, 0);
	OUT_BATCH((src_y1 << 16) | src_x1); /* src x1,y1 */
	OUT_BATCH(src_pitch);
	OUT_RELOC_FENCED(src_bo, I915_GEM_DOMAIN_RENDER, 0, 0);
	ADVANCE_BATCH();

#define CMD_POLY_STIPPLE_OFFSET       0x7906
	if (gen == 5) {
		BEGIN_BATCH(2, 0);
		OUT_BATCH(CMD_POLY_STIPPLE_OFFSET << 16);
		OUT_BATCH(0);
		ADVANCE_BATCH();
	}

	if (gen >= 6 && src_bo == dst_bo) {
		BEGIN_BATCH(3, 0);
		OUT_BATCH(XY_SETUP_CLIP_BLT_CMD);
		OUT_BATCH(0);
		OUT_BATCH(0);
		ADVANCE_BATCH();
	}

	intel_batchbuffer_flush(batch);
}

/**
 * intel_copy_bo:
 * @batch: batchbuffer object
 * @src_bo: source libdrm buffer object
 * @dst_bo: destination libdrm buffer object
 * @size: size of the copy range in bytes
 *
 * This emits a copy operation using blitter commands into the supplied batch
 * buffer object. A total of @size bytes from the start of @src_bo is copied
 * over to @dst_bo. Note that @size must be page-aligned.
 */
void
intel_copy_bo(struct intel_batchbuffer *batch,
	      drm_intel_bo *dst_bo, drm_intel_bo *src_bo,
	      long int size)
{
	igt_assert(size % 4096 == 0);

	intel_blt_copy(batch,
		       src_bo, 0, 0, 4096,
		       dst_bo, 0, 0, 4096,
		       4096/4, size/4096, 32);
}

/**
 * igt_buf_width:
 * @buf: the i-g-t buffer object
 *
 * Computes the width in 32-bit pixels of the given buffer.
 *
 * Returns:
 * The width of the buffer.
 */
unsigned igt_buf_width(const struct igt_buf *buf)
{
	return buf->surface[0].stride/(buf->bpp / 8);
}

/**
 * igt_buf_height:
 * @buf: the i-g-t buffer object
 *
 * Computes the height in 32-bit pixels of the given buffer.
 *
 * Returns:
 * The height of the buffer.
 */
unsigned igt_buf_height(const struct igt_buf *buf)
{
	return buf->surface[0].size/buf->surface[0].stride;
}

/**
 * igt_buf_intel_ccs_width:
 * @buf: the Intel i-g-t buffer object
 * @gen: device generation
 *
 * Computes the width of ccs buffer when considered as Intel surface data.
 *
 * Returns:
 * The width of the ccs buffer data.
 */
unsigned int igt_buf_intel_ccs_width(unsigned int gen, const struct igt_buf *buf)
{
	/*
	 * GEN12+: The CCS unit size is 64 bytes mapping 4 main surface
	 * tiles. Thus the width of the CCS unit is 4*32=128 pixels on the
	 * main surface.
	 */
	if (gen >= 12)
		return DIV_ROUND_UP(igt_buf_width(buf), 128) * 64;

	return DIV_ROUND_UP(igt_buf_width(buf), 1024) * 128;
}

/**
 * igt_buf_intel_ccs_height:
 * @buf: the i-g-t buffer object
 * @gen: device generation
 *
 * Computes the height of ccs buffer when considered as Intel surface data.
 *
 * Returns:
 * The height of the ccs buffer data.
 */
unsigned int igt_buf_intel_ccs_height(unsigned int gen, const struct igt_buf *buf)
{
	/*
	 * GEN12+: The CCS unit size is 64 bytes mapping 4 main surface
	 * tiles. Thus the height of the CCS unit is 32 pixel rows on the main
	 * surface.
	 */
	if (gen >= 12)
		return DIV_ROUND_UP(igt_buf_height(buf), 32);

	return DIV_ROUND_UP(igt_buf_height(buf), 512) * 32;
}

/*
 * pitches are in bytes if the surfaces are linear, number of dwords
 * otherwise
 */
static uint32_t fast_copy_pitch(unsigned int stride, unsigned int tiling)
{
	if (tiling != I915_TILING_NONE)
		return stride / 4;
	else
		return stride;
}

static uint32_t fast_copy_dword0(unsigned int src_tiling,
				 unsigned int dst_tiling)
{
	uint32_t dword0 = 0;

	dword0 |= XY_FAST_COPY_BLT;

	switch (src_tiling) {
	case I915_TILING_X:
		dword0 |= XY_FAST_COPY_SRC_TILING_X;
		break;
	case I915_TILING_Y:
	case I915_TILING_4:
	case I915_TILING_Yf:
		dword0 |= XY_FAST_COPY_SRC_TILING_Yb_Yf;
		break;
	case I915_TILING_Ys:
		dword0 |= XY_FAST_COPY_SRC_TILING_Ys;
		break;
	case I915_TILING_NONE:
	default:
		break;
	}

	switch (dst_tiling) {
	case I915_TILING_X:
		dword0 |= XY_FAST_COPY_DST_TILING_X;
		break;
	case I915_TILING_Y:
	case I915_TILING_4:
	case I915_TILING_Yf:
		dword0 |= XY_FAST_COPY_DST_TILING_Yb_Yf;
		break;
	case I915_TILING_Ys:
		dword0 |= XY_FAST_COPY_DST_TILING_Ys;
		break;
	case I915_TILING_NONE:
	default:
		break;
	}

	return dword0;
}

static uint32_t fast_copy_dword1(unsigned int src_tiling,
				 unsigned int dst_tiling,
				 int bpp)
{
	uint32_t dword1 = 0;

	if (src_tiling == I915_TILING_Yf || src_tiling == I915_TILING_4)
		/* Repurposed as Tile-4 on DG2 */
		dword1 |= XY_FAST_COPY_SRC_TILING_Yf;
	if (dst_tiling == I915_TILING_Yf || dst_tiling == I915_TILING_4)
		/* Repurposed as Tile-4 on DG2 */
		dword1 |= XY_FAST_COPY_DST_TILING_Yf;

	switch (bpp) {
	case 8:
		dword1 |= XY_FAST_COPY_COLOR_DEPTH_8;
		break;
	case 16:
		dword1 |= XY_FAST_COPY_COLOR_DEPTH_16;
		break;
	case 32:
		dword1 |= XY_FAST_COPY_COLOR_DEPTH_32;
		break;
	case 64:
		dword1 |= XY_FAST_COPY_COLOR_DEPTH_64;
		break;
	case 128:
		dword1 |= XY_FAST_COPY_COLOR_DEPTH_128;
		break;
	default:
		igt_assert(0);
	}

	return dword1;
}

static void
fill_relocation(struct drm_i915_gem_relocation_entry *reloc,
		uint32_t gem_handle, uint64_t presumed_offset,
		uint32_t delta, /* in bytes */
		uint32_t offset, /* in dwords */
		uint32_t read_domains, uint32_t write_domains)
{
	reloc->target_handle = gem_handle;
	reloc->delta = delta;
	reloc->offset = offset * sizeof(uint32_t);
	reloc->presumed_offset = presumed_offset;
	reloc->read_domains = read_domains;
	reloc->write_domain = write_domains;
}

static void
fill_object(struct drm_i915_gem_exec_object2 *obj,
	    uint32_t gem_handle, uint64_t gem_offset,
	    struct drm_i915_gem_relocation_entry *relocs, uint32_t count)
{
	memset(obj, 0, sizeof(*obj));
	obj->handle = gem_handle;
	obj->offset = gem_offset;
	obj->relocation_count = count;
	obj->relocs_ptr = to_user_pointer(relocs);
}

static uint32_t find_engine(const intel_ctx_cfg_t *cfg, unsigned int class)
{
	unsigned int i;
	uint32_t engine_id = -1;

	for (i = 0; i < cfg->num_engines; i++) {
		if (cfg->engines[i].engine_class == class)
			engine_id = i;
	}

	igt_assert_f(engine_id != -1, "Requested engine not found!\n");

	return engine_id;
}

static void exec_blit(int fd,
		      struct drm_i915_gem_exec_object2 *objs,
		      uint32_t count, unsigned int gen,
		      uint32_t ctx, const intel_ctx_cfg_t *cfg)
{
	struct drm_i915_gem_execbuffer2 exec;
	uint32_t devid = intel_get_drm_devid(fd);
	uint32_t blt_id = HAS_BLT_RING(devid) ? I915_EXEC_BLT : I915_EXEC_DEFAULT;

	if (cfg)
		blt_id = find_engine(cfg, I915_ENGINE_CLASS_COPY);

	exec = (struct drm_i915_gem_execbuffer2) {
		.buffers_ptr = to_user_pointer(objs),
		.buffer_count = count,
		.flags = blt_id | I915_EXEC_NO_RELOC,
		.rsvd1 = ctx,
	};

	gem_execbuf(fd, &exec);
}

static uint32_t src_copy_dword0(uint32_t src_tiling, uint32_t dst_tiling,
				uint32_t bpp, uint32_t device_gen)
{
	uint32_t dword0 = 0;

	dword0 |= XY_SRC_COPY_BLT_CMD;
	if (bpp == 32)
		dword0 |= XY_SRC_COPY_BLT_WRITE_RGB |
			XY_SRC_COPY_BLT_WRITE_ALPHA;

	if (device_gen >= 4 && src_tiling)
		dword0 |= XY_SRC_COPY_BLT_SRC_TILED;
	if (device_gen >= 4 && dst_tiling)
		dword0 |= XY_SRC_COPY_BLT_DST_TILED;

	return dword0;
}

static uint32_t src_copy_dword1(uint32_t dst_pitch, uint32_t bpp)
{
	uint32_t dword1 = 0;

	switch (bpp) {
	case 8:
		break;
	case 16:
		dword1 |= 1 << 24; /* Only support 565 color */
		break;
	case 32:
		dword1 |= 3 << 24;
		break;
	default:
		igt_assert(0);
	}

	dword1 |= 0xcc << 16;
	dword1 |= dst_pitch;

	return dword1;
}

/**
 * igt_blitter_src_copy:
 * @fd: file descriptor of the i915 driver
 * @ahnd: handle to an allocator
 * @ctx: context within which execute copy blit
 * @cfg: intel_ctx configuration, NULL for default context or legacy mode
 * @src_handle: GEM handle of the source buffer
 * @src_delta: offset into the source GEM bo, in bytes
 * @src_stride: Stride (in bytes) of the source buffer
 * @src_tiling: Tiling mode of the source buffer
 * @src_x: X coordinate of the source region to copy
 * @src_y: Y coordinate of the source region to copy
 * @src_size: size of the src bo required for allocator and softpin
 * @width: Width of the region to copy
 * @height: Height of the region to copy
 * @bpp: source and destination bits per pixel
 * @dst_handle: GEM handle of the destination buffer
 * @dst_delta: offset into the destination GEM bo, in bytes
 * @dst_stride: Stride (in bytes) of the destination buffer
 * @dst_tiling: Tiling mode of the destination buffer
 * @dst_x: X coordinate of destination
 * @dst_y: Y coordinate of destination
 * @dst_size: size of the dst bo required for allocator and softpin
 *
 * Copy @src into @dst using the XY_SRC blit command.
 */
void igt_blitter_src_copy(int fd,
			  uint64_t ahnd,
			  uint32_t ctx,
			  const intel_ctx_cfg_t *cfg,
			  /* src */
			  uint32_t src_handle,
			  uint32_t src_delta,
			  uint32_t src_stride,
			  uint32_t src_tiling,
			  uint32_t src_x, uint32_t src_y,
			  uint64_t src_size,

			  /* size */
			  uint32_t width, uint32_t height,

			  /* bpp */
			  uint32_t bpp,

			  /* dst */
			  uint32_t dst_handle,
			  uint32_t dst_delta,
			  uint32_t dst_stride,
			  uint32_t dst_tiling,
			  uint32_t dst_x, uint32_t dst_y,
			  uint64_t dst_size)
{
	uint32_t batch[32];
	struct drm_i915_gem_exec_object2 objs[3];
	struct drm_i915_gem_relocation_entry relocs[2];
	uint32_t batch_handle;
	uint32_t src_pitch, dst_pitch;
	uint32_t dst_reloc_offset, src_reloc_offset;
	uint32_t gen = intel_gen(intel_get_drm_devid(fd));
	uint64_t batch_offset, src_offset, dst_offset;
	const bool has_64b_reloc = gen >= 8;
	int i = 0;

	batch_handle = gem_create(fd, 4096);
	if (ahnd) {
		src_offset = get_offset(ahnd, src_handle, src_size, 0);
		dst_offset = get_offset(ahnd, dst_handle, dst_size, 0);
		batch_offset = get_offset(ahnd, batch_handle, 4096, 0);
	} else {
		src_offset = 16 << 20;
		dst_offset = ALIGN(src_offset + src_size, 1 << 20);
		batch_offset = ALIGN(dst_offset + dst_size, 1 << 20);
	}

	memset(batch, 0, sizeof(batch));

	igt_assert((src_tiling == I915_TILING_NONE) ||
		   (src_tiling == I915_TILING_X) ||
		   (src_tiling == I915_TILING_Y));
	igt_assert((dst_tiling == I915_TILING_NONE) ||
		   (dst_tiling == I915_TILING_X) ||
		   (dst_tiling == I915_TILING_Y));

	src_pitch = (gen >= 4 && src_tiling) ? src_stride / 4 : src_stride;
	dst_pitch = (gen >= 4 && dst_tiling) ? dst_stride / 4 : dst_stride;

	if (bpp == 64) {
		bpp /= 2;
		width *= 2;
	}

	CHECK_RANGE(src_x); CHECK_RANGE(src_y);
	CHECK_RANGE(dst_x); CHECK_RANGE(dst_y);
	CHECK_RANGE(width); CHECK_RANGE(height);
	CHECK_RANGE(src_x + width); CHECK_RANGE(src_y + height);
	CHECK_RANGE(dst_x + width); CHECK_RANGE(dst_y + height);
	CHECK_RANGE(src_pitch); CHECK_RANGE(dst_pitch);

	if ((src_tiling | dst_tiling) >= I915_TILING_Y) {
		unsigned int mask;

		batch[i++] = MI_LOAD_REGISTER_IMM;
		batch[i++] = BCS_SWCTRL;

		mask = (BCS_SRC_Y | BCS_DST_Y) << 16;
		if (src_tiling == I915_TILING_Y)
			mask |= BCS_SRC_Y;
		if (dst_tiling == I915_TILING_Y)
			mask |= BCS_DST_Y;
		batch[i++] = mask;
	}

	batch[i] = src_copy_dword0(src_tiling, dst_tiling, bpp, gen);
	batch[i++] |= 6 + 2 * has_64b_reloc;
	batch[i++] = src_copy_dword1(dst_pitch, bpp);
	batch[i++] = (dst_y << 16) | dst_x; /* dst x1,y1 */
	batch[i++] = ((dst_y + height) << 16) | (dst_x + width); /* dst x2,y2 */
	dst_reloc_offset = i;
	batch[i++] = dst_offset + dst_delta; /* dst address lower bits */
	if (has_64b_reloc)
		batch[i++] = (dst_offset + dst_delta) >> 32; /* dst address upper bits */
	batch[i++] = (src_y << 16) | src_x; /* src x1,y1 */
	batch[i++] = src_pitch;
	src_reloc_offset = i;
	batch[i++] = src_offset + src_delta; /* src address lower bits */
	if (has_64b_reloc)
		batch[i++] = (src_offset + src_delta) >> 32; /* src address upper bits */

	if ((src_tiling | dst_tiling) >= I915_TILING_Y) {
		igt_assert(gen >= 6);
		batch[i++] = MI_FLUSH_DW | 2;
		batch[i++] = 0;
		batch[i++] = 0;
		batch[i++] = 0;

		batch[i++] = MI_LOAD_REGISTER_IMM;
		batch[i++] = BCS_SWCTRL;
		batch[i++] = (BCS_SRC_Y | BCS_DST_Y) << 16;
	}

	batch[i++] = MI_BATCH_BUFFER_END;
	batch[i++] = MI_NOOP;

	igt_assert(i <= ARRAY_SIZE(batch));

	gem_write(fd, batch_handle, 0, batch, sizeof(batch));

	fill_relocation(&relocs[0], dst_handle, dst_offset,
			dst_delta, dst_reloc_offset,
			I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER);
	fill_relocation(&relocs[1], src_handle, src_offset,
			src_delta, src_reloc_offset,
			I915_GEM_DOMAIN_RENDER, 0);

	fill_object(&objs[0], dst_handle, dst_offset, NULL, 0);
	fill_object(&objs[1], src_handle, src_offset, NULL, 0);
	fill_object(&objs[2], batch_handle, batch_offset, relocs, !ahnd ? 2 : 0);

	objs[0].flags |= EXEC_OBJECT_NEEDS_FENCE | EXEC_OBJECT_WRITE;
	objs[1].flags |= EXEC_OBJECT_NEEDS_FENCE;

	if (ahnd) {
		objs[0].flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
		objs[1].flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
		objs[2].flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	}

	exec_blit(fd, objs, 3, gen, ctx, cfg);

	gem_close(fd, batch_handle);
}

/**
 * igt_blitter_fast_copy__raw:
 * @fd: file descriptor of the i915 driver
 * @ahnd: handle to an allocator
 * @ctx: context within which execute copy blit
 * @cfg: intel_ctx configuration, NULL for default context or legacy mode
 * @src_handle: GEM handle of the source buffer
 * @src_delta: offset into the source GEM bo, in bytes
 * @src_stride: Stride (in bytes) of the source buffer
 * @src_tiling: Tiling mode of the source buffer
 * @src_x: X coordinate of the source region to copy
 * @src_y: Y coordinate of the source region to copy
 * @src_size: size of the src bo required for allocator and softpin
 * @width: Width of the region to copy
 * @height: Height of the region to copy
 * @bpp: source and destination bits per pixel
 * @dst_handle: GEM handle of the destination buffer
 * @dst_delta: offset into the destination GEM bo, in bytes
 * @dst_stride: Stride (in bytes) of the destination buffer
 * @dst_tiling: Tiling mode of the destination buffer
 * @dst_x: X coordinate of destination
 * @dst_y: Y coordinate of destination
 * @dst_size: size of the dst bo required for allocator and softpin
 *
 * Like igt_blitter_fast_copy(), but talking to the kernel directly.
 */
void igt_blitter_fast_copy__raw(int fd,
				uint64_t ahnd,
				uint32_t ctx,
				const intel_ctx_cfg_t *cfg,
				/* src */
				uint32_t src_handle,
				unsigned int src_delta,
				unsigned int src_stride,
				unsigned int src_tiling,
				unsigned int src_x, unsigned src_y,
				uint64_t src_size,

				/* size */
				unsigned int width, unsigned int height,

				/* bpp */
				int bpp,

				/* dst */
				uint32_t dst_handle,
				unsigned dst_delta,
				unsigned int dst_stride,
				unsigned int dst_tiling,
				unsigned int dst_x, unsigned dst_y,
				uint64_t dst_size)
{
	uint32_t batch[12];
	struct drm_i915_gem_exec_object2 objs[3];
	struct drm_i915_gem_relocation_entry relocs[2];
	uint32_t batch_handle;
	uint32_t dword0, dword1;
	uint32_t src_pitch, dst_pitch;
	uint64_t batch_offset, src_offset, dst_offset;
	int i = 0;

	batch_handle = gem_create(fd, 4096);
	if (ahnd) {
		src_offset = get_offset(ahnd, src_handle, src_size, 0);
		dst_offset = get_offset(ahnd, dst_handle, dst_size, 0);
		batch_offset = get_offset(ahnd, batch_handle, 4096, 0);
	} else {
		src_offset = 16 << 20;
		dst_offset = ALIGN(src_offset + src_size, 1 << 20);
		batch_offset = ALIGN(dst_offset + dst_size, 1 << 20);
	}

	src_pitch = fast_copy_pitch(src_stride, src_tiling);
	dst_pitch = fast_copy_pitch(dst_stride, dst_tiling);
	dword0 = fast_copy_dword0(src_tiling, dst_tiling);
	dword1 = fast_copy_dword1(src_tiling, dst_tiling, bpp);

	CHECK_RANGE(src_x); CHECK_RANGE(src_y);
	CHECK_RANGE(dst_x); CHECK_RANGE(dst_y);
	CHECK_RANGE(width); CHECK_RANGE(height);
	CHECK_RANGE(src_x + width); CHECK_RANGE(src_y + height);
	CHECK_RANGE(dst_x + width); CHECK_RANGE(dst_y + height);
	CHECK_RANGE(src_pitch); CHECK_RANGE(dst_pitch);

	batch[i++] = dword0;
	batch[i++] = dword1 | dst_pitch;
	batch[i++] = (dst_y << 16) | dst_x; /* dst x1,y1 */
	batch[i++] = ((dst_y + height) << 16) | (dst_x + width); /* dst x2,y2 */
	batch[i++] = dst_offset + dst_delta; /* dst address lower bits */
	batch[i++] = (dst_offset + dst_delta) >> 32; /* dst address upper bits */
	batch[i++] = (src_y << 16) | src_x; /* src x1,y1 */
	batch[i++] = src_pitch;
	batch[i++] = src_offset + src_delta; /* src address lower bits */
	batch[i++] = (src_offset + src_delta) >> 32; /* src address upper bits */
	batch[i++] = MI_BATCH_BUFFER_END;
	batch[i++] = MI_NOOP;

	igt_assert(i == ARRAY_SIZE(batch));

	gem_write(fd, batch_handle, 0, batch, sizeof(batch));

	fill_relocation(&relocs[0], dst_handle, dst_offset, dst_delta, 4,
			I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER);
	fill_relocation(&relocs[1], src_handle, src_offset, src_delta, 8,
			I915_GEM_DOMAIN_RENDER, 0);

	fill_object(&objs[0], dst_handle, dst_offset, NULL, 0);
	objs[0].flags |= EXEC_OBJECT_WRITE;
	fill_object(&objs[1], src_handle, src_offset, NULL, 0);
	fill_object(&objs[2], batch_handle, batch_offset, relocs, !ahnd ? 2 : 0);

	if (ahnd) {
		objs[0].flags |= EXEC_OBJECT_PINNED;
		objs[1].flags |= EXEC_OBJECT_PINNED;
		objs[2].flags |= EXEC_OBJECT_PINNED;
	}

	exec_blit(fd, objs, 3, intel_gen(intel_get_drm_devid(fd)), ctx, cfg);

	gem_close(fd, batch_handle);
}

/**
 * igt_blitter_fast_copy:
 * @batch: batchbuffer object
 * @src: source i-g-t buffer object
 * @src_delta: offset into the source i-g-t bo
 * @src_x: source pixel x-coordination
 * @src_y: source pixel y-coordination
 * @width: width of the copied rectangle
 * @height: height of the copied rectangle
 * @dst: destination i-g-t buffer object
 * @dst_delta: offset into the destination i-g-t bo
 * @dst_x: destination pixel x-coordination
 * @dst_y: destination pixel y-coordination
 *
 * Copy @src into @dst using the gen9 fast copy blitter command.
 *
 * The source and destination surfaces cannot overlap.
 */
void igt_blitter_fast_copy(struct intel_batchbuffer *batch,
			   const struct igt_buf *src, unsigned src_delta,
			   unsigned src_x, unsigned src_y,
			   unsigned width, unsigned height,
			   int bpp,
			   const struct igt_buf *dst, unsigned dst_delta,
			   unsigned dst_x, unsigned dst_y)
{
	uint32_t src_pitch, dst_pitch;
	uint32_t dword0, dword1;

	igt_assert(src->bpp == dst->bpp);

	src_pitch = fast_copy_pitch(src->surface[0].stride, src->tiling);
	dst_pitch = fast_copy_pitch(dst->surface[0].stride, src->tiling);
	dword0 = fast_copy_dword0(src->tiling, dst->tiling);
	dword1 = fast_copy_dword1(src->tiling, dst->tiling, dst->bpp);

	CHECK_RANGE(src_x); CHECK_RANGE(src_y);
	CHECK_RANGE(dst_x); CHECK_RANGE(dst_y);
	CHECK_RANGE(width); CHECK_RANGE(height);
	CHECK_RANGE(src_x + width); CHECK_RANGE(src_y + height);
	CHECK_RANGE(dst_x + width); CHECK_RANGE(dst_y + height);
	CHECK_RANGE(src_pitch); CHECK_RANGE(dst_pitch);

	BEGIN_BATCH(10, 2);
	OUT_BATCH(dword0);
	OUT_BATCH(dword1 | dst_pitch);
	OUT_BATCH((dst_y << 16) | dst_x); /* dst x1,y1 */
	OUT_BATCH(((dst_y + height) << 16) | (dst_x + width)); /* dst x2,y2 */
	OUT_RELOC(dst->bo, I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER, dst_delta);
	OUT_BATCH(0);	/* dst address upper bits */
	OUT_BATCH((src_y << 16) | src_x); /* src x1,y1 */
	OUT_BATCH(src_pitch);
	OUT_RELOC(src->bo, I915_GEM_DOMAIN_RENDER, 0, src_delta);
	OUT_BATCH(0);	/* src address upper bits */
	ADVANCE_BATCH();

	intel_batchbuffer_flush(batch);
}

/**
 * igt_get_render_copyfunc:
 * @devid: pci device id
 *
 * Returns:
 *
 * The platform-specific render copy function pointer for the device
 * specified with @devid. Will return NULL when no render copy function is
 * implemented.
 */
igt_render_copyfunc_t igt_get_render_copyfunc(int devid)
{
	igt_render_copyfunc_t copy = NULL;

	if (IS_GEN2(devid))
		copy = gen2_render_copyfunc;
	else if (IS_GEN3(devid))
		copy = gen3_render_copyfunc;
	else if (IS_GEN4(devid) || IS_GEN5(devid))
		copy = gen4_render_copyfunc;
	else if (IS_GEN6(devid))
		copy = gen6_render_copyfunc;
	else if (IS_GEN7(devid))
		copy = gen7_render_copyfunc;
	else if (IS_GEN8(devid))
		copy = gen8_render_copyfunc;
	else if (IS_GEN9(devid) || IS_GEN10(devid))
		copy = gen9_render_copyfunc;
	else if (IS_GEN11(devid))
		copy = gen11_render_copyfunc;
	else if (HAS_4TILE(devid))
		copy = gen12p71_render_copyfunc;
	else if (IS_GEN12(devid))
		copy = gen12_render_copyfunc;

	return copy;
}

igt_vebox_copyfunc_t igt_get_vebox_copyfunc(int devid)
{
	igt_vebox_copyfunc_t copy = NULL;

	if (IS_GEN12(devid))
		copy = gen12_vebox_copyfunc;

	return copy;
}

igt_render_clearfunc_t igt_get_render_clearfunc(int devid)
{
	if (IS_DG2(devid)) {
		return gen12p71_render_clearfunc;
	} else if (IS_GEN12(devid)) {
		return gen12_render_clearfunc;
	} else {
		return NULL;
	}
}

/**
 * igt_get_media_fillfunc:
 * @devid: pci device id
 *
 * Returns:
 *
 * The platform-specific media fill function pointer for the device specified
 * with @devid. Will return NULL when no media fill function is implemented.
 */
igt_fillfunc_t igt_get_media_fillfunc(int devid)
{
	igt_fillfunc_t fill = NULL;

	if (IS_GEN12(devid))
		fill = gen12_media_fillfunc;
	else if (IS_GEN9(devid) || IS_GEN10(devid) || IS_GEN11(devid))
		fill = gen9_media_fillfunc;
	else if (IS_GEN8(devid))
		fill = gen8_media_fillfunc;
	else if (IS_GEN7(devid))
		fill = gen7_media_fillfunc;

	return fill;
}

igt_vme_func_t igt_get_media_vme_func(int devid)
{
	igt_vme_func_t fill = NULL;
	const struct intel_device_info *devinfo = intel_get_device_info(devid);

	if (IS_GEN11(devid) && !devinfo->is_elkhartlake && !devinfo->is_jasperlake)
		fill = gen11_media_vme_func;

	return fill;
}

/**
 * igt_get_gpgpu_fillfunc:
 * @devid: pci device id
 *
 * Returns:
 *
 * The platform-specific gpgpu fill function pointer for the device specified
 * with @devid. Will return NULL when no gpgpu fill function is implemented.
 */
igt_fillfunc_t igt_get_gpgpu_fillfunc(int devid)
{
	igt_fillfunc_t fill = NULL;

	if (IS_GEN7(devid))
		fill = gen7_gpgpu_fillfunc;
	else if (IS_GEN8(devid))
		fill = gen8_gpgpu_fillfunc;
	else if (IS_GEN9(devid) || IS_GEN10(devid))
		fill = gen9_gpgpu_fillfunc;
	else if (IS_GEN11(devid))
		fill = gen11_gpgpu_fillfunc;
	else if (IS_GEN12(devid))
		fill = gen12_gpgpu_fillfunc;

	return fill;
}

/**
 * igt_get_media_spinfunc:
 * @devid: pci device id
 *
 * Returns:
 *
 * The platform-specific media spin function pointer for the device specified
 * with @devid. Will return NULL when no media spin function is implemented.
 */
igt_media_spinfunc_t igt_get_media_spinfunc(int devid)
{
	igt_media_spinfunc_t spin = NULL;

	if (IS_GEN9(devid))
		spin = gen9_media_spinfunc;
	else if (IS_GEN8(devid))
		spin = gen8_media_spinfunc;

	return spin;
}

/* Intel batchbuffer v2 */
static bool intel_bb_debug_tree = false;

/*
 * __reallocate_objects:
 * @ibb: pointer to intel_bb
 *
 * Increases number of objects if necessary.
 */
static void __reallocate_objects(struct intel_bb *ibb)
{
	const uint32_t inc = 4096 / sizeof(*ibb->objects);

	if (ibb->num_objects == ibb->allocated_objects) {
		ibb->objects = realloc(ibb->objects,
				       sizeof(*ibb->objects) *
				       (inc + ibb->allocated_objects));

		igt_assert(ibb->objects);
		ibb->allocated_objects += inc;

		memset(&ibb->objects[ibb->num_objects],	0,
		       inc * sizeof(*ibb->objects));
	}
}

static inline uint64_t __intel_bb_get_offset(struct intel_bb *ibb,
					     uint32_t handle,
					     uint64_t size,
					     uint32_t alignment)
{
	uint64_t offset;

	if (ibb->enforce_relocs)
		return 0;

	offset = intel_allocator_alloc(ibb->allocator_handle,
				       handle, size, alignment);

	return offset;
}

/**
 * __intel_bb_create:
 * @i915: drm fd
 * @ctx: context id
 * @cfg: intel_ctx configuration, NULL for default context or legacy mode
 * @size: size of the batchbuffer
 * @do_relocs: use relocations or allocator
 * @allocator_type: allocator type, must be INTEL_ALLOCATOR_NONE for relocations
 *
 * intel-bb assumes it will work in one of two modes - with relocations or
 * with using allocator (currently RANDOM and SIMPLE are implemented).
 * Some description is required to describe how they maintain the addresses.
 *
 * Before entering into each scenarios generic rule is intel-bb keeps objects
 * and their offsets in the internal cache and reuses in subsequent execs.
 *
 * 1. intel-bb with relocations
 *
 * Creating new intel-bb adds handle to cache implicitly and sets its address
 * to 0. Objects added to intel-bb later also have address 0 set for first run.
 * After calling execbuf cache is altered with new addresses. As intel-bb
 * works in reloc mode addresses are only suggestion to the driver and we
 * cannot be sure they won't change at next exec.
 *
 * 2. with allocator
 *
 * This mode is valid only for ppgtt. Addresses are acquired from allocator
 * and softpinned. intel-bb cache must be then coherent with allocator
 * (simple is coherent, random is not due to fact we don't keep its state).
 * When we do intel-bb reset with purging cache it has to reacquire addresses
 * from allocator (allocator should return same address - what is true for
 * simple allocator and false for random as mentioned before).
 *
 * If we do reset without purging caches we use addresses from intel-bb cache
 * during execbuf objects construction.
 *
 * If we do reset with purging caches allocator entries are freed as well.
 *
 * __intel_bb_create checks if a context configuration for intel_ctx_t was
 * passed in. If this is the case, it copies the information over to the
 * newly created batch buffer.
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 */
static struct intel_bb *
__intel_bb_create(int i915, uint32_t ctx, const intel_ctx_cfg_t *cfg,
		  uint32_t size, bool do_relocs,
		  uint64_t start, uint64_t end,
		  uint8_t allocator_type, enum allocator_strategy strategy)
{
	struct drm_i915_gem_exec_object2 *object;
	struct intel_bb *ibb = calloc(1, sizeof(*ibb));

	igt_assert(ibb);

	ibb->uses_full_ppgtt = gem_uses_full_ppgtt(i915);
	ibb->devid = intel_get_drm_devid(i915);
	ibb->gen = intel_gen(ibb->devid);

	/*
	 * If we don't have full ppgtt driver can change our addresses
	 * so allocator is useless in this case. Just enforce relocations
	 * for such gens and don't use allocator at all.
	 */
	if (!ibb->uses_full_ppgtt)
		do_relocs = true;

	/*
	 * For softpin mode allocator has full control over offsets allocation
	 * so we want kernel to not interfere with this.
	 */
	if (do_relocs)
		ibb->allows_obj_alignment = gem_allows_obj_alignment(i915);

	/* Use safe start offset instead assuming 0x0 is safe */
	start = max_t(uint64_t, start, gem_detect_safe_start_offset(i915));

	/* if relocs are set we won't use an allocator */
	if (do_relocs)
		allocator_type = INTEL_ALLOCATOR_NONE;
	else
		ibb->allocator_handle = intel_allocator_open_full(i915, ctx,
								  start, end,
								  allocator_type,
								  strategy, 0);
	ibb->allocator_type = allocator_type;
	ibb->allocator_strategy = strategy;
	ibb->allocator_start = start;
	ibb->allocator_end = end;

	ibb->i915 = i915;
	ibb->enforce_relocs = do_relocs;
	ibb->handle = gem_create(i915, size);
	ibb->size = size;
	ibb->alignment = gem_detect_safe_alignment(i915);
	ibb->ctx = ctx;
	ibb->vm_id = 0;
	ibb->batch = calloc(1, size);
	igt_assert(ibb->batch);
	ibb->ptr = ibb->batch;
	ibb->fence = -1;

	/* Cache context configuration */
	if (cfg) {
		ibb->cfg = malloc(sizeof(*cfg));
		igt_assert(ibb->cfg);
		memcpy(ibb->cfg, cfg, sizeof(*cfg));
	}

	ibb->gtt_size = gem_aperture_size(i915);
	if ((ibb->gtt_size - 1) >> 32)
		ibb->supports_48b_address = true;

	object = intel_bb_add_object(ibb, ibb->handle, ibb->size,
				     INTEL_BUF_INVALID_ADDRESS, ibb->alignment,
				     false);
	ibb->batch_offset = object->offset;

	IGT_INIT_LIST_HEAD(&ibb->intel_bufs);

	ibb->refcount = 1;

	if (intel_bb_do_tracking && ibb->allocator_type != INTEL_ALLOCATOR_NONE) {
		pthread_mutex_lock(&intel_bb_list_lock);
		igt_list_add(&ibb->link, &intel_bb_list);
		pthread_mutex_unlock(&intel_bb_list_lock);
	}

	return ibb;
}

/**
 * intel_bb_create_full:
 * @i915: drm fd
 * @ctx: context
 * @cfg: intel_ctx configuration, NULL for default context or legacy mode
 * @size: size of the batchbuffer
 * @start: allocator vm start address
 * @end: allocator vm start address
 * @allocator_type: allocator type, SIMPLE, RANDOM, ...
 * @strategy: allocation strategy
 *
 * Creates bb with context passed in @ctx, size in @size and allocator type
 * in @allocator_type. Relocations are set to false because IGT allocator
 * is used in that case. VM range is passed to allocator (@start and @end)
 * and allocation @strategy (suggestion to allocator about address allocation
 * preferences).
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 */
struct intel_bb *intel_bb_create_full(int i915, uint32_t ctx,
				      const intel_ctx_cfg_t *cfg, uint32_t size,
				      uint64_t start, uint64_t end,
				      uint8_t allocator_type,
				      enum allocator_strategy strategy)
{
	return __intel_bb_create(i915, ctx, cfg, size, false, start, end,
				 allocator_type, strategy);
}

/**
 * intel_bb_create_with_allocator:
 * @i915: drm fd
 * @ctx: context
 * @cfg: intel_ctx configuration, NULL for default context or legacy mode
 * @size: size of the batchbuffer
 * @allocator_type: allocator type, SIMPLE, RANDOM, ...
 *
 * Creates bb with context passed in @ctx, size in @size and allocator type
 * in @allocator_type. Relocations are set to false because IGT allocator
 * is used in that case.
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 */
struct intel_bb *intel_bb_create_with_allocator(int i915, uint32_t ctx,
						const intel_ctx_cfg_t *cfg,
						uint32_t size,
						uint8_t allocator_type)
{
	return __intel_bb_create(i915, ctx, cfg, size, false, 0, 0,
				 allocator_type, ALLOC_STRATEGY_HIGH_TO_LOW);
}

static bool aux_needs_softpin(int i915)
{
	return intel_gen(intel_get_drm_devid(i915)) >= 12;
}

static bool has_ctx_cfg(struct intel_bb *ibb)
{
	return ibb->cfg && ibb->cfg->num_engines > 0;
}

/**
 * intel_bb_create:
 * @i915: drm fd
 * @size: size of the batchbuffer
 *
 * Creates bb with default context.
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 *
 * Notes:
 *
 * intel_bb must not be created in igt_fixture. The reason is intel_bb
 * "opens" connection to the allocator and when test completes it can
 * leave the allocator in unknown state (mostly for failed tests).
 * As igt_core was armed to reset the allocator infrastructure
 * connection to it inside intel_bb is not valid anymore.
 * Trying to use it leads to catastrofic errors.
 */
struct intel_bb *intel_bb_create(int i915, uint32_t size)
{
	bool relocs = gem_has_relocations(i915);

	return __intel_bb_create(i915, 0, NULL, size,
				 relocs && !aux_needs_softpin(i915), 0, 0,
				 INTEL_ALLOCATOR_SIMPLE,
				 ALLOC_STRATEGY_HIGH_TO_LOW);
}

/**
 * intel_bb_create_with_context:
 * @i915: drm fd
 * @ctx: context id
 * @cfg: intel_ctx configuration, NULL for default context or legacy mode
 * @size: size of the batchbuffer
 *
 * Creates bb with context passed in @ctx and @cfg configuration (when
 * working with custom engines layout).
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 */
struct intel_bb *
intel_bb_create_with_context(int i915, uint32_t ctx,
			     const intel_ctx_cfg_t *cfg, uint32_t size)
{
	bool relocs = gem_has_relocations(i915);

	return __intel_bb_create(i915, ctx, cfg, size,
				 relocs && !aux_needs_softpin(i915), 0, 0,
				 INTEL_ALLOCATOR_SIMPLE,
				 ALLOC_STRATEGY_HIGH_TO_LOW);
}

/**
 * intel_bb_create_with_relocs:
 * @i915: drm fd
 * @size: size of the batchbuffer
 *
 * Creates bb which will disable passing addresses.
 * This will lead to relocations when objects are not previously pinned.
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 */
struct intel_bb *intel_bb_create_with_relocs(int i915, uint32_t size)
{
	igt_require(gem_has_relocations(i915));

	return __intel_bb_create(i915, 0, NULL, size, true, 0, 0,
				 INTEL_ALLOCATOR_NONE, ALLOC_STRATEGY_NONE);
}

/**
 * intel_bb_create_with_relocs_and_context:
 * @i915: drm fd
 * @ctx: context
 * @cfg: intel_ctx configuration, NULL for default context or legacy mode
 * @size: size of the batchbuffer
 *
 * Creates bb with default context which will disable passing addresses.
 * This will lead to relocations when objects are not previously pinned.
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 */
struct intel_bb *
intel_bb_create_with_relocs_and_context(int i915, uint32_t ctx,
					const intel_ctx_cfg_t *cfg,
					uint32_t size)
{
	igt_require(gem_has_relocations(i915));

	return __intel_bb_create(i915, ctx, cfg, size, true, 0, 0,
				 INTEL_ALLOCATOR_NONE, ALLOC_STRATEGY_NONE);
}

/**
 * intel_bb_create_no_relocs:
 * @i915: drm fd
 * @size: size of the batchbuffer
 *
 * Creates bb with disabled relocations.
 * This enables passing addresses and requires pinning objects.
 *
 * Returns:
 *
 * Pointer the intel_bb, asserts on failure.
 */
struct intel_bb *intel_bb_create_no_relocs(int i915, uint32_t size)
{
	igt_require(gem_uses_full_ppgtt(i915));

	return __intel_bb_create(i915, 0, NULL, size, false, 0, 0,
				 INTEL_ALLOCATOR_SIMPLE,
				 ALLOC_STRATEGY_HIGH_TO_LOW);
}

static void __intel_bb_destroy_relocations(struct intel_bb *ibb)
{
	uint32_t i;

	/* Free relocations */
	for (i = 0; i < ibb->num_objects; i++) {
		free(from_user_pointer(ibb->objects[i]->relocs_ptr));
		ibb->objects[i]->relocs_ptr = to_user_pointer(NULL);
		ibb->objects[i]->relocation_count = 0;
	}

	ibb->relocs = NULL;
	ibb->num_relocs = 0;
	ibb->allocated_relocs = 0;
}

static void __intel_bb_destroy_objects(struct intel_bb *ibb)
{
	free(ibb->objects);
	ibb->objects = NULL;

	tdestroy(ibb->current, free);
	ibb->current = NULL;

	ibb->num_objects = 0;
	ibb->allocated_objects = 0;
}

static void __intel_bb_destroy_cache(struct intel_bb *ibb)
{
	tdestroy(ibb->root, free);
	ibb->root = NULL;
}

static void __intel_bb_remove_intel_bufs(struct intel_bb *ibb)
{
	struct intel_buf *entry, *tmp;

	igt_list_for_each_entry_safe(entry, tmp, &ibb->intel_bufs, link)
		intel_bb_remove_intel_buf(ibb, entry);
}

/**
 * intel_bb_destroy:
 * @ibb: pointer to intel_bb
 *
 * Frees all relocations / objects allocated during filling the batch.
 */
void intel_bb_destroy(struct intel_bb *ibb)
{
	igt_assert(ibb);

	ibb->refcount--;
	igt_assert_f(ibb->refcount == 0, "Trying to destroy referenced bb!");

	__intel_bb_remove_intel_bufs(ibb);
	__intel_bb_destroy_relocations(ibb);
	__intel_bb_destroy_objects(ibb);
	__intel_bb_destroy_cache(ibb);

	if (ibb->allocator_type != INTEL_ALLOCATOR_NONE) {
		if (intel_bb_do_tracking) {
			pthread_mutex_lock(&intel_bb_list_lock);
			igt_list_del(&ibb->link);
			pthread_mutex_unlock(&intel_bb_list_lock);
		}

		intel_allocator_free(ibb->allocator_handle, ibb->handle);
		intel_allocator_close(ibb->allocator_handle);
	}
	gem_close(ibb->i915, ibb->handle);

	if (ibb->fence >= 0)
		close(ibb->fence);

	free(ibb->batch);
	free(ibb->cfg);
	free(ibb);
}

/*
 * intel_bb_reset:
 * @ibb: pointer to intel_bb
 * @purge_objects_cache: if true destroy internal execobj and relocs + cache
 *
 * Recreate batch bo when there's no additional reference.
 *
 * When purge_object_cache == true we destroy cache as well as remove intel_buf
 * from intel-bb tracking list. Removing intel_bufs releases their addresses
 * in the allocator.
*/

void intel_bb_reset(struct intel_bb *ibb, bool purge_objects_cache)
{
	uint32_t i;

	if (purge_objects_cache && ibb->refcount > 1)
		igt_warn("Cannot purge objects cache on bb, refcount > 1!");

	/* Someone keeps reference, just exit */
	if (ibb->refcount > 1)
		return;

	/*
	 * To avoid relocation objects previously pinned to high virtual
	 * addresses should keep 48bit flag. Ensure we won't clear it
	 * in the reset path.
	 */
	for (i = 0; i < ibb->num_objects; i++)
		ibb->objects[i]->flags &= EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	__intel_bb_destroy_relocations(ibb);
	__intel_bb_destroy_objects(ibb);
	__reallocate_objects(ibb);

	if (purge_objects_cache) {
		__intel_bb_remove_intel_bufs(ibb);
		__intel_bb_destroy_cache(ibb);
	}

	/*
	 * When we use allocators we're in no-reloc mode so we have to free
	 * and reacquire offset (ibb->handle can change in multiprocess
	 * environment). We also have to remove and add it again to
	 * objects and cache tree.
	 */
	if (ibb->allocator_type != INTEL_ALLOCATOR_NONE && !purge_objects_cache)
		intel_bb_remove_object(ibb, ibb->handle, ibb->batch_offset,
				       ibb->size);

	gem_close(ibb->i915, ibb->handle);
	ibb->handle = gem_create(ibb->i915, ibb->size);

	/* Keep address for bb in reloc mode and RANDOM allocator */
	if (ibb->allocator_type == INTEL_ALLOCATOR_SIMPLE)
		ibb->batch_offset = __intel_bb_get_offset(ibb,
							  ibb->handle,
							  ibb->size,
							  ibb->alignment);

	intel_bb_add_object(ibb, ibb->handle, ibb->size,
			    ibb->batch_offset,
			    ibb->alignment, false);
	ibb->ptr = ibb->batch;
	memset(ibb->batch, 0, ibb->size);
}

/*
 * intel_bb_sync:
 * @ibb: pointer to intel_bb
 *
 * Waits for bb completion. Returns 0 on success, otherwise errno.
 */
int intel_bb_sync(struct intel_bb *ibb)
{
	int ret;

	if (ibb->fence < 0)
		return 0;

	ret = sync_fence_wait(ibb->fence, -1);
	if (ret == 0) {
		close(ibb->fence);
		ibb->fence = -1;
	}

	return ret;
}

/*
 * intel_bb_print:
 * @ibb: pointer to intel_bb
 *
 * Prints batch to stdout.
 */
void intel_bb_print(struct intel_bb *ibb)
{
	igt_info("drm fd: %d, gen: %d, devid: %u, debug: %d\n",
		 ibb->i915, ibb->gen, ibb->devid, ibb->debug);
	igt_info("handle: %u, size: %u, batch: %p, ptr: %p\n",
		 ibb->handle, ibb->size, ibb->batch, ibb->ptr);
	igt_info("gtt_size: %" PRIu64 ", supports 48bit: %d\n",
		 ibb->gtt_size, ibb->supports_48b_address);
	igt_info("ctx: %u\n", ibb->ctx);
	igt_info("root: %p\n", ibb->root);
	igt_info("objects: %p, num_objects: %u, allocated obj: %u\n",
		 ibb->objects, ibb->num_objects, ibb->allocated_objects);
	igt_info("relocs: %p, num_relocs: %u, allocated_relocs: %u\n----\n",
		 ibb->relocs, ibb->num_relocs, ibb->allocated_relocs);
}

/*
 * intel_bb_dump:
 * @ibb: pointer to intel_bb
 * @filename: name to which write bb
 *
 * Dump batch bo to file.
 */
void intel_bb_dump(struct intel_bb *ibb, const char *filename)
{
	FILE *out;
	void *ptr;

	ptr = gem_mmap__device_coherent(ibb->i915, ibb->handle, 0, ibb->size,
					PROT_READ);
	out = fopen(filename, "wb");
	igt_assert(out);
	fwrite(ptr, ibb->size, 1, out);
	fclose(out);
	munmap(ptr, ibb->size);
}

/**
 * intel_bb_set_debug:
 * @ibb: pointer to intel_bb
 * @debug: true / false
 *
 * Sets debug to true / false. Execbuf is then called synchronously and
 * object/reloc arrays are printed after execution.
 */
void intel_bb_set_debug(struct intel_bb *ibb, bool debug)
{
	ibb->debug = debug;
}

/**
 * intel_bb_set_dump_base64:
 * @ibb: pointer to intel_bb
 * @dump: true / false
 *
 * Do bb dump as base64 string before execbuf call.
 */
void intel_bb_set_dump_base64(struct intel_bb *ibb, bool dump)
{
	ibb->dump_base64 = dump;
}

static int __compare_objects(const void *p1, const void *p2)
{
	const struct drm_i915_gem_exec_object2 *o1 = p1, *o2 = p2;

	return (int) ((int64_t) o1->handle - (int64_t) o2->handle);
}

static struct drm_i915_gem_exec_object2 *
__add_to_cache(struct intel_bb *ibb, uint32_t handle)
{
	struct drm_i915_gem_exec_object2 **found, *object;

	object = malloc(sizeof(*object));
	igt_assert(object);

	object->handle = handle;
	object->alignment = 0;
	found = tsearch((void *) object, &ibb->root, __compare_objects);

	if (*found == object) {
		memset(object, 0, sizeof(*object));
		object->handle = handle;
		object->offset = INTEL_BUF_INVALID_ADDRESS;
	} else {
		free(object);
		object = *found;
	}

	return object;
}

static bool __remove_from_cache(struct intel_bb *ibb, uint32_t handle)
{
	struct drm_i915_gem_exec_object2 **found, *object;

	object = intel_bb_find_object(ibb, handle);
	if (!object) {
		igt_warn("Object: handle: %u not found\n", handle);
		return false;
	}

	found = tdelete((void *) object, &ibb->root, __compare_objects);
	if (!found)
		return false;

	free(object);

	return true;
}

static int __compare_handles(const void *p1, const void *p2)
{
	return (int) (*(int32_t *) p1 - *(int32_t *) p2);
}

static void __add_to_objects(struct intel_bb *ibb,
			     struct drm_i915_gem_exec_object2 *object)
{
	uint32_t **found, *handle;

	handle = malloc(sizeof(*handle));
	igt_assert(handle);

	*handle = object->handle;
	found = tsearch((void *) handle, &ibb->current, __compare_handles);

	if (*found == handle) {
		__reallocate_objects(ibb);
		igt_assert(ibb->num_objects < ibb->allocated_objects);
		ibb->objects[ibb->num_objects++] = object;
	} else {
		free(handle);
	}
}

static void __remove_from_objects(struct intel_bb *ibb,
				  struct drm_i915_gem_exec_object2 *object)
{
	uint32_t i, **handle, *to_free;
	bool found = false;

	for (i = 0; i < ibb->num_objects; i++) {
		if (ibb->objects[i] == object) {
			found = true;
			break;
		}
	}

	/*
	 * When we reset bb (without purging) we have:
	 * 1. cache which contains all cached objects
	 * 2. objects array which contains only bb object (cleared in reset
	 *    path with bb object added at the end)
	 * So !found is normal situation and no warning is added here.
	 */
	if (!found)
		return;

	ibb->num_objects--;
	if (i < ibb->num_objects)
		memmove(&ibb->objects[i], &ibb->objects[i + 1],
			sizeof(object) * (ibb->num_objects - i));

	handle = tfind((void *) &object->handle,
		       &ibb->current, __compare_handles);
	if (!handle) {
		igt_warn("Object %u doesn't exist in the tree, can't remove",
			 object->handle);
		return;
	}

	to_free = *handle;
	tdelete((void *) &object->handle, &ibb->current, __compare_handles);
	free(to_free);
}

/**
 * intel_bb_add_object:
 * @ibb: pointer to intel_bb
 * @handle: which handle to add to objects array
 * @size: object size
 * @offset: presumed offset of the object when no relocation is enforced
 * @alignment: alignment of the object, if 0 it will be set to page size
 * @write: does a handle is a render target
 *
 * Function adds or updates execobj slot in bb objects array and
 * in the object tree. When object is a render target it has to
 * be marked with EXEC_OBJECT_WRITE flag.
 */
struct drm_i915_gem_exec_object2 *
intel_bb_add_object(struct intel_bb *ibb, uint32_t handle, uint64_t size,
		    uint64_t offset, uint64_t alignment, bool write)
{
	struct drm_i915_gem_exec_object2 *object;

	igt_assert(INVALID_ADDR(offset) || alignment == 0
		   || ALIGN(offset, alignment) == offset);
	igt_assert(is_power_of_two(alignment));

	object = __add_to_cache(ibb, handle);
	alignment = max_t(uint64_t, alignment, gem_detect_safe_alignment(ibb->i915));
	__add_to_objects(ibb, object);

	/*
	 * If object->offset == INVALID_ADDRESS we added freshly object to the
	 * cache. In that case we have two choices:
	 * a) get new offset (passed offset was invalid)
	 * b) use offset passed in the call (valid)
	 */
	if (INVALID_ADDR(object->offset)) {
		if (INVALID_ADDR(offset)) {
			offset = __intel_bb_get_offset(ibb, handle, size,
						       alignment);
		} else {
			offset = offset & (ibb->gtt_size - 1);

			/*
			 * For simple allocator check entry consistency
			 * - reserve if it is not already allocated.
			 */
			if (ibb->allocator_type == INTEL_ALLOCATOR_SIMPLE) {
				bool allocated, reserved;

				reserved = intel_allocator_reserve_if_not_allocated(ibb->allocator_handle,
										    handle, size, offset,
										    &allocated);
				igt_assert_f(allocated || reserved,
					     "Can't get offset, allocated: %d, reserved: %d\n",
					     allocated, reserved);
			}
		}
	} else {
		/*
		 * This assertion makes sense only when we have to be consistent
		 * with underlying allocator. For relocations and when !ppgtt
		 * we can expect addresses passed by the user can be moved
		 * within the driver.
		 */
		if (ibb->allocator_type == INTEL_ALLOCATOR_SIMPLE)
			igt_assert_f(object->offset == offset,
				     "(pid: %ld) handle: %u, offset not match: %" PRIx64 " <> %" PRIx64 "\n",
				     (long) getpid(), handle,
				     (uint64_t) object->offset,
				     offset);
	}

	object->offset = offset;

	if (write)
		object->flags |= EXEC_OBJECT_WRITE;

	if (ibb->supports_48b_address)
		object->flags |= EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	if (ibb->uses_full_ppgtt && !ibb->enforce_relocs)
		object->flags |= EXEC_OBJECT_PINNED;

	if (ibb->allows_obj_alignment)
		object->alignment = alignment;

	return object;
}

bool intel_bb_remove_object(struct intel_bb *ibb, uint32_t handle,
			    uint64_t offset, uint64_t size)
{
	struct drm_i915_gem_exec_object2 *object;
	bool is_reserved;

	object = intel_bb_find_object(ibb, handle);
	if (!object)
		return false;

	if (ibb->allocator_type != INTEL_ALLOCATOR_NONE) {
		intel_allocator_free(ibb->allocator_handle, handle);
		is_reserved = intel_allocator_is_reserved(ibb->allocator_handle,
							  size, offset);
		if (is_reserved)
			intel_allocator_unreserve(ibb->allocator_handle, handle,
						  size, offset);
	}

	__remove_from_objects(ibb, object);
	__remove_from_cache(ibb, handle);

	return true;
}

static struct drm_i915_gem_exec_object2 *
__intel_bb_add_intel_buf(struct intel_bb *ibb, struct intel_buf *buf,
			 uint64_t alignment, bool write)
{
	struct drm_i915_gem_exec_object2 *obj;

	igt_assert(ibb);
	igt_assert(buf);
	igt_assert(!buf->ibb || buf->ibb == ibb);
	igt_assert(ALIGN(alignment, 4096) == alignment);

	if (!alignment) {
		alignment = 0x1000;

		if (ibb->gen >= 12 && buf->compression)
			alignment = 0x10000;

		/* For gen3 ensure tiled buffers are aligned to power of two size */
		if (ibb->gen == 3 && buf->tiling) {
			alignment = 1024 * 1024;

			while (alignment < buf->surface[0].size)
				alignment <<= 1;
		}
	}

	obj = intel_bb_add_object(ibb, buf->handle, intel_buf_bo_size(buf),
				  buf->addr.offset, alignment, write);
	buf->addr.offset = obj->offset;

	if (igt_list_empty(&buf->link)) {
		igt_list_add_tail(&buf->link, &ibb->intel_bufs);
		buf->ibb = ibb;
	} else {
		igt_assert(buf->ibb == ibb);
	}

	return obj;
}

struct drm_i915_gem_exec_object2 *
intel_bb_add_intel_buf(struct intel_bb *ibb, struct intel_buf *buf, bool write)
{
	return __intel_bb_add_intel_buf(ibb, buf, 0, write);
}

struct drm_i915_gem_exec_object2 *
intel_bb_add_intel_buf_with_alignment(struct intel_bb *ibb, struct intel_buf *buf,
				      uint64_t alignment, bool write)
{
	return __intel_bb_add_intel_buf(ibb, buf, alignment, write);
}

bool intel_bb_remove_intel_buf(struct intel_bb *ibb, struct intel_buf *buf)
{
	bool removed;

	igt_assert(ibb);
	igt_assert(buf);
	igt_assert(!buf->ibb || buf->ibb == ibb);

	if (igt_list_empty(&buf->link))
		return false;

	removed = intel_bb_remove_object(ibb, buf->handle,
					 buf->addr.offset,
					 intel_buf_bo_size(buf));
	if (removed) {
		buf->addr.offset = INTEL_BUF_INVALID_ADDRESS;
		buf->ibb = NULL;
		igt_list_del_init(&buf->link);
	}

	return removed;
}

void intel_bb_print_intel_bufs(struct intel_bb *ibb)
{
	struct intel_buf *entry;

	igt_list_for_each_entry(entry, &ibb->intel_bufs, link) {
		igt_info("handle: %u, ibb: %p, offset: %lx\n",
			 entry->handle, entry->ibb,
			 (long) entry->addr.offset);
	}
}

struct drm_i915_gem_exec_object2 *
intel_bb_find_object(struct intel_bb *ibb, uint32_t handle)
{
	struct drm_i915_gem_exec_object2 object = { .handle = handle };
	struct drm_i915_gem_exec_object2 **found;

	found = tfind((void *) &object, &ibb->root, __compare_objects);
	if (!found)
		return NULL;

	return *found;
}

bool
intel_bb_object_set_flag(struct intel_bb *ibb, uint32_t handle, uint64_t flag)
{
	struct drm_i915_gem_exec_object2 object = { .handle = handle };
	struct drm_i915_gem_exec_object2 **found;

	igt_assert_f(ibb->root, "Trying to search in null tree\n");

	found = tfind((void *) &object, &ibb->root, __compare_objects);
	if (!found) {
		igt_warn("Trying to set fence on not found handle: %u\n",
			 handle);
		return false;
	}

	(*found)->flags |= flag;

	return true;
}

bool
intel_bb_object_clear_flag(struct intel_bb *ibb, uint32_t handle, uint64_t flag)
{
	struct drm_i915_gem_exec_object2 object = { .handle = handle };
	struct drm_i915_gem_exec_object2 **found;

	found = tfind((void *) &object, &ibb->root, __compare_objects);
	if (!found) {
		igt_warn("Trying to set fence on not found handle: %u\n",
			 handle);
		return false;
	}

	(*found)->flags &= ~flag;

	return true;
}

/*
 * intel_bb_add_reloc:
 * @ibb: pointer to intel_bb
 * @to_handle: object handle in which do relocation
 * @handle: object handle which address will be taken to patch the @to_handle
 * @read_domains: gem domain bits for the relocation
 * @write_domain: gem domain bit for the relocation
 * @delta: delta value to add to @buffer's gpu address
 * @offset: offset within bb to be patched
 *
 * When relocations are requested function allocates additional relocation slot
 * in reloc array for a handle.
 * Object must be previously added to bb.
 */
static uint64_t intel_bb_add_reloc(struct intel_bb *ibb,
				   uint32_t to_handle,
				   uint32_t handle,
				   uint32_t read_domains,
				   uint32_t write_domain,
				   uint64_t delta,
				   uint64_t offset,
				   uint64_t presumed_offset)
{
	struct drm_i915_gem_relocation_entry *relocs;
	struct drm_i915_gem_exec_object2 *object, *to_object;
	uint32_t i;

	object = intel_bb_find_object(ibb, handle);
	igt_assert(object);

	/* In no-reloc mode we just return the previously assigned address */
	if (!ibb->enforce_relocs)
		goto out;

	/* For ibb we have relocs allocated in chunks */
	if (to_handle == ibb->handle) {
		relocs = ibb->relocs;
		if (ibb->num_relocs == ibb->allocated_relocs) {
			ibb->allocated_relocs += 4096 / sizeof(*relocs);
			relocs = realloc(relocs, sizeof(*relocs) * ibb->allocated_relocs);
			igt_assert(relocs);
			ibb->relocs = relocs;
		}
		i = ibb->num_relocs++;
	} else {
		to_object = intel_bb_find_object(ibb, to_handle);
		igt_assert_f(to_object, "object has to be added to ibb first!\n");

		i = to_object->relocation_count++;
		relocs = from_user_pointer(to_object->relocs_ptr);
		relocs = realloc(relocs, sizeof(*relocs) * to_object->relocation_count);
		to_object->relocs_ptr = to_user_pointer(relocs);
		igt_assert(relocs);
	}

	memset(&relocs[i], 0, sizeof(*relocs));
	relocs[i].target_handle = handle;
	relocs[i].read_domains = read_domains;
	relocs[i].write_domain = write_domain;
	relocs[i].delta = delta;
	relocs[i].offset = offset;
	if (ibb->enforce_relocs)
		relocs[i].presumed_offset = -1;
	else
		relocs[i].presumed_offset = object->offset;

	igt_debug("add reloc: to_handle: %u, handle: %u, r/w: 0x%x/0x%x, "
		  "delta: 0x%" PRIx64 ", "
		  "offset: 0x%" PRIx64 ", "
		  "poffset: %p\n",
		  to_handle, handle, read_domains, write_domain,
		  delta, offset,
		  from_user_pointer(relocs[i].presumed_offset));

out:
	return object->offset;
}

static uint64_t __intel_bb_emit_reloc(struct intel_bb *ibb,
				      uint32_t to_handle,
				      uint32_t to_offset,
				      uint32_t handle,
				      uint32_t read_domains,
				      uint32_t write_domain,
				      uint64_t delta,
				      uint64_t presumed_offset)
{
	uint64_t address;

	igt_assert(ibb);

	address = intel_bb_add_reloc(ibb, to_handle, handle,
				     read_domains, write_domain,
				     delta, to_offset,
				     presumed_offset);

	intel_bb_out(ibb, delta + address);
	if (ibb->gen >= 8)
		intel_bb_out(ibb, (delta + address) >> 32);

	return address;
}

/**
 * intel_bb_emit_reloc:
 * @ibb: pointer to intel_bb
 * @handle: object handle which address will be taken to patch the bb
 * @read_domains: gem domain bits for the relocation
 * @write_domain: gem domain bit for the relocation
 * @delta: delta value to add to @buffer's gpu address
 * @presumed_offset: address of the object in address space. If -1 is passed
 * then final offset of the object will be randomized (for no-reloc bb) or
 * 0 (for reloc bb, in that case reloc.presumed_offset will be -1). In
 * case address is known it should passed in @presumed_offset (for no-reloc).
 * @write: does a handle is a render target
 *
 * Function prepares relocation (execobj if required + reloc) and emits
 * offset in bb. For I915_EXEC_NO_RELOC presumed_offset is a hint we already
 * have object in valid place and relocation step can be skipped in this case.
 *
 * Note: delta is value added to address, mostly used when some instructions
 * require modify-bit set to apply change. Which delta is valid depends
 * on instruction (see instruction specification).
 */
uint64_t intel_bb_emit_reloc(struct intel_bb *ibb,
			     uint32_t handle,
			     uint32_t read_domains,
			     uint32_t write_domain,
			     uint64_t delta,
			     uint64_t presumed_offset)
{
	igt_assert(ibb);

	return __intel_bb_emit_reloc(ibb, ibb->handle, intel_bb_offset(ibb),
				     handle, read_domains, write_domain,
				     delta, presumed_offset);
}

uint64_t intel_bb_emit_reloc_fenced(struct intel_bb *ibb,
				    uint32_t handle,
				    uint32_t read_domains,
				    uint32_t write_domain,
				    uint64_t delta,
				    uint64_t presumed_offset)
{
	uint64_t address;

	address = intel_bb_emit_reloc(ibb, handle, read_domains, write_domain,
				      delta, presumed_offset);

	intel_bb_object_set_flag(ibb, handle, EXEC_OBJECT_NEEDS_FENCE);

	return address;
}

/**
 * intel_bb_offset_reloc:
 * @ibb: pointer to intel_bb
 * @handle: object handle which address will be taken to patch the bb
 * @read_domains: gem domain bits for the relocation
 * @write_domain: gem domain bit for the relocation
 * @offset: offset within bb to be patched
 * @presumed_offset: address of the object in address space. If -1 is passed
 * then final offset of the object will be randomized (for no-reloc bb) or
 * 0 (for reloc bb, in that case reloc.presumed_offset will be -1). In
 * case address is known it should passed in @presumed_offset (for no-reloc).
 *
 * Function prepares relocation (execobj if required + reloc). It it used
 * for editing batchbuffer via modifying structures. It means when we're
 * preparing batchbuffer it is more descriptive to edit the structure
 * than emitting dwords. But it require for some fields to point the
 * relocation. For that case @offset is passed by the user and it points
 * to the offset in bb where the relocation will be applied.
 */
uint64_t intel_bb_offset_reloc(struct intel_bb *ibb,
			       uint32_t handle,
			       uint32_t read_domains,
			       uint32_t write_domain,
			       uint32_t offset,
			       uint64_t presumed_offset)
{
	igt_assert(ibb);

	return intel_bb_add_reloc(ibb, ibb->handle, handle,
				  read_domains, write_domain,
				  0, offset, presumed_offset);
}

uint64_t intel_bb_offset_reloc_with_delta(struct intel_bb *ibb,
					  uint32_t handle,
					  uint32_t read_domains,
					  uint32_t write_domain,
					  uint32_t delta,
					  uint32_t offset,
					  uint64_t presumed_offset)
{
	igt_assert(ibb);

	return intel_bb_add_reloc(ibb, ibb->handle, handle,
				  read_domains, write_domain,
				  delta, offset, presumed_offset);
}

uint64_t intel_bb_offset_reloc_to_object(struct intel_bb *ibb,
					 uint32_t to_handle,
					 uint32_t handle,
					 uint32_t read_domains,
					 uint32_t write_domain,
					 uint32_t delta,
					 uint32_t offset,
					 uint64_t presumed_offset)
{
	igt_assert(ibb);

	return intel_bb_add_reloc(ibb, to_handle, handle,
				  read_domains, write_domain,
				  delta, offset, presumed_offset);
}

/*
 * @intel_bb_set_pxp:
 * @ibb: pointer to intel_bb
 * @new_state: enable or disable pxp session
 * @apptype: pxp session input identifies what type of session to enable
 * @appid: pxp session input provides which appid to use
 *
 * This function merely stores the pxp state and session information to
 * be retrieved and programmed later by supporting libraries such as
 * gen12_render_copy that must program the HW within the same dispatch
 */
void intel_bb_set_pxp(struct intel_bb *ibb, bool new_state,
		      uint32_t apptype, uint32_t appid)
{
	igt_assert(ibb);

	ibb->pxp.enabled = new_state;
	ibb->pxp.apptype = new_state ? apptype : 0;
	ibb->pxp.appid   = new_state ? appid : 0;
}

static void intel_bb_dump_execbuf(struct intel_bb *ibb,
				  struct drm_i915_gem_execbuffer2 *execbuf)
{
	struct drm_i915_gem_exec_object2 *objects;
	struct drm_i915_gem_relocation_entry *relocs, *reloc;
	int i, j;
	uint64_t address;

	igt_debug("execbuf [pid: %ld, fd: %d, ctx: %u]\n",
		  (long) getpid(), ibb->i915, ibb->ctx);
	igt_debug("execbuf batch len: %u, start offset: 0x%x, "
		  "DR1: 0x%x, DR4: 0x%x, "
		  "num clip: %u, clipptr: 0x%llx, "
		  "flags: 0x%llx, rsvd1: 0x%llx, rsvd2: 0x%llx\n",
		  execbuf->batch_len, execbuf->batch_start_offset,
		  execbuf->DR1, execbuf->DR4,
		  execbuf->num_cliprects, execbuf->cliprects_ptr,
		  execbuf->flags, execbuf->rsvd1, execbuf->rsvd2);

	igt_debug("execbuf buffer_count: %d\n", execbuf->buffer_count);
	for (i = 0; i < execbuf->buffer_count; i++) {
		objects = &((struct drm_i915_gem_exec_object2 *)
			    from_user_pointer(execbuf->buffers_ptr))[i];
		relocs = from_user_pointer(objects->relocs_ptr);
		address = objects->offset;
		igt_debug(" [%d] handle: %u, reloc_count: %d, reloc_ptr: %p, "
			  "align: 0x%llx, offset: 0x%" PRIx64 ", flags: 0x%llx, "
			  "rsvd1: 0x%llx, rsvd2: 0x%llx\n",
			  i, objects->handle, objects->relocation_count,
			  relocs,
			  objects->alignment,
			  address,
			  objects->flags,
			  objects->rsvd1, objects->rsvd2);
		if (objects->relocation_count) {
			igt_debug("\texecbuf relocs:\n");
			for (j = 0; j < objects->relocation_count; j++) {
				reloc = &relocs[j];
				address = reloc->presumed_offset;
				igt_debug("\t [%d] target handle: %u, "
					  "offset: 0x%llx, delta: 0x%x, "
					  "presumed_offset: 0x%" PRIx64 ", "
					  "read_domains: 0x%x, "
					  "write_domain: 0x%x\n",
					  j, reloc->target_handle,
					  reloc->offset, reloc->delta,
					  address,
					  reloc->read_domains,
					  reloc->write_domain);
			}
		}
	}
}

static void intel_bb_dump_base64(struct intel_bb *ibb, int linelen)
{
	int outsize;
	gchar *str, *pos;

	igt_info("--- bb ---\n");
	pos = str = g_base64_encode((const guchar *) ibb->batch, ibb->size);
	outsize = strlen(str);

	while (outsize > 0) {
		igt_info("%.*s\n", min(outsize, linelen), pos);
		pos += linelen;
		outsize -= linelen;
	}

	free(str);
}

static void print_node(const void *node, VISIT which, int depth)
{
	const struct drm_i915_gem_exec_object2 *object =
		*(const struct drm_i915_gem_exec_object2 **) node;
	(void) depth;

	switch (which) {
	case preorder:
	case endorder:
		break;

	case postorder:
	case leaf:
		igt_info("\t handle: %u, offset: 0x%" PRIx64 "\n",
			 object->handle, (uint64_t) object->offset);
		break;
	}
}

void intel_bb_dump_cache(struct intel_bb *ibb)
{
	igt_info("[pid: %ld] dump cache\n", (long) getpid());
	twalk(ibb->root, print_node);
}

static struct drm_i915_gem_exec_object2 *
create_objects_array(struct intel_bb *ibb)
{
	struct drm_i915_gem_exec_object2 *objects;
	uint32_t i;

	objects = malloc(sizeof(*objects) * ibb->num_objects);
	igt_assert(objects);

	for (i = 0; i < ibb->num_objects; i++) {
		objects[i] = *(ibb->objects[i]);
		objects[i].offset = CANONICAL(objects[i].offset);
	}

	return objects;
}

static void update_offsets(struct intel_bb *ibb,
			   struct drm_i915_gem_exec_object2 *objects)
{
	struct drm_i915_gem_exec_object2 *object;
	struct intel_buf *entry;
	uint32_t i;

	for (i = 0; i < ibb->num_objects; i++) {
		object = intel_bb_find_object(ibb, objects[i].handle);
		igt_assert(object);

		object->offset = DECANONICAL(objects[i].offset);

		if (i == 0)
			ibb->batch_offset = object->offset;
	}

	igt_list_for_each_entry(entry, &ibb->intel_bufs, link) {
		object = intel_bb_find_object(ibb, entry->handle);
		igt_assert(object);

		if (ibb->allocator_type == INTEL_ALLOCATOR_SIMPLE)
			igt_assert(object->offset == entry->addr.offset);
		else
			entry->addr.offset = object->offset;

		entry->addr.ctx = ibb->ctx;
	}
}

#define LINELEN 76
/*
 * __intel_bb_exec:
 * @ibb: pointer to intel_bb
 * @end_offset: offset of the last instruction in the bb
 * @flags: flags passed directly to execbuf
 * @sync: if true wait for execbuf completion, otherwise caller is responsible
 * to wait for completion
 *
 * Returns: 0 on success, otherwise errno.
 *
 * Note: In this step execobj for bb is allocated and inserted to the objects
 * array.
*/
int __intel_bb_exec(struct intel_bb *ibb, uint32_t end_offset,
			   uint64_t flags, bool sync)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 *objects;
	int ret, fence, new_fence;

	ibb->objects[0]->relocs_ptr = to_user_pointer(ibb->relocs);
	ibb->objects[0]->relocation_count = ibb->num_relocs;
	ibb->objects[0]->handle = ibb->handle;
	ibb->objects[0]->offset = ibb->batch_offset;

	gem_write(ibb->i915, ibb->handle, 0, ibb->batch, ibb->size);

	memset(&execbuf, 0, sizeof(execbuf));
	objects = create_objects_array(ibb);
	execbuf.buffers_ptr = to_user_pointer(objects);
	execbuf.buffer_count = ibb->num_objects;
	execbuf.batch_len = end_offset;
	execbuf.rsvd1 = ibb->ctx;
	execbuf.flags = flags | I915_EXEC_BATCH_FIRST | I915_EXEC_FENCE_OUT;
	if (ibb->enforce_relocs)
		execbuf.flags &= ~I915_EXEC_NO_RELOC;
	execbuf.rsvd2 = 0;

	if (ibb->dump_base64)
		intel_bb_dump_base64(ibb, LINELEN);

	/* For debugging on CI, remove in final series */
	intel_bb_dump_execbuf(ibb, &execbuf);

	ret = __gem_execbuf_wr(ibb->i915, &execbuf);
	if (ret) {
		intel_bb_dump_execbuf(ibb, &execbuf);
		free(objects);
		return ret;
	}

	/* Update addresses in the cache */
	update_offsets(ibb, objects);

	/* Save/merge fences */
	fence = execbuf.rsvd2 >> 32;

	if (ibb->fence < 0) {
		ibb->fence = fence;
	} else {
		new_fence = sync_fence_merge(ibb->fence, fence);
		close(ibb->fence);
		close(fence);
		ibb->fence = new_fence;
	}

	if (sync || ibb->debug)
		igt_assert(intel_bb_sync(ibb) == 0);

	if (ibb->debug) {
		intel_bb_dump_execbuf(ibb, &execbuf);
		if (intel_bb_debug_tree) {
			igt_info("\nTree:\n");
			twalk(ibb->root, print_node);
		}
	}

	free(objects);

	return 0;
}

/**
 * intel_bb_exec:
 * @ibb: pointer to intel_bb
 * @end_offset: offset of the last instruction in the bb
 * @flags: flags passed directly to execbuf
 * @sync: if true wait for execbuf completion, otherwise caller is responsible
 * to wait for completion
 *
 * Do execbuf on context selected during bb creation. Asserts on failure.
*/
void intel_bb_exec(struct intel_bb *ibb, uint32_t end_offset,
		   uint64_t flags, bool sync)
{
	igt_assert_eq(__intel_bb_exec(ibb, end_offset, flags, sync), 0);
}

/**
 * intel_bb_get_object_address:
 * @ibb: pointer to intel_bb
 * @handle: object handle
 *
 * When objects addresses are previously pinned and we don't want to relocate
 * we need to acquire them from previous execbuf. Function returns previous
 * object offset for @handle or 0 if object is not found.
 */
uint64_t intel_bb_get_object_offset(struct intel_bb *ibb, uint32_t handle)
{
	struct drm_i915_gem_exec_object2 object = { .handle = handle };
	struct drm_i915_gem_exec_object2 **found;

	igt_assert(ibb);

	found = tfind((void *)&object, &ibb->root, __compare_objects);
	if (!found)
		return INTEL_BUF_INVALID_ADDRESS;

	return (*found)->offset;
}

/*
 * intel_bb_emit_bbe:
 * @ibb: batchbuffer
 *
 * Outputs MI_BATCH_BUFFER_END and ensures batch is properly aligned.
 */
uint32_t intel_bb_emit_bbe(struct intel_bb *ibb)
{
	/* Mark the end of the buffer. */
	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 8);

	return intel_bb_offset(ibb);
}

/*
 * intel_bb_emit_flush_common:
 * @ibb: batchbuffer
 *
 * Emits instructions which completes batch buffer.
 *
 * Returns: offset in batch buffer where there's end of instructions.
 */
uint32_t intel_bb_emit_flush_common(struct intel_bb *ibb)
{
	if (intel_bb_offset(ibb) == 0)
		return 0;

	if (ibb->gen == 5) {
		/*
		 * emit gen5 w/a without batch space checks - we reserve that
		 * already.
		 */
		intel_bb_out(ibb, CMD_POLY_STIPPLE_OFFSET << 16);
		intel_bb_out(ibb, 0);
	}

	/* Round batchbuffer usage to 2 DWORDs. */
	if ((intel_bb_offset(ibb) & 4) == 0)
		intel_bb_out(ibb, 0);

	intel_bb_emit_bbe(ibb);

	return intel_bb_offset(ibb);
}

static void intel_bb_exec_with_ring(struct intel_bb *ibb,uint32_t ring)
{
	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      ring | I915_EXEC_NO_RELOC, false);
	intel_bb_reset(ibb, false);
}

/*
 * intel_bb_flush:
 * @ibb: batchbuffer
 * @ring: ring
 *
 * If batch is not empty emit batch buffer end, execute on ring,
 * then reset the batch.
 */
void intel_bb_flush(struct intel_bb *ibb, uint32_t ring)
{
	if (intel_bb_emit_flush_common(ibb) == 0)
		return;

	intel_bb_exec_with_ring(ibb, ring);
}

/*
 * intel_bb_flush_render:
 * @ibb: batchbuffer
 *
 * If batch is not empty emit batch buffer end, find the render engine id,
 * execute on the ring and reset the batch. Context used to execute
 * is batch context.
 */
void intel_bb_flush_render(struct intel_bb *ibb)
{
	uint32_t ring;

	if (intel_bb_emit_flush_common(ibb) == 0)
		return;

	if (has_ctx_cfg(ibb))
		ring = find_engine(ibb->cfg, I915_ENGINE_CLASS_RENDER);
	else
		ring = I915_EXEC_RENDER;

	intel_bb_exec_with_ring(ibb, ring);
}

/*
 * intel_bb_flush_blit:
 * @ibb: batchbuffer
 *
 * If batch is not empty emit batch buffer end, find a suitable ring
 * (depending on gen and context configuration) and reset the batch.
 * Context used to execute is batch context.
 */
void intel_bb_flush_blit(struct intel_bb *ibb)
{
	uint32_t ring;

	if (intel_bb_emit_flush_common(ibb) == 0)
		return;

	if (has_ctx_cfg(ibb))
		ring = find_engine(ibb->cfg, I915_ENGINE_CLASS_COPY);
	else
		ring = HAS_BLT_RING(ibb->devid) ? I915_EXEC_BLT : I915_EXEC_DEFAULT;

	intel_bb_exec_with_ring(ibb, ring);
}

/*
 * intel_bb_copy_data:
 * @ibb: batchbuffer
 * @data: pointer of data which should be copied into batch
 * @bytes: number of bytes to copy, must be dword multiplied
 * @align: alignment in the batch
 *
 * Function copies @bytes of data pointed by @data into batch buffer.
 */
uint32_t intel_bb_copy_data(struct intel_bb *ibb,
			    const void *data, unsigned int bytes,
			    uint32_t align)
{
	uint32_t *subdata, offset;

	igt_assert((bytes & 3) == 0);

	intel_bb_ptr_align(ibb, align);
	offset = intel_bb_offset(ibb);
	igt_assert(offset + bytes < ibb->size);

	subdata = intel_bb_ptr(ibb);
	memcpy(subdata, data, bytes);
	intel_bb_ptr_add(ibb, bytes);

	return offset;
}

/*
 * intel_bb_blit_start:
 * @ibb: batchbuffer
 * @flags: flags to blit command
 *
 * Function emits XY_SRC_COPY_BLT instruction with size appropriate size
 * which depend on gen.
 */
void intel_bb_blit_start(struct intel_bb *ibb, uint32_t flags)
{
	intel_bb_out(ibb, XY_SRC_COPY_BLT_CMD |
		     XY_SRC_COPY_BLT_WRITE_ALPHA |
		     XY_SRC_COPY_BLT_WRITE_RGB |
		     flags |
		     (6 + 2 * (ibb->gen >= 8)));
}

/*
 * intel_bb_emit_blt_copy:
 * @ibb: batchbuffer
 * @src: source buffer (intel_buf)
 * @src_x1: source x1 position
 * @src_y1: source y1 position
 * @src_pitch: source pitch
 * @dst: destination buffer (intel_buf)
 * @dst_x1: destination x1 position
 * @dst_y1: destination y1 position
 * @dst_pitch: destination pitch
 * @width: width of data to copy
 * @height: height of data to copy
 *
 * Function emits complete blit command.
 */
void intel_bb_emit_blt_copy(struct intel_bb *ibb,
			    struct intel_buf *src,
			    int src_x1, int src_y1, int src_pitch,
			    struct intel_buf *dst,
			    int dst_x1, int dst_y1, int dst_pitch,
			    int width, int height, int bpp)
{
	const unsigned int gen = ibb->gen;
	uint32_t cmd_bits = 0;
	uint32_t br13_bits;
	uint32_t mask;

	igt_assert(bpp*(src_x1 + width) <= 8*src_pitch);
	igt_assert(bpp*(dst_x1 + width) <= 8*dst_pitch);
	igt_assert(src_pitch * (src_y1 + height) <= src->surface[0].size);
	igt_assert(dst_pitch * (dst_y1 + height) <= dst->surface[0].size);

	if (gen >= 4 && src->tiling != I915_TILING_NONE) {
		src_pitch /= 4;
		cmd_bits |= XY_SRC_COPY_BLT_SRC_TILED;
	}

	if (gen >= 4 && dst->tiling != I915_TILING_NONE) {
		dst_pitch /= 4;
		cmd_bits |= XY_SRC_COPY_BLT_DST_TILED;
	}

	CHECK_RANGE(src_x1); CHECK_RANGE(src_y1);
	CHECK_RANGE(dst_x1); CHECK_RANGE(dst_y1);
	CHECK_RANGE(width); CHECK_RANGE(height);
	CHECK_RANGE(src_x1 + width); CHECK_RANGE(src_y1 + height);
	CHECK_RANGE(dst_x1 + width); CHECK_RANGE(dst_y1 + height);
	CHECK_RANGE(src_pitch); CHECK_RANGE(dst_pitch);

	br13_bits = 0;
	switch (bpp) {
	case 8:
		break;
	case 16:		/* supporting only RGB565, not ARGB1555 */
		br13_bits |= 1 << 24;
		break;
	case 32:
		br13_bits |= 3 << 24;
		cmd_bits |= (XY_SRC_COPY_BLT_WRITE_ALPHA |
			     XY_SRC_COPY_BLT_WRITE_RGB);
		break;
	default:
		igt_fail(IGT_EXIT_FAILURE);
	}

	if ((src->tiling | dst->tiling) >= I915_TILING_Y) {
		intel_bb_out(ibb, MI_LOAD_REGISTER_IMM);
		intel_bb_out(ibb, BCS_SWCTRL);

		mask = (BCS_SRC_Y | BCS_DST_Y) << 16;
		if (src->tiling == I915_TILING_Y)
			mask |= BCS_SRC_Y;
		if (dst->tiling == I915_TILING_Y)
			mask |= BCS_DST_Y;
		intel_bb_out(ibb, mask);
	}

	intel_bb_add_intel_buf(ibb, src, false);
	intel_bb_add_intel_buf(ibb, dst, true);

	intel_bb_blit_start(ibb, cmd_bits);
	intel_bb_out(ibb, (br13_bits) |
		     (0xcc << 16) | /* copy ROP */
		     dst_pitch);
	intel_bb_out(ibb, (dst_y1 << 16) | dst_x1); /* dst x1,y1 */
	intel_bb_out(ibb, ((dst_y1 + height) << 16) | (dst_x1 + width)); /* dst x2,y2 */
	intel_bb_emit_reloc_fenced(ibb, dst->handle,
				   I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER,
				   0, dst->addr.offset);
	intel_bb_out(ibb, (src_y1 << 16) | src_x1); /* src x1,y1 */
	intel_bb_out(ibb, src_pitch);
	intel_bb_emit_reloc_fenced(ibb, src->handle,
				   I915_GEM_DOMAIN_RENDER, 0,
				   0, src->addr.offset);

	if (gen >= 6 && src->handle == dst->handle) {
		intel_bb_out(ibb, XY_SETUP_CLIP_BLT_CMD);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);
	}

	if ((src->tiling | dst->tiling) >= I915_TILING_Y) {
		igt_assert(ibb->gen >= 6);
		intel_bb_out(ibb, MI_FLUSH_DW | 2);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);

		intel_bb_out(ibb, MI_LOAD_REGISTER_IMM);
		intel_bb_out(ibb, BCS_SWCTRL);
		intel_bb_out(ibb, (BCS_SRC_Y | BCS_DST_Y) << 16);
	}
}

void intel_bb_blt_copy(struct intel_bb *ibb,
		       struct intel_buf *src,
		       int src_x1, int src_y1, int src_pitch,
		       struct intel_buf *dst,
		       int dst_x1, int dst_y1, int dst_pitch,
		       int width, int height, int bpp)
{
	intel_bb_emit_blt_copy(ibb, src, src_x1, src_y1, src_pitch,
			       dst, dst_x1, dst_y1, dst_pitch,
			       width, height, bpp);
	intel_bb_flush_blit(ibb);
}

/**
 * intel_bb_copy_intel_buf:
 * @batch: batchbuffer object
 * @src: source buffer (intel_buf)
 * @dst: destination libdrm buffer object
 * @size: size of the copy range in bytes
 *
 * Emits a copy operation using blitter commands into the supplied batch.
 * A total of @size bytes from the start of @src is copied
 * over to @dst. Note that @size must be page-aligned.
 */
void intel_bb_copy_intel_buf(struct intel_bb *ibb,
			     struct intel_buf *src, struct intel_buf *dst,
			     long int size)
{
	igt_assert(size % 4096 == 0);

	intel_bb_blt_copy(ibb,
			  src, 0, 0, 4096,
			  dst, 0, 0, 4096,
			  4096/4, size/4096, 32);
}

/**
 * igt_get_huc_copyfunc:
 * @devid: pci device id
 *
 * Returns:
 *
 * The platform-specific huc copy function pointer for the device specified
 * with @devid. Will return NULL when no media spin function is implemented.
 */
igt_huc_copyfunc_t igt_get_huc_copyfunc(int devid)
{
	igt_huc_copyfunc_t copy = NULL;

	if (IS_GEN12(devid) || IS_GEN11(devid) || IS_GEN9(devid))
		copy = gen9_huc_copyfunc;

	return copy;
}

/**
 * intel_bb_track:
 * @do_tracking: bool
 *
 * Turn on (true) or off (false) tracking for intel_batchbuffers.
 */
void intel_bb_track(bool do_tracking)
{
	if (intel_bb_do_tracking == do_tracking)
		return;

	if (intel_bb_do_tracking) {
		struct intel_bb *entry, *tmp;

		pthread_mutex_lock(&intel_bb_list_lock);
		igt_list_for_each_entry_safe(entry, tmp, &intel_bb_list, link)
			igt_list_del(&entry->link);
		pthread_mutex_unlock(&intel_bb_list_lock);
	}

	intel_bb_do_tracking = do_tracking;
}

static void __intel_bb_reinit_alloc(struct intel_bb *ibb)
{
	if (ibb->allocator_type == INTEL_ALLOCATOR_NONE)
		return;

	ibb->allocator_handle = intel_allocator_open_full(ibb->i915, ibb->ctx,
							  ibb->allocator_start, ibb->allocator_end,
							  ibb->allocator_type,
							  ibb->allocator_strategy,
							  0);
	intel_bb_reset(ibb, true);
}

/**
 * intel_bb_reinit_allocator:
 *
 * Reinit allocator and get offsets in tracked intel_batchbuffers.
 */
void intel_bb_reinit_allocator(void)
{
	struct intel_bb *iter;

	if (!intel_bb_do_tracking)
		return;

	pthread_mutex_lock(&intel_bb_list_lock);
	igt_list_for_each_entry(iter, &intel_bb_list, link)
		__intel_bb_reinit_alloc(iter);
	pthread_mutex_unlock(&intel_bb_list_lock);
}
