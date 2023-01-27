// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

#include <stdatomic.h>
#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"
#include "igt_aux.h"
#include "intel_allocator.h"

#define OBJ_SIZE 1024

struct test_obj {
	uint32_t handle;
	uint64_t offset;
	uint64_t size;
};

static _Atomic(uint32_t) next_handle;

static inline uint32_t gem_handle_gen(void)
{
	return atomic_fetch_add(&next_handle, 1);
}

static void alloc_simple(int fd)
{
	uint64_t ahnd;
	uint64_t offset0, offset1, size = 0x1000, align = 0x1000, start, end;
	bool is_allocated, freed;

	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_SIMPLE);

	offset0 = intel_allocator_alloc(ahnd, 1, size, align);
	offset1 = intel_allocator_alloc(ahnd, 1, size, align);
	igt_assert(offset0 == offset1);

	is_allocated = intel_allocator_is_allocated(ahnd, 1, size, offset0);
	igt_assert(is_allocated);

	freed = intel_allocator_free(ahnd, 1);
	igt_assert(freed);

	is_allocated = intel_allocator_is_allocated(ahnd, 1, size, offset0);
	igt_assert(!is_allocated);

	freed = intel_allocator_free(ahnd, 1);
	igt_assert(!freed);

	intel_allocator_get_address_range(ahnd, &start, &end);
	offset0 = intel_allocator_alloc(ahnd, 1, end - start, 0);
	offset1 = __intel_allocator_alloc(ahnd, 2, 4096, 0, ALLOC_STRATEGY_NONE);
	igt_assert(offset1 == ALLOC_INVALID_ADDRESS);
	intel_allocator_free(ahnd, 1);

	offset0 = intel_allocator_alloc_with_strategy(ahnd, 1, 4096, 0,
						      ALLOC_STRATEGY_HIGH_TO_LOW);
	offset1 = intel_allocator_alloc_with_strategy(ahnd, 2, 4096, 0,
						      ALLOC_STRATEGY_LOW_TO_HIGH);
	igt_assert(offset0 > offset1);

	intel_allocator_free(ahnd, 1);
	intel_allocator_free(ahnd, 2);

	igt_assert_eq(intel_allocator_close(ahnd), true);
}

static void reserve_simple(int fd)
{
	uint64_t ahnd, start, size = 0x1000;
	bool reserved, unreserved;

	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_SIMPLE);
	intel_allocator_get_address_range(ahnd, &start, NULL);

	reserved = intel_allocator_reserve(ahnd, 0, size, start);
	igt_assert(reserved);

	reserved = intel_allocator_is_reserved(ahnd, size, start);
	igt_assert(reserved);

	reserved = intel_allocator_reserve(ahnd, 0, size, start);
	igt_assert(!reserved);

	unreserved = intel_allocator_unreserve(ahnd, 0, size, start);
	igt_assert(unreserved);

	reserved = intel_allocator_is_reserved(ahnd, size, start);
	igt_assert(!reserved);

	igt_assert_eq(intel_allocator_close(ahnd), true);
}

static void reserve(int fd, uint8_t type)
{
	struct test_obj obj;
	uint64_t ahnd, offset = 0x40000, size = 0x1000;

	ahnd = intel_allocator_open(fd, 0, type);

	igt_assert_eq(intel_allocator_reserve(ahnd, 0, size, offset), true);
	/* try overlapping won't succeed */
	igt_assert_eq(intel_allocator_reserve(ahnd, 0, size, offset + size/2), false);

	obj.handle = gem_handle_gen();
	obj.size = OBJ_SIZE;
	obj.offset = intel_allocator_alloc(ahnd, obj.handle, obj.size, 0);

	igt_assert_eq(intel_allocator_reserve(ahnd, 0, obj.size, obj.offset), false);
	intel_allocator_free(ahnd, obj.handle);
	igt_assert_eq(intel_allocator_reserve(ahnd, 0, obj.size, obj.offset), true);

	igt_assert_eq(intel_allocator_unreserve(ahnd, 0, obj.size, obj.offset), true);
	igt_assert_eq(intel_allocator_unreserve(ahnd, 0, size, offset), true);
	igt_assert_eq(intel_allocator_reserve(ahnd, 0, size, offset + size/2), true);
	igt_assert_eq(intel_allocator_unreserve(ahnd, 0, size, offset + size/2), true);

	igt_assert_eq(intel_allocator_close(ahnd), true);
}

