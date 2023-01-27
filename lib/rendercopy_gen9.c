#include <assert.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <getopt.h>

#include <drm.h>
#include <i915_drm.h>

#include "drmtest.h"
#include "intel_aux_pgtable.h"
#include "intel_bufops.h"
#include "intel_batchbuffer.h"
#include "intel_io.h"
#include "rendercopy.h"
#include "gen9_render.h"
#include "intel_reg.h"
#include "igt_aux.h"
#include "intel_chipset.h"

#define VERTEX_SIZE (3*4)

#if DEBUG_RENDERCPY
static void dump_batch(struct intel_bb *ibb)
{
	intel_bb_dump(ibb, "/tmp/gen9-batchbuffers.dump");
}
#else
#define dump_batch(x) do { } while(0)
#endif

static struct {
	uint32_t cc_state;
	uint32_t blend_state;
} cc;

static struct {
	uint32_t cc_state;
	uint32_t sf_clip_state;
} viewport;

/* see lib/i915/shaders/ps/blit.g7a */
static const uint32_t ps_kernel_gen9[][4] = {
#if 1
	{ 0x0080005a, 0x2f403ae8, 0x3a0000c0, 0x008d0040 },
	{ 0x0080005a, 0x2f803ae8, 0x3a0000d0, 0x008d0040 },
	{ 0x02800031, 0x2e203a48, 0x0e8d0f40, 0x08840001 },
	{ 0x05800031, 0x20003a40, 0x0e8d0e20, 0x90031000 },
#else
	/* Write all -1 */
	{ 0x00600001, 0x2e000608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2e200608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2e400608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2e600608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2e800608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2ea00608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2ec00608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2ee00608, 0x00000000, 0x3f800000 },
	{ 0x05800031, 0x200022e0, 0x0e000e00, 0x90031000 },
#endif
};

/* see lib/i915/shaders/ps/blit.g11a */
static const uint32_t ps_kernel_gen11[][4] = {
#if 1
	{ 0x0060005b, 0x2000c01c, 0x07206601, 0x01800404 },
	{ 0x0060005b, 0x7100480c, 0x0722003b, 0x01880406 },
	{ 0x0060005b, 0x2000c01c, 0x07206601, 0x01800408 },
	{ 0x0060005b, 0x7200480c, 0x0722003b, 0x0188040a },
	{ 0x0060005b, 0x2000c01c, 0x07206e01, 0x01a00404 },
	{ 0x0060005b, 0x7300480c, 0x0722003b, 0x01a80406 },
	{ 0x0060005b, 0x2000c01c, 0x07206e01, 0x01a00408 },
	{ 0x0060005b, 0x7400480c, 0x0722003b, 0x01a8040a },
	{ 0x02800031, 0x21804a4c, 0x06000e20, 0x08840001 },
	{ 0x00800001, 0x2e204b28, 0x008d0180, 0x00000000 },
	{ 0x00800001, 0x2e604b28, 0x008d01c0, 0x00000000 },
	{ 0x00800001, 0x2ea04b28, 0x008d0200, 0x00000000 },
	{ 0x00800001, 0x2ee04b28, 0x008d0240, 0x00000000 },
	{ 0x05800031, 0x20004a44, 0x06000e20, 0x90031000 },
#else
	/* Write all -1 */
	{ 0x00600001, 0x2e000608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2e200608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2e400608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2e600608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2e800608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2ea00608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2ec00608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2ee00608, 0x00000000, 0x3f800000 },
	{ 0x05800031, 0x200022e0, 0x0e000e00, 0x90031000 },
#endif
};

/* see lib/i915/shaders/ps/gen12_render_copy.asm */
static const uint32_t gen12_render_copy[][4] = {
	{ 0x8003005b, 0x200002f0, 0x0a0a0664, 0x06040205 },
	{ 0x8003005b, 0x71040fa8, 0x0a0a2001, 0x06240305 },
	{ 0x8003005b, 0x200002f0, 0x0a0a0664, 0x06040405 },
	{ 0x8003005b, 0x72040fa8, 0x0a0a2001, 0x06240505 },
	{ 0x8003005b, 0x200002f0, 0x0a0a06e4, 0x06840205 },
	{ 0x8003005b, 0x73040fa8, 0x0a0a2001, 0x06a40305 },
	{ 0x8003005b, 0x200002f0, 0x0a0a06e4, 0x06840405 },
	{ 0x8003005b, 0x74040fa8, 0x0a0a2001, 0x06a40505 },
	{ 0x80049031, 0x0c440000, 0x20027124, 0x01000000 },
	{ 0x00042061, 0x71050aa0, 0x00460c05, 0x00000000 },
	{ 0x00040061, 0x73050aa0, 0x00460e05, 0x00000000 },
	{ 0x00040061, 0x75050aa0, 0x00461005, 0x00000000 },
	{ 0x00040061, 0x77050aa0, 0x00461205, 0x00000000 },
	{ 0x80040131, 0x00000004, 0x50007144, 0x00c40000 },
};

/* see lib/i915/shaders/ps/gen12p71_render_copy.asm */
static const uint32_t gen12p71_render_copy[][4] = {
	{ 0x8003005b, 0x200002a0, 0x0a0a0664, 0x06040205 },
	{ 0x8003005b, 0x71040aa8, 0x0a0a2001, 0x06240305 },
	{ 0x8003005b, 0x200002a0, 0x0a0a0664, 0x06040405 },
	{ 0x8003005b, 0x72040aa8, 0x0a0a2001, 0x06240505 },
	{ 0x8003005b, 0x200002a0, 0x0a0a06e4, 0x06840205 },
	{ 0x8003005b, 0x73040aa8, 0x0a0a2001, 0x06a40305 },
	{ 0x8003005b, 0x200002a0, 0x0a0a06e4, 0x06840405 },
	{ 0x8003005b, 0x74040aa8, 0x0a0a2001, 0x06a40505 },
	{ 0x80031101, 0x00010000, 0x00000000, 0x00000000 },
	{ 0x80044031, 0x0c440000, 0x20027124, 0x01000000 },
	{ 0x00042061, 0x71050aa0, 0x00460c05, 0x00000000 },
	{ 0x00040061, 0x73050aa0, 0x00460e05, 0x00000000 },
	{ 0x00040061, 0x75050aa0, 0x00461005, 0x00000000 },
	{ 0x00040061, 0x77050aa0, 0x00461205, 0x00000000 },
	{ 0x80041131, 0x00000004, 0x50007144, 0x00c40000 },
};

