/*
 * Copyright © 2016 Intel Corporation
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

#include <linux/userfaultfd.h>

#include <pthread.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_vm.h"
#include "igt.h"
#include "igt_power.h"
#include "igt_rand.h"
#include "igt_sysfs.h"
#include "igt_vgem.h"
#include "intel_ctx.h"
#include "sw_sync.h"

#define LO 0
#define HI 1
#define NOISE 2

#define MAX_PRIO I915_CONTEXT_MAX_USER_PRIORITY
#define MIN_PRIO I915_CONTEXT_MIN_USER_PRIORITY

#define MAX_CONTEXTS 1024
#define MAX_ELSP_QLEN 16

#define MI_SEMAPHORE_WAIT		(0x1c << 23)
#define   MI_SEMAPHORE_POLL             (1 << 15)
#define   MI_SEMAPHORE_SAD_GT_SDD       (0 << 12)
#define   MI_SEMAPHORE_SAD_GTE_SDD      (1 << 12)
#define   MI_SEMAPHORE_SAD_LT_SDD       (2 << 12)
#define   MI_SEMAPHORE_SAD_LTE_SDD      (3 << 12)
#define   MI_SEMAPHORE_SAD_EQ_SDD       (4 << 12)
#define   MI_SEMAPHORE_SAD_NEQ_SDD      (5 << 12)

IGT_TEST_DESCRIPTION("Check that we can control the order of execution");

static unsigned int offset_in_page(void *addr)
{
	return (uintptr_t)addr & 4095;
}

static inline
uint32_t __sync_read_u32(int fd, uint32_t handle, uint64_t offset)
{
	uint32_t value;

	gem_set_domain(fd, handle, /* No write hazard lies! */
		       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_read(fd, handle, offset, &value, sizeof(value));

	return value;
}

static inline
void __sync_read_u32_count(int fd, uint32_t handle, uint32_t *dst, uint64_t size)
{
	gem_set_domain(fd, handle, /* No write hazard lies! */
		       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_read(fd, handle, 0, dst, size);
}

static uint32_t __store_dword(int fd, uint64_t ahnd, const intel_ctx_t *ctx,
			      unsigned ring, uint32_t target, uint64_t target_offset,
			      uint32_t offset, uint32_t value,
			      uint32_t cork, uint64_t cork_offset,
			      int fence, unsigned write_domain)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
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

	if (fence != -1) {
		execbuf.flags |= I915_EXEC_FENCE_IN;
		execbuf.rsvd2 = fence;
	}

	memset(obj, 0, sizeof(obj));
	obj[0].handle = cork;
	obj[1].handle = target;
	obj[2].handle = gem_create(fd, 4096);
	if (ahnd) {
		obj[0].offset = cork_offset;
		obj[0].flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
		obj[1].offset = target_offset;
		obj[1].flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
		if (write_domain)
			obj[1].flags |= EXEC_OBJECT_WRITE;
		obj[2].offset = get_offset(ahnd, obj[2].handle, 4096, 0);
		obj[2].flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
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
		batch[++i] = (reloc.presumed_offset + reloc.delta) >> 32;
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
	gem_write(fd, obj[2].handle, 0, batch, sizeof(batch));
	gem_execbuf(fd, &execbuf);

	return obj[2].handle;
}

static void store_dword(int fd, uint64_t ahnd, const intel_ctx_t *ctx,
			unsigned ring,
			uint32_t target, uint64_t target_offset,
			uint32_t offset, uint32_t value,
			unsigned write_domain)
{
	uint32_t batch = __store_dword(fd, ahnd, ctx, ring,
				       target, target_offset, offset, value,
				       0, 0, -1, write_domain);
	gem_close(fd, batch);
	put_offset(ahnd, batch);
}

static void store_dword_plug(int fd, uint64_t ahnd, const intel_ctx_t *ctx,
			     unsigned ring,
			     uint32_t target, uint64_t target_offset,
			     uint32_t offset, uint32_t value,
			     uint32_t cork, uint64_t cork_offset,
			     unsigned write_domain)
{
	uint32_t batch = __store_dword(fd, ahnd, ctx, ring,
				       target, target_offset, offset, value,
				       cork, cork_offset, -1, write_domain);

	gem_close(fd, batch);
	put_offset(ahnd, batch);
}

static void store_dword_fenced(int fd, uint64_t ahnd, const intel_ctx_t *ctx,
			       unsigned ring,
			       uint32_t target, uint64_t target_offset,
			       uint32_t offset, uint32_t value,
			       int fence, unsigned write_domain)
{
	uint32_t batch = __store_dword(fd, ahnd, ctx, ring,
				       target, target_offset, offset, value,
				       0, 0, fence, write_domain);

	gem_close(fd, batch);
	put_offset(ahnd, batch);
}

static const intel_ctx_t *
create_highest_priority(int fd, const intel_ctx_cfg_t *cfg)
{
	const intel_ctx_t *ctx = intel_ctx_create(fd, cfg);

	/*
	 * If there is no priority support, all contexts will have equal
	 * priority (and therefore the max user priority), so no context
	 * can overtake us, and we effectively can form a plug.
	 */
	__gem_context_set_priority(fd, ctx->id, MAX_PRIO);

	return ctx;
}

static void unplug_show_queue(int fd, struct igt_cork *c,
			      const intel_ctx_cfg_t *cfg,
			      unsigned int engine)
{
	igt_spin_t *spin[MAX_ELSP_QLEN];
	int max = MAX_ELSP_QLEN;

	/* If no scheduler, all batches are emitted in submission order */
	if (!gem_scheduler_enabled(fd))
		max = 1;

	for (int n = 0; n < max; n++) {
		const intel_ctx_t *ctx = create_highest_priority(fd, cfg);
		uint64_t ahnd = get_reloc_ahnd(fd, ctx->id);

		spin[n] = __igt_spin_new(fd, .ahnd = ahnd, .ctx = ctx,
					 .engine = engine);
		intel_ctx_destroy(fd, ctx);
	}

	igt_cork_unplug(c); /* batches will now be queued on the engine */
	igt_debugfs_dump(fd, "i915_engine_info");

	/* give time to the kernel to complete the queueing */
	usleep(25000);

	for (int n = 0; n < max; n++) {
		uint64_t ahnd = spin[n]->opts.ahnd;
		igt_spin_free(fd, spin[n]);
		put_ahnd(ahnd);
	}

}

static void fifo(int fd, const intel_ctx_t *ctx, unsigned ring)
{
	IGT_CORK_FENCE(cork);
	uint32_t scratch;
	uint32_t result;
	int fence;
	uint64_t ahnd = get_reloc_ahnd(fd, ctx->id), scratch_offset;

	scratch = gem_create(fd, 4096);
	scratch_offset = get_offset(ahnd, scratch, 4096, 0);

	fence = igt_cork_plug(&cork, fd);

	/* Same priority, same timeline, final result will be the second eb */
	store_dword_fenced(fd, ahnd, ctx, ring, scratch, scratch_offset,
			   0, 1, fence, 0);
	store_dword_fenced(fd, ahnd, ctx, ring, scratch, scratch_offset,
			   0, 2, fence, 0);

	unplug_show_queue(fd, &cork, &ctx->cfg, ring);
	close(fence);

	result =  __sync_read_u32(fd, scratch, 0);
	gem_close(fd, scratch);
	put_offset(ahnd, scratch);
	put_ahnd(ahnd);

	igt_assert_eq_u32(result, 2);
}

enum implicit_dir {
	READ_WRITE = 0x1,
	WRITE_READ = 0x2,
};

static void implicit_rw(int i915, const intel_ctx_t *ctx, unsigned int ring,
			enum implicit_dir dir)
{
	const struct intel_execution_engine2 *e;
	IGT_CORK_FENCE(cork);
	unsigned int count;
	uint32_t scratch;
	uint32_t result;
	int fence;
	uint64_t ahnd = get_reloc_ahnd(i915, ctx->id), scratch_offset;

	count = 0;
	for_each_ctx_engine(i915, ctx, e) {
		if (e->flags == ring)
			continue;

		if (!gem_class_can_store_dword(i915, e->class))
			continue;

		count++;
	}
	igt_require(count);

	scratch = gem_create(i915, 4096);
	scratch_offset = get_offset(ahnd, scratch, 4096, 0);
	fence = igt_cork_plug(&cork, i915);

	if (dir & WRITE_READ)
		store_dword_fenced(i915, ahnd, ctx,
				   ring, scratch, scratch_offset, 0, ~ring,
				   fence, I915_GEM_DOMAIN_RENDER);

	for_each_ctx_engine(i915, ctx, e) {
		if (e->flags == ring)
			continue;

		if (!gem_class_can_store_dword(i915, e->class))
			continue;

		store_dword_fenced(i915, ahnd, ctx,
				   e->flags, scratch, scratch_offset, 0, e->flags,
				   fence, 0);
	}

	if (dir & READ_WRITE)
		store_dword_fenced(i915, ahnd, ctx,
				   ring, scratch, scratch_offset, 0, ring,
				   fence, I915_GEM_DOMAIN_RENDER);

	unplug_show_queue(i915, &cork, &ctx->cfg, ring);
	close(fence);

	result =  __sync_read_u32(i915, scratch, 0);
	gem_close(i915, scratch);
	put_offset(ahnd, scratch);
	put_ahnd(ahnd);

	if (dir & WRITE_READ)
		igt_assert_neq_u32(result, ~ring);
	if (dir & READ_WRITE)
		igt_assert_eq_u32(result, ring);
}

static void independent(int fd, const intel_ctx_t *ctx, unsigned int engine,
			unsigned long flags)
{
	const struct intel_execution_engine2 *e;
	IGT_CORK_FENCE(cork);
	igt_spin_t *spin = NULL;
	uint32_t scratch, batch;
	uint32_t *ptr;
	int fence;
	uint64_t ahnd = get_reloc_ahnd(fd, ctx->id), scratch_offset;

	scratch = gem_create(fd, 4096);
	scratch_offset = get_offset(ahnd, scratch, 4096, 0);
	ptr = gem_mmap__device_coherent(fd, scratch, 0, 4096, PROT_READ);
	igt_assert_eq(ptr[0], 0);

	fence = igt_cork_plug(&cork, fd);

	/* Check that we can submit to engine while all others are blocked */
	for_each_ctx_engine(fd, ctx, e) {
		if (e->flags == engine)
			continue;

		if (!gem_class_can_store_dword(fd, e->class))
			continue;

		if (spin == NULL) {
			spin = __igt_spin_new(fd,
					      .ahnd = ahnd,
					      .ctx = ctx,
					      .engine = e->flags,
					      .flags = flags);
		} else {
			struct drm_i915_gem_execbuffer2 eb = {
				.buffer_count = 1,
				.buffers_ptr = to_user_pointer(&spin->obj[IGT_SPIN_BATCH]),
				.rsvd1 = ctx->id,
				.flags = e->flags,
			};
			gem_execbuf(fd, &eb);
		}

		store_dword_fenced(fd, ahnd, ctx, e->flags,
				   scratch, scratch_offset,
				   0, e->flags, fence, 0);
	}
	igt_require(spin);

	/* Same priority, but different timeline (as different engine) */
	batch = __store_dword(fd, ahnd, ctx, engine, scratch, scratch_offset,
			      0, engine, 0, 0, fence, 0);

	unplug_show_queue(fd, &cork, &ctx->cfg, engine);
	close(fence);

	gem_sync(fd, batch);
	igt_assert(!gem_bo_busy(fd, batch));
	igt_assert(gem_bo_busy(fd, spin->handle));
	gem_close(fd, batch);

	/* Only the local engine should be free to complete. */
	igt_assert(gem_bo_busy(fd, scratch));
	igt_assert_eq(ptr[0], engine);

	igt_spin_free(fd, spin);
	gem_quiescent_gpu(fd);
	put_offset(ahnd, batch);
	put_offset(ahnd, scratch);
	put_ahnd(ahnd);

	/* And we expect the others to have overwritten us, order unspecified */
	igt_assert(!gem_bo_busy(fd, scratch));
	igt_assert_neq(ptr[0], engine);

	munmap(ptr, 4096);
	gem_close(fd, scratch);
}

static void smoketest(int fd, const intel_ctx_cfg_t *cfg,
		      unsigned ring, unsigned timeout)
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	const struct intel_execution_engine2 *e;
	unsigned engines[GEM_MAX_ENGINES];
	unsigned nengine;
	unsigned engine;
	uint32_t scratch;
	uint32_t result[2 * ncpus];
	uint64_t scratch_offset;

	nengine = 0;
	if (ring == ALL_ENGINES) {
		for_each_ctx_cfg_engine(fd, cfg, e)
			if (gem_class_can_store_dword(fd, e->class))
				engines[nengine++] = e->flags;
	} else {
		engines[nengine++] = ring;
	}
	igt_require(nengine);

	scratch = gem_create(fd, 4096);

	igt_fork(child, ncpus) {
		unsigned long count = 0;
		const intel_ctx_t *ctx;
		uint64_t ahnd;

		intel_allocator_init();

		hars_petruska_f54_1_random_perturb(child);

		ctx = intel_ctx_create(fd, cfg);
		ahnd = get_reloc_ahnd(fd, ctx->id);
		scratch_offset = get_offset(ahnd, scratch, 4096, 0);
		igt_until_timeout(timeout) {
			int prio;

			prio = hars_petruska_f54_1_random_unsafe_max(MAX_PRIO - MIN_PRIO) + MIN_PRIO;
			gem_context_set_priority(fd, ctx->id, prio);

			engine = engines[hars_petruska_f54_1_random_unsafe_max(nengine)];
			store_dword(fd, ahnd, ctx, engine,
				    scratch, scratch_offset,
				    8*child + 0, ~child, 0);
			for (unsigned int step = 0; step < 8; step++)
				store_dword(fd, ahnd, ctx, engine,
					    scratch, scratch_offset,
					    8*child + 4, count++,
					    0);
		}
		intel_ctx_destroy(fd, ctx);
		put_offset(ahnd, scratch);
		put_ahnd(ahnd);
	}
	igt_waitchildren();

	__sync_read_u32_count(fd, scratch, result, sizeof(result));
	gem_close(fd, scratch);

	for (unsigned n = 0; n < ncpus; n++) {
		igt_assert_eq_u32(result[2 * n], ~n);
		/*
		 * Note this count is approximate due to unconstrained
		 * ordering of the dword writes between engines.
		 *
		 * Take the result with a pinch of salt.
		 */
		igt_info("Child[%d] completed %u cycles\n",  n, result[(2 * n) + 1]);
	}
}