static void default_alignment(int fd)
{
	struct test_obj obj[3];
	uint64_t ahnd, default_alignment = 0x4000;

	ahnd = intel_allocator_open_full(fd, 0, 0, 0, INTEL_ALLOCATOR_SIMPLE,
					 ALLOC_STRATEGY_LOW_TO_HIGH,
					 default_alignment);

	for (int i = 0; i < ARRAY_SIZE(obj); i++) {
		obj[i].handle = gem_handle_gen();
		obj[i].offset = intel_allocator_alloc(ahnd, obj[i].handle, 4096,
				i == 2 ? 4096 : 0);
		igt_debug("obj[%d].offset: %llx, handle: %u\n", i,
			 (long long) obj[i].offset, obj[i].handle);
	}

	igt_assert_eq(obj[1].offset - obj[0].offset, default_alignment);
	/* obj[2] should be between obj[0] and obj[1] */
	igt_assert(obj[0].offset < obj[2].offset);
	igt_assert(obj[2].offset < obj[1].offset);

	for (int i = 0; i < ARRAY_SIZE(obj); i++)
		intel_allocator_free(ahnd, obj[i].handle);

	igt_assert_eq(intel_allocator_close(ahnd), true);
}

static bool overlaps(struct test_obj *buf1, struct test_obj *buf2)
{
	uint64_t begin1 = buf1->offset;
	uint64_t end1 = buf1->offset + buf1->size;
	uint64_t begin2 = buf2->offset;
	uint64_t end2 = buf2->offset + buf2->size;

	return (end1 > begin2 && end2 > end1) || (end2 > begin1 && end1 > end2);
}

static void basic_alloc(int fd, int cnt, uint8_t type)
{
	struct test_obj *obj;
	uint64_t ahnd;
	int i, j;

	ahnd = intel_allocator_open(fd, 0, type);
	obj = malloc(sizeof(struct test_obj) * cnt);

	for (i = 0; i < cnt; i++) {
		igt_progress("allocating objects: ", i, cnt);
		obj[i].handle = gem_handle_gen();
		obj[i].size = OBJ_SIZE;
		obj[i].offset = intel_allocator_alloc(ahnd, obj[i].handle,
						      obj[i].size, 4096);
		igt_assert_eq(obj[i].offset % 4096, 0);
	}

	for (i = 0; i < cnt; i++) {
		igt_progress("check overlapping: ", i, cnt);

		for (j = 0; j < cnt; j++) {
			if (j == i)
				continue;
			igt_assert(!overlaps(&obj[i], &obj[j]));
		}
	}

	for (i = 0; i < cnt; i++) {
		igt_progress("freeing objects: ", i, cnt);
		intel_allocator_free(ahnd, obj[i].handle);
	}

	free(obj);
	igt_assert_eq(intel_allocator_close(ahnd), true);
}

#define NUM_OBJS 128
static void reuse(int fd, uint8_t type)
{
	struct test_obj obj[128], tmp;
	uint64_t ahnd, prev_offset;
	uint64_t align = 0x40;
	int i;

	ahnd = intel_allocator_open(fd, 0, type);

	for (i = 0; i < NUM_OBJS; i++) {
		obj[i].handle = gem_handle_gen();
		obj[i].size = OBJ_SIZE;
		obj[i].offset = intel_allocator_alloc(ahnd, obj[i].handle,
						      obj[i].size, align);
	}

	/* check reuse */
	for (i = 0; i < NUM_OBJS; i++) {
		prev_offset = obj[i].offset;
		obj[i].offset = intel_allocator_alloc(ahnd, obj[i].handle,
						      obj[i].size, 0);
		igt_assert(prev_offset == obj[i].offset);
	}
	i--;

	/* free previously allocated bo */
	intel_allocator_free(ahnd, obj[i].handle);
	/* alloc different buffer to fill freed hole */
	tmp.handle = gem_handle_gen();
	tmp.offset = intel_allocator_alloc(ahnd, tmp.handle, OBJ_SIZE, align);

	/* Simple will return previously returned offset if fits */
	if (type == INTEL_ALLOCATOR_SIMPLE)
		igt_assert(prev_offset == tmp.offset);
	/* Reloc is moving forward for new allocations */
	else if (type == INTEL_ALLOCATOR_RELOC)
		igt_assert(prev_offset != tmp.offset);

	obj[i].offset = intel_allocator_alloc(ahnd, obj[i].handle,
					      obj[i].size, 0);
	igt_assert(prev_offset != obj[i].offset);
	intel_allocator_free(ahnd, tmp.handle);

	for (i = 0; i < 128; i++)
		intel_allocator_free(ahnd, obj[i].handle);

	igt_assert_eq(intel_allocator_close(ahnd), true);
}