/*
 * Gen >= 12 onwards don't have a setting for PTE,
 * so using I915_MOCS_PTE as mocs index may lead to
 * some undefined MOCS behavior.
 * Correct MOCS index should be referred from BSpec
 * and programmed accordingly.
 * This helper function is providing appropriate UC index.
 */
static uint8_t
intel_get_uc_mocs(int fd) {

	uint16_t devid = intel_get_drm_devid(fd);
	uint8_t  uc_index;

	if (IS_DG1(devid))
		uc_index = 1;
	else if (IS_GEN12(devid))
		uc_index = 3;
	else
		uc_index = I915_MOCS_PTE;

	/*
	 * BitField [6:1] represents index to MOCS Tables
	 * BitField [0] represents Encryption/Decryption
	 */
	return uc_index << 1;
}

/* Mostly copy+paste from gen6, except height, width, pitch moved */
static uint32_t
gen8_bind_buf(struct intel_bb *ibb, const struct intel_buf *buf, int is_dst,
	      bool fast_clear) {
	struct gen9_surface_state *ss;
	uint32_t write_domain, read_domain;
	uint64_t address;
	int i915 = buf_ops_get_fd(buf->bops);

	igt_assert_lte(buf->surface[0].stride, 256*1024);
	igt_assert_lte(intel_buf_width(buf), 16384);
	igt_assert_lte(intel_buf_height(buf), 16384);

	if (is_dst) {
		write_domain = read_domain = I915_GEM_DOMAIN_RENDER;
	} else {
		write_domain = 0;
		read_domain = I915_GEM_DOMAIN_SAMPLER;
	}

	ss = intel_bb_ptr_align(ibb, 64);

	ss->ss0.surface_type = SURFACE_2D;
	switch (buf->bpp) {
		case 8: ss->ss0.surface_format = SURFACEFORMAT_R8_UNORM; break;
		case 16: ss->ss0.surface_format = SURFACEFORMAT_R8G8_UNORM; break;
		case 32: ss->ss0.surface_format = SURFACEFORMAT_B8G8R8A8_UNORM; break;
		case 64: ss->ss0.surface_format = SURFACEFORMAT_R16G16B16A16_FLOAT; break;
		default: igt_assert(0);
	}
	ss->ss0.vertical_alignment = 1; /* align 4 */
	ss->ss0.horizontal_alignment = 1; /* align 4 or HALIGN_32 on display ver >= 13*/

	if (HAS_4TILE(ibb->devid)) {
		/*
		 * mocs table version 1 index 3 groub wb use l3
		 */
		ss->ss1.memory_object_control = 3 << 1;
		ss->ss5.mip_tail_start_lod = 0;
	} else {
		ss->ss0.render_cache_read_write = 1;
		ss->ss1.memory_object_control = intel_get_uc_mocs(i915);
		ss->ss5.mip_tail_start_lod = 1; /* needed with trmode */
	}

	if (buf->tiling == I915_TILING_X)
		ss->ss0.tiled_mode = 2;
	else if (buf->tiling != I915_TILING_NONE)
		ss->ss0.tiled_mode = 3;

	if (intel_buf_pxp(buf))
		ss->ss1.memory_object_control |= 1;

	if (buf->tiling == I915_TILING_Yf)
		ss->ss5.trmode = 1;
	else if (buf->tiling == I915_TILING_Ys)
		ss->ss5.trmode = 2;

	address = intel_bb_offset_reloc(ibb, buf->handle,
					read_domain, write_domain,
					intel_bb_offset(ibb) + 4 * 8,
					buf->addr.offset);
	ss->ss8.base_addr = address;
	ss->ss9.base_addr_hi = address >> 32;

	ss->ss2.height = intel_buf_height(buf) - 1;
	ss->ss2.width  = intel_buf_width(buf) - 1;
	ss->ss3.pitch  = buf->surface[0].stride - 1;

	ss->ss7.skl.shader_chanel_select_r = 4;
	ss->ss7.skl.shader_chanel_select_g = 5;
	ss->ss7.skl.shader_chanel_select_b = 6;
	ss->ss7.skl.shader_chanel_select_a = 7;

	if (buf->compression == I915_COMPRESSION_MEDIA)
		ss->ss7.tgl.media_compression = 1;
	else if (buf->compression == I915_COMPRESSION_RENDER) {
		ss->ss6.aux_mode = 0x5; /* AUX_CCS_E */

		if (buf->ccs[0].stride) {

			ss->ss6.aux_pitch = (buf->ccs[0].stride / 128) - 1;

			address = intel_bb_offset_reloc_with_delta(ibb, buf->handle,
								   read_domain, write_domain,
								   (buf->cc.offset ? (1 << 10) : 0)
								   | buf->ccs[0].offset,
								   intel_bb_offset(ibb) + 4 * 10,
								   buf->addr.offset);
			ss->ss10.aux_base_addr = (address + buf->ccs[0].offset) >> 12;
			ss->ss11.aux_base_addr_hi = (address + buf->ccs[0].offset) >> 32;
		}

		if (fast_clear || (buf->cc.offset && !HAS_FLATCCS(ibb->devid))) {
			igt_assert(buf->compression == I915_COMPRESSION_RENDER);

			ss->ss10.clearvalue_addr_enable = 1;

			address = intel_bb_offset_reloc_with_delta(ibb, buf->handle,
								   read_domain, write_domain,
								   buf->cc.offset,
								   intel_bb_offset(ibb) + 4 * 12,
								   buf->addr.offset);

			/*
			 * If this assert doesn't hold below clear address will be
			 * written wrong.
			 */

			igt_assert(__builtin_ctzl(address + buf->cc.offset) >= 6 &&
				   (__builtin_clzl(address + buf->cc.offset) >= 16));

			ss->ss12.clear_address = (address + buf->cc.offset) >> 6;
			ss->ss13.clear_address_hi = (address + buf->cc.offset) >> 32;
		} else if (HAS_FLATCCS(ibb->devid)) {
			ss->ss7.dg2.memory_compression_type = 0;
			ss->ss7.dg2.memory_compression_enable = 0;
			ss->ss7.dg2.disable_support_for_multi_gpu_partial_writes = 1;
			ss->ss7.dg2.disable_support_for_multi_gpu_atomics = 1;

			/*
			 * For now here is coming only 32bpp rgb format
			 * which is marked below as B8G8R8X8_UNORM = '8'
			 * If here ever arrive other formats below need to be
			 * fixed to take that into account.
			 */
			ss->ss12.compression_format = 8;
		}
	}

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*ss));
}