static uint32_t timeslicing_batches(int i915, uint32_t *offset)
{
        uint32_t handle = gem_create(i915, 4096);
        uint32_t cs[256];

	*offset += 4000;
	for (int pair = 0; pair <= 1; pair++) {
		int x = 1;
		int i = 0;

		for (int step = 0; step < 8; step++) {
			if (pair) {
				cs[i++] =
					MI_SEMAPHORE_WAIT |
					MI_SEMAPHORE_POLL |
					MI_SEMAPHORE_SAD_EQ_SDD |
					(4 - 2);
				cs[i++] = x++;
				cs[i++] = *offset;
				cs[i++] = 0;
			}

			cs[i++] = MI_STORE_DWORD_IMM;
			cs[i++] = *offset;
			cs[i++] = 0;
			cs[i++] = x++;

			if (!pair) {
				cs[i++] =
					MI_SEMAPHORE_WAIT |
					MI_SEMAPHORE_POLL |
					MI_SEMAPHORE_SAD_EQ_SDD |
					(4 - 2);
				cs[i++] = x++;
				cs[i++] = *offset;
				cs[i++] = 0;
			}
		}

		cs[i++] = MI_BATCH_BUFFER_END;
		igt_assert(i < ARRAY_SIZE(cs));
		gem_write(i915, handle, pair * sizeof(cs), cs, sizeof(cs));
	}

	*offset = sizeof(cs);
        return handle;
}

static void timeslice(int i915, const intel_ctx_cfg_t *cfg,
		      unsigned int engine)
{
	unsigned int offset = 24 << 20;
	struct drm_i915_gem_exec_object2 obj = {
		.offset = offset,
		.flags = EXEC_OBJECT_PINNED,
	};
	struct drm_i915_gem_execbuffer2 execbuf  = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
	};
	const intel_ctx_t *ctx[2];
	uint32_t *result;
	int out;

	/*
	 * Create a pair of interlocking batches, that ping pong
	 * between each other, and only advance one step at a time.
	 * We require the kernel to preempt at each semaphore and
	 * switch to the other batch in order to advance.
	 */

	igt_require(gem_scheduler_has_timeslicing(i915));
	igt_require(intel_gen(intel_get_drm_devid(i915)) >= 8);

	ctx[0] = intel_ctx_create(i915, cfg);
	obj.handle = timeslicing_batches(i915, &offset);
	result = gem_mmap__device_coherent(i915, obj.handle, 0, 4096, PROT_READ);

	execbuf.flags = engine | I915_EXEC_FENCE_OUT;
	execbuf.batch_start_offset = 0;
	execbuf.rsvd1 = ctx[0]->id;
	gem_execbuf_wr(i915, &execbuf);
	intel_ctx_destroy(i915, ctx[0]);

	/* No coupling between requests; free to timeslice */

	ctx[1] = intel_ctx_create(i915, cfg);
	execbuf.rsvd1 = ctx[1]->id;
	execbuf.rsvd2 >>= 32;
	execbuf.flags = engine | I915_EXEC_FENCE_OUT;
	execbuf.batch_start_offset = offset;
	gem_execbuf_wr(i915, &execbuf);
	intel_ctx_destroy(i915, ctx[1]);

	gem_sync(i915, obj.handle);
	gem_close(i915, obj.handle);

	/* no hangs! */
	out = execbuf.rsvd2;
	igt_assert_eq(sync_fence_status(out), 1);
	close(out);

	out = execbuf.rsvd2 >> 32;
	igt_assert_eq(sync_fence_status(out), 1);
	close(out);

	igt_assert_eq(result[1000], 16);
	munmap(result, 4096);
}

static uint32_t timesliceN_batches(int i915, uint32_t offset, int count)
{
        uint32_t handle = gem_create(i915, (count + 1) * 1024);
        uint32_t cs[256];

	for (int pair = 0; pair < count; pair++) {
		int x = pair;
		int i = 0;

		for (int step = 0; step < 8; step++) {
			cs[i++] =
				MI_SEMAPHORE_WAIT |
				MI_SEMAPHORE_POLL |
				MI_SEMAPHORE_SAD_EQ_SDD |
				(4 - 2);
			cs[i++] = x;
			cs[i++] = offset;
			cs[i++] = 0;

			cs[i++] = MI_STORE_DWORD_IMM;
			cs[i++] = offset;
			cs[i++] = 0;
			cs[i++] = x + 1;

			x += count;
		}

		cs[i++] = MI_BATCH_BUFFER_END;
		igt_assert(i < ARRAY_SIZE(cs));
		gem_write(i915, handle, (pair + 1) * sizeof(cs),
			  cs, sizeof(cs));
	}

        return handle;
}

static void timesliceN(int i915, const intel_ctx_cfg_t *cfg,
		       unsigned int engine, int count)
{
	const unsigned int sz = ALIGN((count + 1) * 1024, 4096);
	unsigned int offset = 24 << 20;
	struct drm_i915_gem_exec_object2 obj = {
		.handle = timesliceN_batches(i915, offset, count),
		.offset = offset,
		.flags = EXEC_OBJECT_PINNED,
	};
	struct drm_i915_gem_execbuffer2 execbuf  = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = engine | I915_EXEC_FENCE_OUT,
	};
	uint32_t *result =
		gem_mmap__device_coherent(i915, obj.handle, 0, sz, PROT_READ);
	const intel_ctx_t *ctx;
	int fence[count];

	/*
	 * Create a pair of interlocking batches, that ping pong
	 * between each other, and only advance one step at a time.
	 * We require the kernel to preempt at each semaphore and
	 * switch to the other batch in order to advance.
	 */

	igt_require(gem_scheduler_has_timeslicing(i915));
	igt_require(intel_gen(intel_get_drm_devid(i915)) >= 8);

	/* No coupling between requests; free to timeslice */

	for (int i = 0; i < count; i++) {
		ctx = intel_ctx_create(i915, cfg);
		execbuf.rsvd1 = ctx->id;
		execbuf.batch_start_offset = (i + 1) * 1024;;
		gem_execbuf_wr(i915, &execbuf);
		intel_ctx_destroy(i915, ctx);

		fence[i] = execbuf.rsvd2 >> 32;
	}

	gem_sync(i915, obj.handle);
	gem_close(i915, obj.handle);

	/* no hangs! */
	for (int i = 0; i < count; i++) {
		igt_assert_eq(sync_fence_status(fence[i]), 1);
		close(fence[i]);
	}

	igt_assert_eq(*result, 8 * count);
	munmap(result, sz);
}

static void lateslice(int i915, const intel_ctx_cfg_t *cfg,
		      unsigned int engine, unsigned long flags)
{
	const intel_ctx_t *ctx;
	igt_spin_t *spin[3];
	uint64_t ahnd[3];

	igt_require(gem_scheduler_has_timeslicing(i915));
	igt_require(intel_gen(intel_get_drm_devid(i915)) >= 8);

	ctx = intel_ctx_create(i915, cfg);
	ahnd[0] = get_reloc_ahnd(i915, ctx->id);
	spin[0] = igt_spin_new(i915, .ahnd = ahnd[0], .ctx = ctx,
			       .engine = engine,
			       .flags = (IGT_SPIN_POLL_RUN |
					 IGT_SPIN_FENCE_OUT |
					 flags));
	intel_ctx_destroy(i915, ctx);

	igt_spin_busywait_until_started(spin[0]);

	ctx = intel_ctx_create(i915, cfg);
	ahnd[1] = get_reloc_ahnd(i915, ctx->id);
	spin[1] = igt_spin_new(i915, .ahnd = ahnd[1], .ctx = ctx,
			       .engine = engine,
			       .fence = spin[0]->out_fence,
			       .flags = (IGT_SPIN_POLL_RUN |
					 IGT_SPIN_FENCE_IN |
					 flags));
	intel_ctx_destroy(i915, ctx);

	usleep(5000); /* give some time for the new spinner to be scheduled */

	/*
	 * Now that we have two spinners in the HW submission queue [ELSP],
	 * and since they are strictly ordered, the timeslicing timer may
	 * be disabled as no reordering is possible. However, upon adding a
	 * third spinner we then expect timeslicing to be real enabled.
	 */

	ctx = intel_ctx_create(i915, cfg);
	ahnd[2] = get_reloc_ahnd(i915, ctx->id);
	spin[2] = igt_spin_new(i915, .ahnd = ahnd[2], .ctx = ctx,
			       .engine = engine,
			       .flags = IGT_SPIN_POLL_RUN | flags);
	intel_ctx_destroy(i915, ctx);

	igt_spin_busywait_until_started(spin[2]);

	igt_assert(gem_bo_busy(i915, spin[0]->handle));
	igt_assert(gem_bo_busy(i915, spin[1]->handle));
	igt_assert(gem_bo_busy(i915, spin[2]->handle));

	igt_assert(!igt_spin_has_started(spin[1]));
	igt_spin_free(i915, spin[0]);

	/* Now just spin[1] and spin[2] active */
	igt_spin_busywait_until_started(spin[1]);

	igt_assert(gem_bo_busy(i915, spin[2]->handle));
	igt_spin_free(i915, spin[2]);

	igt_assert(gem_bo_busy(i915, spin[1]->handle));
	igt_spin_free(i915, spin[1]);

	for (int i = 0; i < ARRAY_SIZE(ahnd); i++)
		put_ahnd(ahnd[i]);
}

static void cancel_spinner(int i915,
			   const intel_ctx_t *ctx, unsigned int engine,
			   igt_spin_t *spin)
{
	struct drm_i915_gem_exec_object2 obj = {
		.handle = gem_create(i915, 4096),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = engine | I915_EXEC_FENCE_SUBMIT,
		.rsvd1 = ctx->id, /* same vm */
		.rsvd2 = spin->out_fence,
	};
	uint32_t *map, *cs;

	map = gem_mmap__device_coherent(i915, obj.handle, 0, 4096, PROT_WRITE);
	cs = map;

	*cs++ = MI_STORE_DWORD_IMM;
	*cs++ = spin->obj[IGT_SPIN_BATCH].offset +
		offset_in_page(spin->condition);
	*cs++ = spin->obj[IGT_SPIN_BATCH].offset >> 32;
	*cs++ = MI_BATCH_BUFFER_END;

	*cs++ = MI_BATCH_BUFFER_END;
	munmap(map, 4096);

	gem_execbuf(i915, &execbuf);
	gem_close(i915, obj.handle);
}

static void submit_slice(int i915, const intel_ctx_cfg_t *cfg,
			 const struct intel_execution_engine2 *e,
			 unsigned int flags)
#define EARLY_SUBMIT 0x1
#define LATE_SUBMIT 0x2
#define USERPTR 0x4
{
	const struct intel_execution_engine2 *cancel;
	intel_ctx_cfg_t engine_cfg = {
		.num_engines = 1,
	};
	const intel_ctx_t *ctx, *bg_ctx;
	uint64_t ahnd, bg_ahnd;

	/*
	 * When using a submit fence, we do not want to block concurrent work,
	 * especially when that work is coperating with the spinner.
	 */

	igt_require(gem_scheduler_has_timeslicing(i915));
	igt_require(intel_gen(intel_get_drm_devid(i915)) >= 8);
	igt_require(gem_has_vm(i915));

	engine_cfg.vm = gem_vm_create(i915);
	ahnd = intel_allocator_open_vm(i915, engine_cfg.vm, INTEL_ALLOCATOR_RELOC);
	bg_ctx = intel_ctx_create(i915, cfg);
	bg_ahnd = get_reloc_ahnd(i915, bg_ctx->id);

	for_each_ctx_cfg_engine(i915, cfg, cancel) {
		igt_spin_t *bg, *spin;
		int timeline = -1;
		int fence = -1;

		if (!gem_class_can_store_dword(i915, cancel->class))
			continue;

		igt_debug("Testing cancellation from %s\n", e->name);

		bg = igt_spin_new(i915, .ahnd = bg_ahnd, .ctx = bg_ctx,
				  .engine = e->flags);

		if (flags & LATE_SUBMIT) {
			timeline = sw_sync_timeline_create();
			fence = sw_sync_timeline_create_fence(timeline, 1);
		}

		engine_cfg.engines[0].engine_class = e->class;
		engine_cfg.engines[0].engine_instance = e->instance;
		ctx = intel_ctx_create(i915, &engine_cfg);
		spin = igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
				    .fence = fence,
				    .flags =
				    IGT_SPIN_POLL_RUN |
				    (flags & LATE_SUBMIT ? IGT_SPIN_FENCE_IN : 0) |
				    (flags & USERPTR ? IGT_SPIN_USERPTR : 0) |
				    IGT_SPIN_FENCE_OUT);
		if (fence != -1)
			close(fence);

		if (flags & EARLY_SUBMIT)
			igt_spin_busywait_until_started(spin);

		intel_ctx_destroy(i915, ctx);

		engine_cfg.engines[0].engine_class = cancel->class;
		engine_cfg.engines[0].engine_instance = cancel->instance;
		ctx = intel_ctx_create(i915, &engine_cfg);

		cancel_spinner(i915, ctx, 0, spin);

		if (timeline != -1)
			close(timeline);

		gem_sync(i915, spin->handle);
		igt_spin_free(i915, spin);
		igt_spin_free(i915, bg);

		intel_ctx_destroy(i915, ctx);
	}

	gem_vm_destroy(i915, engine_cfg.vm);
	intel_ctx_destroy(i915, bg_ctx);
	put_ahnd(bg_ahnd);
	put_ahnd(ahnd);
}

static uint32_t __batch_create(int i915, uint32_t offset)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle;

	handle = gem_create(i915, ALIGN(offset + 4, 4096));
	gem_write(i915, handle, offset, &bbe, sizeof(bbe));

	return handle;
}

static uint32_t batch_create(int i915)
{
	return __batch_create(i915, 0);
}

static void semaphore_userlock(int i915, const intel_ctx_t *ctx,
			       unsigned long flags)
{
	const struct intel_execution_engine2 *e;
	struct drm_i915_gem_exec_object2 obj = {
		.handle = batch_create(i915),
	};
	igt_spin_t *spin = NULL;
	uint32_t scratch;
	const intel_ctx_t *tmp_ctx;
	uint64_t ahnd = get_reloc_ahnd(i915, ctx->id);

	igt_require(gem_scheduler_has_timeslicing(i915));

	/*
	 * Given the use of semaphores to govern parallel submission
	 * of nearly-ready work to HW, we still want to run actually
	 * ready work immediately. Without semaphores, the dependent
	 * work wouldn't be submitted so our ready work will run.
	 */

	scratch = gem_create(i915, 4096);
	for_each_ctx_engine(i915, ctx, e) {
		if (!spin) {
			spin = igt_spin_new(i915,
					    .ahnd = ahnd,
					    .ctx = ctx,
					    .dependency = scratch,
					    .engine = e->flags,
					    .flags = flags);
		} else {
			uint64_t saved = spin->execbuf.flags;

			spin->execbuf.flags &= ~I915_EXEC_RING_MASK;
			spin->execbuf.flags |= e->flags;

			gem_execbuf(i915, &spin->execbuf);

			spin->execbuf.flags = saved;
		}
	}
	igt_require(spin);
	gem_close(i915, scratch);

	/*
	 * On all dependent engines, the request may be executing (busywaiting
	 * on a HW semaphore) but it should not prevent any real work from
	 * taking precedence.
	 */
	tmp_ctx = intel_ctx_create(i915, &ctx->cfg);
	for_each_ctx_engine(i915, ctx, e) {
		struct drm_i915_gem_execbuffer2 execbuf = {
			.buffers_ptr = to_user_pointer(&obj),
			.buffer_count = 1,
			.flags = e->flags,
			.rsvd1 = tmp_ctx->id,
		};

		if (e->flags == (spin->execbuf.flags & I915_EXEC_RING_MASK))
			continue;

		gem_execbuf(i915, &execbuf);
	}
	intel_ctx_destroy(i915, tmp_ctx);
	gem_sync(i915, obj.handle); /* to hang unless we can preempt */
	gem_close(i915, obj.handle);

	igt_spin_free(i915, spin);
	put_ahnd(ahnd);
}

