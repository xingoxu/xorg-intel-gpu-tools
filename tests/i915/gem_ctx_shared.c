/*
 * Copyright © 2017-2019 Intel Corporation
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

#include <unistd.h>
#include <sched.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include <drm.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_engine_topology.h"
#include "i915/gem_vm.h"
#include "igt.h"
#include "igt_types.h"
#include "igt_rand.h"
#include "igt_vgem.h"
#include "sw_sync.h"
#include "sync_file.h"

#define LO 0
#define HI 1
#define NOISE 2

#define MAX_PRIO I915_CONTEXT_MAX_USER_PRIORITY
#define MIN_PRIO I915_CONTEXT_MIN_USER_PRIORITY

static int priorities[] = {
	[LO] = MIN_PRIO / 2,
	[HI] = MAX_PRIO / 2,
};

#define MAX_ELSP_QLEN 16

IGT_TEST_DESCRIPTION("Test shared contexts.");

static int __get_vm(int i915, uint32_t ctx, uint32_t *vm)
{
	struct drm_i915_gem_context_param p = {
		.ctx_id = ctx,
		.param = I915_CONTEXT_PARAM_VM,
	};
	int err = __gem_context_get_param(i915, &p);
	if (err)
		return err;

	igt_assert(p.value > 0 && p.value < UINT32_MAX);
	*vm = p.value;

	return 0;
}

static uint32_t get_vm(int i915, uint32_t ctx)
{
	uint32_t vm;
	igt_assert_eq(__get_vm(i915, ctx, &vm), 0);
	return vm;
}

static void set_vm(int i915, uint32_t ctx, uint32_t vm)
{
	struct drm_i915_gem_context_param p = {
		.ctx_id = ctx,
		.param = I915_CONTEXT_PARAM_VM,
		.value = vm
	};
	gem_context_set_param(i915, &p);
}

static void copy_vm(int i915, uint32_t dst, uint32_t src)
{
	uint32_t vm = get_vm(i915, src);
	set_vm(i915, dst, vm);

	/* GETPARAM gets a reference to the VM which we have to drop */
	gem_vm_destroy(i915, vm);
}

static void create_shared_gtt(int i915, unsigned int flags)
#define DETACHED 0x1
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj = {
		.handle = gem_create(i915, 4096),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
	};
	uint32_t parent, child;

	gem_write(i915, obj.handle, 0, &bbe, sizeof(bbe));
	gem_execbuf(i915, &execbuf);
	gem_sync(i915, obj.handle);

	child = flags & DETACHED ? gem_context_create(i915) : 0;
	igt_until_timeout(2) {
		parent = flags & DETACHED ? child : 0;
		child = gem_context_create(i915);
		copy_vm(i915, child, parent);

		execbuf.rsvd1 = child;
		gem_execbuf(i915, &execbuf);

		if (flags & DETACHED) {
			gem_context_destroy(i915, parent);
			gem_execbuf(i915, &execbuf);
		} else {
			parent = child;
			gem_context_destroy(i915, parent);
		}

		execbuf.rsvd1 = parent;
		igt_assert_eq(__gem_execbuf(i915, &execbuf), -ENOENT);
		igt_assert_eq(__get_vm(i915, parent, &parent), -ENOENT);
	}
	if (flags & DETACHED)
		gem_context_destroy(i915, child);

	gem_sync(i915, obj.handle);
	gem_close(i915, obj.handle);
}

static void disjoint_timelines(int i915, const intel_ctx_cfg_t *cfg)
{
	IGT_CORK_HANDLE(cork);
	intel_ctx_cfg_t vm_cfg;
	const intel_ctx_t *ctx[2];
	igt_spin_t *spin[2];
	uint32_t plug;
	uint64_t ahnd;

	igt_require(gem_uses_ppgtt(i915) && gem_scheduler_enabled(i915));

	/*
	 * Each context, although they share a vm, are expected to be
	 * distinct timelines. A request queued to one context should be
	 * independent of any shared contexts.
	 */
	vm_cfg = *cfg;
	vm_cfg.vm = gem_vm_create(i915);
	ctx[0] = intel_ctx_create(i915, &vm_cfg);
	ctx[1] = intel_ctx_create(i915, &vm_cfg);
	/* Context id is not important, we share vm */
	ahnd = get_reloc_ahnd(i915, 0);

	plug = igt_cork_plug(&cork, i915);

	spin[0] = __igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx[0], .dependency = plug);
	spin[1] = __igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx[1]);

	/* Wait for the second spinner, will hang if stuck behind the first */
	igt_spin_end(spin[1]);
	gem_sync(i915, spin[1]->handle);

	igt_cork_unplug(&cork);

	igt_spin_free(i915, spin[1]);
	igt_spin_free(i915, spin[0]);
	put_ahnd(ahnd);

	intel_ctx_destroy(i915, ctx[0]);
	intel_ctx_destroy(i915, ctx[1]);
	gem_vm_destroy(i915, vm_cfg.vm);
}