static uint32_t
gen8_bind_surfaces(struct intel_bb *ibb,
		   const struct intel_buf *src,
		   const struct intel_buf *dst)
{
	uint32_t *binding_table, binding_table_offset;
	bool fast_clear = !src;

	binding_table = intel_bb_ptr_align(ibb, 32);
	binding_table_offset = intel_bb_ptr_add_return_prev_offset(ibb, 32);

	binding_table[0] = gen8_bind_buf(ibb, dst, 1, fast_clear);

	if (src != NULL)
		binding_table[1] = gen8_bind_buf(ibb, src, 0, false);

	return binding_table_offset;
}

/* Mostly copy+paste from gen6, except wrap modes moved */
static uint32_t
gen8_create_sampler(struct intel_bb *ibb) {
	struct gen8_sampler_state *ss;

	ss = intel_bb_ptr_align(ibb, 64);

	ss->ss0.min_filter = GEN4_MAPFILTER_NEAREST;
	ss->ss0.mag_filter = GEN4_MAPFILTER_NEAREST;
	ss->ss3.r_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;
	ss->ss3.s_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;
	ss->ss3.t_wrap_mode = GEN4_TEXCOORDMODE_CLAMP;

	/* I've experimented with non-normalized coordinates and using the LD
	 * sampler fetch, but couldn't make it work. */
	ss->ss3.non_normalized_coord = 0;

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*ss));
}

static uint32_t
gen8_fill_ps(struct intel_bb *ibb,
	     const uint32_t kernel[][4],
	     size_t size)
{
	return intel_bb_copy_data(ibb, kernel, size, 64);
}

/*
 * gen7_fill_vertex_buffer_data populate vertex buffer with data.
 *
 * The vertex buffer consists of 3 vertices to construct a RECTLIST. The 4th
 * vertex is implied (automatically derived by the HW). Each element has the
 * destination offset, and the normalized texture offset (src). The rectangle
 * itself will span the entire subsurface to be copied.
 *
 * see gen6_emit_vertex_elements
 */
static uint32_t
gen7_fill_vertex_buffer_data(struct intel_bb *ibb,
			     const struct intel_buf *src,
			     uint32_t src_x, uint32_t src_y,
			     uint32_t dst_x, uint32_t dst_y,
			     uint32_t width, uint32_t height)
{
	uint32_t offset;

	intel_bb_ptr_align(ibb, 8);
	offset = intel_bb_offset(ibb);

	if (src != NULL) {
		emit_vertex_2s(ibb, dst_x + width, dst_y + height);

		emit_vertex_normalized(ibb, src_x + width, intel_buf_width(src));
		emit_vertex_normalized(ibb, src_y + height, intel_buf_height(src));

		emit_vertex_2s(ibb, dst_x, dst_y + height);

		emit_vertex_normalized(ibb, src_x, intel_buf_width(src));
		emit_vertex_normalized(ibb, src_y + height, intel_buf_height(src));

		emit_vertex_2s(ibb, dst_x, dst_y);

		emit_vertex_normalized(ibb, src_x, intel_buf_width(src));
		emit_vertex_normalized(ibb, src_y, intel_buf_height(src));
	} else {
		emit_vertex_2s(ibb, DIV_ROUND_UP(dst_x + width, 64), DIV_ROUND_UP(dst_y + height, 16));

		emit_vertex_normalized(ibb, 0, 0);
		emit_vertex_normalized(ibb, 0, 0);

		emit_vertex_2s(ibb, dst_x/64, DIV_ROUND_UP(dst_y + height, 16));

		emit_vertex_normalized(ibb, 0, 0);
		emit_vertex_normalized(ibb, 0, 0);

		emit_vertex_2s(ibb, dst_x/64, dst_y/16);

		emit_vertex_normalized(ibb, 0, 0);
		emit_vertex_normalized(ibb, 0, 0);
	}

	return offset;
}

/*
 * gen6_emit_vertex_elements - The vertex elements describe the contents of the
 * vertex buffer. We pack the vertex buffer in a semi weird way, conforming to
 * what gen6_rendercopy did. The most straightforward would be to store
 * everything as floats.
 *
 * see gen7_fill_vertex_buffer_data() for where the corresponding elements are
 * packed.
 */