static void semaphore_codependency(int i915, const intel_ctx_t *ctx,
				   unsigned long flags)
{
	const struct intel_execution_engine2 *e;
	struct {
		igt_spin_t *xcs, *rcs;
	} task[2];
	uint64_t ahnd;
	int i;

	/*
	 * Consider two tasks, task A runs on (xcs0, rcs0) and task B
	 * on (xcs1, rcs0). That is they must both run a dependent
	 * batch on rcs0, after first running in parallel on separate
	 * engines. To maximise throughput, we want the shorter xcs task
	 * to start on rcs first. However, if we insert semaphores we may
	 * pick wrongly and end up running the requests in the least
	 * optimal order.
	 */

	i = 0;
	for_each_ctx_engine(i915, ctx, e) {
		const intel_ctx_t *tmp_ctx;

		if (!e->flags) {
			igt_require(gem_class_can_store_dword(i915, e->class));
			continue;
		}

		if (!gem_class_can_store_dword(i915, e->class))
			continue;

		tmp_ctx = intel_ctx_create(i915, &ctx->cfg);
		ahnd = get_simple_l2h_ahnd(i915, tmp_ctx->id);

		task[i].xcs =
			__igt_spin_new(i915,
				       .ahnd = ahnd,
				       .ctx = tmp_ctx,
				       .engine = e->flags,
				       .flags = IGT_SPIN_POLL_RUN | flags);
		igt_spin_busywait_until_started(task[i].xcs);

		/* Common rcs tasks will be queued in FIFO */
		task[i].rcs =
			__igt_spin_new(i915,
				       .ahnd = ahnd,
				       .ctx = tmp_ctx,
				       .engine = 0,
				       .dependency = task[i].xcs->handle);

		intel_ctx_destroy(i915, tmp_ctx);

		if (++i == ARRAY_SIZE(task))
			break;
	}
	igt_require(i == ARRAY_SIZE(task));

	/* Since task[0] was queued first, it will be first in queue for rcs */
	igt_spin_end(task[1].xcs);
	igt_spin_end(task[1].rcs);
	gem_sync(i915, task[1].rcs->handle); /* to hang if task[0] hogs rcs */

	for (i = 0; i < ARRAY_SIZE(task); i++) {
		igt_spin_end(task[i].xcs);
		igt_spin_end(task[i].rcs);
	}

	for (i = 0; i < ARRAY_SIZE(task); i++) {
		ahnd = task[i].rcs->opts.ahnd;
		igt_spin_free(i915, task[i].xcs);
		igt_spin_free(i915, task[i].rcs);
		put_ahnd(ahnd);
	}
}

static void semaphore_resolve(int i915, const intel_ctx_cfg_t *cfg,
			      unsigned long flags)
{
	const struct intel_execution_engine2 *e;
	const uint32_t SEMAPHORE_ADDR = 64 << 10;
	uint32_t semaphore, *sema;
	const intel_ctx_t *spin_ctx, *outer, *inner;
	uint64_t ahnd = get_reloc_ahnd(i915, 0);

	/*
	 * Userspace may submit batches that wait upon unresolved
	 * semaphores. Ideally, we want to put those blocking batches
	 * to the back of the execution queue if we have something else
	 * that is ready to run right away. This test exploits a failure
	 * to reorder batches around a blocking semaphore by submitting
	 * the release of that semaphore from a later context.
	 */

	igt_require(gem_scheduler_has_preemption(i915));
	igt_require(intel_get_drm_devid(i915) >= 8); /* for MI_SEMAPHORE_WAIT */

	spin_ctx = intel_ctx_create(i915, cfg);
	outer = intel_ctx_create(i915, cfg);
	inner = intel_ctx_create(i915, cfg);

	semaphore = gem_create(i915, 4096);
	sema = gem_mmap__device_coherent(i915, semaphore, 0, 4096, PROT_WRITE);

	for_each_ctx_cfg_engine(i915, cfg, e) {
		struct drm_i915_gem_exec_object2 obj[3];
		struct drm_i915_gem_execbuffer2 eb;
		uint32_t handle, cancel;
		uint32_t *cs, *map;
		igt_spin_t *spin;
		int64_t poke = 1;

		if (!gem_class_can_store_dword(i915, e->class))
			continue;

		spin = __igt_spin_new(i915, .ahnd = ahnd, .ctx = spin_ctx,
				      .engine = e->flags, .flags = flags);
		igt_spin_end(spin); /* we just want its address for later */
		gem_sync(i915, spin->handle);
		igt_spin_reset(spin);

		handle = gem_create(i915, 4096);
		cs = map = gem_mmap__cpu(i915, handle, 0, 4096, PROT_WRITE);

		/* Set semaphore initially to 1 for polling and signaling */
		*cs++ = MI_STORE_DWORD_IMM;
		*cs++ = SEMAPHORE_ADDR;
		*cs++ = 0;
		*cs++ = 1;

		/* Wait until another batch writes to our semaphore */
		*cs++ = MI_SEMAPHORE_WAIT |
			MI_SEMAPHORE_POLL |
			MI_SEMAPHORE_SAD_EQ_SDD |
			(4 - 2);
		*cs++ = 0;
		*cs++ = SEMAPHORE_ADDR;
		*cs++ = 0;

		/* Then cancel the spinner */
		*cs++ = MI_STORE_DWORD_IMM;
		*cs++ = spin->obj[IGT_SPIN_BATCH].offset +
			offset_in_page(spin->condition);
		*cs++ = 0;
		*cs++ = MI_BATCH_BUFFER_END;

		*cs++ = MI_BATCH_BUFFER_END;
		munmap(map, 4096);

		memset(&eb, 0, sizeof(eb));

		/* First up is our spinning semaphore */
		memset(obj, 0, sizeof(obj));
		obj[0] = spin->obj[IGT_SPIN_BATCH];
		obj[1].handle = semaphore;
		obj[1].offset = SEMAPHORE_ADDR;
		obj[1].flags = EXEC_OBJECT_PINNED;
		obj[2].handle = handle;
		eb.buffer_count = 3;
		eb.buffers_ptr = to_user_pointer(obj);
		eb.rsvd1 = outer->id;
		gem_execbuf(i915, &eb);

		/* Then add the GPU hang intermediatory */
		memset(obj, 0, sizeof(obj));
		obj[0].handle = handle;
		obj[0].flags = EXEC_OBJECT_WRITE; /* always after semaphore */
		obj[1] = spin->obj[IGT_SPIN_BATCH];
		eb.buffer_count = 2;
		eb.rsvd1 = 0;
		gem_execbuf(i915, &eb);

		while (READ_ONCE(*sema) == 0)
			;

		/* Now the semaphore is spinning, cancel it */
		cancel = gem_create(i915, 4096);
		cs = map = gem_mmap__cpu(i915, cancel, 0, 4096, PROT_WRITE);
		*cs++ = MI_STORE_DWORD_IMM;
		*cs++ = SEMAPHORE_ADDR;
		*cs++ = 0;
		*cs++ = 0;
		*cs++ = MI_BATCH_BUFFER_END;
		munmap(map, 4096);

		memset(obj, 0, sizeof(obj));
		obj[0].handle = semaphore;
		obj[0].offset = SEMAPHORE_ADDR;
		obj[0].flags = EXEC_OBJECT_PINNED;
		obj[1].handle = cancel;
		eb.buffer_count = 2;
		eb.rsvd1 = inner->id;
		gem_execbuf(i915, &eb);
		gem_wait(i915, cancel, &poke); /* match sync's WAIT_PRIORITY */
		gem_close(i915, cancel);

		gem_sync(i915, handle); /* To hang unless cancel runs! */
		gem_close(i915, handle);
		igt_spin_free(i915, spin);

		igt_assert_eq(*sema, 0);
	}

	munmap(sema, 4096);
	gem_close(i915, semaphore);

	intel_ctx_destroy(i915, inner);
	intel_ctx_destroy(i915, outer);
	intel_ctx_destroy(i915, spin_ctx);
	put_ahnd(ahnd);
}

static void semaphore_noskip(int i915, const intel_ctx_cfg_t *cfg,
			     unsigned long flags)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	const struct intel_execution_engine2 *outer, *inner;
	const intel_ctx_t *ctx0, *ctx1;
	uint64_t ahnd;

	igt_require(gen >= 6); /* MI_STORE_DWORD_IMM convenience */

	ctx0 = intel_ctx_create(i915, cfg);
	ctx1 = intel_ctx_create(i915, cfg);
	ahnd = get_reloc_ahnd(i915, ctx0->id);

	for_each_ctx_engine(i915, ctx0, outer) {
	for_each_ctx_engine(i915, ctx0, inner) {
		struct drm_i915_gem_exec_object2 obj[3];
		struct drm_i915_gem_execbuffer2 eb;
		uint32_t handle, *cs, *map;
		igt_spin_t *chain, *spin;

		if (inner->flags == outer->flags ||
		    !gem_class_can_store_dword(i915, inner->class))
			continue;

		chain = __igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx0,
				       .engine = outer->flags, .flags = flags);

		spin = __igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx0,
				      .engine = inner->flags, .flags = flags);
		igt_spin_end(spin); /* we just want its address for later */
		gem_sync(i915, spin->handle);
		igt_spin_reset(spin);

		handle = gem_create(i915, 4096);
		cs = map = gem_mmap__cpu(i915, handle, 0, 4096, PROT_WRITE);

		/* Cancel the following spinner */
		*cs++ = MI_STORE_DWORD_IMM;
		if (gen >= 8) {
			*cs++ = spin->obj[IGT_SPIN_BATCH].offset +
				offset_in_page(spin->condition);
			*cs++ = 0;
		} else {
			*cs++ = 0;
			*cs++ = spin->obj[IGT_SPIN_BATCH].offset +
				offset_in_page(spin->condition);
		}
		*cs++ = MI_BATCH_BUFFER_END;

		*cs++ = MI_BATCH_BUFFER_END;
		munmap(map, 4096);

		/* port0: implicit semaphore from engine */
		memset(obj, 0, sizeof(obj));
		obj[0] = chain->obj[IGT_SPIN_BATCH];
		obj[0].flags |= EXEC_OBJECT_WRITE;
		obj[1] = spin->obj[IGT_SPIN_BATCH];
		obj[2].handle = handle;
		memset(&eb, 0, sizeof(eb));
		eb.buffer_count = 3;
		eb.buffers_ptr = to_user_pointer(obj);
		eb.rsvd1 = ctx1->id;
		eb.flags = inner->flags;
		gem_execbuf(i915, &eb);

		/* port1: dependency chain from port0 */
		memset(obj, 0, sizeof(obj));
		obj[0].handle = handle;
		obj[0].flags = EXEC_OBJECT_WRITE;
		obj[1] = spin->obj[IGT_SPIN_BATCH];
		memset(&eb, 0, sizeof(eb));
		eb.buffer_count = 2;
		eb.buffers_ptr = to_user_pointer(obj);
		eb.flags = inner->flags;
		eb.rsvd1 = ctx0->id;
		gem_execbuf(i915, &eb);

		igt_spin_set_timeout(chain, NSEC_PER_SEC / 100);
		gem_sync(i915, spin->handle); /* To hang unless cancel runs! */

		gem_close(i915, handle);
		igt_spin_free(i915, spin);
		igt_spin_free(i915, chain);
	}
	}

	intel_ctx_destroy(i915, ctx0);
	intel_ctx_destroy(i915, ctx1);
	put_ahnd(ahnd);
}

static void
noreorder(int i915, const intel_ctx_cfg_t *cfg,
	  unsigned int engine, int prio, unsigned int flags)
#define CORKED 0x1
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	const struct intel_execution_engine2 *e;
	struct drm_i915_gem_exec_object2 obj = {
		.handle = gem_create(i915, 4096),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = engine,
	};
	intel_ctx_cfg_t vm_cfg = *cfg;
	const intel_ctx_t *ctx;
	IGT_CORK_FENCE(cork);
	uint32_t *map, *cs;
	igt_spin_t *slice;
	igt_spin_t *spin;
	int fence = -1;
	uint64_t addr;
	uint64_t ahnd[2];

	if (flags & CORKED)
		fence = igt_cork_plug(&cork, i915);

	if (gem_uses_full_ppgtt(i915))
		vm_cfg.vm = gem_vm_create(i915);

	ctx = intel_ctx_create(i915, &vm_cfg);
	ahnd[0] = get_reloc_ahnd(i915, ctx->id);

	spin = igt_spin_new(i915, .ahnd = ahnd[0], .ctx = ctx,
			    .engine = engine,
			    .fence = fence,
			    .flags = IGT_SPIN_FENCE_OUT | IGT_SPIN_FENCE_IN);
	close(fence);

	/* Loop around the engines, creating a chain of fences */
	spin->execbuf.rsvd2 = (uint64_t)dup(spin->out_fence) << 32;
	spin->execbuf.rsvd2 |= 0xffffffff;
	for_each_ctx_engine(i915, ctx, e) {
		if (e->flags == engine)
			continue;

		close(spin->execbuf.rsvd2);
		spin->execbuf.rsvd2 >>= 32;

		spin->execbuf.flags =
			e->flags | I915_EXEC_FENCE_IN | I915_EXEC_FENCE_OUT;
		gem_execbuf_wr(i915, &spin->execbuf);
	}
	close(spin->execbuf.rsvd2);
	spin->execbuf.rsvd2 >>= 32;
	intel_ctx_destroy(i915, ctx);

	/*
	 * Wait upon the fence chain, and try to terminate the spinner.
	 *
	 * If the scheduler skips a link in the chain and doesn't reach the
	 * dependency on the same engine, we may preempt that spinner to
	 * execute the terminating batch; and the spinner will untimely
	 * exit.
	 */
	map = gem_mmap__device_coherent(i915, obj.handle, 0, 4096, PROT_WRITE);
	cs = map;

	addr = spin->obj[IGT_SPIN_BATCH].offset +
		offset_in_page(spin->condition);
	if (gen >= 8) {
		*cs++ = MI_STORE_DWORD_IMM;
		*cs++ = addr;
		addr >>= 32;
	} else if (gen >= 4) {
		*cs++ = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
		*cs++ = 0;
	} else {
		*cs++ = (MI_STORE_DWORD_IMM | 1 << 22) - 1;
	}
	*cs++ = addr;
	*cs++ = MI_BATCH_BUFFER_END;
	*cs++ = MI_BATCH_BUFFER_END;
	munmap(map, 4096);

	execbuf.rsvd2 = spin->execbuf.rsvd2;
	execbuf.flags |= I915_EXEC_FENCE_IN;

	ctx = intel_ctx_create(i915, &vm_cfg);
	gem_context_set_priority(i915, ctx->id, prio);
	execbuf.rsvd1 = ctx->id;

	gem_execbuf(i915, &execbuf);
	gem_close(i915, obj.handle);
	intel_ctx_destroy(i915, ctx);
	if (cork.fd != -1)
		igt_cork_unplug(&cork);

	/*
	 * Then wait for a timeslice.
	 *
	 * If we start the next spinner it means we have expired the first
	 * spinner's timeslice and the second batch would have already been run,
	 * if it will ever be.
	 *
	 * Without timeslices, fallback to waiting a second.
	 */
	ctx = intel_ctx_create(i915, &vm_cfg);
	ahnd[1] = get_reloc_ahnd(i915, ctx->id);
	slice = igt_spin_new(i915,
			    .ahnd = ahnd[1],
			    .ctx = ctx,
			    .engine = engine,
			    .flags = IGT_SPIN_POLL_RUN);
	igt_until_timeout(1) {
		if (igt_spin_has_started(slice))
			break;
	}
	igt_spin_free(i915, slice);
	intel_ctx_destroy(i915, ctx);

	if (vm_cfg.vm)
		gem_vm_destroy(i915, vm_cfg.vm);

	/* Check the store did not run before the spinner */
	igt_assert_eq(sync_fence_status(spin->out_fence), 0);
	igt_spin_free(i915, spin);
	gem_quiescent_gpu(i915);
	put_ahnd(ahnd[0]);
	put_ahnd(ahnd[1]);
}