struct ial_thread_args {
	uint64_t ahnd;
	pthread_t thread;
	uint32_t *handles;
	uint64_t *offsets;
	uint32_t count;
	int threads;
	int idx;
};

static void *alloc_bo_in_thread(void *arg)
{
	struct ial_thread_args *a = arg;
	int i;

	for (i = a->idx; i < a->count; i += a->threads) {
		a->handles[i] = gem_handle_gen();
		a->offsets[i] = intel_allocator_alloc(a->ahnd, a->handles[i], OBJ_SIZE,
						      1UL << ((random() % 20) + 1));
	}

	return NULL;
}

static void *free_bo_in_thread(void *arg)
{
	struct ial_thread_args *a = arg;
	int i;

	for (i = (a->idx + 1) % a->threads; i < a->count; i += a->threads)
		intel_allocator_free(a->ahnd, a->handles[i]);

	return NULL;
}

#define THREADS 6

static void parallel_one(int fd, uint8_t type)
{
	struct ial_thread_args a[THREADS];
	uint32_t *handles;
	uint64_t ahnd, *offsets;
	int count, i;

	srandom(0xdeadbeef);
	ahnd = intel_allocator_open(fd, 0, type);
	count = 1UL << 12;

	handles = malloc(sizeof(uint32_t) * count);
	offsets = calloc(1, sizeof(uint64_t) * count);

	for (i = 0; i < THREADS; i++) {
		a[i].ahnd = ahnd;
		a[i].handles = handles;
		a[i].offsets = offsets;
		a[i].count = count;
		a[i].threads = THREADS;
		a[i].idx = i;
		pthread_create(&a[i].thread, NULL, alloc_bo_in_thread, &a[i]);
	}

	for (i = 0; i < THREADS; i++)
		pthread_join(a[i].thread, NULL);

	/* Check if all objects are allocated */
	for (i = 0; i < count; i++) {
		/* Reloc don't have state. */
		if (type == INTEL_ALLOCATOR_RELOC)
			break;

		igt_assert_eq(offsets[i],
			      intel_allocator_alloc(a->ahnd, handles[i], OBJ_SIZE, 0));
	}

	for (i = 0; i < THREADS; i++)
		pthread_create(&a[i].thread, NULL, free_bo_in_thread, &a[i]);

	for (i = 0; i < THREADS; i++)
		pthread_join(a[i].thread, NULL);

	free(handles);
	free(offsets);

	igt_assert_eq(intel_allocator_close(ahnd), true);
}

static void standalone(int fd)
{
	uint64_t ahnd, offset, size = 4096;
	uint32_t handle = 1, child_handle = 2;
	uint64_t *shared;

	shared = mmap(0, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);

	intel_allocator_multiprocess_start();

	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_SIMPLE);
	offset = intel_allocator_alloc(ahnd, handle, size, 0);

	igt_fork(child, 2) {
		/*
		 * Use standalone allocator for child 0, detach from parent,
		 * child 1 use allocator from parent.
		 */
		if (child == 0)
			intel_allocator_init();

		ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_SIMPLE);
		shared[child] = intel_allocator_alloc(ahnd, child_handle, size, 0);

		intel_allocator_free(ahnd, child_handle);
		intel_allocator_close(ahnd);
	}
	igt_waitchildren();
	igt_assert_eq(offset, shared[0]);
	igt_assert_neq(offset, shared[1]);

	intel_allocator_free(ahnd, handle);
	igt_assert_eq(intel_allocator_close(ahnd), true);

	intel_allocator_multiprocess_stop();

	munmap(shared, 4096);
}