static void
gen6_emit_vertex_elements(struct intel_bb *ibb) {
	/*
	 * The VUE layout
	 *    dword 0-3: pad (0, 0, 0. 0)
	 *    dword 4-7: position (x, y, 0, 1.0),
	 *    dword 8-11: texture coordinate 0 (u0, v0, 0, 1.0)
	 */
	intel_bb_out(ibb, GEN4_3DSTATE_VERTEX_ELEMENTS | (3 * 2 + 1 - 2));

	/* Element state 0. These are 4 dwords of 0 required for the VUE format.
	 * We don't really know or care what they do.
	 */
	intel_bb_out(ibb, 0 << GEN6_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN6_VE0_VALID |
		     SURFACEFORMAT_R32G32B32A32_FLOAT << VE0_FORMAT_SHIFT |
		     0 << VE0_OFFSET_SHIFT); /* we specify 0, but it's really does not exist */
	intel_bb_out(ibb, GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_0_SHIFT |
		     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_1_SHIFT |
		     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT |
		     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_3_SHIFT);

	/* Element state 1 - Our "destination" vertices. These are passed down
	 * through the pipeline, and eventually make it to the pixel shader as
	 * the offsets in the destination surface. It's packed as the 16
	 * signed/scaled because of gen6 rendercopy. I see no particular reason
	 * for doing this though.
	 */
	intel_bb_out(ibb, 0 << GEN6_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN6_VE0_VALID |
		     SURFACEFORMAT_R16G16_SSCALED << VE0_FORMAT_SHIFT |
		     0 << VE0_OFFSET_SHIFT); /* offsets vb in bytes */
	intel_bb_out(ibb, GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT |
		     GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
		     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT |
		     GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT);

	/* Element state 2. Last but not least we store the U,V components as
	 * normalized floats. These will be used in the pixel shader to sample
	 * from the source buffer.
	 */
	intel_bb_out(ibb, 0 << GEN6_VE0_VERTEX_BUFFER_INDEX_SHIFT | GEN6_VE0_VALID |
		     SURFACEFORMAT_R32G32_FLOAT << VE0_FORMAT_SHIFT |
		     4 << VE0_OFFSET_SHIFT);	/* offset vb in bytes */
	intel_bb_out(ibb, GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_0_SHIFT |
		     GEN4_VFCOMPONENT_STORE_SRC << VE1_VFCOMPONENT_1_SHIFT |
		     GEN4_VFCOMPONENT_STORE_0 << VE1_VFCOMPONENT_2_SHIFT |
		     GEN4_VFCOMPONENT_STORE_1_FLT << VE1_VFCOMPONENT_3_SHIFT);
}

/*
 * gen7_emit_vertex_buffer emit the vertex buffers command
 *
 * @batch
 * @offset - bytw offset within the @batch where the vertex buffer starts.
 */
static void gen7_emit_vertex_buffer(struct intel_bb *ibb, uint32_t offset)
{
	intel_bb_out(ibb, GEN4_3DSTATE_VERTEX_BUFFERS | (1 + (4 * 1) - 2));
	intel_bb_out(ibb, 0 << GEN6_VB0_BUFFER_INDEX_SHIFT | /* VB 0th index */
		     GEN8_VB0_BUFFER_ADDR_MOD_EN | /* Address Modify Enable */
		     VERTEX_SIZE << VB0_BUFFER_PITCH_SHIFT);
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_VERTEX, 0,
			    offset, ibb->batch_offset);
	intel_bb_out(ibb, 3 * VERTEX_SIZE);
}

static uint32_t
gen6_create_cc_state(struct intel_bb *ibb)
{
	struct gen6_color_calc_state *cc_state;

	cc_state = intel_bb_ptr_align(ibb, 64);

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*cc_state));
}

static uint32_t
gen8_create_blend_state(struct intel_bb *ibb)
{
	struct gen8_blend_state *blend;
	int i;

	blend = intel_bb_ptr_align(ibb, 64);

	for (i = 0; i < 16; i++) {
		blend->bs[i].dest_blend_factor = GEN6_BLENDFACTOR_ZERO;
		blend->bs[i].source_blend_factor = GEN6_BLENDFACTOR_ONE;
		blend->bs[i].color_blend_func = GEN6_BLENDFUNCTION_ADD;
		blend->bs[i].pre_blend_color_clamp = 1;
		blend->bs[i].color_buffer_blend = 0;
	}

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*blend));
}

static uint32_t
gen6_create_cc_viewport(struct intel_bb *ibb)
{
	struct gen4_cc_viewport *vp;

	vp = intel_bb_ptr_align(ibb, 32);

	/* XXX I don't understand this */
	vp->min_depth = -1.e35;
	vp->max_depth = 1.e35;

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*vp));
}

static uint32_t
gen7_create_sf_clip_viewport(struct intel_bb *ibb) {
	/* XXX these are likely not needed */
	struct gen7_sf_clip_viewport *scv_state;

	scv_state = intel_bb_ptr_align(ibb, 64);

	scv_state->guardband.xmin = 0;
	scv_state->guardband.xmax = 1.0f;
	scv_state->guardband.ymin = 0;
	scv_state->guardband.ymax = 1.0f;

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*scv_state));
}

static uint32_t
gen6_create_scissor_rect(struct intel_bb *ibb)
{
	struct gen6_scissor_rect *scissor;

	scissor = intel_bb_ptr_align(ibb, 64);

	return intel_bb_ptr_add_return_prev_offset(ibb, sizeof(*scissor));
}

static void
gen8_emit_sip(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN4_STATE_SIP | (3 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen7_emit_push_constants(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_VS);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_HS);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_DS);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, GEN8_3DSTATE_PUSH_CONSTANT_ALLOC_GS);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, GEN7_3DSTATE_PUSH_CONSTANT_ALLOC_PS);
	intel_bb_out(ibb, 0);
}