static void reorder(int fd, const intel_ctx_cfg_t *cfg,
		    unsigned ring, unsigned flags)
#define EQUAL 1
{
	IGT_CORK_FENCE(cork);
	uint32_t scratch;
	uint32_t result;
	const intel_ctx_t *ctx[2];
	int fence;
	uint64_t ahnd, scratch_offset;

	/*
	 * We use reloc ahnd for default context because we're interested
	 * acquiring distinct offsets only. This saves us typing - otherwise
	 * we should get scratch_offset for each context separately.
	 */
	ahnd = get_reloc_ahnd(fd, 0);

	ctx[LO] = intel_ctx_create(fd, cfg);
	gem_context_set_priority(fd, ctx[LO]->id, MIN_PRIO);

	ctx[HI] = intel_ctx_create(fd, cfg);
	gem_context_set_priority(fd, ctx[HI]->id, flags & EQUAL ? MIN_PRIO : 0);

	scratch = gem_create(fd, 4096);
	scratch_offset = get_offset(ahnd, scratch, 4096, 0);

	fence = igt_cork_plug(&cork, fd);

	/* We expect the high priority context to be executed first, and
	 * so the final result will be value from the low priority context.
	 */
	store_dword_fenced(fd, ahnd, ctx[LO], ring, scratch, scratch_offset,
			   0, ctx[LO]->id, fence, 0);
	store_dword_fenced(fd, ahnd, ctx[HI], ring, scratch, scratch_offset,
			   0, ctx[HI]->id, fence, 0);

	unplug_show_queue(fd, &cork, cfg, ring);
	close(fence);

	result =  __sync_read_u32(fd, scratch, 0);
	gem_close(fd, scratch);
	put_offset(ahnd, scratch);
	put_ahnd(ahnd);

	if (flags & EQUAL) /* equal priority, result will be fifo */
		igt_assert_eq_u32(result, ctx[HI]->id);
	else
		igt_assert_eq_u32(result, ctx[LO]->id);

	intel_ctx_destroy(fd, ctx[LO]);
	intel_ctx_destroy(fd, ctx[HI]);
}

static void promotion(int fd, const intel_ctx_cfg_t *cfg, unsigned ring)
{
	IGT_CORK_FENCE(cork);
	uint32_t result, dep;
	uint32_t result_read, dep_read;
	const intel_ctx_t *ctx[3];
	int fence;
	uint64_t ahnd = get_reloc_ahnd(fd, 0), result_offset, dep_offset;

	ctx[LO] = intel_ctx_create(fd, cfg);
	gem_context_set_priority(fd, ctx[LO]->id, MIN_PRIO);

	ctx[HI] = intel_ctx_create(fd, cfg);
	gem_context_set_priority(fd, ctx[HI]->id, MAX_PRIO);

	ctx[NOISE] = intel_ctx_create(fd, cfg);
	gem_context_set_priority(fd, ctx[NOISE]->id, 0);

	result = gem_create(fd, 4096);
	result_offset = get_offset(ahnd, result, 4096, 0);
	dep = gem_create(fd, 4096);
	dep_offset = get_offset(ahnd, dep, 4096, 0);

	fence = igt_cork_plug(&cork, fd);

	/* Expect that HI promotes LO, so the order will be LO, HI, NOISE.
	 *
	 * fifo would be NOISE, LO, HI.
	 * strict priority would be  HI, NOISE, LO
	 */
	store_dword_fenced(fd, ahnd, ctx[NOISE], ring, result, result_offset,
			   0, ctx[NOISE]->id, fence, 0);
	store_dword_fenced(fd, ahnd, ctx[LO], ring, result, result_offset,
			   0, ctx[LO]->id, fence, 0);

	/* link LO <-> HI via a dependency on another buffer */
	store_dword(fd, ahnd, ctx[LO], ring, dep, dep_offset,
		    0, ctx[LO]->id, I915_GEM_DOMAIN_INSTRUCTION);
	store_dword(fd, ahnd, ctx[HI], ring, dep, dep_offset,
		    0, ctx[HI]->id, 0);

	store_dword(fd, ahnd, ctx[HI], ring, result, result_offset,
		    0, ctx[HI]->id, 0);

	unplug_show_queue(fd, &cork, cfg, ring);
	close(fence);

	dep_read = __sync_read_u32(fd, dep, 0);
	gem_close(fd, dep);

	result_read = __sync_read_u32(fd, result, 0);
	gem_close(fd, result);
	put_offset(ahnd, result);
	put_offset(ahnd, dep);
	put_ahnd(ahnd);

	igt_assert_eq_u32(dep_read, ctx[HI]->id);
	igt_assert_eq_u32(result_read, ctx[NOISE]->id);

	intel_ctx_destroy(fd, ctx[NOISE]);
	intel_ctx_destroy(fd, ctx[LO]);
	intel_ctx_destroy(fd, ctx[HI]);
}

static bool set_preempt_timeout(int i915,
				const struct intel_execution_engine2 *e,
				int timeout_ms)
{
	return gem_engine_property_printf(i915, e->name,
					  "preempt_timeout_ms",
					  "%d", timeout_ms) > 0;
}

#define NEW_CTX (0x1 << 0)
#define HANG_LP (0x1 << 1)
static void preempt(int fd, const intel_ctx_cfg_t *cfg,
		    const struct intel_execution_engine2 *e, unsigned flags)
{
	uint32_t result = gem_create(fd, 4096);
	uint32_t result_read;
	igt_spin_t *spin[MAX_ELSP_QLEN];
	const intel_ctx_t *ctx[2];
	igt_hang_t hang;
	uint64_t ahnd = get_reloc_ahnd(fd, 0);
	uint64_t ahnd_lo_arr[MAX_ELSP_QLEN], ahnd_lo;
	uint64_t result_offset = get_offset(ahnd, result, 4096, 0);

	/* Set a fast timeout to speed the test up (if available) */
	set_preempt_timeout(fd, e, 150);

	ctx[LO] = intel_ctx_create(fd, cfg);
	gem_context_set_priority(fd, ctx[LO]->id, MIN_PRIO);
	ahnd_lo = get_reloc_ahnd(fd, ctx[LO]->id);

	ctx[HI] = intel_ctx_create(fd, cfg);
	gem_context_set_priority(fd, ctx[HI]->id, MAX_PRIO);

	if (flags & HANG_LP)
		hang = igt_hang_ctx_with_ahnd(fd, ahnd_lo, ctx[LO]->id, e->flags, 0);

	for (int n = 0; n < ARRAY_SIZE(spin); n++) {
		uint64_t currahnd = ahnd_lo;

		if (flags & NEW_CTX) {
			intel_ctx_destroy(fd, ctx[LO]);
			ctx[LO] = intel_ctx_create(fd, cfg);
			gem_context_set_priority(fd, ctx[LO]->id, MIN_PRIO);
			ahnd_lo_arr[n] = get_reloc_ahnd(fd, ctx[LO]->id);
			currahnd = ahnd_lo_arr[n];
		}
		spin[n] = __igt_spin_new(fd,
					 .ahnd = currahnd,
					 .ctx = ctx[LO],
					 .engine = e->flags,
					 .flags = flags & USERPTR ? IGT_SPIN_USERPTR : 0);
		igt_debug("spin[%d].handle=%d\n", n, spin[n]->handle);

		store_dword(fd, ahnd, ctx[HI], e->flags, result, result_offset,
			    0, n + 1, I915_GEM_DOMAIN_RENDER);

		result_read = __sync_read_u32(fd, result, 0);
		igt_assert_eq_u32(result_read, n + 1);
		igt_assert(gem_bo_busy(fd, spin[0]->handle));
	}

	for (int n = 0; n < ARRAY_SIZE(spin); n++)
		igt_spin_free(fd, spin[n]);

	if (flags & HANG_LP)
		igt_post_hang_ring(fd, hang);

	intel_ctx_destroy(fd, ctx[LO]);
	intel_ctx_destroy(fd, ctx[HI]);
	put_ahnd(ahnd);
	put_ahnd(ahnd_lo);

	if (flags & NEW_CTX) {
		for (int n = 0; n < ARRAY_SIZE(spin); n++)
			put_ahnd(ahnd_lo_arr[n]);
	}

	gem_close(fd, result);
}

#define CHAIN 0x1
#define CONTEXTS 0x2

static igt_spin_t *__noise(int fd, uint64_t ahnd, const intel_ctx_t *ctx,
			   int prio, igt_spin_t *spin)
{
	const struct intel_execution_engine2 *e;

	gem_context_set_priority(fd, ctx->id, prio);

	for_each_ctx_engine(fd, ctx, e) {
		if (spin == NULL) {
			spin = __igt_spin_new(fd,
					      .ahnd = ahnd,
					      .ctx = ctx,
					      .engine = e->flags);
		} else {
			struct drm_i915_gem_execbuffer2 eb = {
				.buffer_count = 1,
				.buffers_ptr = to_user_pointer(&spin->obj[IGT_SPIN_BATCH]),
				.rsvd1 = ctx->id,
				.flags = e->flags,
			};
			gem_execbuf(fd, &eb);
		}
	}

	return spin;
}

static void __preempt_other(int fd,
			    uint64_t *ahnd,
			    const intel_ctx_t **ctx,
			    unsigned int target, unsigned int primary,
			    unsigned flags)
{
	const struct intel_execution_engine2 *e;
	uint32_t result = gem_create(fd, 4096);
	uint32_t result_read[4096 / sizeof(uint32_t)];
	unsigned int n, i;
	uint64_t result_offset_lo = get_offset(ahnd[LO], result, 4096, 0);
	uint64_t result_offset_hi = get_offset(ahnd[HI], result, 4096, 0);

	n = 0;
	store_dword(fd, ahnd[LO], ctx[LO], primary,
		    result, result_offset_lo, (n + 1)*sizeof(uint32_t), n + 1,
		    I915_GEM_DOMAIN_RENDER);
	n++;

	if (flags & CHAIN) {
		for_each_ctx_engine(fd, ctx[LO], e) {
			store_dword(fd, ahnd[LO], ctx[LO], e->flags,
				    result, result_offset_lo,
				     (n + 1)*sizeof(uint32_t), n + 1,
				    I915_GEM_DOMAIN_RENDER);
			n++;
		}
	}

	store_dword(fd, ahnd[HI], ctx[HI], target,
		    result, result_offset_hi, (n + 1)*sizeof(uint32_t), n + 1,
		    I915_GEM_DOMAIN_RENDER);

	igt_debugfs_dump(fd, "i915_engine_info");
	gem_set_domain(fd, result, I915_GEM_DOMAIN_GTT, 0);

	n++;

	__sync_read_u32_count(fd, result, result_read, sizeof(result_read));
	for (i = 0; i <= n; i++)
		igt_assert_eq_u32(result_read[i], i);

	gem_close(fd, result);
	put_offset(ahnd[LO], result);
	put_offset(ahnd[HI], result);
}

static void preempt_other(int fd, const intel_ctx_cfg_t *cfg,
			  unsigned ring, unsigned int flags)
{
	const struct intel_execution_engine2 *e;
	igt_spin_t *spin = NULL;
	const intel_ctx_t *ctx[3];
	uint64_t ahnd[3];

	/* On each engine, insert
	 * [NOISE] spinner,
	 * [LOW] write
	 *
	 * Then on our target engine do a [HIGH] write which should then
	 * prompt its dependent LOW writes in front of the spinner on
	 * each engine. The purpose of this test is to check that preemption
	 * can cross engines.
	 */

	ctx[LO] = intel_ctx_create(fd, cfg);
	gem_context_set_priority(fd, ctx[LO]->id, MIN_PRIO);
	ahnd[LO] = get_reloc_ahnd(fd, ctx[LO]->id);

	ctx[NOISE] = intel_ctx_create(fd, cfg);
	ahnd[NOISE] = get_reloc_ahnd(fd, ctx[NOISE]->id);
	spin = __noise(fd, ahnd[NOISE], ctx[NOISE], 0, NULL);

	ctx[HI] = intel_ctx_create(fd, cfg);
	gem_context_set_priority(fd, ctx[HI]->id, MAX_PRIO);
	ahnd[HI] = get_reloc_ahnd(fd, ctx[HI]->id);

	for_each_ctx_cfg_engine(fd, cfg, e) {
		igt_debug("Primary engine: %s\n", e->name);
		__preempt_other(fd, ahnd, ctx, ring, e->flags, flags);

	}

	igt_assert(gem_bo_busy(fd, spin->handle));
	igt_spin_free(fd, spin);

	intel_ctx_destroy(fd, ctx[LO]);
	intel_ctx_destroy(fd, ctx[NOISE]);
	intel_ctx_destroy(fd, ctx[HI]);
	put_ahnd(ahnd[LO]);
	put_ahnd(ahnd[NOISE]);
	put_ahnd(ahnd[HI]);
}