static void exhaust_shared_gtt(int i915, unsigned int flags)
#define EXHAUST_LRC 0x1
{
	struct drm_i915_gem_context_create_ext_setparam vm_create_ext = {
		.base = {
			.name = I915_CONTEXT_CREATE_EXT_SETPARAM,
		},
		.param = {
			.param = I915_CONTEXT_PARAM_VM,
		},
	};

	i915 = gem_reopen_driver(i915);

	vm_create_ext.param.value = gem_vm_create(i915);

	igt_fork(pid, 1) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 obj = {
			.handle = gem_create(i915, 4096)
		};
		struct drm_i915_gem_execbuffer2 execbuf = {
			.buffers_ptr = to_user_pointer(&obj),
			.buffer_count = 1,
		};
		unsigned long count = 0;
		int err;

		gem_write(i915, obj.handle, 0, &bbe, sizeof(bbe));

		for (;;) {
			uint32_t ctx;
			err = __gem_context_create_ext(i915, 0,
						       to_user_pointer(&vm_create_ext),
						       &ctx);
			if (err)
				break;

			if (flags & EXHAUST_LRC) {
				execbuf.rsvd1 = ctx;
				err = __gem_execbuf(i915, &execbuf);
				if (err)
					break;
			}

			count++;
		}
		gem_sync(i915, obj.handle);

		igt_info("Created %lu shared contexts, before %d (%s)\n",
			 count, err, strerror(-err));
	}
	close(i915);
	igt_waitchildren();
}

static void exec_shared_gtt(int i915, const intel_ctx_cfg_t *cfg,
			    unsigned int ring)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj = {};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = ring,
	};
	intel_ctx_cfg_t vm_cfg;
	const intel_ctx_t *ctx[2];
	uint32_t scratch, *s;
	uint32_t batch, cs[16];
	uint64_t offset;
	int timeline;
	int i;

	vm_cfg = *cfg;
	vm_cfg.vm = gem_vm_create(i915);
	ctx[0] = intel_ctx_create(i915, &vm_cfg);
	ctx[1] = intel_ctx_create(i915, &vm_cfg);

	/* Find a hole big enough for both objects later */
	scratch = gem_create(i915, 16384);
	gem_write(i915, scratch, 0, &bbe, sizeof(bbe));
	obj.handle = scratch;
	execbuf.rsvd1 = ctx[0]->id;
	gem_execbuf(i915, &execbuf);
	obj.flags |= EXEC_OBJECT_PINNED; /* reuse this address */
	execbuf.rsvd1 = ctx[1]->id; /* and bind the second context image */
	gem_execbuf(i915, &execbuf);
	execbuf.rsvd1 = ctx[0]->id;
	gem_close(i915, scratch);

	timeline = sw_sync_timeline_create();
	execbuf.rsvd2 = sw_sync_timeline_create_fence(timeline, 1);
	execbuf.flags |= I915_EXEC_FENCE_IN;

	scratch = gem_create(i915, 4096);
	s =  gem_mmap__device_coherent(i915, scratch, 0, 4096, PROT_WRITE);
	gem_set_domain(i915, scratch, I915_GEM_DOMAIN_WC, I915_GEM_DOMAIN_WC);
	s[0] = bbe;
	s[64] = bbe;

	/* Load object into place in the GTT */
	obj.handle = scratch;
	execbuf.flags |= I915_EXEC_FENCE_OUT;
	gem_execbuf_wr(i915, &execbuf);
	execbuf.flags &= ~I915_EXEC_FENCE_OUT;
	execbuf.rsvd2 >>= 32;
	offset = obj.offset;

	/* Presume nothing causes an eviction in the meantime! */

	batch = gem_create(i915, 4096);

	i = 0;
	cs[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		cs[++i] = obj.offset;
		cs[++i] = obj.offset >> 32;
	} else if (gen >= 4) {
		cs[++i] = 0;
		cs[++i] = obj.offset;
	} else {
		cs[i]--;
		cs[++i] = obj.offset;
	}
	cs[++i] = 0xc0ffee;
	cs[++i] = bbe;
	gem_write(i915, batch, 0, cs, sizeof(cs));

	obj.handle = batch;
	obj.offset += 8192; /* make sure we don't cause an eviction! */
	execbuf.rsvd1 = ctx[1]->id;
	if (gen > 3 && gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;
	gem_execbuf(i915, &execbuf);

	/* Check the scratch didn't move */
	obj.handle = scratch;
	obj.offset = -1;
	obj.flags &= ~EXEC_OBJECT_PINNED;
	execbuf.batch_start_offset = 64 * sizeof(s[0]);
	gem_execbuf(i915, &execbuf);
	igt_assert_eq_u64(obj.offset, offset);

	gem_close(i915, batch);
	sw_sync_timeline_inc(timeline, 1);
	close(timeline);

	gem_sync(i915, scratch); /* write hazard lies */
	igt_assert_eq(sync_fence_status(execbuf.rsvd2), 1);
	close(execbuf.rsvd2);

	/*
	 * If we created the new context with the old GTT, the write
	 * into the stale location of scratch will have landed in the right
	 * object. Otherwise, it should read the previous value of
	 * MI_BATCH_BUFFER_END.
	 */
	igt_assert_eq_u32(*s, 0xc0ffee);

	munmap(s, 4096);
	gem_close(i915, scratch);

	intel_ctx_destroy(i915, ctx[0]);
	intel_ctx_destroy(i915, ctx[1]);
	gem_vm_destroy(i915, vm_cfg.vm);
}