#define SIMPLE_GROUP_ALLOCS 8
static void __simple_allocs(int fd)
{
	uint32_t handles[SIMPLE_GROUP_ALLOCS];
	uint64_t ahnd;
	uint32_t ctx;
	int i;

	ctx = rand() % 2;
	ahnd = intel_allocator_open(fd, ctx, INTEL_ALLOCATOR_SIMPLE);

	for (i = 0; i < SIMPLE_GROUP_ALLOCS; i++) {
		uint32_t size;

		size = (rand() % 4 + 1) * 0x1000;
		handles[i] = gem_create(fd, size);
		intel_allocator_alloc(ahnd, handles[i], size, 0x1000);
	}

	for (i = 0; i < SIMPLE_GROUP_ALLOCS; i++) {
		igt_assert_f(intel_allocator_free(ahnd, handles[i]) == 1,
			     "Error freeing handle: %u\n", handles[i]);
		gem_close(fd, handles[i]);
	}

	intel_allocator_close(ahnd);
}

static void fork_simple_once(int fd)
{
	intel_allocator_multiprocess_start();

	igt_fork(child, 1)
		__simple_allocs(fd);

	igt_waitchildren();

	intel_allocator_multiprocess_stop();
}

#define SIMPLE_TIMEOUT 5
static void *__fork_simple_thread(void *data)
{
	int fd = (int) (long) data;

	igt_until_timeout(SIMPLE_TIMEOUT) {
		__simple_allocs(fd);
	}

	return NULL;
}

static void fork_simple_stress(int fd, bool two_level_inception)
{
	pthread_t thread0, thread1;
	uint64_t ahnd0, ahnd1;
	bool are_empty;

	__intel_allocator_multiprocess_prepare();

	igt_fork(child, 8) {
		if (two_level_inception) {
			pthread_create(&thread0, NULL, __fork_simple_thread,
				       (void *) (long) fd);
			pthread_create(&thread1, NULL, __fork_simple_thread,
				       (void *) (long) fd);
		}

		igt_until_timeout(SIMPLE_TIMEOUT) {
			__simple_allocs(fd);
		}

		if (two_level_inception) {
			pthread_join(thread0, NULL);
			pthread_join(thread1, NULL);
		}
	}

	pthread_create(&thread0, NULL, __fork_simple_thread, (void *) (long) fd);
	pthread_create(&thread1, NULL, __fork_simple_thread, (void *) (long) fd);

	ahnd0 = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_SIMPLE);
	ahnd1 = intel_allocator_open(fd, 1, INTEL_ALLOCATOR_SIMPLE);

	__intel_allocator_multiprocess_start();

	igt_waitchildren();

	pthread_join(thread0, NULL);
	pthread_join(thread1, NULL);

	are_empty = intel_allocator_close(ahnd0);
	are_empty &= intel_allocator_close(ahnd1);

	intel_allocator_multiprocess_stop();

	igt_assert_f(are_empty, "Allocators were not emptied\n");
}

static void __reopen_allocs(int fd1, int fd2, bool check)
{
	uint64_t ahnd0, ahnd1, ahnd2;

	ahnd0 = intel_allocator_open(fd1, 0, INTEL_ALLOCATOR_SIMPLE);
	ahnd1 = intel_allocator_open(fd2, 0, INTEL_ALLOCATOR_SIMPLE);
	ahnd2 = intel_allocator_open(fd2, 0, INTEL_ALLOCATOR_SIMPLE);
	igt_assert(ahnd0 != ahnd1);
	igt_assert(ahnd1 != ahnd2);

	/* in fork mode we can have more references, so skip check */
	if (!check) {
		intel_allocator_close(ahnd0);
		intel_allocator_close(ahnd1);
		intel_allocator_close(ahnd2);
	} else {
		igt_assert_eq(intel_allocator_close(ahnd0), true);
		igt_assert_eq(intel_allocator_close(ahnd1), false);
		igt_assert_eq(intel_allocator_close(ahnd2), true);
	}
}