static void __preempt_queue(int fd, const intel_ctx_cfg_t *cfg,
			    unsigned target, unsigned primary,
			    unsigned depth, unsigned flags)
{
	const struct intel_execution_engine2 *e;
	uint32_t result = gem_create(fd, 4096);
	uint32_t result_read[4096 / sizeof(uint32_t)];
	uint64_t result_offset;
	igt_spin_t *above = NULL, *below = NULL;
	const intel_ctx_t *ctx[3] = {
		intel_ctx_create(fd, cfg),
		intel_ctx_create(fd, cfg),
		intel_ctx_create(fd, cfg),
	};
	uint64_t ahnd[3] = {
		get_reloc_ahnd(fd, ctx[0]->id),
		get_reloc_ahnd(fd, ctx[1]->id),
		get_reloc_ahnd(fd, ctx[2]->id),
	};
	int prio = MAX_PRIO;
	unsigned int n, i;

	for (n = 0; n < depth; n++) {
		if (flags & CONTEXTS) {
			intel_ctx_destroy(fd, ctx[NOISE]);
			ctx[NOISE] = intel_ctx_create(fd, cfg);
		}
		above = __noise(fd, ahnd[NOISE], ctx[NOISE], prio--, above);
	}

	gem_context_set_priority(fd, ctx[HI]->id, prio--);

	for (; n < MAX_ELSP_QLEN; n++) {
		if (flags & CONTEXTS) {
			intel_ctx_destroy(fd, ctx[NOISE]);
			ctx[NOISE] = intel_ctx_create(fd, cfg);
		}
		below = __noise(fd, ahnd[NOISE], ctx[NOISE], prio--, below);
	}

	gem_context_set_priority(fd, ctx[LO]->id, prio--);

	n = 0;
	result_offset = get_offset(ahnd[LO], result, 4096, 0);
	store_dword(fd, ahnd[LO], ctx[LO], primary,
		    result, result_offset, (n + 1)*sizeof(uint32_t), n + 1,
		    I915_GEM_DOMAIN_RENDER);
	n++;

	if (flags & CHAIN) {
		for_each_ctx_engine(fd, ctx[LO], e) {
			store_dword(fd, ahnd[LO], ctx[LO], e->flags,
				    result, result_offset,
				     (n + 1)*sizeof(uint32_t), n + 1,
				    I915_GEM_DOMAIN_RENDER);
			n++;
		}
	}

	result_offset = get_offset(ahnd[HI], result, 4096, 0);
	store_dword(fd, ahnd[HI], ctx[HI], target,
		    result, result_offset, (n + 1)*sizeof(uint32_t), n + 1,
		    I915_GEM_DOMAIN_RENDER);

	igt_debugfs_dump(fd, "i915_engine_info");

	if (above) {
		igt_assert(gem_bo_busy(fd, above->handle));
		igt_spin_free(fd, above);
	}

	gem_set_domain(fd, result, I915_GEM_DOMAIN_GTT, 0);

	__sync_read_u32_count(fd, result, result_read, sizeof(result_read));

	n++;
	for (i = 0; i <= n; i++)
		igt_assert_eq_u32(result_read[i], i);

	if (below) {
		igt_assert(gem_bo_busy(fd, below->handle));
		igt_spin_free(fd, below);
	}

	intel_ctx_destroy(fd, ctx[LO]);
	intel_ctx_destroy(fd, ctx[NOISE]);
	intel_ctx_destroy(fd, ctx[HI]);

	gem_close(fd, result);
	put_offset(ahnd[LO], result);
	put_offset(ahnd[HI], result);
	put_ahnd(ahnd[LO]);
	put_ahnd(ahnd[NOISE]);
	put_ahnd(ahnd[HI]);
}

static void preempt_queue(int fd, const intel_ctx_cfg_t *cfg,
			  unsigned ring, unsigned int flags)
{
	const struct intel_execution_engine2 *e;

	for (unsigned depth = 1; depth <= MAX_ELSP_QLEN; depth *= 4)
		__preempt_queue(fd, cfg, ring, ring, depth, flags);

	for_each_ctx_cfg_engine(fd, cfg, e) {
		if (ring == e->flags)
			continue;

		__preempt_queue(fd, cfg, ring, e->flags, MAX_ELSP_QLEN, flags);
	}
}

static void preempt_engines(int i915,
			    const struct intel_execution_engine2 *e,
			    unsigned int flags)
{
	struct pnode {
		struct igt_list_head spinners;
		struct igt_list_head link;
	} pnode[GEM_MAX_ENGINES], *p;
	struct intel_ctx_cfg cfg = {
		.num_engines = GEM_MAX_ENGINES,
	};
	IGT_LIST_HEAD(plist);
	igt_spin_t *spin, *sn;
	const intel_ctx_t *ctx;
	uint64_t ahnd;

	/*
	 * A quick test that each engine within a context is an independent
	 * timeline that we can reprioritise and shuffle amongst themselves.
	 */

	igt_require(gem_has_engine_topology(i915));

	for (int n = 0; n < GEM_MAX_ENGINES; n++) {
		cfg.engines[n].engine_class = e->class;
		cfg.engines[n].engine_instance = e->instance;
		IGT_INIT_LIST_HEAD(&pnode[n].spinners);
		igt_list_add(&pnode[n].link, &plist);
	}
	ctx = intel_ctx_create(i915, &cfg);
	ahnd = get_reloc_ahnd(i915, ctx->id);

	for (int n = -(GEM_MAX_ENGINES - 1); n < GEM_MAX_ENGINES; n++) {
		unsigned int engine = n & I915_EXEC_RING_MASK;

		gem_context_set_priority(i915, ctx->id, n);
		spin = igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
				   .engine = engine);

		igt_list_move_tail(&spin->link, &pnode[engine].spinners);
		igt_list_move(&pnode[engine].link, &plist);
	}

	igt_list_for_each_entry(p, &plist, link) {
		igt_list_for_each_entry_safe(spin, sn, &p->spinners, link) {
			igt_spin_end(spin);
			gem_sync(i915, spin->handle);
			igt_spin_free(i915, spin);
		}
	}
	intel_ctx_destroy(i915, ctx);
	put_ahnd(ahnd);
}

static void preempt_self(int fd, const intel_ctx_cfg_t *cfg,
			 unsigned ring)
{
	const struct intel_execution_engine2 *e;
	uint32_t result = gem_create(fd, 4096);
	uint32_t result_read[4096 / sizeof(uint32_t)];
	igt_spin_t *spin[MAX_ELSP_QLEN];
	unsigned int n, i;
	const intel_ctx_t *ctx[3];
	uint64_t ahnd[3], result_offset;

	/* On each engine, insert
	 * [NOISE] spinner,
	 * [self/LOW] write
	 *
	 * Then on our target engine do a [self/HIGH] write which should then
	 * preempt its own lower priority task on any engine.
	 */

	ctx[NOISE] = intel_ctx_create(fd, cfg);
	ctx[HI] = intel_ctx_create(fd, cfg);
	ahnd[NOISE] = get_reloc_ahnd(fd, ctx[NOISE]->id);
	ahnd[HI] = get_reloc_ahnd(fd, ctx[HI]->id);
	result_offset = get_offset(ahnd[HI], result, 4096, 0);

	n = 0;
	gem_context_set_priority(fd, ctx[HI]->id, MIN_PRIO);
	for_each_ctx_cfg_engine(fd, cfg, e) {
		spin[n] = __igt_spin_new(fd,
					 .ahnd = ahnd[NOISE],
					 .ctx = ctx[NOISE],
					 .engine = e->flags);
		store_dword(fd, ahnd[HI], ctx[HI], e->flags,
			    result, result_offset,
			     (n + 1)*sizeof(uint32_t), n + 1,
			    I915_GEM_DOMAIN_RENDER);
		n++;
	}
	gem_context_set_priority(fd, ctx[HI]->id, MAX_PRIO);
	store_dword(fd, ahnd[HI], ctx[HI], ring,
		    result, result_offset, (n + 1)*sizeof(uint32_t), n + 1,
		    I915_GEM_DOMAIN_RENDER);

	gem_set_domain(fd, result, I915_GEM_DOMAIN_GTT, 0);

	for (i = 0; i < n; i++) {
		igt_assert(gem_bo_busy(fd, spin[i]->handle));
		igt_spin_free(fd, spin[i]);
	}

	__sync_read_u32_count(fd, result, result_read, sizeof(result_read));

	n++;
	for (i = 0; i <= n; i++)
		igt_assert_eq_u32(result_read[i], i);

	intel_ctx_destroy(fd, ctx[NOISE]);
	intel_ctx_destroy(fd, ctx[HI]);

	gem_close(fd, result);
	put_offset(ahnd[HI], result);
	put_ahnd(ahnd[NOISE]);
	put_ahnd(ahnd[HI]);
}

static void preemptive_hang(int fd, const intel_ctx_cfg_t *cfg,
			    const struct intel_execution_engine2 *e)
{
	igt_spin_t *spin[MAX_ELSP_QLEN];
	igt_hang_t hang;
	const intel_ctx_t *ctx[2];
	uint64_t ahnd_hi, ahnd_lo;

	/* Set a fast timeout to speed the test up (if available) */
	set_preempt_timeout(fd, e, 150);

	ctx[HI] = intel_ctx_create(fd, cfg);
	gem_context_set_priority(fd, ctx[HI]->id, MAX_PRIO);
	ahnd_hi = get_reloc_ahnd(fd, ctx[HI]->id);

	for (int n = 0; n < ARRAY_SIZE(spin); n++) {
		ctx[LO] = intel_ctx_create(fd, cfg);
		gem_context_set_priority(fd, ctx[LO]->id, MIN_PRIO);
		ahnd_lo = get_reloc_ahnd(fd, ctx[LO]->id);

		spin[n] = __igt_spin_new(fd,
					 .ahnd = ahnd_lo,
					 .ctx = ctx[LO],
					 .engine = e->flags);

		intel_ctx_destroy(fd, ctx[LO]);
	}

	hang = igt_hang_ctx_with_ahnd(fd, ahnd_hi, ctx[HI]->id, e->flags, 0);
	igt_post_hang_ring(fd, hang);

	for (int n = 0; n < ARRAY_SIZE(spin); n++) {
		/* Current behavior is to execute requests in order of submission.
		 * This is subject to change as the scheduler evolve. The test should
		 * be updated to reflect such changes.
		 */
		ahnd_lo = spin[n]->opts.ahnd;
		igt_assert(gem_bo_busy(fd, spin[n]->handle));
		igt_spin_free(fd, spin[n]);
		put_ahnd(ahnd_lo);
	}

	intel_ctx_destroy(fd, ctx[HI]);
	put_ahnd(ahnd_hi);
}

static void deep(int fd, const intel_ctx_cfg_t *cfg,
		 unsigned ring)
{
#define XS 8
	const unsigned int max_req = MAX_PRIO - MIN_PRIO;
	const unsigned size = ALIGN(4*max_req, 4096);
	struct timespec tv = {};
	IGT_CORK_HANDLE(cork);
	unsigned int nreq;
	uint32_t plug;
	uint32_t result, dep[XS];
	uint32_t read_buf[size / sizeof(uint32_t)];
	uint32_t expected = 0;
	uint64_t ahnd = get_reloc_ahnd(fd, 0);
	uint64_t result_offset, dep_offset[XS], plug_offset;
	const intel_ctx_t **ctx;
	int dep_nreq;
	int n;

	ctx = malloc(sizeof(*ctx) * MAX_CONTEXTS);
	for (n = 0; n < MAX_CONTEXTS; n++) {
		ctx[n] = intel_ctx_create(fd, cfg);
	}

	nreq = gem_submission_measure(fd, cfg, ring) / (3 * XS) * MAX_CONTEXTS;
	if (nreq > max_req)
		nreq = max_req;
	igt_info("Using %d requests (prio range %d)\n", nreq, max_req);

	result = gem_create(fd, size);
	result_offset = get_offset(ahnd, result, size, 0);
	for (int m = 0; m < XS; m ++)
		dep[m] = gem_create(fd, size);

	/* Bind all surfaces and contexts before starting the timeout. */
	{
		struct drm_i915_gem_exec_object2 obj[XS + 2];
		struct drm_i915_gem_execbuffer2 execbuf;
		const uint32_t bbe = MI_BATCH_BUFFER_END;

		memset(obj, 0, sizeof(obj));
		for (n = 0; n < XS; n++) {
			obj[n].handle = dep[n];
			if (ahnd) {
				obj[n].offset = get_offset(ahnd, obj[n].handle,
							   size, 0);
				dep_offset[n] = obj[n].offset;
				obj[n].flags |= EXEC_OBJECT_PINNED;
			}
		}
		obj[XS].handle = result;
		obj[XS].offset = result_offset;
		obj[XS+1].handle = gem_create(fd, 4096);
		obj[XS+1].offset = get_offset(ahnd, obj[XS+1].handle, 4096, 0);
		if (ahnd) {
			obj[XS].flags |= EXEC_OBJECT_PINNED;
			obj[XS+1].flags |= EXEC_OBJECT_PINNED;
		}
		gem_write(fd, obj[XS+1].handle, 0, &bbe, sizeof(bbe));

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = to_user_pointer(obj);
		execbuf.buffer_count = XS + 2;
		execbuf.flags = ring;
		for (n = 0; n < MAX_CONTEXTS; n++) {
			execbuf.rsvd1 = ctx[n]->id;
			gem_execbuf(fd, &execbuf);
		}
		gem_close(fd, obj[XS+1].handle);
		gem_sync(fd, result);
	}

	plug = igt_cork_plug(&cork, fd);
	plug_offset = get_offset(ahnd, plug, 4096, 0);

	/* Create a deep dependency chain, with a few branches */
	for (n = 0; n < nreq && igt_seconds_elapsed(&tv) < 2; n++) {
		const intel_ctx_t *context = ctx[n % MAX_CONTEXTS];
		gem_context_set_priority(fd, context->id, MAX_PRIO - nreq + n);

		for (int m = 0; m < XS; m++)
			store_dword_plug(fd, ahnd, context, ring,
					 dep[m], dep_offset[m], 4*n,
					 context->id, plug, plug_offset,
					 I915_GEM_DOMAIN_INSTRUCTION);
	}
	igt_info("First deptree: %d requests [%.3fs]\n",
		 n * XS, 1e-9*igt_nsec_elapsed(&tv));
	dep_nreq = n;

	for (n = 0; n < nreq && igt_seconds_elapsed(&tv) < 4; n++) {
		const intel_ctx_t *context = ctx[n % MAX_CONTEXTS];
		gem_context_set_priority(fd, context->id, MAX_PRIO - nreq + n);

		expected = context->id;
		for (int m = 0; m < XS; m++) {
			store_dword_plug(fd, ahnd, context, ring, result, result_offset,
					 4*n, expected, dep[m], dep_offset[m], 0);
			store_dword(fd, ahnd, context, ring, result, result_offset,
				    4*m, expected, I915_GEM_DOMAIN_INSTRUCTION);
		}
	}
	igt_info("Second deptree: %d requests [%.3fs]\n",
		 n * XS, 1e-9*igt_nsec_elapsed(&tv));

	unplug_show_queue(fd, &cork, cfg, ring);
	gem_close(fd, plug);
	igt_require(expected); /* too slow */

	for (int m = 0; m < XS; m++) {
		__sync_read_u32_count(fd, dep[m], read_buf, sizeof(read_buf));
		gem_close(fd, dep[m]);

		for (n = 0; n < dep_nreq; n++)
			igt_assert_eq_u32(read_buf[n], ctx[n % MAX_CONTEXTS]->id);
	}

	for (n = 0; n < MAX_CONTEXTS; n++)
		intel_ctx_destroy(fd, ctx[n]);

	__sync_read_u32_count(fd, result, read_buf, sizeof(read_buf));
	gem_close(fd, result);

	/* No reordering due to PI on all contexts because of the common dep */
	for (int m = 0; m < XS; m++) {
		put_offset(ahnd, dep[m]);
		igt_assert_eq_u32(read_buf[m], expected);
	}
	put_offset(ahnd, result);
	put_offset(ahnd, plug);
	put_ahnd(ahnd);

	free(ctx);
#undef XS
}