static void
gen9_emit_state_base_address(struct intel_bb *ibb) {

	/* WaBindlessSurfaceStateModifyEnable:skl,bxt */
	/* The length has to be one less if we dont modify
	   bindless state */
	intel_bb_out(ibb, GEN4_STATE_BASE_ADDRESS | (19 - 1 - 2));

	/* general */
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);
	intel_bb_out(ibb, 0);

	/* stateless data port */
	intel_bb_out(ibb, 0 | BASE_ADDRESS_MODIFY);

	/* surface */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_SAMPLER, 0,
			    BASE_ADDRESS_MODIFY, ibb->batch_offset);

	/* dynamic */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_RENDER | I915_GEM_DOMAIN_INSTRUCTION, 0,
			    BASE_ADDRESS_MODIFY, ibb->batch_offset);

	/* indirect */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	/* instruction */
	intel_bb_emit_reloc(ibb, ibb->handle,
			    I915_GEM_DOMAIN_INSTRUCTION, 0,
			    BASE_ADDRESS_MODIFY, ibb->batch_offset);

	/* general state buffer size */
	intel_bb_out(ibb, 0xfffff000 | 1);
	/* dynamic state buffer size */
	intel_bb_out(ibb, 1 << 12 | 1);
	/* indirect object buffer size */
	intel_bb_out(ibb, 0xfffff000 | 1);
	/* intruction buffer size */
	intel_bb_out(ibb, 1 << 12 | 1);

	/* Bindless surface state base address */
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen7_emit_urb(struct intel_bb *ibb) {
	/* XXX: Min valid values from mesa */
	const int vs_entries = 64;
	const int vs_size = 2;
	const int vs_start = 4;

	intel_bb_out(ibb, GEN7_3DSTATE_URB_VS);
	intel_bb_out(ibb, vs_entries | ((vs_size - 1) << 16) | (vs_start << 25));
	intel_bb_out(ibb, GEN7_3DSTATE_URB_GS);
	intel_bb_out(ibb, vs_start << 25);
	intel_bb_out(ibb, GEN7_3DSTATE_URB_HS);
	intel_bb_out(ibb, vs_start << 25);
	intel_bb_out(ibb, GEN7_3DSTATE_URB_DS);
	intel_bb_out(ibb, vs_start << 25);
}

static void
gen8_emit_cc(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN7_3DSTATE_BLEND_STATE_POINTERS);
	intel_bb_out(ibb, cc.blend_state | 1);

	intel_bb_out(ibb, GEN6_3DSTATE_CC_STATE_POINTERS);
	intel_bb_out(ibb, cc.cc_state | 1);
}

static void
gen8_emit_multisample(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN8_3DSTATE_MULTISAMPLE | 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN6_3DSTATE_SAMPLE_MASK);
	intel_bb_out(ibb, 1);
}

