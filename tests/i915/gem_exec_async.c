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

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"

IGT_TEST_DESCRIPTION("Check that we can issue concurrent writes across the engines.");

#define SZ_1M (1024 * 1024)

static void store_dword(int fd, int id, const intel_ctx_t *ctx,
			 unsigned ring, uint32_t target, uint64_t target_offset,
			 uint32_t offset, uint32_t value)
{
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t batch[16];
	int i;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(obj);
	execbuf.buffer_count = 2;
	execbuf.flags = ring;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;
	execbuf.rsvd1 = ctx->id;

	memset(obj, 0, sizeof(obj));
	obj[0].handle = target;
	obj[0].flags = EXEC_OBJECT_ASYNC;
	obj[1].handle = gem_create(fd, 4096);

	if (id) {
		obj[0].offset = target_offset;
		obj[0].flags |= EXEC_OBJECT_PINNED | EXEC_OBJECT_WRITE |
				EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
		obj[1].offset = (id + 1) * SZ_1M;
		obj[1].flags |= EXEC_OBJECT_PINNED |
				EXEC_OBJECT_SUPPORTS_48B_ADDRESS;
	}

	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = obj[0].handle;
	reloc.presumed_offset = 0;
	reloc.offset = sizeof(uint32_t);
	reloc.delta = offset;
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = I915_GEM_DOMAIN_INSTRUCTION;
	obj[1].relocs_ptr = to_user_pointer(&reloc);
	obj[1].relocation_count = !id ? 1 : 0;

	i = 0;
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = target_offset + offset;
		batch[++i] = (target_offset + offset) >> 32;
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = offset;
		reloc.offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = offset;
	}
	batch[++i] = value;
	batch[++i] = MI_BATCH_BUFFER_END;
	gem_write(fd, obj[1].handle, 0, batch, sizeof(batch));
	gem_execbuf(fd, &execbuf);
	gem_sync(fd, obj[1].handle);
	gem_close(fd, obj[1].handle);
}

static void one(int fd, const intel_ctx_t *ctx,
		unsigned engine, unsigned int flags)
#define FORKED (1 << 0)
{
	const struct intel_execution_engine2 *e;
	uint32_t scratch = gem_create(fd, 4096);
	igt_spin_t *spin;
	uint32_t *result;
	uint64_t ahnd = get_simple_l2h_ahnd(fd, ctx->id);
	uint64_t scratch_offset = get_offset(ahnd, scratch, 4096, 0);
	int i;

	/*
	 * On the target ring, create a looping batch that marks
	 * the scratch for write. Then on the other rings try and
	 * write into that target. If it blocks we hang the GPU...
	 */
	spin = igt_spin_new(fd,
			    .ahnd = ahnd,
			    .ctx = ctx,
			    .engine = engine,
			    .dependency = scratch);

	i = 0;
	for_each_ctx_engine(fd, ctx, e) {
		/*
		 * We need to have same scratch_offset within spinner and
		 * store_dword(). That's why simple allocator was chosen.
		 * We can pass ahnd (simple) to store_dword() but it cannot
		 * be used to acquire batch offset, because likely get same
		 * offset for different batches.
		 *
		 * That's why we pass id which allows us calculate distinct
		 * batch offset for each child.
		 */
		int id = ahnd ? (i + 1) : 0;

		if (e->flags == engine)
			continue;

		if (!gem_class_can_store_dword(fd, e->class))
			continue;

		if (flags & FORKED) {
			igt_fork(child, 1) {
				store_dword(fd, id, ctx, e->flags,
					    scratch, scratch_offset,
					    4*i, ~i);
			}
		} else {
			store_dword(fd, id, ctx, e->flags,
				    scratch, scratch_offset,
				    4*i, ~i);
		}
		i++;
	}
	igt_waitchildren();

	result = gem_mmap__device_coherent(fd, scratch, 0, 4096, PROT_READ);
	while (i--)
		igt_assert_eq_u32(result[i], ~i);
	munmap(result, 4096);

	igt_spin_free(fd, spin);
	gem_close(fd, scratch);
	put_ahnd(ahnd);
}

static bool has_async_execbuf(int fd)
{
	drm_i915_getparam_t gp;
	int async = -1;

	gp.param = I915_PARAM_HAS_EXEC_ASYNC;
	gp.value = &async;
	drmIoctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);

	return async > 0;
}

#define test_each_engine(T, i915, ctx, e) \
	igt_subtest_with_dynamic(T) for_each_ctx_engine(i915, ctx, e) \
		igt_dynamic_f("%s", (e)->name)

igt_main
{
	const struct intel_execution_engine2 *e;
	const intel_ctx_t *ctx;
	int fd = -1;

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		igt_require_gem(fd);
		gem_require_mmap_device_coherent(fd);
		igt_require(has_async_execbuf(fd));

		ctx = intel_ctx_create_all_physical(fd);

		igt_fork_hang_detector(fd);
	}

	test_each_engine("concurrent-writes", fd, ctx, e)
		one(fd, ctx, e->flags, 0);

	igt_subtest_group {
		igt_fixture {
			intel_allocator_multiprocess_start();
		}

		test_each_engine("forked-writes", fd, ctx, e)
			one(fd, ctx, e->flags, FORKED);

		igt_fixture {
			intel_allocator_multiprocess_stop();
		}
	}

	igt_fixture {
		igt_stop_hang_detector();
		intel_ctx_destroy(fd, ctx);
		close(fd);
	}
}