static void alarm_handler(int sig)
{
}

static int __execbuf(int fd, struct drm_i915_gem_execbuffer2 *execbuf)
{
	int err = 0;
	if (ioctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, execbuf))
		err = -errno;
	return err;
}

static void wide(int fd, const intel_ctx_cfg_t *cfg, unsigned ring)
{
	const unsigned int ring_size = gem_submission_measure(fd, cfg, ring);
	struct timespec tv = {};
	IGT_CORK_FENCE(cork);
	uint32_t result;
	uint32_t result_read[MAX_CONTEXTS];
	const intel_ctx_t **ctx;
	unsigned int count;
	int fence;
	uint64_t ahnd = get_reloc_ahnd(fd, 0), result_offset;

	ctx = malloc(sizeof(*ctx)*MAX_CONTEXTS);
	for (int n = 0; n < MAX_CONTEXTS; n++)
		ctx[n] = intel_ctx_create(fd, cfg);

	result = gem_create(fd, 4*MAX_CONTEXTS);
	result_offset = get_offset(ahnd, result, 4 * MAX_CONTEXTS, 0);

	fence = igt_cork_plug(&cork, fd);

	/* Lots of in-order requests, plugged and submitted simultaneously */
	for (count = 0;
	     igt_seconds_elapsed(&tv) < 5 && count < ring_size;
	     count++) {
		for (int n = 0; n < MAX_CONTEXTS; n++) {
			store_dword_fenced(fd, ahnd, ctx[n], ring,
					   result, result_offset, 4*n, ctx[n]->id,
					   fence, I915_GEM_DOMAIN_INSTRUCTION);
		}
	}
	igt_info("Submitted %d requests over %d contexts in %.1fms\n",
		 count, MAX_CONTEXTS, igt_nsec_elapsed(&tv) * 1e-6);

	unplug_show_queue(fd, &cork, cfg, ring);
	close(fence);

	__sync_read_u32_count(fd, result, result_read, sizeof(result_read));
	for (int n = 0; n < MAX_CONTEXTS; n++)
		igt_assert_eq_u32(result_read[n], ctx[n]->id);

	for (int n = 0; n < MAX_CONTEXTS; n++)
		intel_ctx_destroy(fd, ctx[n]);

	gem_close(fd, result);
	free(ctx);
	put_offset(ahnd, result);
	put_ahnd(ahnd);
}

static void reorder_wide(int fd, const intel_ctx_cfg_t *cfg, unsigned ring)
{
	const unsigned int ring_size = gem_submission_measure(fd, cfg, ring);
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	const int priorities[] = { MIN_PRIO, MAX_PRIO };
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t result_read[1024];
	uint32_t result, target;
	IGT_CORK_FENCE(cork);
	uint32_t *expected;
	int fence;
	uint64_t ahnd = get_reloc_ahnd(fd, 0), result_offset;
	unsigned int sz = ALIGN(ring_size * 64, 4096);

	result = gem_create(fd, 4096);
	result_offset = get_offset(ahnd, result, 4096, 0);
	target = gem_create(fd, 4096);
	fence = igt_cork_plug(&cork, fd);

	expected = gem_mmap__cpu(fd, target, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, target, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = result;
	obj[0].offset = result_offset;
	obj[1].relocs_ptr = to_user_pointer(&reloc);
	obj[1].relocation_count = !ahnd ? 1 : 0;

	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = result;
	reloc.presumed_offset = obj[0].offset;
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = 0; /* lies */

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = ARRAY_SIZE(obj);
	execbuf.flags = ring;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	execbuf.flags |= I915_EXEC_FENCE_IN;
	execbuf.rsvd2 = fence;

	if (ahnd) {
		obj[0].flags |= EXEC_OBJECT_PINNED;
		obj[1].flags |= EXEC_OBJECT_PINNED;
	}

	for (int n = 0, x = 1; n < ARRAY_SIZE(priorities); n++, x++) {
		uint32_t *batch;
		const intel_ctx_t *tmp_ctx;

		tmp_ctx = intel_ctx_create(fd, cfg);
		gem_context_set_priority(fd, tmp_ctx->id, priorities[n]);
		execbuf.rsvd1 = tmp_ctx->id;

		obj[1].handle = gem_create(fd, sz);
		if (ahnd)
			obj[1].offset = get_offset(ahnd, obj[1].handle, sz, 0);

		batch = gem_mmap__device_coherent(fd, obj[1].handle, 0, sz, PROT_WRITE);
		gem_set_domain(fd, obj[1].handle, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

		for (int m = 0; m < ring_size; m++) {
			uint64_t addr;
			int idx = hars_petruska_f54_1_random_unsafe_max(1024);
			int i;

			execbuf.batch_start_offset = m * 64;
			reloc.offset = execbuf.batch_start_offset + sizeof(uint32_t);
			reloc.delta = idx * sizeof(uint32_t);
			addr = reloc.presumed_offset + reloc.delta;

			i = execbuf.batch_start_offset / sizeof(uint32_t);
			batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
			if (gen >= 8) {
				batch[++i] = addr;
				batch[++i] = addr >> 32;
			} else if (gen >= 4) {
				batch[++i] = 0;
				batch[++i] = addr;
				reloc.offset += sizeof(uint32_t);
			} else {
				batch[i]--;
				batch[++i] = addr;
			}
			batch[++i] = x;
			batch[++i] = MI_BATCH_BUFFER_END;

			if (!expected[idx])
				expected[idx] =  x;

			gem_execbuf(fd, &execbuf);
		}

		munmap(batch, sz);
		gem_close(fd, obj[1].handle);
		put_offset(ahnd, obj[1].handle);
		intel_ctx_destroy(fd, tmp_ctx);
	}

	unplug_show_queue(fd, &cork, cfg, ring);
	close(fence);

	__sync_read_u32_count(fd, result, result_read, sizeof(result_read));
	for (int n = 0; n < 1024; n++)
		igt_assert_eq_u32(result_read[n], expected[n]);

	munmap(expected, 4096);

	gem_close(fd, result);
	gem_close(fd, target);
	put_offset(ahnd, result);
	put_ahnd(ahnd);
}

static void bind_to_cpu(int cpu)
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	struct sched_param rt = {.sched_priority = 99 };
	cpu_set_t allowed;

	igt_assert(sched_setscheduler(getpid(), SCHED_RR | SCHED_RESET_ON_FORK, &rt) == 0);

	CPU_ZERO(&allowed);
	CPU_SET(cpu % ncpus, &allowed);
	igt_assert(sched_setaffinity(getpid(), sizeof(cpu_set_t), &allowed) == 0);
}

static void test_pi_ringfull(int fd, const intel_ctx_cfg_t *cfg,
			     unsigned int engine, unsigned int flags)
#define SHARED BIT(0)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct sigaction sa = { .sa_handler = alarm_handler };
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[2];
	const intel_ctx_t *ctx, *vip;
	unsigned int last, count;
	struct itimerval itv;
	IGT_CORK_HANDLE(c);
	bool *result;

	/*
	 * We start simple. A low priority client should never prevent a high
	 * priority client from submitting their work; even if the low priority
	 * client exhausts their ringbuffer and so is throttled.
	 *
	 * SHARED: A variant on the above rule is that even is the 2 clients
	 * share a read-only resource, the blocked low priority client should
	 * not prevent the high priority client from executing. A buffer,
	 * e.g. the batch buffer, that is shared only for reads (no write
	 * hazard, so the reads can be executed in parallel or in any order),
	 * so not cause priority inversion due to the resource conflict.
	 *
	 * First, we have the low priority context who fills their ring and so
	 * blocks. As soon as that context blocks, we try to submit a high
	 * priority batch, which should be executed immediately before the low
	 * priority context is unblocked.
	 */

	result = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(result != MAP_FAILED);

	memset(&execbuf, 0, sizeof(execbuf));
	memset(&obj, 0, sizeof(obj));

	obj[1].handle = gem_create(fd, 4096);
	gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));

	execbuf.buffers_ptr = to_user_pointer(&obj[1]);
	execbuf.buffer_count = 1;

	/* Warm up both (hi/lo) contexts */
	ctx = intel_ctx_create(fd, cfg);
	gem_context_set_priority(fd, ctx->id, MAX_PRIO);
	execbuf.rsvd1 = ctx->id;
	gem_execbuf(fd, &execbuf);
	gem_sync(fd, obj[1].handle);
	vip = ctx;

	ctx = intel_ctx_create(fd, cfg);
	gem_context_set_priority(fd, ctx->id, MIN_PRIO);
	execbuf.rsvd1 = ctx->id;
	gem_execbuf(fd, &execbuf);
	gem_sync(fd, obj[1].handle);

	/* Fill the low-priority ring */
	obj[0].handle = igt_cork_plug(&c, fd);

	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;

	sigaction(SIGALRM, &sa, NULL);
	itv.it_interval.tv_sec = 0;
	itv.it_interval.tv_usec = 1000;
	itv.it_value.tv_sec = 0;
	itv.it_value.tv_usec = 10000;
	setitimer(ITIMER_REAL, &itv, NULL);

	last = -1;
	count = 0;
	do {
		if (__execbuf(fd, &execbuf) == 0) {
			count++;
			continue;
		}

		if (last == count)
			break;

		last = count;
	} while (1);
	igt_debug("Filled low-priority ring with %d batches\n", count);

	memset(&itv, 0, sizeof(itv));
	setitimer(ITIMER_REAL, &itv, NULL);

	execbuf.buffers_ptr = to_user_pointer(&obj[1]);
	execbuf.buffer_count = 1;

	/* both parent + child on the same cpu, only parent is RT */
	bind_to_cpu(0);

	igt_fork(child, 1) {
		int err;

		/* Replace our batch to avoid conflicts over shared resources? */
		if (!(flags & SHARED)) {
			obj[1].handle = gem_create(fd, 4096);
			gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));
		}

		result[0] = vip->id != execbuf.rsvd1;

		igt_debug("Waking parent\n");
		kill(getppid(), SIGALRM);
		sched_yield();
		result[1] = true;

		sigaction(SIGALRM, &sa, NULL);
		itv.it_value.tv_sec = 0;
		itv.it_value.tv_usec = 10000;
		setitimer(ITIMER_REAL, &itv, NULL);

		/*
		 * Since we are the high priority task, we expect to be
		 * able to add ourselves to *our* ring without interruption.
		 */
		igt_debug("HP child executing\n");
		execbuf.rsvd1 = vip->id;
		err = __execbuf(fd, &execbuf);
		igt_debug("HP execbuf returned %d\n", err);

		memset(&itv, 0, sizeof(itv));
		setitimer(ITIMER_REAL, &itv, NULL);

		result[2] = err == 0;

		if (!(flags & SHARED))
			gem_close(fd, obj[1].handle);
	}

	/* Relinquish CPU just to allow child to create a context */
	sleep(1);
	igt_assert_f(result[0], "HP context (child) not created\n");
	igt_assert_f(!result[1], "Child released too early!\n");

	/* Parent sleeps waiting for ringspace, releasing child */
	itv.it_value.tv_sec = 0;
	itv.it_value.tv_usec = 50000;
	setitimer(ITIMER_REAL, &itv, NULL);
	igt_debug("LP parent executing\n");
	igt_assert_eq(__execbuf(fd, &execbuf), -EINTR);
	igt_assert_f(result[1], "Child was not released!\n");
	igt_assert_f(result[2],
		     "High priority child unable to submit within 10ms\n");

	igt_cork_unplug(&c);
	igt_waitchildren();

	intel_ctx_destroy(fd, ctx);
	intel_ctx_destroy(fd, vip);
	gem_close(fd, obj[1].handle);
	gem_close(fd, obj[0].handle);
	munmap(result, 4096);
}

static int userfaultfd(int flags)
{
	return syscall(SYS_userfaultfd, flags);
}

struct ufd_thread {
	uint32_t batch;
	uint32_t scratch;
	uint32_t *page;
	const intel_ctx_cfg_t *cfg;
	unsigned int engine;
	int i915;

	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int count;

	uint64_t ahnd;
	uint64_t batch_offset;
	uint64_t scratch_offset;
};

static uint32_t create_userptr(int i915, void *page)
{
	uint32_t handle;

	gem_userptr(i915, page, 4096, 0, 0, &handle);
	return handle;
}

static void *ufd_thread(void *arg)
{
	struct ufd_thread *t = arg;
	struct drm_i915_gem_exec_object2 obj[2] = {
		{ .handle = create_userptr(t->i915, t->page) },
		{ .handle = t->batch },
	};
	const intel_ctx_t *ctx = intel_ctx_create(t->i915, t->cfg);
	struct drm_i915_gem_execbuffer2 eb = {
		.buffers_ptr = to_user_pointer(obj),
		.buffer_count = ARRAY_SIZE(obj),
		.flags = t->engine,
		.rsvd1 = ctx->id,
	};
	gem_context_set_priority(t->i915, eb.rsvd1, MIN_PRIO);

	igt_debug("submitting fault\n");
	gem_execbuf(t->i915, &eb);
	gem_sync(t->i915, obj[0].handle);
	gem_close(t->i915, obj[0].handle);

	intel_ctx_destroy(t->i915, ctx);

	t->i915 = -1;
	return NULL;
}