static void
gen8_emit_vs(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN6_3DSTATE_CONSTANT_VS | (11-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_BINDING_TABLE_POINTERS_VS);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_SAMPLER_STATE_POINTERS_VS);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN6_3DSTATE_VS | (9-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen8_emit_hs(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN7_3DSTATE_CONSTANT_HS | (11-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_HS | (9-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_BINDING_TABLE_POINTERS_HS);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN8_3DSTATE_SAMPLER_STATE_POINTERS_HS);
	intel_bb_out(ibb, 0);
}

static void
gen8_emit_gs(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN6_3DSTATE_CONSTANT_GS | (11-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN6_3DSTATE_GS | (10-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_BINDING_TABLE_POINTERS_GS);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_SAMPLER_STATE_POINTERS_GS);
	intel_bb_out(ibb, 0);
}

static void
gen9_emit_ds(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN7_3DSTATE_CONSTANT_DS | (11-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_DS | (11-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_BINDING_TABLE_POINTERS_DS);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN8_3DSTATE_SAMPLER_STATE_POINTERS_DS);
	intel_bb_out(ibb, 0);
}


static void
gen8_emit_wm_hz_op(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN8_3DSTATE_WM_HZ_OP | (5-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen8_emit_null_state(struct intel_bb *ibb) {
	gen8_emit_wm_hz_op(ibb);
	gen8_emit_hs(ibb);
	intel_bb_out(ibb, GEN7_3DSTATE_TE | (4-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	gen8_emit_gs(ibb);
	gen9_emit_ds(ibb);
	gen8_emit_vs(ibb);
}

static void
gen7_emit_clip(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN6_3DSTATE_CLIP | (4 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0); /*  pass-through */
	intel_bb_out(ibb, 0);
}

static void
gen8_emit_sf(struct intel_bb *ibb)
{
	int i;

	intel_bb_out(ibb, GEN7_3DSTATE_SBE | (6 - 2));
	intel_bb_out(ibb, 1 << GEN7_SBE_NUM_OUTPUTS_SHIFT |
		     GEN8_SBE_FORCE_URB_ENTRY_READ_LENGTH |
		     GEN8_SBE_FORCE_URB_ENTRY_READ_OFFSET |
		     1 << GEN7_SBE_URB_ENTRY_READ_LENGTH_SHIFT |
		     1 << GEN8_SBE_URB_ENTRY_READ_OFFSET_SHIFT);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, GEN9_SBE_ACTIVE_COMPONENT_XYZW << 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN8_3DSTATE_SBE_SWIZ | (11 - 2));
	for (i = 0; i < 8; i++)
		intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN8_3DSTATE_RASTER | (5 - 2));
	intel_bb_out(ibb, GEN8_RASTER_FRONT_WINDING_CCW | GEN8_RASTER_CULL_NONE);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN6_3DSTATE_SF | (4 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen8_emit_ps(struct intel_bb *ibb, uint32_t kernel, bool fast_clear) {
	const int max_threads = 63;

	intel_bb_out(ibb, GEN6_3DSTATE_WM | (2 - 2));
	intel_bb_out(ibb, /* XXX: I don't understand the BARYCENTRIC stuff, but it
		   * appears we need it to put our setup data in the place we
		   * expect (g6, see below) */
		     GEN8_3DSTATE_PS_PERSPECTIVE_PIXEL_BARYCENTRIC);

	intel_bb_out(ibb, GEN6_3DSTATE_CONSTANT_PS | (11-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_PS | (12-2));
	intel_bb_out(ibb, kernel);
	intel_bb_out(ibb, 0); /* kernel hi */

	if (fast_clear)
		intel_bb_out(ibb, 1 <<  GEN6_3DSTATE_WM_BINDING_TABLE_ENTRY_COUNT_SHIFT);
	else
		intel_bb_out(ibb, 1 << GEN6_3DSTATE_WM_SAMPLER_COUNT_SHIFT |
		             2 << GEN6_3DSTATE_WM_BINDING_TABLE_ENTRY_COUNT_SHIFT);

	intel_bb_out(ibb, 0); /* scratch space stuff */
	intel_bb_out(ibb, 0); /* scratch hi */
	intel_bb_out(ibb, (max_threads - 1) << GEN8_3DSTATE_PS_MAX_THREADS_SHIFT |
	             GEN6_3DSTATE_WM_16_DISPATCH_ENABLE |
	             (fast_clear ? GEN8_3DSTATE_FAST_CLEAR_ENABLE : 0));
	intel_bb_out(ibb, 6 << GEN6_3DSTATE_WM_DISPATCH_START_GRF_0_SHIFT);
	intel_bb_out(ibb, 0); // kernel 1
	intel_bb_out(ibb, 0); /* kernel 1 hi */
	intel_bb_out(ibb, 0); // kernel 2
	intel_bb_out(ibb, 0); /* kernel 2 hi */

	intel_bb_out(ibb, GEN8_3DSTATE_PS_BLEND | (2 - 2));
	intel_bb_out(ibb, GEN8_PS_BLEND_HAS_WRITEABLE_RT);

	intel_bb_out(ibb, GEN8_3DSTATE_PS_EXTRA | (2 - 2));
	intel_bb_out(ibb, GEN8_PSX_PIXEL_SHADER_VALID | GEN8_PSX_ATTRIBUTE_ENABLE);
}

static void
gen9_emit_depth(struct intel_bb *ibb)
{
	bool need_10dw = HAS_4TILE(ibb->devid);

	intel_bb_out(ibb, GEN8_3DSTATE_WM_DEPTH_STENCIL | (4 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN7_3DSTATE_DEPTH_BUFFER | (need_10dw ? (10-2) : (8-2)));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	if (need_10dw) {
		intel_bb_out(ibb, 0);
		intel_bb_out(ibb, 0);
	}

	intel_bb_out(ibb, GEN8_3DSTATE_HIER_DEPTH_BUFFER | (5-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN8_3DSTATE_STENCIL_BUFFER | (5-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
}

static void
gen7_emit_clear(struct intel_bb *ibb) {
	intel_bb_out(ibb, GEN7_3DSTATE_CLEAR_PARAMS | (3-2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 1); // clear valid
}

static void
gen6_emit_drawing_rectangle(struct intel_bb *ibb, const struct intel_buf *dst)
{
	intel_bb_out(ibb, GEN4_3DSTATE_DRAWING_RECTANGLE | (4 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, (intel_buf_height(dst) - 1) << 16 | (intel_buf_width(dst) - 1));
	intel_bb_out(ibb, 0);
}

static void gen8_emit_vf_topology(struct intel_bb *ibb)
{
	intel_bb_out(ibb, GEN8_3DSTATE_VF_TOPOLOGY);
	intel_bb_out(ibb, _3DPRIM_RECTLIST);
}

/* Vertex elements MUST be defined before this according to spec */
static void gen8_emit_primitive(struct intel_bb *ibb, uint32_t offset)
{
	intel_bb_out(ibb, GEN8_3DSTATE_VF | (2 - 2));
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN8_3DSTATE_VF_INSTANCING | (3 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	intel_bb_out(ibb, GEN4_3DPRIMITIVE | (7-2));
	intel_bb_out(ibb, 0);	/* gen8+ ignore the topology type field */
	intel_bb_out(ibb, 3);	/* vertex count */
	intel_bb_out(ibb, 0);	/*  We're specifying this instead with offset in GEN6_3DSTATE_VERTEX_BUFFERS */
	intel_bb_out(ibb, 1);	/* single instance */
	intel_bb_out(ibb, 0);	/* start instance location */
	intel_bb_out(ibb, 0);	/* index buffer offset, ignored */
}

#define GFX_OP_PIPE_CONTROL    ((3 << 29) | (3 << 27) | (2 << 24))
#define PIPE_CONTROL_CS_STALL	            (1 << 20)
#define PIPE_CONTROL_RENDER_TARGET_FLUSH    (1 << 12)
#define PIPE_CONTROL_FLUSH_ENABLE           (1 << 7)
#define PIPE_CONTROL_DATA_CACHE_INVALIDATE  (1 << 5)
#define PIPE_CONTROL_PROTECTEDPATH_DISABLE  (1 << 27)
#define PIPE_CONTROL_PROTECTEDPATH_ENABLE   (1 << 22)
#define PIPE_CONTROL_POST_SYNC_OP           (1 << 14)
#define PIPE_CONTROL_POST_SYNC_OP_STORE_DW_IDX (1 << 21)
#define PS_OP_TAG_START                     0x1234fed0
#define PS_OP_TAG_END                       0x5678cbaf
static void gen12_emit_pxp_state(struct intel_bb *ibb, bool enable,
		 uint32_t pxp_write_op_offset)
{
	uint32_t pipe_ctl_flags;
	uint32_t set_app_id, ps_op_id;

	if (enable) {
		pipe_ctl_flags = PIPE_CONTROL_FLUSH_ENABLE;
		intel_bb_out(ibb, GFX_OP_PIPE_CONTROL);
		intel_bb_out(ibb, pipe_ctl_flags);

		set_app_id =  MI_SET_APPID |
			      APPTYPE(intel_bb_pxp_apptype(ibb)) |
			      APPID(intel_bb_pxp_appid(ibb));
		intel_bb_out(ibb, set_app_id);

		pipe_ctl_flags = PIPE_CONTROL_PROTECTEDPATH_ENABLE;
		ps_op_id = PS_OP_TAG_START;
	} else {
		pipe_ctl_flags = PIPE_CONTROL_PROTECTEDPATH_DISABLE;
		ps_op_id = PS_OP_TAG_END;
	}

	pipe_ctl_flags |= (PIPE_CONTROL_CS_STALL |
			   PIPE_CONTROL_RENDER_TARGET_FLUSH |
			   PIPE_CONTROL_DATA_CACHE_INVALIDATE |
			   PIPE_CONTROL_POST_SYNC_OP);
	intel_bb_out(ibb, GFX_OP_PIPE_CONTROL | 4);
	intel_bb_out(ibb, pipe_ctl_flags);
	intel_bb_emit_reloc(ibb, ibb->handle, 0, I915_GEM_DOMAIN_COMMAND,
			    (enable ? pxp_write_op_offset : (pxp_write_op_offset+8)),
			    ibb->batch_offset);
	intel_bb_out(ibb, ps_op_id);
	intel_bb_out(ibb, ps_op_id);
}

/* The general rule is if it's named gen6 it is directly copied from
 * gen6_render_copyfunc.
 *
 * This sets up most of the 3d pipeline, and most of that to NULL state. The
 * docs aren't specific about exactly what must be set up NULL, but the general
 * rule is we could be run at any time, and so the most state we set to NULL,
 * the better our odds of success.
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
 * The batch commands point to state within tthe batch, so all state offsets should be
 * 0 < offset < 4096. Both commands and state build upwards, and are constructed
 * in that order. This means too many batch commands can delete state if not
 * careful.
 *
 */

#define BATCH_STATE_SPLIT 2048

static
void _gen9_render_op(struct intel_bb *ibb,
		     struct intel_buf *src,
		     unsigned int src_x, unsigned int src_y,
		     unsigned int width, unsigned int height,
		     struct intel_buf *dst,
		     unsigned int dst_x, unsigned int dst_y,
		     struct intel_buf *aux_pgtable_buf,
		     const float clear_color[4],
		     const uint32_t ps_kernel[][4],
		     uint32_t ps_kernel_size)
{
	uint32_t ps_sampler_state, ps_kernel_off, ps_binding_table;
	uint32_t scissor_state;
	uint32_t vertex_buffer;
	uint32_t aux_pgtable_state;
	bool fast_clear = !src;
	uint32_t pxp_scratch_offset;

	if (!fast_clear)
		igt_assert(src->bpp == dst->bpp);

	intel_bb_flush_render(ibb);

	intel_bb_add_intel_buf(ibb, dst, true);

	if (!fast_clear)
		intel_bb_add_intel_buf(ibb, src, false);

	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	ps_binding_table  = gen8_bind_surfaces(ibb, src, dst);
	ps_sampler_state  = gen8_create_sampler(ibb);
	ps_kernel_off = gen8_fill_ps(ibb, ps_kernel, ps_kernel_size);
	vertex_buffer = gen7_fill_vertex_buffer_data(ibb, src,
						     src_x, src_y,
						     dst_x, dst_y,
						     width, height);
	cc.cc_state = gen6_create_cc_state(ibb);
	cc.blend_state = gen8_create_blend_state(ibb);
	viewport.cc_state = gen6_create_cc_viewport(ibb);
	viewport.sf_clip_state = gen7_create_sf_clip_viewport(ibb);
	scissor_state = gen6_create_scissor_rect(ibb);
	aux_pgtable_state = gen12_create_aux_pgtable_state(ibb, aux_pgtable_buf);

	/* TODO: there is other state which isn't setup */
	pxp_scratch_offset = intel_bb_offset(ibb);
	intel_bb_ptr_set(ibb, 0);

	if (intel_bb_pxp_enabled(ibb))
		gen12_emit_pxp_state(ibb, true, pxp_scratch_offset);

	/* Start emitting the commands. The order roughly follows the mesa blorp
	 * order */
	intel_bb_out(ibb, G4X_PIPELINE_SELECT | PIPELINE_SELECT_3D |
		     GEN9_PIPELINE_SELECTION_MASK);

	gen12_emit_aux_pgtable_state(ibb, aux_pgtable_state, true);

	if (fast_clear) {
		for (int i = 0; i < 4; i++) {
			intel_bb_out(ibb, MI_STORE_DWORD_IMM);
			intel_bb_emit_reloc(ibb, dst->handle,
					    I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER,
                                            dst->cc.offset + i*sizeof(float),
					    dst->addr.offset);
			intel_bb_out(ibb, *(uint32_t*)&clear_color[i]);
               }
       }


	gen8_emit_sip(ibb);

	gen7_emit_push_constants(ibb);

	gen9_emit_state_base_address(ibb);

	if (HAS_4TILE(ibb->devid) || intel_gen(ibb->devid) > 12) {
		intel_bb_out(ibb, GEN4_3DSTATE_BINDING_TABLE_POOL_ALLOC | 2);
		intel_bb_emit_reloc(ibb, ibb->handle,
				    I915_GEM_DOMAIN_RENDER | I915_GEM_DOMAIN_INSTRUCTION, 0,
				    0, ibb->batch_offset);
		intel_bb_out(ibb, 1 << 12);
	}

	intel_bb_out(ibb, GEN7_3DSTATE_VIEWPORT_STATE_POINTERS_CC);
	intel_bb_out(ibb, viewport.cc_state);
	intel_bb_out(ibb, GEN8_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP);
	intel_bb_out(ibb, viewport.sf_clip_state);

	gen7_emit_urb(ibb);

	gen8_emit_cc(ibb);

	gen8_emit_multisample(ibb);

	gen8_emit_null_state(ibb);

	intel_bb_out(ibb, GEN7_3DSTATE_STREAMOUT | (5 - 2));
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);
	intel_bb_out(ibb, 0);

	gen7_emit_clip(ibb);

	gen8_emit_sf(ibb);

	gen8_emit_ps(ibb, ps_kernel_off, fast_clear);

	intel_bb_out(ibb, GEN7_3DSTATE_BINDING_TABLE_POINTERS_PS);
	intel_bb_out(ibb, ps_binding_table);

	intel_bb_out(ibb, GEN7_3DSTATE_SAMPLER_STATE_POINTERS_PS);
	intel_bb_out(ibb, ps_sampler_state);

	intel_bb_out(ibb, GEN8_3DSTATE_SCISSOR_STATE_POINTERS);
	intel_bb_out(ibb, scissor_state);

	gen9_emit_depth(ibb);

	gen7_emit_clear(ibb);

	gen6_emit_drawing_rectangle(ibb, dst);

	gen7_emit_vertex_buffer(ibb, vertex_buffer);
	gen6_emit_vertex_elements(ibb);

	gen8_emit_vf_topology(ibb);
	gen8_emit_primitive(ibb, vertex_buffer);

	if (intel_bb_pxp_enabled(ibb))
		gen12_emit_pxp_state(ibb, false, pxp_scratch_offset);

	intel_bb_emit_bbe(ibb);
	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_RENDER | I915_EXEC_NO_RELOC, false);
	dump_batch(ibb);
	intel_bb_reset(ibb, false);
}

void gen9_render_copyfunc(struct intel_bb *ibb,
			  struct intel_buf *src,
			  unsigned int src_x, unsigned int src_y,
			  unsigned int width, unsigned int height,
			  struct intel_buf *dst,
			  unsigned int dst_x, unsigned int dst_y)

{
	_gen9_render_op(ibb, src, src_x, src_y,
		        width, height, dst, dst_x, dst_y, NULL, NULL,
		        ps_kernel_gen9, sizeof(ps_kernel_gen9));
}

void gen11_render_copyfunc(struct intel_bb *ibb,
			   struct intel_buf *src,
			   unsigned int src_x, unsigned int src_y,
			   unsigned int width, unsigned int height,
			   struct intel_buf *dst,
			   unsigned int dst_x, unsigned int dst_y)
{
	_gen9_render_op(ibb, src, src_x, src_y,
		        width, height, dst, dst_x, dst_y, NULL, NULL,
		        ps_kernel_gen11, sizeof(ps_kernel_gen11));
}

void gen12_render_copyfunc(struct intel_bb *ibb,
			   struct intel_buf *src,
			   unsigned int src_x, unsigned int src_y,
			   unsigned int width, unsigned int height,
			   struct intel_buf *dst,
			   unsigned int dst_x, unsigned int dst_y)
{
	struct aux_pgtable_info pgtable_info = { };

	gen12_aux_pgtable_init(&pgtable_info, ibb, src, dst);

	_gen9_render_op(ibb, src, src_x, src_y,
		        width, height, dst, dst_x, dst_y,
		        pgtable_info.pgtable_buf,
		        NULL,
		        gen12_render_copy,
		        sizeof(gen12_render_copy));

	gen12_aux_pgtable_cleanup(ibb, &pgtable_info);
}

void gen12p71_render_copyfunc(struct intel_bb *ibb,
			      struct intel_buf *src,
			      unsigned int src_x, unsigned int src_y,
			      unsigned int width, unsigned int height,
			      struct intel_buf *dst,
			      unsigned int dst_x, unsigned int dst_y)
{
	_gen9_render_op(ibb, src, src_x, src_y,
			width, height, dst, dst_x, dst_y,
			NULL,
			NULL,
			gen12p71_render_copy,
			sizeof(gen12p71_render_copy));
}

void gen12_render_clearfunc(struct intel_bb *ibb,
			    struct intel_buf *dst,
			    unsigned int dst_x, unsigned int dst_y,
			    unsigned int width, unsigned int height,
			    const float clear_color[4])
{
	struct aux_pgtable_info pgtable_info = { };

	gen12_aux_pgtable_init(&pgtable_info, ibb, NULL, dst);

	_gen9_render_op(ibb, NULL, 0, 0,
			width, height, dst, dst_x, dst_y,
			pgtable_info.pgtable_buf,
			clear_color,
			gen12_render_copy,
			sizeof(gen12_render_copy));
	gen12_aux_pgtable_cleanup(ibb, &pgtable_info);
}

void gen12p71_render_clearfunc(struct intel_bb *ibb,
			       struct intel_buf *dst,
			       unsigned int dst_x, unsigned int dst_y,
			       unsigned int width, unsigned int height,
			       const float clear_color[4])
{
	_gen9_render_op(ibb, NULL, 0, 0,
			width, height, dst, dst_x, dst_y,
			NULL,
			clear_color,
			gen12p71_render_copy,
			sizeof(gen12p71_render_copy));
}
