/*
 * Copyright © 2018 Intel Corporation
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

#include <i915_drm.h>

#include "media_fill.h"
#include "gen7_media.h"
#include "gen8_media.h"
#include "intel_reg.h"
#include "drmtest.h"
#include "gpu_cmds.h"
#include <assert.h>

static const uint32_t gen7_media_kernel[][4] = {
	{ 0x00400001, 0x20200231, 0x00000020, 0x00000000 },
	{ 0x00600001, 0x20800021, 0x008d0000, 0x00000000 },
	{ 0x00200001, 0x20800021, 0x00450040, 0x00000000 },
	{ 0x00000001, 0x20880061, 0x00000000, 0x000f000f },
	{ 0x00800001, 0x20a00021, 0x00000020, 0x00000000 },
	{ 0x00800001, 0x20e00021, 0x00000020, 0x00000000 },
	{ 0x00800001, 0x21200021, 0x00000020, 0x00000000 },
	{ 0x00800001, 0x21600021, 0x00000020, 0x00000000 },
	{ 0x05800031, 0x24001ca8, 0x00000080, 0x120a8000 },
	{ 0x00600001, 0x2e000021, 0x008d0000, 0x00000000 },
	{ 0x07800031, 0x20001ca8, 0x00000e00, 0x82000010 },
};

static const uint32_t gen8_media_kernel[][4] = {
	{ 0x00400001, 0x20202288, 0x00000020, 0x00000000 },
	{ 0x00600001, 0x20800208, 0x008d0000, 0x00000000 },
	{ 0x00200001, 0x20800208, 0x00450040, 0x00000000 },
	{ 0x00000001, 0x20880608, 0x00000000, 0x000f000f },
	{ 0x00800001, 0x20a00208, 0x00000020, 0x00000000 },
	{ 0x00800001, 0x20e00208, 0x00000020, 0x00000000 },
	{ 0x00800001, 0x21200208, 0x00000020, 0x00000000 },
	{ 0x00800001, 0x21600208, 0x00000020, 0x00000000 },
	{ 0x0c800031, 0x24000a40, 0x0e000080, 0x120a8000 },
	{ 0x00600001, 0x2e000208, 0x008d0000, 0x00000000 },
	{ 0x07800031, 0x20000a40, 0x0e000e00, 0x82000010 },
};

static const uint32_t gen11_media_vme_kernel[][4] = {
	{ 0x00600001, 0x20302e68,  0x00000000,  0x20000000 },
	{ 0x00600001, 0x22802e68,  0x00000000,  0x00000001 },
	{ 0x00000001, 0x20284f2c,  0x00000000,  0x3818000c },
	{ 0x00600001, 0x22902e68,  0x00000000,  0x00000010 },
	{ 0x00600001, 0x22a02e68,  0x00000000,  0x00010000 },
	{ 0x00000001, 0x202c4f2c,  0x00000000,  0x22222222 },
	{ 0x00000040, 0x22000a20,  0x0e000020,  0x10782000 },
	{ 0x00600001, 0x20404f28,  0x00000000,  0x00000000 },
	{ 0x00600001, 0x20a04f28,  0x00000000,  0x00000000 },
	{ 0x00600001, 0x20c04f28,  0x00000000,  0x00000000 },
	{ 0x00600001, 0x21204f28,  0x00000000,  0x00000000 },
	{ 0x00600001, 0x20601a28,  0x008d0030,  0x00000000 },
	{ 0x00600041, 0x20800a28,  0x1a000028,  0x008d0280 },
	{ 0x00600041, 0x20e01a28,  0x1e8d0290,  0x01000100 },
	{ 0x00600041, 0x21000a28,  0x1a00002c,  0x008d02a0 },
	{ 0x00000001, 0x22284f2c,  0x00000000,  0x00000000 },
	{ 0x0d80c031, 0x21404a48,  0x00000040,  0x00000200 },
	{ 0x00000001, 0x215c4708,  0x00000000,  0xbeefbeef },
	{ 0x00000040, 0x22000204,  0x06000024,  0x020a0400 },
	{ 0x00000001, 0x215e4708,  0x00000000,  0xdeaddead },
	{ 0x00000001, 0x22484f2c,  0x00000000,  0x00000008 },
	{ 0x00000001, 0x22684f2c,  0x00000000,  0x0000000c },
	{ 0x00600001, 0x2fe04b2c,  0x008d0000,  0x00000000 },
	{ 0x0a800033, 0x0000a054,  0x00002224,  0x00000000 },
	{ 0x00000040, 0x22000204,  0x06000024,  0x020a0300 },
	{ 0x0a800033, 0x0000e054,  0x00002242,  0x00000000 },
	{ 0x00000040, 0x22000204,  0x06000024,  0x020a0200 },
	{ 0x0a600033, 0x00010014,  0x00002261,  0x00000000 },
	{ 0x07600031, 0x20004a04,  0x06000fe0,  0x82000010 },
	{ 0x00000000, 0x00000000,  0x00000000,  0x00000000 },
	{ 0x00000000, 0x00000000,  0x00000000,  0x00000000 },
	{ 0x00000000, 0x00000000,  0x00000000,  0x00000000 },
	{ 0x00000000, 0x00000000,  0x00000000,  0x00000000 },
	{ 0x00000000, 0x00000000,  0x00000000,  0x00000000 },
	{ 0x00000000, 0x00000000,  0x00000000,  0x00000000 },
	{ 0x00000000, 0x00000000,  0x00000000,  0x00000000 },
	{ 0x00000000, 0x00000000,  0x00000000,  0x00000000 },
};

static const uint32_t gen12_media_kernel[][4] = {
	{ 0x00020061, 0x01050000, 0x00000104, 0x00000000 },
	{ 0x00030061, 0x04050220, 0x00460005, 0x00000000 },
	{ 0x00030061, 0x04050220, 0x00220205, 0x00000000 },
	{ 0x00000061, 0x04454220, 0x00000000, 0x000f000f },
	{ 0x00040461, 0x05050220, 0x00000104, 0x00000000 },
	{ 0x00040561, 0x07050220, 0x00000104, 0x00000000 },
	{ 0x00040661, 0x09050220, 0x00000104, 0x00000000 },
	{ 0x00040761, 0x0b050220, 0x00000104, 0x00000000 },
	{ 0x00049031, 0x00000000, 0xc000044c, 0x12a00000 },
	{ 0x00030061, 0x70050220, 0x00460005, 0x00000000 },
	{ 0x00040131, 0x00000004, 0x7020700c, 0x10000000 },
};

/*
 * This sets up the media pipeline,
 *
 * +---------------+ <---- 4096
 * |       ^       |
 * |       |       |
 * |    various    |
 * |      state    |
 * |       |       |
 * |_______|_______| <---- 2048 + ?
 * |       ^       |
 * |       |       |
 * |   batch       |
 * |    commands   |
 * |       |       |
 * |       |       |
 * +---------------+ <---- 0 + ?
 *
 */