static void test_pi_userfault(int i915,
			      const intel_ctx_cfg_t *cfg,
			      unsigned int engine)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct uffdio_api api = { .api = UFFD_API };
	struct uffdio_register reg;
	struct uffdio_copy copy;
	struct uffd_msg msg;
	struct ufd_thread t;
	pthread_t thread;
	char buf[4096];
	char *poison;
	int ufd;

	/*
	 * Resource contention can easily lead to priority inversion problems,
	 * that we wish to avoid. Here, we simulate one simple form of resource
	 * starvation by using an arbitrary slow userspace fault handler to cause
	 * the low priority context to block waiting for its resource. While it
	 * is blocked, it should not prevent a higher priority context from
	 * executing.
	 *
	 * This is only a very simple scenario, in more general tests we will
	 * need to simulate contention on the shared resource such that both
	 * low and high priority contexts are starving and must fight over
	 * the meagre resources. One step at a time.
	 */

	ufd = userfaultfd(0);
	igt_require_f(ufd != -1, "kernel support for userfaultfd\n");
	igt_require_f(ioctl(ufd, UFFDIO_API, &api) == 0 && api.api == UFFD_API,
		      "userfaultfd API v%lld:%lld\n", UFFD_API, api.api);

	t.i915 = i915;
	t.cfg = cfg;
	t.engine = engine;

	t.page = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, 0, 0);
	igt_assert(t.page != MAP_FAILED);

	t.batch = gem_create(i915, 4096);
	poison = gem_mmap__device_coherent(i915, t.batch, 0, 4096, PROT_WRITE);
	memset(poison, 0xff, 4096);

	/* Register our fault handler for t.page */
	memset(&reg, 0, sizeof(reg));
	reg.mode = UFFDIO_REGISTER_MODE_MISSING;
	reg.range.start = to_user_pointer(t.page);
	reg.range.len = 4096;
	do_ioctl(ufd, UFFDIO_REGISTER, &reg);

	/* Kick off the low priority submission */
	igt_assert(pthread_create(&thread, NULL, ufd_thread, &t) == 0);

	/* Wait until the low priority thread is blocked on a fault */
	igt_assert_eq(read(ufd, &msg, sizeof(msg)), sizeof(msg));
	igt_assert_eq(msg.event, UFFD_EVENT_PAGEFAULT);
	igt_assert(from_user_pointer(msg.arg.pagefault.address) == t.page);

	/* While the low priority context is blocked; execute a vip */
	if (1) {
		struct drm_i915_gem_exec_object2 obj = {
			.handle = gem_create(i915, 4096),
		};
		struct pollfd pfd;
		const intel_ctx_t *ctx = intel_ctx_create(i915, cfg);
		struct drm_i915_gem_execbuffer2 eb = {
			.buffers_ptr = to_user_pointer(&obj),
			.buffer_count = 1,
			.flags = engine | I915_EXEC_FENCE_OUT,
			.rsvd1 = ctx->id,
		};
		gem_context_set_priority(i915, eb.rsvd1, MAX_PRIO);
		gem_write(i915, obj.handle, 0, &bbe, sizeof(bbe));
		gem_execbuf_wr(i915, &eb);
		gem_close(i915, obj.handle);

		memset(&pfd, 0, sizeof(pfd));
		pfd.fd = eb.rsvd2 >> 32;
		pfd.events = POLLIN;
		poll(&pfd, 1, -1);
		igt_assert_eq(sync_fence_status(pfd.fd), 1);
		close(pfd.fd);

		intel_ctx_destroy(i915, ctx);
	}

	/* Confirm the low priority context is still waiting */
	igt_assert_eq(t.i915, i915);
	memcpy(poison, &bbe, sizeof(bbe));
	munmap(poison, 4096);

	/* Service the fault; releasing the low priority context */
	memset(&copy, 0, sizeof(copy));
	copy.dst = msg.arg.pagefault.address;
	copy.src = to_user_pointer(memset(buf, 0xc5, sizeof(buf)));
	copy.len = 4096;
	do_ioctl(ufd, UFFDIO_COPY, &copy);

	pthread_join(thread, NULL);

	gem_close(i915, t.batch);
	munmap(t.page, 4096);
	close(ufd);
}

static void *iova_thread(struct ufd_thread *t, int prio)
{
	const intel_ctx_t *ctx;

	ctx = intel_ctx_create(t->i915, t->cfg);
	gem_context_set_priority(t->i915, ctx->id, prio);

	store_dword_plug(t->i915, t->ahnd, ctx, t->engine,
			 t->scratch, t->scratch_offset, 0, prio,
			 t->batch, t->batch_offset, 0 /* no write hazard! */);

	pthread_mutex_lock(&t->mutex);
	if (!--t->count)
		pthread_cond_signal(&t->cond);
	pthread_mutex_unlock(&t->mutex);

	intel_ctx_destroy(t->i915, ctx);
	return NULL;
}

static void *iova_low(void *arg)
{
	return iova_thread(arg, MIN_PRIO);
}

static void *iova_high(void *arg)
{
	return iova_thread(arg, MAX_PRIO);
}

static void test_pi_iova(int i915, const intel_ctx_cfg_t *cfg,
			 unsigned int engine, unsigned int flags)
{
	intel_ctx_cfg_t ufd_cfg = *cfg;
	const intel_ctx_t *spinctx;
	struct uffdio_api api = { .api = UFFD_API };
	struct uffdio_register reg;
	struct uffdio_copy copy;
	struct uffd_msg msg;
	struct ufd_thread t;
	igt_spin_t *spin;
	pthread_t hi, lo;
	char poison[4096];
	int ufd;
	uint64_t ahnd;

	/*
	 * In this scenario, we have a pair of contending contexts that
	 * share the same resource. That resource is stuck behind a slow
	 * page fault such that neither context has immediate access to it.
	 * What is expected is that as soon as that resource becomes available,
	 * the two contexts are queued with the high priority context taking
	 * precedence. We need to check that we do not cross-contaminate
	 * the two contents with the page fault on the shared resource
	 * initiated by the low priority context. (Consider that the low
	 * priority context may install an exclusive fence for the page
	 * fault, which is then used for strict ordering by the high priority
	 * context, causing an unwanted implicit dependency between the two
	 * and promoting the low priority context to high.)
	 *
	 * SHARED: the two contexts share a vm, but still have separate
	 * timelines that should not mingle.
	 */

	ufd = userfaultfd(0);
	igt_require_f(ufd != -1, "kernel support for userfaultfd\n");
	igt_require_f(ioctl(ufd, UFFDIO_API, &api) == 0 && api.api == UFFD_API,
		      "userfaultfd API v%lld:%lld\n", UFFD_API, api.api);

	if ((flags & SHARED) && gem_uses_full_ppgtt(i915))
		ufd_cfg.vm = gem_vm_create(i915);

	spinctx = intel_ctx_create(i915, cfg);
	ahnd = get_reloc_ahnd(i915, spinctx->id);
	t.i915 = i915;
	t.cfg = &ufd_cfg;
	t.engine = engine;
	t.ahnd = ahnd;

	t.count = 2;
	pthread_cond_init(&t.cond, NULL);
	pthread_mutex_init(&t.mutex, NULL);

	t.page = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, 0, 0);
	igt_assert(t.page != MAP_FAILED);
	t.batch = create_userptr(i915, t.page);
	t.scratch = gem_create(i915, 4096);
	t.batch_offset = get_offset(ahnd, t.batch, 4096, 0);
	t.scratch_offset = get_offset(ahnd, t.scratch, 4096, 0);

	/* Register our fault handler for t.page */
	memset(&reg, 0, sizeof(reg));
	reg.mode = UFFDIO_REGISTER_MODE_MISSING;
	reg.range.start = to_user_pointer(t.page);
	reg.range.len = 4096;
	do_ioctl(ufd, UFFDIO_REGISTER, &reg);

	/*
	 * Fill the engine with spinners; the store_dword() is too quick!
	 *
	 * It is not that it is too quick, it that the order in which the
	 * requests are signaled from the pagefault completion is loosely
	 * defined (currently, it's in order of attachment so low context
	 * wins), then submission into the execlists is immediate with the
	 * low context filling the last slot in the ELSP. Preemption will
	 * not take place until after the low priority context has had a
	 * chance to run, and since the task is very short there is no
	 * arbitration point inside the batch buffer so we only preempt
	 * after the low priority context has completed.
	 *
	 * One way to prevent such opportunistic execution of the low priority
	 * context would be to remove direct submission and wait until all
	 * signals are delivered (as the signal delivery is under the irq lock,
	 * the local tasklet will not run until after all signals have been
	 * delivered... but another tasklet might).
	 */
	spin = igt_spin_new(i915, .ahnd = ahnd, .ctx = spinctx, .engine = engine);
	for (int i = 0; i < MAX_ELSP_QLEN; i++) {
		const intel_ctx_t *ctx = create_highest_priority(i915, cfg);
		spin->execbuf.rsvd1 = ctx->id;
		gem_execbuf(i915, &spin->execbuf);
		intel_ctx_destroy(i915, ctx);
	}

	/* Kick off the submission threads */
	igt_assert(pthread_create(&lo, NULL, iova_low, &t) == 0);

	/* Wait until the low priority thread is blocked on the fault */
	igt_assert_eq(read(ufd, &msg, sizeof(msg)), sizeof(msg));
	igt_assert_eq(msg.event, UFFD_EVENT_PAGEFAULT);
	igt_assert(from_user_pointer(msg.arg.pagefault.address) == t.page);

	/* Then release a very similar thread, but at high priority! */
	igt_assert(pthread_create(&hi, NULL, iova_high, &t) == 0);

	/* Service the fault; releasing both contexts */
	memset(&copy, 0, sizeof(copy));
	copy.dst = msg.arg.pagefault.address;
	copy.src = to_user_pointer(memset(poison, 0xc5, sizeof(poison)));
	copy.len = 4096;
	do_ioctl(ufd, UFFDIO_COPY, &copy);

	/* Wait until both threads have had a chance to submit */
	pthread_mutex_lock(&t.mutex);
	while (t.count)
		pthread_cond_wait(&t.cond, &t.mutex);
	pthread_mutex_unlock(&t.mutex);
	igt_debugfs_dump(i915, "i915_engine_info");
	igt_spin_free(i915, spin);
	intel_ctx_destroy(i915, spinctx);
	put_offset(ahnd, t.scratch);
	put_offset(ahnd, t.batch);
	put_ahnd(ahnd);

	pthread_join(hi, NULL);
	pthread_join(lo, NULL);
	gem_close(i915, t.batch);

	igt_assert_eq(__sync_read_u32(i915, t.scratch, 0), MIN_PRIO);
	gem_close(i915, t.scratch);

	munmap(t.page, 4096);

	if (ufd_cfg.vm)
		gem_vm_destroy(i915, ufd_cfg.vm);

	close(ufd);
}

static void measure_semaphore_power(int i915, const intel_ctx_t *ctx)
{
	const struct intel_execution_engine2 *signaler, *e;
	struct igt_power gpu, pkg;
	uint64_t ahnd = get_simple_l2h_ahnd(i915, ctx->id);

	igt_require(igt_power_open(i915, &gpu, "gpu") == 0);
	igt_power_open(i915, &pkg, "pkg");

	for_each_ctx_engine(i915, ctx, signaler) {
		struct {
			struct power_sample pkg, gpu;
		} s_spin[2], s_sema[2];
		double baseline, total;
		int64_t jiffie = 1;
		igt_spin_t *spin, *sema[GEM_MAX_ENGINES] = {};
		int i;

		if (!gem_class_can_store_dword(i915, signaler->class))
			continue;

		spin = __igt_spin_new(i915,
				      .ahnd = ahnd,
				      .ctx = ctx,
				      .engine = signaler->flags,
				      .flags = IGT_SPIN_POLL_RUN);
		gem_wait(i915, spin->handle, &jiffie); /* waitboost */
		igt_spin_busywait_until_started(spin);

		igt_power_get_energy(&pkg, &s_spin[0].pkg);
		igt_power_get_energy(&gpu, &s_spin[0].gpu);
		usleep(100*1000);
		igt_power_get_energy(&gpu, &s_spin[1].gpu);
		igt_power_get_energy(&pkg, &s_spin[1].pkg);

		/* Add a waiter to each engine */
		i = 0;
		for_each_ctx_engine(i915, ctx, e) {
			if (e->flags == signaler->flags) {
				i++;
				continue;
			}

			/*
			 * We need same spin->handle offset for each sema
			 * so we need to use SIMPLE allocator. As freeing
			 * spinner lead to alloc same offset for next batch
			 * we would serialize spinners. To avoid this on
			 * SIMPLE we just defer freeing spinners when
			 * all of them will be created and each of them
			 * will have separate offsets for batchbuffer.
			 */
			sema[i] = __igt_spin_new(i915,
						 .ahnd = ahnd,
						 .ctx = ctx,
						 .engine = e->flags,
						 .dependency = spin->handle);
			i++;
		}
		for (i = 0; i < GEM_MAX_ENGINES; i++)
			if (sema[i])
				igt_spin_free(i915, sema[i]);
		usleep(10); /* just give the tasklets a chance to run */

		igt_power_get_energy(&pkg, &s_sema[0].pkg);
		igt_power_get_energy(&gpu, &s_sema[0].gpu);
		usleep(100*1000);
		igt_power_get_energy(&gpu, &s_sema[1].gpu);
		igt_power_get_energy(&pkg, &s_sema[1].pkg);

		igt_spin_free(i915, spin);

		baseline = igt_power_get_mW(&gpu, &s_spin[0].gpu, &s_spin[1].gpu);
		total = igt_power_get_mW(&gpu, &s_sema[0].gpu, &s_sema[1].gpu);
		igt_info("%s: %.1fmW + %.1fmW (total %1.fmW)\n",
			 signaler->name,
			 baseline,
			 (total - baseline),
			 total);

		if (igt_power_valid(&pkg)) {
			baseline = igt_power_get_mW(&pkg, &s_spin[0].pkg, &s_spin[1].pkg);
			total = igt_power_get_mW(&pkg, &s_sema[0].pkg, &s_sema[1].pkg);
			igt_info("pkg: %.1fmW + %.1fmW (total %1.fmW)\n",
				 baseline,
				 (total - baseline),
				 total);
		}
	}
	igt_power_close(&gpu);
	igt_power_close(&pkg);
	put_ahnd(ahnd);
}

static int read_timestamp_frequency(int i915)
{
	int value = 0;
	drm_i915_getparam_t gp = {
		.value = &value,
		.param = I915_PARAM_CS_TIMESTAMP_FREQUENCY,
	};
	ioctl(i915, DRM_IOCTL_I915_GETPARAM, &gp);
	return value;
}

static uint64_t div64_u64_round_up(uint64_t x, uint64_t y)
{
	return (x + y - 1) / y;
}

static uint64_t ticks_to_ns(int i915, uint64_t ticks)
{
	return div64_u64_round_up(ticks * NSEC_PER_SEC,
				  read_timestamp_frequency(i915));
}

static int cmp_u32(const void *A, const void *B)
{
	const uint32_t *a = A, *b = B;

	if (*a < *b)
		return -1;
	else if (*a > *b)
		return 1;
	else
		return 0;
}

