/*
 * Copyright © 2009 Intel Corporation
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

/** @file gem_exec_whisper.c
 *
 * Pass around a value to write into a scratch buffer between lots of batches
 */

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "i915/gem_vm.h"
#include "igt.h"
#include "igt_debugfs.h"
#include "igt_gt.h"
#include "igt_power.h"
#include "igt_rand.h"
#include "igt_sysfs.h"
#include "intel_ctx.h"

#define ENGINE_MASK  (I915_EXEC_RING_MASK | I915_EXEC_BSD_MASK)

#define VERIFY 0

#define TIMEOUT 20
#define QLEN 16

static void check_bo(int fd, uint32_t handle, int pass)
{
	uint32_t *map;

	igt_debug("Verifying result\n");
	map = gem_mmap__cpu(fd, handle, 0, 4096, PROT_READ);
	gem_set_domain(fd, handle, I915_GEM_DOMAIN_CPU, 0);
	for (int i = 0; i < pass; i++)
		igt_assert_eq(map[i], i);
	munmap(map, 4096);
}

static void verify_reloc(int fd, uint32_t handle,
			 const struct drm_i915_gem_relocation_entry *reloc)
{
	if (VERIFY) {
		uint64_t target = 0;
		if (intel_gen(intel_get_drm_devid(fd)) >= 8)
			gem_read(fd, handle, reloc->offset, &target, 8);
		else
			gem_read(fd, handle, reloc->offset, &target, 4);
		igt_assert_eq_u64(target,
				  reloc->presumed_offset + reloc->delta);
	}
}

#define CONTEXTS 0x1
#define FDS 0x2
#define INTERRUPTIBLE 0x4
#define CHAIN 0x8
#define FORKED 0x10
#define HANG 0x20
#define SYNC 0x40
#define PRIORITY 0x80
#define ALL 0x100
#define QUEUES 0x200
#define BASIC 0x400

struct hang {
	const intel_ctx_t *ctx;
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	int fd;
	uint64_t ahnd;
	uint64_t bb_offset;
};

static void init_hang(struct hang *h, int fd, const intel_ctx_cfg_t *cfg)
{
	uint32_t *batch;
	int i, gen;

	h->fd = gem_reopen_driver(fd);
	igt_allow_hang(h->fd, 0, 0);

	gen = intel_gen(intel_get_drm_devid(h->fd));

	if (gem_has_contexts(fd)) {
		h->ctx = intel_ctx_create(h->fd, cfg);
		h->execbuf.rsvd1 = h->ctx->id;
		h->ahnd = get_reloc_ahnd(fd, h->ctx->id);
	} else {
		h->ctx = NULL;
		h->ahnd = get_reloc_ahnd(fd, 0);
	}

	memset(&h->execbuf, 0, sizeof(h->execbuf));
	h->execbuf.buffers_ptr = to_user_pointer(&h->obj);
	h->execbuf.buffer_count = 1;

	memset(&h->obj, 0, sizeof(h->obj));
	h->obj.handle = gem_create(h->fd, 4096);
	h->bb_offset = get_offset(h->ahnd, h->obj.handle, 4096, 0);
	if (h->ahnd)
		h->obj.flags |= EXEC_OBJECT_PINNED;

	h->obj.relocs_ptr = to_user_pointer(&h->reloc);
	h->obj.relocation_count = !h->ahnd ? 1 : 0;
	memset(&h->reloc, 0, sizeof(h->reloc));

	batch = gem_mmap__cpu(h->fd, h->obj.handle, 0, 4096, PROT_WRITE);
	gem_set_domain(h->fd, h->obj.handle,
		       I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);

	h->reloc.target_handle = h->obj.handle; /* recurse */
	h->reloc.presumed_offset = 0;
	h->reloc.offset = 5*sizeof(uint32_t);
	h->reloc.delta = 0;
	h->reloc.read_domains = I915_GEM_DOMAIN_COMMAND;
	h->reloc.write_domain = 0;

	i = 0;
	batch[i++] = 0xffffffff;
	batch[i++] = 0xdeadbeef;
	batch[i++] = 0xc00fee00;
	batch[i++] = 0x00c00fee;
	batch[i] = MI_BATCH_BUFFER_START;
	if (gen >= 8) {
		batch[i] |= 1 << 8 | 1;
		batch[++i] = h->bb_offset;
		batch[++i] = h->bb_offset >> 32;
	} else if (gen >= 6) {
		batch[i] |= 1 << 8;
		batch[++i] = 0;
	} else {
		batch[i] |= 2 << 6;
		batch[++i] = 0;
		if (gen < 4) {
			batch[i] |= 1;
			h->reloc.delta = 1;
		}
	}
	munmap(batch, 4096);
}