static int nop_sync(int i915, const intel_ctx_t *ctx, unsigned int ring,
		    int64_t timeout)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 obj = {
		.handle = gem_create(i915, 4096),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = ring,
		.rsvd1 = ctx->id,
	};
	int err;

	gem_write(i915, obj.handle, 0, &bbe, sizeof(bbe));
	gem_execbuf(i915, &execbuf);
	err = gem_wait(i915, obj.handle, &timeout);
	gem_close(i915, obj.handle);

	return err;
}

static void single_timeline(int i915, const intel_ctx_cfg_t *cfg)
{
	const struct intel_execution_engine2 *e;
	struct sync_fence_info rings[64];
	struct sync_file_info sync_file_info = {
		.num_fences = 1,
	};
	intel_ctx_cfg_t st_cfg;
	const intel_ctx_t *ctx;
	igt_spin_t *spin;
	uint64_t ahnd = get_reloc_ahnd(i915, 0);
	int n;

	igt_require(gem_context_has_single_timeline(i915));

	spin = igt_spin_new(i915, .ahnd = ahnd);

	/*
	 * For a "single timeline" context, each ring is on the common
	 * timeline, unlike a normal context where each ring has an
	 * independent timeline. That is no matter which engine we submit
	 * to, it reports the same timeline name and fence context. However,
	 * the fence context is not reported through the sync_fence_info.
	 */
	st_cfg = *cfg;
	st_cfg.flags |= I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE;
	ctx = intel_ctx_create(i915, &st_cfg);
	spin->execbuf.rsvd1 = ctx->id;
	n = 0;
	for_each_ctx_engine(i915, ctx, e) {
		spin->execbuf.flags = e->flags | I915_EXEC_FENCE_OUT;

		gem_execbuf_wr(i915, &spin->execbuf);
		sync_file_info.sync_fence_info = to_user_pointer(&rings[n]);
		do_ioctl(spin->execbuf.rsvd2 >> 32, SYNC_IOC_FILE_INFO, &sync_file_info);
		close(spin->execbuf.rsvd2 >> 32);

		igt_info("ring[%d] fence: %s %s\n",
			 n, rings[n].driver_name, rings[n].obj_name);
		if (++n == ARRAY_SIZE(rings))
			break;
	}
	igt_spin_free(i915, spin);

	for (int i = 1; i < n; i++) {
		igt_assert(!strcmp(rings[0].driver_name, rings[i].driver_name));
		igt_assert(!strcmp(rings[0].obj_name, rings[i].obj_name));
	}
	intel_ctx_destroy(i915, ctx);
	put_ahnd(ahnd);
}