static void reopen(int fd)
{
	int fd2;

	igt_require_gem(fd);

	fd2 = gem_reopen_driver(fd);

	__reopen_allocs(fd, fd2, true);

	close(fd2);
}

#define REOPEN_TIMEOUT 3
static void reopen_fork(int fd)
{
	int fd2;

	igt_require_gem(fd);

	intel_allocator_multiprocess_start();

	fd2 = gem_reopen_driver(fd);

	igt_fork(child, 2) {
		igt_until_timeout(REOPEN_TIMEOUT)
			__reopen_allocs(fd, fd2, false);
	}
	igt_until_timeout(REOPEN_TIMEOUT)
		__reopen_allocs(fd, fd2, false);

	igt_waitchildren();

	/* Check references at the end */
	__reopen_allocs(fd, fd2, true);

	close(fd2);

	intel_allocator_multiprocess_stop();
}

static void open_vm(int fd)
{
	uint64_t ahnd[4], offset[4], size = 0x1000;
	int i, n = ARRAY_SIZE(ahnd);

	ahnd[0] = intel_allocator_open_vm(fd, 1, INTEL_ALLOCATOR_SIMPLE);
	ahnd[1] = intel_allocator_open_vm(fd, 1, INTEL_ALLOCATOR_SIMPLE);
	ahnd[2] = intel_allocator_open_vm_as(ahnd[1], 2);
	ahnd[3] = intel_allocator_open(fd, 3, INTEL_ALLOCATOR_SIMPLE);

	offset[0] = intel_allocator_alloc(ahnd[0], 1, size, 0);
	offset[1] = intel_allocator_alloc(ahnd[1], 2, size, 0);
	igt_assert(offset[0] != offset[1]);

	offset[2] = intel_allocator_alloc(ahnd[2], 3, size, 0);
	igt_assert(offset[0] != offset[2] && offset[1] != offset[2]);

	offset[3] = intel_allocator_alloc(ahnd[3], 1, size, 0);
	igt_assert(offset[0] == offset[3]);

	/*
	 * As ahnd[0-2] lead to same allocator check can we free all handles
	 * using selected ahnd.
	 */
	intel_allocator_free(ahnd[0], 1);
	intel_allocator_free(ahnd[0], 2);
	intel_allocator_free(ahnd[0], 3);
	intel_allocator_free(ahnd[3], 1);

	for (i = 0; i < n - 1; i++)
		igt_assert_eq(intel_allocator_close(ahnd[i]), (i == n - 2));
	igt_assert_eq(intel_allocator_close(ahnd[n-1]), true);
}