#define PAGE_SIZE 4096
#define BATCH_STATE_SPLIT 2048
/* VFE STATE params */
#define THREADS 1
#define MEDIA_URB_ENTRIES 2
#define MEDIA_URB_SIZE 2
#define MEDIA_CURBE_SIZE 2
#define GEN7_VFE_STATE_MEDIA_MODE 0

void
gen7_media_fillfunc(int i915,
		    struct intel_buf *buf,
		    unsigned int x, unsigned int y,
		    unsigned int width, unsigned int height,
		    uint8_t color)
{
	struct intel_bb *ibb;
	uint32_t curbe_buffer, interface_descriptor;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	intel_bb_add_intel_buf(ibb, buf, true);

	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	curbe_buffer = gen7_fill_curbe_buffer_data(ibb, color);
	interface_descriptor = gen7_fill_interface_descriptor(ibb, buf,
					gen7_media_kernel,
					sizeof(gen7_media_kernel));
	intel_bb_ptr_set(ibb, 0);

	/* media pipeline */
	intel_bb_out(ibb, GEN7_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA);
	gen7_emit_state_base_address(ibb);

	gen7_emit_vfe_state(ibb, THREADS, MEDIA_URB_ENTRIES, MEDIA_URB_SIZE,
			    MEDIA_CURBE_SIZE, GEN7_VFE_STATE_MEDIA_MODE);

	gen7_emit_curbe_load(ibb, curbe_buffer);

	gen7_emit_interface_descriptor_load(ibb, interface_descriptor);

	gen7_emit_media_objects(ibb, x, y, width, height);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 32);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);

	intel_bb_destroy(ibb);
}