static void exec_single_timeline(int i915, const intel_ctx_cfg_t *cfg,
				 unsigned int engine)
{
	const struct intel_execution_engine2 *e;
	igt_spin_t *spin;
	intel_ctx_cfg_t st_cfg;
	const intel_ctx_t *ctx;
	uint64_t ahnd;

	/*
	 * On an ordinary context, a blockage on one engine doesn't prevent
	 * execution on an other.
	 */
	ctx = intel_ctx_create(i915, cfg);
	ahnd = get_reloc_ahnd(i915, ctx->id);
	spin = NULL;
	for_each_ctx_cfg_engine(i915, cfg, e) {
		if (e->flags == engine)
			continue;

		if (spin == NULL) {
			spin = __igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
					      .engine = e->flags);
		} else {
			struct drm_i915_gem_execbuffer2 execbuf = {
				.buffers_ptr = spin->execbuf.buffers_ptr,
				.buffer_count = spin->execbuf.buffer_count,
				.flags = e->flags,
				.rsvd1 = ctx->id,
			};
			gem_execbuf(i915, &execbuf);
		}
	}
	igt_require(spin);
	igt_assert_eq(nop_sync(i915, ctx, engine, NSEC_PER_SEC), 0);
	igt_spin_free(i915, spin);
	intel_ctx_destroy(i915, ctx);
	put_ahnd(ahnd);

	/*
	 * But if we create a context with just a single shared timeline,
	 * then it will block waiting for the earlier requests on the
	 * other engines.
	 */
	st_cfg = *cfg;
	st_cfg.flags |= I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE;
	ctx = intel_ctx_create(i915, &st_cfg);
	ahnd = get_reloc_ahnd(i915, ctx->id);
	spin = NULL;
	for_each_ctx_cfg_engine(i915, &st_cfg, e) {
		if (e->flags == engine)
			continue;

		if (spin == NULL) {
			spin = __igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
					      .engine = e->flags);
		} else {
			struct drm_i915_gem_execbuffer2 execbuf = {
				.buffers_ptr = spin->execbuf.buffers_ptr,
				.buffer_count = spin->execbuf.buffer_count,
				.flags = e->flags,
				.rsvd1 = ctx->id,
			};
			gem_execbuf(i915, &execbuf);
		}
	}
	igt_assert(spin);
	igt_assert_eq(nop_sync(i915, ctx, engine, NSEC_PER_SEC), -ETIME);
	igt_spin_free(i915, spin);
	intel_ctx_destroy(i915, ctx);
	put_ahnd(ahnd);
}

static void store_dword(int i915, uint64_t ahnd, const intel_ctx_t *ctx,
			unsigned int ring, uint32_t target, uint64_t target_size,
			uint32_t offset, uint32_t value,
			uint32_t cork, uint64_t cork_size,
			unsigned write_domain)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t batch[16];
	int i;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj + !cork);
	execbuf.buffer_count = 2 + !!cork;
	execbuf.flags = ring;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;
	execbuf.rsvd1 = ctx->id;

	memset(obj, 0, sizeof(obj));
	obj[0].handle = cork;
	obj[1].handle = target;
	obj[2].handle = gem_create(i915, 4096);
	if (ahnd) {
		obj[0].offset = get_offset(ahnd, cork, cork_size, 0);
		obj[0].flags |= EXEC_OBJECT_PINNED;
		obj[1].offset = get_offset(ahnd, target, target_size, 0);
		obj[1].flags |= EXEC_OBJECT_PINNED;
		if (write_domain)
			obj[1].flags |= EXEC_OBJECT_WRITE;
		obj[2].offset = get_offset(ahnd, obj[2].handle, 4096, 0x0);
		obj[2].flags |= EXEC_OBJECT_PINNED;
		execbuf.flags |= I915_EXEC_NO_RELOC;
	} else {
		obj[0].offset = cork << 20;
		obj[1].offset = target << 20;
		obj[2].offset = 256 << 10;
		obj[2].offset += (random() % 128) << 12;
	}

	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = obj[1].handle;
	reloc.presumed_offset = obj[1].offset;
	reloc.offset = sizeof(uint32_t);
	reloc.delta = offset;
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = write_domain;
	obj[2].relocs_ptr = to_user_pointer(&reloc);
	obj[2].relocation_count = !ahnd ? 1 : 0;

	i = 0;
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = reloc.presumed_offset + reloc.delta;
		batch[++i] = 0;
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = reloc.presumed_offset + reloc.delta;
		reloc.offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = reloc.presumed_offset + reloc.delta;
	}
	batch[++i] = value;
	batch[++i] = MI_BATCH_BUFFER_END;
	gem_write(i915, obj[2].handle, 0, batch, sizeof(batch));
	gem_execbuf(i915, &execbuf);
	gem_close(i915, obj[2].handle);
}