static void submit_hang(struct hang *h, unsigned *engines, int nengine, unsigned flags)
{
	while (nengine--) {
		h->execbuf.flags &= ~ENGINE_MASK;
		h->execbuf.flags |= *engines++;
		gem_execbuf(h->fd, &h->execbuf);
	}
	if (flags & SYNC)
		gem_sync(h->fd, h->obj.handle);
}

static void fini_hang(struct hang *h)
{
	put_offset(h->ahnd, h->bb_offset);
	put_ahnd(h->ahnd);
	intel_ctx_destroy(h->fd, h->ctx);
	close(h->fd);
}

static void ctx_set_random_priority(int fd, uint32_t ctx)
{
	int prio = hars_petruska_f54_1_random_unsafe_max(1024) - 512;
	gem_context_set_priority(fd, ctx, prio);
}

static void whisper(int fd, const intel_ctx_t *ctx,
		    unsigned engine, unsigned flags)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	const unsigned int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 batches[QLEN];
	struct drm_i915_gem_relocation_entry inter[QLEN];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_exec_object2 store, scratch;
	struct drm_i915_gem_exec_object2 tmp[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	unsigned engines[I915_EXEC_RING_MASK + 1];
	const struct intel_execution_engine2 *e;
	struct hang hang;
	int fds[64];
	intel_ctx_cfg_t local_cfg;
	const intel_ctx_t *contexts[64];
	unsigned nengine;
	uint32_t batch[16];
	unsigned int relocations = 0;
	unsigned int reloc_migrations = 0;
	unsigned int reloc_interruptions = 0;
	unsigned int eb_migrations = 0;
	struct power_sample sample[2];
	struct igt_power gpu;
	uint64_t old_offset;
	int i, n, loc;
	int debugfs;
	int nchild;
	bool has_relocs = gem_has_relocations(fd);

	if (flags & PRIORITY) {
		igt_require(gem_scheduler_enabled(fd));
		igt_require(gem_scheduler_has_ctx_priority(fd));
	}

	debugfs = igt_debugfs_dir(fd);
	igt_power_open(fd, &gpu, "gpu");

	nengine = 0;
	if (engine == ALL_ENGINES) {
		for_each_ctx_engine(fd, ctx, e) {
			if (gem_class_can_store_dword(fd, e->class))
				engines[nengine++] = e->flags;
		}
	} else {
		igt_assert(!(flags & ALL));
		engines[nengine++] = engine;
	}
	igt_require(nengine);

	if (flags & FDS)
		igt_require(gen >= 6);

	if (flags & CONTEXTS)
		gem_require_contexts(fd);

	if (flags & QUEUES) {
		igt_require(gem_has_vm(fd));
		igt_require(gem_context_has_single_timeline(fd));
	}

	if (flags & HANG)
		init_hang(&hang, fd, &ctx->cfg);

	nchild = 1;
	if (flags & FORKED)
		nchild *= sysconf(_SC_NPROCESSORS_ONLN);
	if (flags & ALL)
		nchild *= nengine;

	intel_detect_and_clear_missed_interrupts(fd);
	igt_power_get_energy(&gpu, &sample[0]);
	igt_fork(child, nchild) {
		unsigned int pass;

		if (flags & ALL) {
			engines[0] = engines[child % nengine];
			nengine = 1;
		}

		memset(&scratch, 0, sizeof(scratch));
		scratch.handle = gem_create(fd, 4096);
		scratch.flags = EXEC_OBJECT_WRITE;

		memset(&store, 0, sizeof(store));
		store.handle = gem_create(fd, 4096);
		store.relocs_ptr = to_user_pointer(&reloc);
		store.relocation_count = has_relocs ? 1 : 0;

		memset(&reloc, 0, sizeof(reloc));
		reloc.offset = sizeof(uint32_t);
		if (gen < 8 && gen >= 4)
			reloc.offset += sizeof(uint32_t);
		loc = 8;
		if (gen >= 4)
			loc += 4;
		reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		reloc.write_domain = I915_GEM_DOMAIN_INSTRUCTION;

		{
			tmp[0] = scratch;
			tmp[1] = store;
			gem_write(fd, store.handle, 0, &bbe, sizeof(bbe));

			memset(&execbuf, 0, sizeof(execbuf));
			execbuf.buffers_ptr = to_user_pointer(tmp);
			execbuf.buffer_count = 2;
			execbuf.flags = I915_EXEC_HANDLE_LUT;
			execbuf.flags |= I915_EXEC_NO_RELOC;
			if (gen < 6)
				execbuf.flags |= I915_EXEC_SECURE;

			execbuf.rsvd1 = ctx->id;
			igt_require(__gem_execbuf(fd, &execbuf) == 0);
			scratch = tmp[0];
			store = tmp[1];
		}

		if (!has_relocs) {
			scratch.flags |= EXEC_OBJECT_PINNED;
			store.flags |= EXEC_OBJECT_PINNED;
		}

		i = 0;
		batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
		if (gen >= 8) {
			batch[++i] = store.offset + loc;
			batch[++i] = (store.offset + loc) >> 32;
		} else if (gen >= 4) {
			batch[++i] = 0;
			batch[++i] = store.offset + loc;
		} else {
			batch[i]--;
			batch[++i] = store.offset + loc;
		}
		batch[++i] = 0xc0ffee;
		igt_assert(loc == sizeof(uint32_t) * i);
		batch[++i] = MI_BATCH_BUFFER_END;

		if (flags & FDS) {
			for (n = 0; n < 64; n++) {
				fds[n] = gem_reopen_driver(fd);
			}
		}
		if (flags & (CONTEXTS | QUEUES | FDS)) {
			local_cfg = ctx->cfg;
			if (flags & QUEUES) {
				igt_assert(!(flags & FDS));
				local_cfg.vm = gem_vm_create(fd);
				local_cfg.flags |= I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE;
			}
			for (n = 0; n < 64; n++) {
				int this_fd = (flags & FDS) ? fds[n] : fd;
				contexts[n] = intel_ctx_create(this_fd, &local_cfg);
			}
		}

		memset(batches, 0, sizeof(batches));
		for (n = 0; n < QLEN; n++) {
			batches[n].handle = gem_create(fd, 4096);
			gem_write(fd, batches[n].handle, 0, &bbe, sizeof(bbe));
		}
		execbuf.buffers_ptr = to_user_pointer(batches);
		execbuf.buffer_count = QLEN;
		gem_execbuf(fd, &execbuf);

		execbuf.buffers_ptr = to_user_pointer(tmp);
		execbuf.buffer_count = 2;

		old_offset = store.offset;
		for (n = 0; n < QLEN; n++) {
			if (gen >= 8) {
				batch[1] = old_offset + loc;
				batch[2] = (old_offset + loc) >> 32;
			} else if (gen >= 4) {
				batch[2] = old_offset + loc;
			} else {
				batch[1] = old_offset + loc;
			}

			inter[n] = reloc;
			inter[n].presumed_offset = old_offset;
			inter[n].delta = loc;
			batches[n].relocs_ptr = to_user_pointer(&inter[n]);
			batches[n].relocation_count = has_relocs ? 1 : 0;
			if (!has_relocs)
				batches[n].flags |= EXEC_OBJECT_PINNED;
			gem_write(fd, batches[n].handle, 0, batch, sizeof(batch));

			old_offset = batches[n].offset;
		}

		igt_while_interruptible(flags & INTERRUPTIBLE) {
			pass = 0;
			igt_until_timeout(TIMEOUT) {
				uint64_t offset;

				if (flags & HANG)
					submit_hang(&hang, engines, nengine, flags);

				if (flags & CHAIN) {
					execbuf.flags &= ~ENGINE_MASK;
					execbuf.flags |= engines[rand() % nengine];
				}

				reloc.presumed_offset = scratch.offset;
				reloc.delta = 4*pass;
				offset = reloc.presumed_offset + reloc.delta;

				i = 0;
				if (gen >= 8) {
					batch[++i] = offset;
					batch[++i] = offset >> 32;
				} else if (gen >= 4) {
					batch[++i] = 0;
					batch[++i] = offset;
				} else {
					batch[++i] = offset;
				}
				batch[++i] = ~pass;
				gem_write(fd, store.handle, 0, batch, sizeof(batch));

				tmp[0] = scratch;
				igt_assert(tmp[0].flags & EXEC_OBJECT_WRITE);
				tmp[1] = store;
				verify_reloc(fd, store.handle, &reloc);
				execbuf.buffers_ptr = to_user_pointer(tmp);

				gem_execbuf(fd, &execbuf);

				igt_assert_eq_u64(reloc.presumed_offset, tmp[0].offset);
				if (flags & SYNC)
					gem_sync(fd, tmp[0].handle);
				scratch = tmp[0];

				gem_write(fd, batches[QLEN-1].handle, loc, &pass, sizeof(pass));
				for (n = QLEN; --n >= 1; ) {
					uint32_t handle[2] = {};
					int this_fd = fd;

					execbuf.buffers_ptr = to_user_pointer(&batches[n-1]);
					reloc_migrations += batches[n-1].offset != inter[n].presumed_offset;
					batches[n-1].offset = inter[n].presumed_offset;
					old_offset = inter[n].presumed_offset;
					batches[n-1].relocation_count = 0;
					batches[n-1].flags |= EXEC_OBJECT_WRITE;
					verify_reloc(fd, batches[n].handle, &inter[n]);

					if (flags & FDS) {
						this_fd = fds[rand() % 64];
						handle[0] = batches[n-1].handle;
						handle[1] = batches[n].handle;
						batches[n-1].handle =
							gem_open(this_fd,
								 gem_flink(fd, handle[0]));
						batches[n].handle =
							gem_open(this_fd,
								 gem_flink(fd, handle[1]));
						if (flags & PRIORITY)
							ctx_set_random_priority(this_fd, 0);
					}

					if (!(flags & CHAIN)) {
						execbuf.flags &= ~ENGINE_MASK;
						execbuf.flags |= engines[rand() % nengine];
					}
					if (flags & (CONTEXTS | QUEUES | FDS)) {
						execbuf.rsvd1 = contexts[rand() % 64]->id;
						if (flags & PRIORITY)
							ctx_set_random_priority(this_fd, execbuf.rsvd1);
					}

					gem_execbuf(this_fd, &execbuf);
					if (inter[n].presumed_offset == -1) {
						reloc_interruptions++;
						inter[n].presumed_offset = batches[n-1].offset;
					}
					igt_assert_eq_u64(inter[n].presumed_offset, batches[n-1].offset);

					if (flags & SYNC)
						gem_sync(this_fd, batches[n-1].handle);
					relocations += inter[n].presumed_offset != old_offset;

					batches[n-1].relocation_count = has_relocs ? 1 : 0;
					batches[n-1].flags &= ~EXEC_OBJECT_WRITE;

					if (this_fd != fd) {
						gem_close(this_fd, batches[n-1].handle);
						batches[n-1].handle = handle[0];

						gem_close(this_fd, batches[n].handle);
						batches[n].handle = handle[1];
					}
				}
				execbuf.flags &= ~ENGINE_MASK;
				execbuf.rsvd1 = ctx->id;
				execbuf.buffers_ptr = to_user_pointer(&tmp);

				tmp[0] = tmp[1];
				tmp[0].relocation_count = 0;
				tmp[0].flags = EXEC_OBJECT_WRITE;
				if (!has_relocs)
					tmp[0].flags |= EXEC_OBJECT_PINNED;
				reloc_migrations += tmp[0].offset != inter[0].presumed_offset;
				tmp[0].offset = inter[0].presumed_offset;
				old_offset = tmp[0].offset;
				tmp[1] = batches[0];
				verify_reloc(fd, batches[0].handle, &inter[0]);
				gem_execbuf(fd, &execbuf);
				if (inter[0].presumed_offset == -1) {
					reloc_interruptions++;
					inter[0].presumed_offset = tmp[0].offset;
				}

				igt_assert_eq_u64(inter[0].presumed_offset, tmp[0].offset);
				relocations += inter[0].presumed_offset != old_offset;
				batches[0] = tmp[1];

				tmp[1] = tmp[0];
				tmp[0] = scratch;
				igt_assert(tmp[0].flags & EXEC_OBJECT_WRITE);
				igt_assert_eq_u64(reloc.presumed_offset, tmp[0].offset);
				igt_assert(tmp[1].relocs_ptr == to_user_pointer(&reloc));
				tmp[1].relocation_count = has_relocs ? 1 : 0;
				tmp[1].flags &= ~EXEC_OBJECT_WRITE;
				verify_reloc(fd, store.handle, &reloc);
				gem_execbuf(fd, &execbuf);
				eb_migrations += tmp[0].offset != scratch.offset;
				eb_migrations += tmp[1].offset != store.offset;
				igt_assert_eq_u64(reloc.presumed_offset, tmp[0].offset);
				if (flags & SYNC)
					gem_sync(fd, tmp[0].handle);

				store = tmp[1];
				scratch = tmp[0];

				if (++pass == 1024)
					break;
			}
			igt_debug("Completed %d/1024 passes\n", pass);
		}
		igt_info("Number of migrations for execbuf: %d\n", eb_migrations);
		igt_info("Number of migrations for reloc: %d, interrupted %d, patched %d\n", reloc_migrations, reloc_interruptions, relocations);

		check_bo(fd, scratch.handle, pass);
		gem_close(fd, scratch.handle);
		gem_close(fd, store.handle);

		if (flags & (CONTEXTS | QUEUES | FDS)) {
			for (n = 0; n < 64; n++) {
				int this_fd = (flags & FDS) ? fds[n] : fd;
				intel_ctx_destroy(this_fd, contexts[n]);
			}
			if (local_cfg.vm) {
				igt_assert(!(flags & FDS));
				gem_vm_destroy(fd, local_cfg.vm);
			}
		}
		for (n = 0; n < QLEN; n++)
			gem_close(fd, batches[n].handle);
		if (flags & FDS) {
			for (n = 0; n < 64; n++)
				close(fds[n]);
		}
	}

	igt_waitchildren();

	if (flags & HANG)
		fini_hang(&hang);
	else
		igt_assert_eq(intel_detect_and_clear_missed_interrupts(fd), 0);

	igt_power_get_energy(&gpu, &sample[1]);
	if (sample[1].energy) {
		igt_info("Total energy used: %.1fmJ\n",
			 igt_power_get_mJ(&gpu, &sample[0], &sample[1]));
	}

	igt_power_close(&gpu);
	close(debugfs);
}