/* Simple execbuf which uses allocator, non-fork mode */
static void execbuf_with_allocator(int fd)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 object[3];
	uint64_t ahnd, sz = 4096, gtt_size;
	unsigned int flags = EXEC_OBJECT_PINNED;
	uint32_t *ptr, batch[32], copied;
	int gen = intel_gen(intel_get_drm_devid(fd));
	int i;
	const uint32_t magic = 0x900df00d;

	igt_require(gem_uses_full_ppgtt(fd));

	gtt_size = gem_aperture_size(fd);
	if ((gtt_size - 1) >> 32)
		flags |= EXEC_OBJECT_SUPPORTS_48B_ADDRESS;

	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_SIMPLE);

	memset(object, 0, sizeof(object));

	/* i == 0 (src), i == 1 (dst), i == 2 (batch) */
	for (i = 0; i < ARRAY_SIZE(object); i++) {
		uint64_t offset;

		object[i].handle = gem_create(fd, sz);
		offset = intel_allocator_alloc(ahnd, object[i].handle, sz, 0);
		object[i].offset = CANONICAL(offset);

		object[i].flags = flags;
		if (i == 1)
			object[i].flags |= EXEC_OBJECT_WRITE;
	}

	/* Prepare src data */
	ptr = gem_mmap__device_coherent(fd, object[0].handle, 0, sz, PROT_WRITE);
	ptr[0] = magic;
	gem_munmap(ptr, sz);

	/* Blit src -> dst */
	i = 0;
	if (gen >= 9) {
		batch[i++] = XY_FAST_COPY_BLT; /* No tiling */
		batch[i++] = XY_FAST_COPY_COLOR_DEPTH_32 | 0x10;
	} else {
		batch[i++] = XY_SRC_COPY_BLT_CMD |
			  XY_SRC_COPY_BLT_WRITE_ALPHA |
			  XY_SRC_COPY_BLT_WRITE_RGB;
		if (gen >= 8)
			batch[i - 1] |= 8;
		else
			batch[i - 1] |= 6;

		batch[i++] = (3 << 24) | (0xcc << 16) | 4;
	}
	batch[i++] = 0;
	batch[i++] = (1 << 16) | 4;
	batch[i++] = object[1].offset;
	if (gen >= 8)
		batch[i++] = object[1].offset >> 32;
	batch[i++] = 0;
	batch[i++] = 4;
	batch[i++] = object[0].offset;
	if (gen >= 8)
		batch[i++] = object[0].offset >> 32;
	batch[i++] = MI_BATCH_BUFFER_END;
	batch[i++] = MI_NOOP;

	gem_write(fd, object[2].handle, 0, batch, i * sizeof(batch[0]));

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = to_user_pointer(object);
	execbuf.buffer_count = 3;
	if (gen >= 6)
		execbuf.flags = I915_EXEC_BLT;
	gem_execbuf(fd, &execbuf);
	gem_sync(fd, object[1].handle);

	/* Check dst data */
	ptr = gem_mmap__device_coherent(fd, object[1].handle, 0, sz, PROT_READ);
	copied = ptr[0];
	gem_munmap(ptr, sz);

	for (i = 0; i < ARRAY_SIZE(object); i++) {
		igt_assert(intel_allocator_free(ahnd, object[i].handle));
		gem_close(fd, object[i].handle);
	}

	igt_assert(copied == magic);
	igt_assert(intel_allocator_close(ahnd) == true);
}

static void fork_reopen_allocator(int fd, uint8_t type)
{
	uint64_t p_ahnd, sh_ahnd, fd_ahnd, ctx_ahnd;
	uint64_t offset;

	intel_allocator_multiprocess_start();

	p_ahnd = intel_allocator_open(fd, 0, type);
	offset = intel_allocator_alloc(p_ahnd, 1, 123, 0);
	if (type == INTEL_ALLOCATOR_SIMPLE)
		igt_assert(intel_allocator_is_allocated(p_ahnd, 1, 123, offset));

	igt_fork(child, 1) {
		sh_ahnd = intel_allocator_open(fd, 0, type);
		if (type == INTEL_ALLOCATOR_SIMPLE)
			igt_assert(intel_allocator_is_allocated(sh_ahnd, 1, 123, offset));

		ctx_ahnd = intel_allocator_open(fd, 1, type);
		igt_assert(!intel_allocator_is_allocated(ctx_ahnd, 1, 123, offset));
		intel_allocator_alloc(ctx_ahnd, 2, 123, 0);

		fd = gem_reopen_driver(fd);
		fd_ahnd = intel_allocator_open(fd, 0, type);
		igt_assert(!intel_allocator_is_allocated(fd_ahnd, 1, 123, offset));
		intel_allocator_alloc(fd_ahnd, 2, 123, 0);

		intel_allocator_close(sh_ahnd);
		intel_allocator_close(ctx_ahnd);
		intel_allocator_close(fd_ahnd);
	}

	igt_waitchildren();
	intel_allocator_close(p_ahnd);

	intel_allocator_multiprocess_stop();
}

static uint32_t single_exec_from_pool(int i915, uint64_t ahnd, uint64_t size)
{
	struct drm_i915_gem_execbuffer2 execbuf = {};
	struct drm_i915_gem_exec_object2 obj = {};
	uint32_t bb = gem_create_from_pool(i915, &size, REGION_SMEM);
	uint32_t *bbptr;

	bbptr = gem_mmap__device_coherent(i915, bb, 0, size, PROT_WRITE);
	*bbptr = MI_BATCH_BUFFER_END;
	gem_munmap(bbptr, size);
	obj.offset = get_offset(ahnd, bb, size, 0);
	if (ahnd)
		obj.flags = EXEC_OBJECT_PINNED;
	obj.handle = bb;
	execbuf.buffer_count = 1;
	execbuf.buffers_ptr = to_user_pointer(&obj);
	gem_execbuf(i915, &execbuf);

	return bb;
}