static const intel_ctx_t *
create_highest_priority(int i915, const intel_ctx_cfg_t *cfg)
{
	const intel_ctx_t *ctx = intel_ctx_create(i915, cfg);

	/*
	 * If there is no priority support, all contexts will have equal
	 * priority (and therefore the max user priority), so no context
	 * can overtake us, and we effectively can form a plug.
	 */
	__gem_context_set_priority(i915, ctx->id, MAX_PRIO);

	return ctx;
}

static void unplug_show_queue(int i915, struct igt_cork *c, uint64_t ahnd,
			      const intel_ctx_cfg_t *cfg, unsigned int engine)
{
	igt_spin_t *spin[MAX_ELSP_QLEN];

	for (int n = 0; n < ARRAY_SIZE(spin); n++) {
		const intel_ctx_t *ctx = create_highest_priority(i915, cfg);

		/*
		 * When we're using same vm we should use allocator handle
		 * passed by the caller. This is the case where cfg->vm
		 * is not NULL.
		 *
		 * For cases where context use its own vm we need separate
		 * ahnd for it.
		 */
		if (!cfg->vm)
			ahnd = get_reloc_ahnd(i915, ctx->id);
		spin[n] = __igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
					 .engine = engine);
		intel_ctx_destroy(i915, ctx);
	}

	igt_cork_unplug(c); /* batches will now be queued on the engine */
	igt_debugfs_dump(i915, "i915_engine_info");

	/* give time to the kernel to complete the queueing */
	usleep(25000);

	for (int n = 0; n < ARRAY_SIZE(spin); n++) {
		ahnd = spin[n]->opts.ahnd;
		igt_spin_free(i915, spin[n]);
		if (!cfg->vm)
			put_ahnd(ahnd);
	}
}

static uint32_t store_timestamp(int i915,
				uint64_t ahnd,
				const intel_ctx_t *ctx,
				unsigned ring,
				unsigned mmio_base,
				int fence,
				int offset)
{
	const bool r64b = intel_gen(intel_get_drm_devid(i915)) >= 8;
	uint32_t handle = gem_create(i915, 4096);
	struct drm_i915_gem_exec_object2 obj = {
		.handle = handle,
		.relocation_count = !ahnd ? 1 : 0,
		.offset = !ahnd ? (32 << 20) + (handle << 16) :
				  get_offset(ahnd, handle, 4096, 0),
		.flags = !ahnd ? 0 : EXEC_OBJECT_PINNED,
	};
	struct drm_i915_gem_relocation_entry reloc = {
		.target_handle = obj.handle,
		.offset = 2 * sizeof(uint32_t),
		.presumed_offset = obj.offset,
		.delta = offset * sizeof(uint32_t),
		.read_domains = I915_GEM_DOMAIN_INSTRUCTION,
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = ring | I915_EXEC_FENCE_IN | (!!ahnd * I915_EXEC_NO_RELOC),
		.rsvd1 = ctx->id,
		.rsvd2 = fence
	};
	uint32_t batch[] = {
		0x24 << 23 | (1 + r64b), /* SRM */
		mmio_base + 0x358,
		reloc.presumed_offset + reloc.delta,
		0,
		MI_BATCH_BUFFER_END
	};

	igt_require(intel_gen(intel_get_drm_devid(i915)) >= 7);

	gem_write(i915, handle, 0, batch, sizeof(batch));
	obj.relocs_ptr = to_user_pointer(&reloc);

	gem_execbuf(i915, &execbuf);

	return handle;
}

static void kick_tasklets(void)
{
	sched_yield();
	usleep(100);
	sched_yield();
}