void
gen8_media_fillfunc(int i915,
		    struct intel_buf *buf,
		    unsigned int x, unsigned int y,
		    unsigned int width, unsigned int height,
		    uint8_t color)
{
	struct intel_bb *ibb;
	uint32_t curbe_buffer, interface_descriptor;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	intel_bb_add_intel_buf(ibb, buf, true);

	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	curbe_buffer = gen7_fill_curbe_buffer_data(ibb, color);
	interface_descriptor = gen8_fill_interface_descriptor(ibb, buf,
					gen8_media_kernel,
					sizeof(gen8_media_kernel));
	intel_bb_ptr_set(ibb, 0);

	/* media pipeline */
	intel_bb_out(ibb, GEN8_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA);
	gen8_emit_state_base_address(ibb);

	gen8_emit_vfe_state(ibb, THREADS, MEDIA_URB_ENTRIES, MEDIA_URB_SIZE,
			    MEDIA_CURBE_SIZE);

	gen7_emit_curbe_load(ibb, curbe_buffer);

	gen7_emit_interface_descriptor_load(ibb, interface_descriptor);

	gen7_emit_media_objects(ibb, x, y, width, height);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 32);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);

	intel_bb_destroy(ibb);
}

static void
__gen9_media_fillfunc(int i915,
		      struct intel_buf *buf,
		      unsigned int x, unsigned int y,
		      unsigned int width, unsigned int height,
		      uint8_t color,
		      const uint32_t kernel[][4], size_t kernel_size)
{
	struct intel_bb *ibb;
	uint32_t curbe_buffer, interface_descriptor;

	ibb = intel_bb_create(i915, PAGE_SIZE);
	intel_bb_add_intel_buf(ibb, buf, true);

	/* setup states */
	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	curbe_buffer = gen7_fill_curbe_buffer_data(ibb, color);
	interface_descriptor = gen8_fill_interface_descriptor(ibb, buf,
							      kernel,
							      kernel_size);
	intel_bb_ptr_set(ibb, 0);

	/* media pipeline */
	intel_bb_out(ibb, GEN8_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA |
		     GEN9_FORCE_MEDIA_AWAKE_ENABLE |
		     GEN9_SAMPLER_DOP_GATE_DISABLE |
		     GEN9_PIPELINE_SELECTION_MASK |
		     GEN9_SAMPLER_DOP_GATE_MASK |
		     GEN9_FORCE_MEDIA_AWAKE_MASK);
	gen9_emit_state_base_address(ibb);

	gen8_emit_vfe_state(ibb, THREADS, MEDIA_URB_ENTRIES, MEDIA_URB_SIZE,
			    MEDIA_CURBE_SIZE);

	gen7_emit_curbe_load(ibb, curbe_buffer);

	gen7_emit_interface_descriptor_load(ibb, interface_descriptor);

	gen7_emit_media_objects(ibb, x, y, width, height);

	intel_bb_out(ibb, GEN8_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA |
		     GEN9_FORCE_MEDIA_AWAKE_DISABLE |
		     GEN9_SAMPLER_DOP_GATE_ENABLE |
		     GEN9_PIPELINE_SELECTION_MASK |
		     GEN9_SAMPLER_DOP_GATE_MASK |
		     GEN9_FORCE_MEDIA_AWAKE_MASK);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 32);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, true);

	intel_bb_destroy(ibb);
}