static uint32_t read_ctx_timestamp(int i915, const intel_ctx_t *ctx,
				   const struct intel_execution_engine2 *e)
{
	const int use_64b = intel_gen(intel_get_drm_devid(i915)) >= 8;
	const uint32_t base = gem_engine_mmio_base(i915, e->name);
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_exec_object2 obj = {
		.handle = gem_create(i915, 4096),
		.offset = 32 << 20,
		.relocs_ptr = to_user_pointer(&reloc),
		.relocation_count = 1,
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = e->flags,
		.rsvd1 = ctx->id,
	};
#define RUNTIME (base + 0x3a8)
	uint32_t *map, *cs;
	uint32_t ts;
	uint64_t ahnd = get_reloc_ahnd(i915, ctx->id);

	igt_require(base);

	if (ahnd) {
		obj.offset = get_offset(ahnd, obj.handle, 4096, 0);
		obj.flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
		obj.relocation_count = 0;
	}

	cs = map = gem_mmap__device_coherent(i915, obj.handle,
					     0, 4096, PROT_WRITE);

	*cs++ = 0x24 << 23 | (1 + use_64b); /* SRM */
	*cs++ = RUNTIME;
	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = obj.handle;
	reloc.presumed_offset = obj.offset;
	reloc.offset = offset_in_page(cs);
	reloc.delta = 4000;
	*cs++ = obj.offset + 4000;
	*cs++ = obj.offset >> 32;

	*cs++ = MI_BATCH_BUFFER_END;

	gem_execbuf(i915, &execbuf);
	gem_sync(i915, obj.handle);
	gem_close(i915, obj.handle);

	ts = map[1000];
	munmap(map, 4096);

	return ts;
}

static void fairslice(int i915, const intel_ctx_cfg_t *cfg,
		      const struct intel_execution_engine2 *e,
		      unsigned long flags,
		      int duration)
{
	const double timeslice_duration_ns = 1e6;
	igt_spin_t *spin = NULL;
	double threshold;
	const intel_ctx_t *ctx[3];
	uint32_t ts[3];
	uint64_t ahnd;

	for (int i = 0; i < ARRAY_SIZE(ctx); i++) {
		ctx[i] = intel_ctx_create(i915, cfg);
		if (spin == NULL) {
			ahnd = get_reloc_ahnd(i915, ctx[i]->id);
			spin = __igt_spin_new(i915,
					      .ahnd = ahnd,
					      .ctx = ctx[i],
					      .engine = e->flags,
					      .flags = flags);
		} else {
			struct drm_i915_gem_execbuffer2 eb = {
				.buffer_count = 1,
				.buffers_ptr = to_user_pointer(&spin->obj[IGT_SPIN_BATCH]),
				.flags = e->flags,
				.rsvd1 = ctx[i]->id,
			};
			gem_execbuf(i915, &eb);
		}
	}

	sleep(duration); /* over the course of many timeslices */

	igt_assert(gem_bo_busy(i915, spin->handle));
	igt_spin_end(spin);
	for (int i = 0; i < ARRAY_SIZE(ctx); i++)
		ts[i] = read_ctx_timestamp(i915, ctx[i], e);

	for (int i = 0; i < ARRAY_SIZE(ctx); i++)
		intel_ctx_destroy(i915, ctx[i]);
	igt_spin_free(i915, spin);
	put_ahnd(ahnd);

	/*
	 * If we imagine that the timeslices are randomly distributed to
	 * the clients, we would expect the variance to be modelled
	 * by a drunken walk; ergo sqrt(num_timeslices).
	 */
	threshold = sqrt(1e9 * duration / timeslice_duration_ns);
	threshold *= timeslice_duration_ns;
	threshold *= 2; /* CI safety factor before crying wolf */

	qsort(ts, 3, sizeof(*ts), cmp_u32);
	igt_info("%s: [%.1f, %.1f, %.1f] ms, expect %1.f +- %.1fms\n", e->name,
		 1e-6 * ticks_to_ns(i915, ts[0]),
		 1e-6 * ticks_to_ns(i915, ts[1]),
		 1e-6 * ticks_to_ns(i915, ts[2]),
		 1e3 * duration / 3,
		 1e-6 * threshold);

	igt_assert_f(ts[2], "CTX_TIMESTAMP not reported!\n");
	igt_assert_f(ticks_to_ns(i915, ts[2] - ts[0]) < 2 * threshold,
		     "Range of timeslices greater than tolerable: %.2fms > %.2fms; unfair!\n",
		     1e-6 * ticks_to_ns(i915, ts[2] - ts[0]),
		     1e-6 * threshold * 2);
}

#define test_each_engine(T, i915, ctx, e) \
	igt_subtest_with_dynamic(T) for_each_ctx_engine(i915, ctx, e) \
		igt_dynamic_f("%s", e->name)

#define test_each_engine_store(T, i915, ctx, e) \
	igt_subtest_with_dynamic(T) for_each_ctx_engine(i915, ctx, e) \
		for_each_if(gem_class_can_store_dword(fd, e->class)) \
		igt_dynamic_f("%s", e->name)

igt_main
{
	int fd = -1;
	const intel_ctx_t *ctx = NULL;

	igt_fixture {
		igt_require_sw_sync();

		fd = drm_open_driver_master(DRIVER_INTEL);
		gem_submission_print_method(fd);
		gem_scheduler_print_capability(fd);

		igt_require_gem(fd);
		gem_require_mmap_device_coherent(fd);
		gem_require_contexts(fd);
		ctx = intel_ctx_create_all_physical(fd);

		igt_fork_hang_detector(fd);
	}

	igt_subtest_group {
		const struct intel_execution_engine2 *e;

		test_each_engine_store("fifo", fd, ctx, e)
			fifo(fd, ctx, e->flags);

		test_each_engine_store("implicit-read-write", fd, ctx, e)
			implicit_rw(fd, ctx, e->flags, READ_WRITE);

		test_each_engine_store("implicit-write-read", fd, ctx, e)
			implicit_rw(fd, ctx, e->flags, WRITE_READ);

		test_each_engine_store("implicit-boths", fd, ctx, e)
			implicit_rw(fd, ctx, e->flags, READ_WRITE | WRITE_READ);

		test_each_engine_store("independent", fd, ctx, e)
			independent(fd, ctx, e->flags, 0);
		test_each_engine_store("u-independent", fd, ctx, e)
			independent(fd, ctx, e->flags, IGT_SPIN_USERPTR);
	}

	igt_subtest_group {
		const struct intel_execution_engine2 *e;

		igt_fixture {
			igt_require(gem_scheduler_enabled(fd));
			igt_require(gem_scheduler_has_ctx_priority(fd));
		}

		test_each_engine("timeslicing", fd, ctx, e)
			timeslice(fd, &ctx->cfg, e->flags);

		test_each_engine("thriceslice", fd, ctx, e)
			timesliceN(fd, &ctx->cfg, e->flags, 3);

		test_each_engine("manyslice", fd, ctx, e)
			timesliceN(fd, &ctx->cfg, e->flags, 67);

		test_each_engine("lateslice", fd, ctx, e)
			lateslice(fd, &ctx->cfg, e->flags, 0);
		test_each_engine("u-lateslice", fd, ctx, e)
			lateslice(fd, &ctx->cfg, e->flags, IGT_SPIN_USERPTR);

		igt_subtest_group {
			igt_fixture {
				igt_require(gem_scheduler_has_timeslicing(fd));
				igt_require(intel_gen(intel_get_drm_devid(fd)) >= 8);
			}

			test_each_engine("fairslice", fd, ctx, e)
				fairslice(fd, &ctx->cfg, e, 0, 2);

			test_each_engine("u-fairslice", fd, ctx, e)
				fairslice(fd, &ctx->cfg, e, IGT_SPIN_USERPTR, 2);

			igt_fixture {
				intel_allocator_multiprocess_start();
			}
			igt_subtest("fairslice-all")  {
				for_each_ctx_engine(fd, ctx, e) {
					igt_fork(child, 1)
						fairslice(fd, &ctx->cfg, e, 0, 2);
				}
				igt_waitchildren();
			}
			igt_subtest("u-fairslice-all")  {
				for_each_ctx_engine(fd, ctx, e) {
					igt_fork(child, 1)
						fairslice(fd, &ctx->cfg, e,
							  IGT_SPIN_USERPTR,
							  2);
				}
				igt_waitchildren();
			}
			igt_fixture {
				intel_allocator_multiprocess_stop();
			}
		}

		test_each_engine("submit-early-slice", fd, ctx, e)
			submit_slice(fd, &ctx->cfg, e, EARLY_SUBMIT);
		test_each_engine("u-submit-early-slice", fd, ctx, e)
			submit_slice(fd, &ctx->cfg, e, EARLY_SUBMIT | USERPTR);
		test_each_engine("submit-golden-slice", fd, ctx, e)
			submit_slice(fd, &ctx->cfg, e, 0);
		test_each_engine("u-submit-golden-slice", fd, ctx, e)
			submit_slice(fd, &ctx->cfg, e, USERPTR);
		test_each_engine("submit-late-slice", fd, ctx, e)
			submit_slice(fd, &ctx->cfg, e, LATE_SUBMIT);
		test_each_engine("u-submit-late-slice", fd, ctx, e)
			submit_slice(fd, &ctx->cfg, e, LATE_SUBMIT | USERPTR);

		igt_subtest("semaphore-user")
			semaphore_userlock(fd, ctx, 0);
		igt_subtest("semaphore-codependency")
			semaphore_codependency(fd, ctx, 0);
		igt_subtest("semaphore-resolve")
			semaphore_resolve(fd, &ctx->cfg, 0);
		igt_subtest("semaphore-noskip")
			semaphore_noskip(fd, &ctx->cfg, 0);

		igt_subtest("u-semaphore-user")
			semaphore_userlock(fd, ctx, IGT_SPIN_USERPTR);
		igt_subtest("u-semaphore-codependency")
			semaphore_codependency(fd, ctx, IGT_SPIN_USERPTR);
		igt_subtest("u-semaphore-resolve")
			semaphore_resolve(fd, &ctx->cfg, IGT_SPIN_USERPTR);
		igt_subtest("u-semaphore-noskip")
			semaphore_noskip(fd, &ctx->cfg, IGT_SPIN_USERPTR);

		igt_subtest("smoketest-all")
			smoketest(fd, &ctx->cfg, ALL_ENGINES, 30);

		test_each_engine_store("in-order", fd, ctx, e)
			reorder(fd, &ctx->cfg, e->flags, EQUAL);

		test_each_engine_store("out-order", fd, ctx, e)
			reorder(fd, &ctx->cfg, e->flags, 0);

		test_each_engine_store("promotion", fd, ctx, e)
			promotion(fd, &ctx->cfg, e->flags);

		igt_subtest_group {
			igt_fixture {
				igt_require(gem_scheduler_has_preemption(fd));
			}

			test_each_engine_store("preempt", fd, ctx, e)
				preempt(fd, &ctx->cfg, e, 0);

			test_each_engine_store("preempt-contexts", fd, ctx, e)
				preempt(fd, &ctx->cfg, e, NEW_CTX);

			test_each_engine_store("preempt-user", fd, ctx, e)
				preempt(fd, &ctx->cfg, e, USERPTR);

			test_each_engine_store("preempt-self", fd, ctx, e)
				preempt_self(fd, &ctx->cfg, e->flags);

			test_each_engine_store("preempt-other", fd, ctx, e)
				preempt_other(fd, &ctx->cfg, e->flags, 0);

			test_each_engine_store("preempt-other-chain", fd, ctx, e)
				preempt_other(fd, &ctx->cfg, e->flags, CHAIN);

			test_each_engine_store("preempt-engines", fd, ctx, e)
				preempt_engines(fd, e, 0);

			igt_subtest_group {
				igt_fixture {
					igt_require(!gem_scheduler_has_static_priority(fd));
				}

				test_each_engine_store("preempt-queue", fd, ctx, e)
					preempt_queue(fd, &ctx->cfg, e->flags, 0);

				test_each_engine_store("preempt-queue-chain", fd, ctx, e)
					preempt_queue(fd, &ctx->cfg, e->flags, CHAIN);
				test_each_engine_store("preempt-queue-contexts", fd, ctx, e)
					preempt_queue(fd, &ctx->cfg, e->flags, CONTEXTS);

				test_each_engine_store("preempt-queue-contexts-chain", fd, ctx, e)
					preempt_queue(fd, &ctx->cfg, e->flags, CONTEXTS | CHAIN);
			}

			igt_subtest_group {
				igt_hang_t hang;

				igt_fixture {
					igt_stop_hang_detector();
					hang = igt_allow_hang(fd, ctx->id, 0);
				}

				test_each_engine_store("preempt-hang", fd, ctx, e)
					preempt(fd, &ctx->cfg, e, NEW_CTX | HANG_LP);

				test_each_engine_store("preemptive-hang", fd, ctx, e)
					preemptive_hang(fd, &ctx->cfg, e);

				igt_fixture {
					igt_disallow_hang(fd, hang);
					igt_fork_hang_detector(fd);
				}
			}
		}

		test_each_engine_store("noreorder", fd, ctx, e)
			noreorder(fd, &ctx->cfg, e->flags, 0, 0);

		test_each_engine_store("noreorder-priority", fd, ctx, e) {
			igt_require(gem_scheduler_enabled(fd));
			noreorder(fd, &ctx->cfg, e->flags, MAX_PRIO, 0);
		}

		test_each_engine_store("noreorder-corked", fd, ctx, e) {
			igt_require(gem_scheduler_enabled(fd));
			noreorder(fd, &ctx->cfg, e->flags, MAX_PRIO, CORKED);
		}

		test_each_engine_store("deep", fd, ctx, e)
			deep(fd, &ctx->cfg, e->flags);

		test_each_engine_store("wide", fd, ctx, e)
			wide(fd, &ctx->cfg, e->flags);

		test_each_engine_store("smoketest", fd, ctx, e)
			smoketest(fd, &ctx->cfg, e->flags, 5);

		igt_subtest_group {
			igt_fixture {
				igt_require(!gem_scheduler_has_static_priority(fd));
			}

			test_each_engine_store("reorder-wide", fd, ctx, e)
				reorder_wide(fd, &ctx->cfg, e->flags);
		}
	}

	igt_subtest_group {
		const struct intel_execution_engine2 *e;

		igt_fixture {
			igt_require(gem_scheduler_enabled(fd));
			igt_require(gem_scheduler_has_ctx_priority(fd));
			igt_require(gem_scheduler_has_preemption(fd));
		}

		test_each_engine("pi-ringfull", fd, ctx, e)
			test_pi_ringfull(fd, &ctx->cfg, e->flags, 0);

		test_each_engine("pi-common", fd, ctx, e)
			test_pi_ringfull(fd, &ctx->cfg, e->flags, SHARED);

		test_each_engine("pi-userfault", fd, ctx, e)
			test_pi_userfault(fd, &ctx->cfg, e->flags);

		test_each_engine("pi-distinct-iova", fd, ctx, e)
			test_pi_iova(fd, &ctx->cfg, e->flags, 0);

		test_each_engine("pi-shared-iova", fd, ctx, e)
			test_pi_iova(fd, &ctx->cfg, e->flags, SHARED);
	}

	igt_subtest_group {
		igt_fixture {
			igt_require(gem_scheduler_enabled(fd));
			igt_require(gem_scheduler_has_semaphores(fd));
		}

		igt_subtest("semaphore-power")
			measure_semaphore_power(fd, ctx);
	}

	igt_fixture {
		igt_stop_hang_detector();
		intel_ctx_destroy(fd, ctx);
		close(fd);
	}
}