static void independent(int i915, const intel_ctx_cfg_t *cfg,
			const struct intel_execution_engine2 *e,
			unsigned flags)
{
	const int TIMESTAMP = 1023;
	uint32_t handle[ARRAY_SIZE(priorities)];
	igt_spin_t *spin[MAX_ELSP_QLEN];
	intel_ctx_cfg_t q_cfg;
	unsigned int mmio_base;
	IGT_CORK_FENCE(cork);
	int fence;
	uint64_t ahnd = get_reloc_ahnd(i915, 0); /* same vm */

	mmio_base = gem_engine_mmio_base(i915, e->name);
	igt_require_f(mmio_base, "mmio base not known\n");

	q_cfg = *cfg;
	q_cfg.vm = gem_vm_create(i915);
	q_cfg.flags |= I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE;

	for (int n = 0; n < ARRAY_SIZE(spin); n++) {
		const intel_ctx_t *ctx = create_highest_priority(i915, &q_cfg);
		spin[n] = __igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
					 .engine = e->flags);
		intel_ctx_destroy(i915, ctx);
	}

	fence = igt_cork_plug(&cork, i915);
	for (int i = 0; i < ARRAY_SIZE(priorities); i++) {
		const intel_ctx_t *ctx = create_highest_priority(i915, &q_cfg);
		gem_context_set_priority(i915, ctx->id, priorities[i]);
		handle[i] = store_timestamp(i915, ahnd, ctx,
					    e->flags, mmio_base,
					    fence, TIMESTAMP);
		intel_ctx_destroy(i915, ctx);
	}
	close(fence);
	kick_tasklets(); /* XXX try to hide cmdparser delays XXX */

	igt_cork_unplug(&cork);
	igt_debugfs_dump(i915, "i915_engine_info");

	for (int n = 0; n < ARRAY_SIZE(spin); n++)
		igt_spin_free(i915, spin[n]);

	for (int i = 0; i < ARRAY_SIZE(priorities); i++) {
		uint32_t *ptr;

		ptr = gem_mmap__device_coherent(i915, handle[i], 0, 4096, PROT_READ);
		gem_set_domain(i915, handle[i], /* no write hazard lies! */
			       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
		gem_close(i915, handle[i]);
		put_offset(ahnd, handle[i]);

		handle[i] = ptr[TIMESTAMP];
		munmap(ptr, 4096);

		igt_debug("ctx[%d] .prio=%d, timestamp=%u\n",
			  i, priorities[i], handle[i]);
	}
	put_ahnd(ahnd);

	igt_assert((int32_t)(handle[HI] - handle[LO]) < 0);

	gem_vm_destroy(i915, q_cfg.vm);
}

static void reorder(int i915, const intel_ctx_cfg_t *cfg,
		    unsigned ring, unsigned flags)