static void gem_pool(int i915)
{
	uint32_t bb[4];
	uint64_t ahnd = get_reloc_ahnd(i915, 0);
	igt_spin_t *spin;

	bb[0] = single_exec_from_pool(i915, ahnd, 4096);
	gem_sync(i915, bb[0]);
	bb[1] = single_exec_from_pool(i915, ahnd, 4096);
	igt_assert(bb[0] == bb[1]);

	bb[2] = single_exec_from_pool(i915, ahnd, 8192);
	gem_sync(i915, bb[2]);
	bb[3] = single_exec_from_pool(i915, ahnd, 8192);
	igt_assert(bb[2] == bb[3]);
	igt_assert(bb[0] != bb[2]);

	spin = igt_spin_new(i915,
			    .ahnd = ahnd,
			    .engine = I915_EXEC_DEFAULT);
	bb[0] = single_exec_from_pool(i915, ahnd, 4096);
	bb[1] = single_exec_from_pool(i915, ahnd, 4096);
	bb[2] = single_exec_from_pool(i915, ahnd, 8192);
	bb[3] = single_exec_from_pool(i915, ahnd, 8192);
	igt_spin_free(i915, spin);
	igt_assert(bb[0] != bb[1]);
	igt_assert(bb[2] != bb[3]);

	put_ahnd(ahnd);

	gem_pool_dump();
}

struct allocators {
	const char *name;
	uint8_t type;
} als[] = {
	{"simple", INTEL_ALLOCATOR_SIMPLE},
	{"reloc",  INTEL_ALLOCATOR_RELOC},
	{NULL, 0},
};

igt_main
{
	int fd;
	struct allocators *a;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		atomic_init(&next_handle, 1);
		srandom(0xdeadbeef);
	}

	igt_subtest_f("alloc-simple")
		alloc_simple(fd);

	igt_subtest_f("reserve-simple")
		reserve_simple(fd);

	igt_describe("For simple allocator check does default alignment is "
		     "properly handled in open and alloc functions");
	igt_subtest_f("default-alignment")
		default_alignment(fd);

	for (a = als; a->name; a++) {
		igt_subtest_with_dynamic_f("%s-allocator", a->name) {
			igt_dynamic("basic")
				basic_alloc(fd, 1UL << 8, a->type);

			igt_dynamic("parallel-one")
				parallel_one(fd, a->type);

			igt_dynamic("print")
				basic_alloc(fd, 1UL << 2, a->type);

			igt_dynamic("reuse")
				reuse(fd, a->type);

			if (a->type == INTEL_ALLOCATOR_SIMPLE) {
				igt_dynamic("reserve")
					reserve(fd, a->type);
			}

			igt_dynamic("fork-reopen-allocator")
				fork_reopen_allocator(fd, a->type);
		}
	}

	igt_subtest_f("standalone")
		standalone(fd);

	igt_subtest_f("fork-simple-once")
		fork_simple_once(fd);

	igt_subtest_f("fork-simple-stress")
		fork_simple_stress(fd, false);

	igt_subtest_f("fork-simple-stress-signal") {
		igt_fork_signal_helper();
		fork_simple_stress(fd, false);
		igt_stop_signal_helper();
	}

	igt_subtest_f("two-level-inception")
		fork_simple_stress(fd, true);

	igt_subtest_f("two-level-inception-interruptible") {
		igt_fork_signal_helper();
		fork_simple_stress(fd, true);
		igt_stop_signal_helper();
	}

	igt_subtest_f("reopen")
		reopen(fd);

	igt_subtest_f("reopen-fork")
		reopen_fork(fd);

	igt_subtest_f("open-vm")
		open_vm(fd);

	igt_subtest_f("execbuf-with-allocator")
		execbuf_with_allocator(fd);

	igt_describe("Verifies creating and executing bb from gem pool");
	igt_subtest_f("gem-pool")
		gem_pool(fd);

	igt_fixture
		close(fd);
}