igt_main
{
	const struct mode {
		const char *name;
		unsigned flags;
	} modes[] = {
		{ "normal", BASIC },
		{ "interruptible", INTERRUPTIBLE },
		{ "forked", BASIC | FORKED },
		{ "sync", BASIC | SYNC },
		{ "chain", CHAIN },
		{ "chain-forked", CHAIN | FORKED },
		{ "chain-interruptible", CHAIN | INTERRUPTIBLE },
		{ "chain-sync", CHAIN | SYNC },
		{ "fds", BASIC | FDS },
		{ "fds-interruptible", FDS | INTERRUPTIBLE},
		{ "fds-forked", BASIC | FDS | FORKED},
		{ "fds-priority", BASIC | FDS | FORKED | PRIORITY },
		{ "fds-chain", FDS | CHAIN},
		{ "fds-sync", FDS | SYNC},
		{ "contexts", BASIC | CONTEXTS },
		{ "contexts-interruptible", CONTEXTS | INTERRUPTIBLE},
		{ "contexts-forked", BASIC | CONTEXTS | FORKED},
		{ "contexts-priority", BASIC | CONTEXTS | FORKED | PRIORITY },
		{ "contexts-chain", CONTEXTS | CHAIN },
		{ "contexts-sync", CONTEXTS | SYNC },
		{ "queues", BASIC | QUEUES },
		{ "queues-interruptible", QUEUES | INTERRUPTIBLE},
		{ "queues-forked", BASIC | QUEUES | FORKED},
		{ "queues-priority", BASIC | QUEUES | FORKED | PRIORITY },
		{ "queues-chain", QUEUES | CHAIN },
		{ "queues-sync", QUEUES | SYNC },
		{ NULL }
	};
	const struct intel_execution_engine2 *e;
	const intel_ctx_t *ctx;
	int fd = -1;

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		igt_require_gem(fd);
		igt_require(gem_can_store_dword(fd, 0));
		gem_submission_print_method(fd);
		ctx = intel_ctx_create_all_physical(fd);

		igt_fork_hang_detector(fd);
		intel_allocator_multiprocess_start();
	}

	for (const struct mode *m = modes; m->name; m++) {
		igt_subtest_f("%s%s",
			      m->flags & BASIC ? "basic-" : "", m->name)
			whisper(fd, ctx, ALL_ENGINES, m->flags);
		igt_subtest_f("%s%s-all",
			      m->flags & BASIC ? "basic-" : "", m->name)
			whisper(fd, ctx, ALL_ENGINES, m->flags | ALL);
	}

	for (const struct mode *m = modes; m->name; m++) {
		if (m->flags & CHAIN)
			continue;

		igt_subtest_with_dynamic_f("%s-engine", m->name) {
			for_each_ctx_engine(fd, ctx, e) {
				if (!gem_class_can_store_dword(fd, e->class))
					continue;

				igt_dynamic_f("%s", e->name)
					whisper(fd, ctx, e->flags, m->flags);
			}
		}
	}

	igt_fixture {
		igt_stop_hang_detector();
	}

	igt_subtest_group {
		for (const struct mode *m = modes; m->name; m++) {
			if (m->flags & INTERRUPTIBLE)
				continue;
			igt_subtest_f("hang-%s", m->name)
				whisper(fd, ctx, ALL_ENGINES, m->flags | HANG);
		}
	}

	igt_fixture {
		intel_allocator_multiprocess_stop();
		intel_ctx_destroy(fd, ctx);
		close(fd);
	}
}