#define EQUAL 1
{
	IGT_CORK_HANDLE(cork);
	uint32_t scratch;
	uint32_t *ptr;
	intel_ctx_cfg_t q_cfg;
	const intel_ctx_t *ctx[2];
	uint32_t plug;
	uint64_t ahnd = get_reloc_ahnd(i915, 0);

	q_cfg = *cfg;
	q_cfg.vm = gem_vm_create(i915);
	q_cfg.flags |= I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE;

	ctx[LO] = intel_ctx_create(i915, &q_cfg);
	gem_context_set_priority(i915, ctx[LO]->id, MIN_PRIO);

	ctx[HI] = intel_ctx_create(i915, &q_cfg);
	gem_context_set_priority(i915, ctx[HI]->id, flags & EQUAL ? MIN_PRIO : 0);

	scratch = gem_create(i915, 4096);
	plug = igt_cork_plug(&cork, i915);

	/* We expect the high priority context to be executed first, and
	 * so the final result will be value from the low priority context.
	 */
	store_dword(i915, ahnd, ctx[LO], ring, scratch, 4096,
		    0, ctx[LO]->id, plug, 4096, 0);
	store_dword(i915, ahnd, ctx[HI], ring, scratch, 4096,
		    0, ctx[HI]->id, plug, 4096, 0);

	unplug_show_queue(i915, &cork, ahnd, &q_cfg, ring);
	gem_close(i915, plug);

	ptr = gem_mmap__device_coherent(i915, scratch, 0, 4096, PROT_READ);
	gem_set_domain(i915, scratch, /* no write hazard lies! */
		       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(i915, scratch);

	if (flags & EQUAL) /* equal priority, result will be fifo */
		igt_assert_eq_u32(ptr[0], ctx[HI]->id);
	else
		igt_assert_eq_u32(ptr[0], ctx[LO]->id);
	munmap(ptr, 4096);

	intel_ctx_destroy(i915, ctx[LO]);
	intel_ctx_destroy(i915, ctx[HI]);
	put_offset(ahnd, scratch);
	put_offset(ahnd, plug);
	put_ahnd(ahnd);

	gem_vm_destroy(i915, q_cfg.vm);
}

static void promotion(int i915, const intel_ctx_cfg_t *cfg, unsigned ring)
{
	IGT_CORK_HANDLE(cork);
	uint32_t result, dep;
	uint32_t *ptr;
	intel_ctx_cfg_t q_cfg;
	const intel_ctx_t *ctx[3];
	uint32_t plug;
	uint64_t ahnd = get_reloc_ahnd(i915, 0);

	q_cfg = *cfg;
	q_cfg.vm = gem_vm_create(i915);
	q_cfg.flags |= I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE;

	ctx[LO] = intel_ctx_create(i915, &q_cfg);
	gem_context_set_priority(i915, ctx[LO]->id, MIN_PRIO);

	ctx[HI] = intel_ctx_create(i915, &q_cfg);
	gem_context_set_priority(i915, ctx[HI]->id, MAX_PRIO);

	ctx[NOISE] = intel_ctx_create(i915, &q_cfg);
	gem_context_set_priority(i915, ctx[NOISE]->id, 0);

	result = gem_create(i915, 4096);
	dep = gem_create(i915, 4096);

	plug = igt_cork_plug(&cork, i915);

	/* Expect that HI promotes LO, so the order will be LO, HI, NOISE.
	 *
	 * fifo would be NOISE, LO, HI.
	 * strict priority would be  HI, NOISE, LO
	 */
	store_dword(i915, ahnd, ctx[NOISE], ring, result, 4096,
		    0, ctx[NOISE]->id, plug, 4096, 0);
	store_dword(i915, ahnd, ctx[LO], ring, result, 4096,
		    0, ctx[LO]->id, plug, 4096, 0);

	/* link LO <-> HI via a dependency on another buffer */
	store_dword(i915, ahnd, ctx[LO], ring, dep, 4096,
		    0, ctx[LO]->id, 0, 0, I915_GEM_DOMAIN_INSTRUCTION);
	store_dword(i915, ahnd, ctx[HI], ring, dep, 4096,
		    0, ctx[HI]->id, 0, 0, 0);

	store_dword(i915, ahnd, ctx[HI], ring, result, 4096,
		    0, ctx[HI]->id, 0, 0, 0);

	unplug_show_queue(i915, &cork, ahnd, &q_cfg, ring);
	gem_close(i915, plug);

	ptr = gem_mmap__device_coherent(i915, dep, 0, 4096, PROT_READ);
	gem_set_domain(i915, dep, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(i915, dep);

	igt_assert_eq_u32(ptr[0], ctx[HI]->id);
	munmap(ptr, 4096);

	ptr = gem_mmap__device_coherent(i915, result, 0, 4096, PROT_READ);
	gem_set_domain(i915, result, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(i915, result);

	igt_assert_eq_u32(ptr[0], ctx[NOISE]->id);
	munmap(ptr, 4096);

	intel_ctx_destroy(i915, ctx[NOISE]);
	intel_ctx_destroy(i915, ctx[LO]);
	intel_ctx_destroy(i915, ctx[HI]);
	put_offset(ahnd, result);
	put_offset(ahnd, dep);
	put_offset(ahnd, plug);
	put_ahnd(ahnd);

	gem_vm_destroy(i915, q_cfg.vm);
}

static void smoketest(int i915, const intel_ctx_cfg_t *cfg,
		      unsigned ring, unsigned timeout)
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	intel_ctx_cfg_t q_cfg;
	unsigned engines[I915_EXEC_RING_MASK + 1];
	unsigned nengine;
	unsigned engine;
	uint32_t scratch;
	uint32_t *ptr;
	uint64_t ahnd = get_reloc_ahnd(i915, 0); /* same vm */

	q_cfg = *cfg;
	q_cfg.vm = gem_vm_create(i915);
	q_cfg.flags |= I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE;

	nengine = 0;
	if (ring == -1) {
		const struct intel_execution_engine2 *e;

		for_each_ctx_cfg_engine(i915, &q_cfg, e)
			engines[nengine++] = e->flags;
	} else {
		engines[nengine++] = ring;
	}
	igt_require(nengine);

	scratch = gem_create(i915, 4096);

	igt_fork(child, ncpus) {
		unsigned long count = 0;
		const intel_ctx_t *ctx;
		ahnd = get_reloc_ahnd(i915, 0); /* ahnd to same vm */

		hars_petruska_f54_1_random_perturb(child);

		ctx = intel_ctx_create(i915, &q_cfg);
		igt_until_timeout(timeout) {
			int prio;

			prio = hars_petruska_f54_1_random_unsafe_max(MAX_PRIO - MIN_PRIO) + MIN_PRIO;
			gem_context_set_priority(i915, ctx->id, prio);

			engine = engines[hars_petruska_f54_1_random_unsafe_max(nengine)];
			store_dword(i915, ahnd, ctx, engine,
				    scratch, 4096,
				    8*child + 0, ~child,
				    0, 0, 0);
			for (unsigned int step = 0; step < 8; step++)
				store_dword(i915, ahnd, ctx, engine,
					    scratch, 4096,
					    8*child + 4, count++,
					    0, 0, 0);
		}
		intel_ctx_destroy(i915, ctx);
		put_ahnd(ahnd);
	}
	igt_waitchildren();

	ptr = gem_mmap__device_coherent(i915, scratch, 0, 4096, PROT_READ);
	gem_set_domain(i915, scratch, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(i915, scratch);
	put_offset(ahnd, scratch);
	put_ahnd(ahnd);

	for (unsigned n = 0; n < ncpus; n++) {
		igt_assert_eq_u32(ptr[2*n], ~n);
		/*
		 * Note this count is approximate due to unconstrained
		 * ordering of the dword writes between engines.
		 *
		 * Take the result with a pinch of salt.
		 */
		igt_info("Child[%d] completed %u cycles\n",  n, ptr[2*n+1]);
	}
	munmap(ptr, 4096);

	gem_vm_destroy(i915, q_cfg.vm);
}

#define for_each_queue(e, i915, cfg) \
	for_each_ctx_cfg_engine(i915, cfg, e) \
		for_each_if(gem_class_can_store_dword(i915, (e)->class)) \
			igt_dynamic_f("%s", e->name)

igt_main
{
	const struct intel_execution_engine2 *e;
	intel_ctx_cfg_t cfg;
	igt_fd_t(i915);

	igt_fixture {
		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);
		cfg = intel_ctx_cfg_all_physical(i915);
	}

	igt_subtest_group {
		igt_fixture {
			igt_require(gem_has_vm(i915));
			igt_fork_hang_detector(i915);
		}

		igt_subtest("create-shared-gtt")
			create_shared_gtt(i915, 0);

		igt_subtest("detached-shared-gtt")
			create_shared_gtt(i915, DETACHED);

		igt_subtest("disjoint-timelines")
			disjoint_timelines(i915, &cfg);

		igt_subtest("single-timeline")
			single_timeline(i915, &cfg);

		igt_subtest_with_dynamic("exec-shared-gtt") {
			for_each_queue(e, i915, &cfg)
				exec_shared_gtt(i915, &cfg, e->flags);
		}

		igt_subtest_with_dynamic("exec-single-timeline") {
			igt_require(gem_context_has_single_timeline(i915));
			for_each_queue(e, i915, &cfg)
				exec_single_timeline(i915, &cfg, e->flags);
		}

		/*
		 * Check that the shared contexts operate independently,
		 * that is requests on one ("queue") can be scheduled
		 * around another queue. We only check the basics here,
		 * enough to reduce the queue into just another context,
		 * and so rely on gem_exec_schedule to prove the rest.
		 */
		igt_subtest_group {
			igt_fixture {
				igt_require(gem_scheduler_enabled(i915));
				igt_require(gem_scheduler_has_ctx_priority(i915));
				igt_require(gem_has_vm(i915));
				igt_require(gem_context_has_single_timeline(i915));
			}

			igt_subtest_with_dynamic("Q-independent") {
				for_each_queue(e, i915, &cfg)
					independent(i915, &cfg, e, 0);
			}

			igt_subtest_with_dynamic("Q-in-order") {
				for_each_queue(e, i915, &cfg)
					reorder(i915, &cfg, e->flags, EQUAL);
			}

			igt_subtest_with_dynamic("Q-out-order") {
				for_each_queue(e, i915, &cfg)
					reorder(i915, &cfg, e->flags, 0);
			}

			igt_subtest_with_dynamic("Q-promotion") {
				for_each_queue(e, i915, &cfg)
					promotion(i915, &cfg, e->flags);
			}
		}

		igt_subtest_group {
			igt_fixture {
				igt_require(gem_scheduler_enabled(i915));
				igt_require(gem_scheduler_has_ctx_priority(i915));
				igt_require(gem_has_vm(i915));
				igt_require(gem_context_has_single_timeline(i915));
				intel_allocator_multiprocess_start();
			}

			igt_subtest_with_dynamic("Q-smoketest") {
				for_each_queue(e, i915, &cfg)
					smoketest(i915, &cfg, e->flags, 5);
			}

			igt_subtest("Q-smoketest-all")
				smoketest(i915, &cfg, -1, 30);

			igt_fixture {
				intel_allocator_multiprocess_stop();
			}
		}


		igt_subtest("exhaust-shared-gtt")
			exhaust_shared_gtt(i915, 0);

		igt_subtest("exhaust-shared-gtt-lrc")
			exhaust_shared_gtt(i915, EXHAUST_LRC);

		igt_fixture {
			igt_stop_hang_detector();
		}
	}
}