void
gen9_media_fillfunc(int i915,
		    struct intel_buf *buf,
		    unsigned int x, unsigned int y,
		    unsigned int width, unsigned int height,
		    uint8_t color)
{
	__gen9_media_fillfunc(i915, buf, x, y, width, height, color,
			      gen8_media_kernel, sizeof(gen8_media_kernel));
}

static void
__gen11_media_vme_func(int i915,
		       uint32_t ctx,
		       struct intel_buf *src,
		       unsigned int width, unsigned int height,
		       struct intel_buf *dst,
		       const uint32_t kernel[][4],
		       size_t kernel_size)
{
	struct intel_bb *ibb;
	uint32_t curbe_buffer, interface_descriptor;

	ibb = intel_bb_create_with_context(i915, ctx, NULL, PAGE_SIZE);
	intel_bb_add_intel_buf(ibb, dst, true);
	intel_bb_add_intel_buf(ibb, src, false);

	/* setup states */
	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	curbe_buffer = gen11_fill_curbe_buffer_data(ibb);
	interface_descriptor = gen11_fill_interface_descriptor(ibb, src, dst,
					kernel, kernel_size);

	intel_bb_ptr_set(ibb, 0);

	/* media pipeline */
	intel_bb_out(ibb, GEN8_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA |
		     GEN9_FORCE_MEDIA_AWAKE_ENABLE |
		     GEN9_SAMPLER_DOP_GATE_DISABLE |
		     GEN9_PIPELINE_SELECTION_MASK |
		     GEN9_SAMPLER_DOP_GATE_MASK |
		     GEN9_FORCE_MEDIA_AWAKE_MASK);
	gen9_emit_state_base_address(ibb);

	gen8_emit_vfe_state(ibb, THREADS, MEDIA_URB_ENTRIES, MEDIA_URB_SIZE,
			    MEDIA_CURBE_SIZE);

	gen7_emit_curbe_load(ibb, curbe_buffer);

	gen7_emit_interface_descriptor_load(ibb, interface_descriptor);

	gen7_emit_media_objects(ibb, 0, 0, width, height);

	intel_bb_out(ibb, GEN8_PIPELINE_SELECT | PIPELINE_SELECT_MEDIA |
		     GEN9_FORCE_MEDIA_AWAKE_DISABLE |
		     GEN9_SAMPLER_DOP_GATE_ENABLE |
		     GEN9_PIPELINE_SELECTION_MASK |
		     GEN9_SAMPLER_DOP_GATE_MASK |
		     GEN9_FORCE_MEDIA_AWAKE_MASK);

	intel_bb_out(ibb, MI_BATCH_BUFFER_END);
	intel_bb_ptr_align(ibb, 32);

	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_DEFAULT | I915_EXEC_NO_RELOC, false);
	intel_bb_destroy(ibb);
}

void
gen11_media_vme_func(int i915,
		     uint32_t ctx,
		     struct intel_buf *src,
		     unsigned int width, unsigned int height,
		     struct intel_buf *dst)
{
	__gen11_media_vme_func(i915, ctx,
			       src,
			       width, height,
			       dst,
			       gen11_media_vme_kernel,
			       sizeof(gen11_media_vme_kernel));
}

void
gen12_media_fillfunc(int i915,
		     struct intel_buf *buf,
		     unsigned int x, unsigned int y,
		     unsigned int width, unsigned int height,
		     uint8_t color)
{
	__gen9_media_fillfunc(i915, buf, x, y, width, height, color,
			      gen12_media_kernel, sizeof(gen12_media_kernel));
}
