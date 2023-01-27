/*
 * Copyright © 2019 Intel Corporation
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

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "drmtest.h"
#include "i915/gem.h"
#include "i915/gem_context.h"
#include "i915/gem_engine_topology.h"
#include "i915/gem_ring.h"
#include "i915/gem_submission.h"
#include "igt_aux.h"
#include "igt_debugfs.h"
#include "igt_dummyload.h"
#include "igt_gt.h"
#include "igt_sysfs.h"
#include "igt_params.h"
#include "ioctl_wrappers.h" /* gem_wait()! */
#include "intel_allocator.h"
#include "sw_sync.h"

#define RESET_TIMEOUT_MS 2 * MSEC_PER_SEC; /* default: 640ms */
static unsigned long reset_timeout_ms = RESET_TIMEOUT_MS;
#define NSEC_PER_MSEC (1000 * 1000ull)

static void cleanup(int i915)
{
	igt_drop_caches_set(i915,
			    /* cancel everything */
			    DROP_RESET_ACTIVE | DROP_RESET_SEQNO |
			    /* cleanup */
			    DROP_ACTIVE | DROP_RETIRE | DROP_IDLE | DROP_FREED);
}

static int wait_for_status(int fence, int timeout)
{
	int err;

	err = sync_fence_wait(fence, timeout);
	if (err)
		return err;

	return sync_fence_status(fence);
}

static bool has_persistence(int i915)
{
	struct drm_i915_gem_context_param p = {
		.param = I915_CONTEXT_PARAM_PERSISTENCE,
	};
	uint64_t saved;

	if (__gem_context_get_param(i915, &p))
		return false;

	saved = p.value;
	p.value = 0;
	if (__gem_context_set_param(i915, &p))
		return false;

	p.value = saved;
	return __gem_context_set_param(i915, &p) == 0;
}

static bool __enable_hangcheck(int dir, bool state)
{
	return igt_sysfs_set(dir, "enable_hangcheck", state ? "1" : "0");
}

static void enable_hangcheck(int i915)
{
	int dir;

	dir = igt_params_open(i915);
	if (dir < 0) /* no parameters, must be default! */
		return;

	/* If i915.hangcheck is removed, assume the default is good */
	__enable_hangcheck(dir, true);
	close(dir);
}

static void flush_delayed_fput(int i915)
{
	rcu_barrier(i915);
	usleep(50 * 1000);
	rcu_barrier(i915); /* flush the delayed fput */

	sched_yield();
	rcu_barrier(i915); /* again, in case it was added after we waited! */
}

static void test_idempotent(int i915)
{
	struct drm_i915_gem_context_param p = {
		.param = I915_CONTEXT_PARAM_PERSISTENCE,
	};
	int expected;

	/*
	 * Simple test to verify that we are able to read back the same boolean
	 * value as we set.
	 *
	 * Each time we invert the current value so that at the end of the test,
	 * if successful, we leave the context in the original state.
	 */

	gem_context_get_param(i915, &p);
	expected = !!p.value;

	expected = !expected;
	p.value = expected;
	gem_context_set_param(i915, &p);
	gem_context_get_param(i915, &p);
	igt_assert_eq(p.value, expected);

	expected = !expected; /* and restores */
	p.value = expected;
	gem_context_set_param(i915, &p);
	gem_context_get_param(i915, &p);
	igt_assert_eq(p.value, expected);
}

static const intel_ctx_t *
ctx_create_persistence(int i915, const intel_ctx_cfg_t *base_cfg, bool persist)
{
	intel_ctx_cfg_t cfg = *base_cfg;
	cfg.nopersist = !persist;
	return intel_ctx_create(i915, &cfg);
}

static void test_persistence(int i915, const intel_ctx_cfg_t *cfg,
			     unsigned int engine)
{
	igt_spin_t *spin;
	int64_t timeout;
	const intel_ctx_t *ctx;
	uint64_t ahnd;

	/*
	 * Default behaviour are contexts remain alive until their last active
	 * request is retired -- no early termination.
	 */

	ctx = ctx_create_persistence(i915, cfg, true);
	ahnd = get_reloc_ahnd(i915, ctx->id);

	spin = igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
			    .engine = engine,
			    .flags = IGT_SPIN_FENCE_OUT);
	intel_ctx_destroy(i915, ctx);

	timeout = reset_timeout_ms * NSEC_PER_MSEC;
	igt_assert_eq(gem_wait(i915, spin->handle, &timeout), -ETIME);

	igt_spin_end(spin);

	timeout = reset_timeout_ms * NSEC_PER_MSEC;
	igt_assert_eq(gem_wait(i915, spin->handle, &timeout), 0);
	igt_assert_eq(sync_fence_status(spin->out_fence), 1);

	igt_spin_free(i915, spin);
	put_ahnd(ahnd);
}

static void test_nonpersistent_cleanup(int i915, const intel_ctx_cfg_t *cfg,
				       unsigned int engine)
{
	int64_t timeout = reset_timeout_ms * NSEC_PER_MSEC;
	igt_spin_t *spin;
	const intel_ctx_t *ctx;
	uint64_t ahnd;

	/*
	 * A nonpersistent context is terminated immediately upon closure,
	 * any inflight request is cancelled.
	 */

	ctx = ctx_create_persistence(i915, cfg, false);
	ahnd = get_reloc_ahnd(i915, ctx->id);

	spin = igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
			    .engine = engine,
			    .flags = IGT_SPIN_FENCE_OUT);
	intel_ctx_destroy(i915, ctx);

	igt_assert_eq(gem_wait(i915, spin->handle, &timeout), 0);
	igt_assert_eq(sync_fence_status(spin->out_fence), -EIO);

	igt_spin_free(i915, spin);
	put_ahnd(ahnd);
}

static void test_nonpersistent_mixed(int i915, const intel_ctx_cfg_t *cfg,
				     unsigned int engine)
{
	int fence[3];

	/*
	 * Only a nonpersistent context is terminated immediately upon
	 * closure, any inflight request is cancelled. If there is also
	 * an active persistent context closed, it should be unafffected.
	 */

	for (int i = 0; i < ARRAY_SIZE(fence); i++) {
		igt_spin_t *spin;
		const intel_ctx_t *ctx;
		uint64_t ahnd;

		ctx = ctx_create_persistence(i915, cfg, i & 1);
		ahnd = get_reloc_ahnd(i915, ctx->id);

		spin = igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
				    .engine = engine,
				    .flags = IGT_SPIN_FENCE_OUT);
		intel_ctx_destroy(i915, ctx);

		fence[i] = spin->out_fence;
		put_ahnd(ahnd);
	}

	/* Outer pair of contexts were non-persistent and killed */
	igt_assert_eq(wait_for_status(fence[0], reset_timeout_ms), -EIO);
	igt_assert_eq(wait_for_status(fence[2], reset_timeout_ms), -EIO);

	/* But the middle context is still running */
	igt_assert_eq(sync_fence_wait(fence[1], 0), -ETIME);
}

static void test_nonpersistent_hostile(int i915, const intel_ctx_cfg_t *cfg,
				       unsigned int engine)
{
	int64_t timeout = reset_timeout_ms * NSEC_PER_MSEC;
	igt_spin_t *spin;
	const intel_ctx_t *ctx;
	uint64_t ahnd;

	/*
	 * If we cannot cleanly cancel the non-persistent context on closure,
	 * e.g. preemption fails, we are forced to reset the GPU to terminate
	 * the requests and cleanup the context.
	 */

	ctx = ctx_create_persistence(i915, cfg, false);
	ahnd = get_reloc_ahnd(i915, ctx->id);

	spin = igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
			    .engine = engine,
			    .flags = IGT_SPIN_NO_PREEMPTION);
	intel_ctx_destroy(i915, ctx);

	igt_assert_eq(gem_wait(i915, spin->handle, &timeout), 0);

	igt_spin_free(i915, spin);
	put_ahnd(ahnd);
}

static void test_nonpersistent_hostile_preempt(int i915, const intel_ctx_cfg_t *cfg,
					       unsigned int engine)
{
	int64_t timeout = reset_timeout_ms * NSEC_PER_MSEC;
	igt_spin_t *spin[2];
	const intel_ctx_t *ctx;
	uint64_t ahnd;

	/*
	 * Double plus ungood.
	 *
	 * Here we would not be able to cancel the hostile non-persistent
	 * context and we cannot preempt-to-idle as it is already waiting
	 * on preemption for itself. Let's hope the kernel can save the
	 * day with a reset.
	 */

	igt_require(gem_scheduler_has_preemption(i915));

	ctx = ctx_create_persistence(i915, cfg, true);
	gem_context_set_priority(i915, ctx->id, 0);
	ahnd = get_reloc_ahnd(i915, ctx->id);
	spin[0] = igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
			       .engine = engine,
			       .flags = (IGT_SPIN_NO_PREEMPTION |
					 IGT_SPIN_POLL_RUN));
	intel_ctx_destroy(i915, ctx);

	igt_spin_busywait_until_started(spin[0]);

	ctx = ctx_create_persistence(i915, cfg, false);
	gem_context_set_priority(i915, ctx->id, 1); /* higher priority than 0 */
	spin[1] = igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
			       .engine = engine,
			       .flags = IGT_SPIN_NO_PREEMPTION);
	intel_ctx_destroy(i915, ctx);

	igt_assert_eq(gem_wait(i915, spin[1]->handle, &timeout), 0);

	igt_spin_free(i915, spin[1]);
	igt_spin_free(i915, spin[0]);
	put_ahnd(ahnd);
}

static void test_nonpersistent_hang(int i915, const intel_ctx_cfg_t *cfg,
				    unsigned int engine)
{
	int64_t timeout = reset_timeout_ms * NSEC_PER_MSEC;
	igt_spin_t *spin;
	const intel_ctx_t *ctx;
	uint64_t ahnd;
	/*
	 * The user made a simple mistake and submitted an invalid batch,
	 * but fortunately under a nonpersistent context. Do we detect it?
	 */

	ctx = ctx_create_persistence(i915, cfg, false);
	ahnd = get_reloc_ahnd(i915, ctx->id);

	spin = igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
			    .engine = engine,
			    .flags = IGT_SPIN_INVALID_CS);
	intel_ctx_destroy(i915, ctx);

	igt_assert_eq(gem_wait(i915, spin->handle, &timeout), 0);

	igt_spin_free(i915, spin);
	put_ahnd(ahnd);
}

static void test_nohangcheck_hostile(int i915, const intel_ctx_cfg_t *cfg)
{
	const struct intel_execution_engine2 *e;
	int dir;

	cleanup(i915);

	/*
	 * Even if the user disables hangcheck during their context,
	 * we forcibly terminate that context.
	 */

	dir = igt_params_open(i915);
	igt_require(dir != -1);

	igt_require(__enable_hangcheck(dir, false));

	for_each_ctx_cfg_engine(i915, cfg, e) {
		int64_t timeout = 10000 * NSEC_PER_MSEC;
		const intel_ctx_t *ctx = intel_ctx_create(i915, cfg);
		uint64_t ahnd = get_reloc_ahnd(i915, ctx->id);
		igt_spin_t *spin;

		spin = igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
				    .engine = e->flags,
				    .flags = IGT_SPIN_NO_PREEMPTION);
		intel_ctx_destroy(i915, ctx);

		igt_assert_eq(gem_wait(i915, spin->handle, &timeout), 0);

		igt_spin_free(i915, spin);
		put_ahnd(ahnd);
	}

	igt_require(__enable_hangcheck(dir, true));
	close(dir);
}

static void test_nohangcheck_hang(int i915, const intel_ctx_cfg_t *cfg)
{
	const struct intel_execution_engine2 *e;
	int testable_engines = 0;
	int dir;

	cleanup(i915);

	/*
	 * Even if the user disables hangcheck during their context,
	 * we forcibly terminate that context.
	 */

	for_each_ctx_cfg_engine(i915, cfg, e) {
		if (!gem_engine_has_cmdparser(i915, cfg, e->flags))
			testable_engines++;
	}
	igt_require(testable_engines);

	dir = igt_params_open(i915);
	igt_require(dir != -1);

	igt_require(__enable_hangcheck(dir, false));

	for_each_ctx_cfg_engine(i915, cfg, e) {
		int64_t timeout = reset_timeout_ms * NSEC_PER_MSEC;
		const intel_ctx_t *ctx;
		igt_spin_t *spin;
		uint64_t ahnd;

		if (!gem_engine_has_cmdparser(i915, cfg, e->flags))
			continue;

		ctx = intel_ctx_create(i915, cfg);
		ahnd = get_reloc_ahnd(i915, ctx->id);
		spin = igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
				    .engine = e->flags,
				    .flags = IGT_SPIN_INVALID_CS);
		intel_ctx_destroy(i915, ctx);

		igt_assert_eq(gem_wait(i915, spin->handle, &timeout), 0);

		igt_spin_free(i915, spin);
		put_ahnd(ahnd);
	}

	igt_require(__enable_hangcheck(dir, true));
	close(dir);
}

static bool set_heartbeat(int i915, const char *name, unsigned int value)
{
	unsigned int x;

	if (gem_engine_property_printf(i915, name,
				       "heartbeat_interval_ms",
				       "%d", value) < 0)
		return false;

	x = ~value;
	gem_engine_property_scanf(i915, name,
				  "heartbeat_interval_ms",
				  "%d", &x);
	igt_assert_eq(x, value);

	return true;
}

static bool set_preempt_timeout(int i915, const char *name, unsigned int value)
{
	unsigned int x;

	if (gem_engine_property_printf(i915, name,
				       "preempt_timeout_ms",
				       "%d", value) < 0)
		return false;

	x = ~value;
	gem_engine_property_scanf(i915, name,
				  "preempt_timeout_ms",
				  "%d", &x);
	igt_assert_eq(x, value);

	return true;
}

static void test_noheartbeat_many(int i915, int count, unsigned int flags)
{
	unsigned long checked = 0;

	cleanup(i915);
	enable_hangcheck(i915);

	/*
	 * If the user disables the heartbeat, after leaving behind
	 * a number of long running *persistent* contexts, check they get
	 * cleaned up.
	 */

	for_each_physical_ring(e, i915) {
		igt_spin_t *spin[count];
		uint64_t ahnd;

		if (!set_preempt_timeout(i915, e->full_name, 250))
			continue;

		if (!set_heartbeat(i915, e->full_name, 0))
			continue;

		igt_assert(set_heartbeat(i915, e->full_name, 500));

		for (int n = 0; n < ARRAY_SIZE(spin); n++) {
			const intel_ctx_t *ctx;

			ctx = intel_ctx_create(i915, NULL);
			ahnd = get_reloc_ahnd(i915, ctx->id);
			spin[n] = igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
					       .engine = eb_ring(e),
					       .flags = (IGT_SPIN_FENCE_OUT |
							 IGT_SPIN_POLL_RUN |
							 flags));
			intel_ctx_destroy(i915, ctx);
		}
		igt_spin_busywait_until_started(spin[0]);

		igt_debug("Testing %s\n", e->full_name);
		igt_assert(set_heartbeat(i915, e->full_name, 0));

		for (int n = 0; n < ARRAY_SIZE(spin); n++) {
			igt_assert_eq(wait_for_status(spin[n]->out_fence, reset_timeout_ms),
				      -EIO);
		}

		for (int n = 0; n < ARRAY_SIZE(spin); n++) {
			ahnd = spin[n]->opts.ahnd;
			igt_spin_free(i915, spin[n]);
			put_ahnd(ahnd);
		}

		set_heartbeat(i915, e->full_name, 2500);
		cleanup(i915);
		checked++;
	}
	igt_require(checked);
}

static void test_noheartbeat_close(int i915, unsigned int flags)
{
	unsigned long checked = 0;

	cleanup(i915);
	enable_hangcheck(i915);

	/*
	 * Check that non-persistent contexts are also cleaned up if we
	 * close the context while they are active, but the engine's
	 * heartbeat has already been disabled.
	 */

	for_each_physical_ring(e, i915) {
		igt_spin_t *spin;
		const intel_ctx_t *ctx;
		uint64_t ahnd;
		int err;

		if (!set_preempt_timeout(i915, e->full_name, 250))
			continue;

		if (!set_heartbeat(i915, e->full_name, 0))
			continue;

		ctx = intel_ctx_create(i915, NULL);
		ahnd = get_reloc_ahnd(i915, ctx->id);
		spin = igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
				    .engine = eb_ring(e),
				    .flags = (IGT_SPIN_FENCE_OUT |
					      IGT_SPIN_POLL_RUN |
					      flags));
		igt_spin_busywait_until_started(spin);

		igt_debug("Testing %s\n", e->full_name);
		intel_ctx_destroy(i915, ctx);
		err = wait_for_status(spin->out_fence, reset_timeout_ms);

		set_heartbeat(i915, e->full_name, 2500);
		igt_spin_free(i915, spin);
		put_ahnd(ahnd);

		igt_assert_eq(err, -EIO);
		cleanup(i915);
		checked++;
	}
	igt_require(checked);
}

static void test_nonpersistent_file(int i915)
{
	int debugfs = i915;
	igt_spin_t *spin;
	uint64_t ahnd;

	cleanup(i915);

	/*
	 * A context may live beyond its initial struct file, except if it
	 * has been made nonpersistent, in which case it must be terminated.
	 */

	i915 = gem_reopen_driver(i915);

	ahnd = get_reloc_ahnd(i915, 0);
	gem_context_set_persistence(i915, 0, false);
	spin = igt_spin_new(i915, .ahnd = ahnd, .flags = IGT_SPIN_FENCE_OUT);

	close(i915);
	flush_delayed_fput(debugfs);

	igt_assert_eq(wait_for_status(spin->out_fence, reset_timeout_ms), -EIO);

	spin->handle = 0;
	igt_spin_free(-1, spin);
	put_ahnd(ahnd);
}

static int __execbuf_wr(int i915, struct drm_i915_gem_execbuffer2 *execbuf)
{
	int err;

	err = 0;
	if (ioctl(i915, DRM_IOCTL_I915_GEM_EXECBUFFER2_WR, execbuf)) {
		err = -errno;
		igt_assume(err);
	}

	errno = 0;
	return err;
}

static void alarm_handler(int sig)
{
}

static void test_nonpersistent_queued(int i915, const intel_ctx_cfg_t *cfg,
				      unsigned int engine)
{
	struct sigaction old_sa, sa = { .sa_handler = alarm_handler };
	struct itimerval itv;
	igt_spin_t *spin;
	int fence = -1;
	const intel_ctx_t *ctx;
	uint64_t ahnd;

	/*
	 * Not only must the immediate batch be cancelled, but
	 * all pending batches in the context.
	 */

	ctx = ctx_create_persistence(i915, cfg, false);
	ahnd = get_reloc_ahnd(i915, ctx->id);
	spin = igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
			    .engine = engine,
			    .flags = IGT_SPIN_FENCE_OUT);

	sigaction(SIGALRM, &sa, &old_sa);
	memset(&itv, 0, sizeof(itv));
	itv.it_value.tv_sec = 1;
	itv.it_value.tv_usec = 0;
	setitimer(ITIMER_REAL, &itv, NULL);

	fcntl(i915, F_SETFL, fcntl(i915, F_GETFL) | O_NONBLOCK);
	while (1) {
		igt_assert(spin->execbuf.flags & I915_EXEC_FENCE_OUT);
		if (__execbuf_wr(i915, &spin->execbuf))
			break;

		if (fence != -1)
			close(fence);

		igt_assert(spin->execbuf.rsvd2);
		fence = spin->execbuf.rsvd2 >> 32;
	}
	fcntl(i915, F_SETFL, fcntl(i915, F_GETFL) & ~O_NONBLOCK);

	memset(&itv, 0, sizeof(itv));
	setitimer(ITIMER_REAL, &itv, NULL);
	sigaction(SIGALRM, &old_sa, NULL);

	intel_ctx_destroy(i915, ctx);

	igt_assert_eq(wait_for_status(spin->out_fence, reset_timeout_ms), -EIO);
	igt_assert_eq(wait_for_status(fence, reset_timeout_ms), -EIO);

	igt_spin_free(i915, spin);
	put_ahnd(ahnd);
}

static void sendfd(int socket, int fd)
{
	char buf[CMSG_SPACE(sizeof(fd))];
	struct iovec io = { .iov_base = (char *)"ABC", .iov_len = 3 };
	struct msghdr msg = {
		.msg_iov = &io,
		.msg_iovlen = 1,
		.msg_control = buf,
		.msg_controllen = CMSG_LEN(sizeof(fd)),
	};
	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = msg.msg_controllen;
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

	igt_assert(sendmsg(socket, &msg, 0) != -1);
}

static int recvfd(int socket)
{
	char m_buffer[256], c_buffer[256];
	struct iovec io = {
		.iov_base = m_buffer,
		.iov_len = sizeof(m_buffer),
	};
	struct msghdr msg = {
		.msg_iov = &io,
		.msg_iovlen = 1,
		.msg_control = c_buffer,
		.msg_controllen = sizeof(c_buffer),
	};

	igt_assert(recvmsg(socket, &msg, 0) != -1);
	return *(int *)CMSG_DATA(CMSG_FIRSTHDR(&msg));
}

static void test_process(int i915)
{
	int fence, sv[2];

	cleanup(i915);

	/*
	 * If a process dies early, any nonpersistent contexts it had
	 * open must be terminated too.
	 */

	igt_require(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);

	igt_fork(child, 1) {
		igt_spin_t *spin;
		uint64_t ahnd;

		intel_allocator_init();
		i915 = gem_reopen_driver(i915);
		gem_quiescent_gpu(i915);

		gem_context_set_persistence(i915, 0, false);
		ahnd = get_reloc_ahnd(i915, 0);
		spin = igt_spin_new(i915, .ahnd = ahnd,
				    .flags = IGT_SPIN_FENCE_OUT);
		sendfd(sv[0], spin->out_fence);

		igt_list_del(&spin->link); /* prevent autocleanup */
	}
	close(sv[0]);
	igt_waitchildren();
	flush_delayed_fput(i915);

	fence = recvfd(sv[1]);
	close(sv[1]);

	igt_assert_eq(wait_for_status(fence, reset_timeout_ms), -EIO);
	close(fence);

	/* We have to manually clean up the orphaned spinner */
	igt_drop_caches_set(i915, DROP_RESET_ACTIVE);

	gem_quiescent_gpu(i915);
}

static void test_userptr(int i915)
{
	int fence, sv[2];

	cleanup(i915);

	/*
	 * When a process dies early, do the userptr or the contexts get cleaned
	 * up first? Since we only cancel the outstanding work along with the
	 * context, but wait on userptr cleanup for oustanding work, if
	 * the userptr is before the context, we end up in a scenario where
	 * we wait forever for the non-peristent context.
	 */

	igt_require(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);

	igt_fork(child, 1) {
		igt_spin_t *spin;
		uint64_t ahnd;

		intel_allocator_init();
		i915 = gem_reopen_driver(i915);
		gem_quiescent_gpu(i915);

		gem_context_set_persistence(i915, 0, false);
		ahnd = get_reloc_ahnd(i915, 0);
		spin = igt_spin_new(i915, .ahnd = ahnd,
				    .flags = IGT_SPIN_FENCE_OUT | IGT_SPIN_USERPTR);
		sendfd(sv[0], spin->out_fence);

		igt_list_del(&spin->link); /* prevent autocleanup */
	}
	close(sv[0]);
	igt_waitchildren();
	flush_delayed_fput(i915);

	fence = recvfd(sv[1]);
	close(sv[1]);

	igt_assert_eq(wait_for_status(fence, reset_timeout_ms), -EIO);
	close(fence);

	/* We have to manually clean up the orphaned spinner */
	igt_drop_caches_set(i915, DROP_RESET_ACTIVE);

	gem_quiescent_gpu(i915);
}

static void test_process_mixed(int pfd, const intel_ctx_cfg_t *cfg,
			       unsigned int engine)
{
	int fence[2], sv[2];

	/*
	 * If a process dies early, any nonpersistent contexts it had
	 * open must be terminated too. But any persistent contexts,
	 * should survive until their requests are complete.
	 */

	igt_require(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);

	igt_fork(child, 1) {
		int i915;

		i915 = gem_reopen_driver(pfd);
		gem_quiescent_gpu(i915);

		for (int persists = 0; persists <= 1; persists++) {
			igt_spin_t *spin;
			const intel_ctx_t *ctx;
			uint64_t ahnd;

			intel_allocator_init();
			ctx = ctx_create_persistence(i915, cfg, persists);
			ahnd = get_reloc_ahnd(i915, ctx->id);
			spin = igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
					    .engine = engine,
					    .flags = IGT_SPIN_FENCE_OUT);

			sendfd(sv[0], spin->out_fence);

			igt_list_del(&spin->link); /* prevent autocleanup */
		}
	}
	close(sv[0]);
	igt_waitchildren();
	flush_delayed_fput(pfd);

	fence[0] = recvfd(sv[1]);
	fence[1] = recvfd(sv[1]);
	close(sv[1]);

	/* First fence is non-persistent, so should be reset */
	igt_assert_eq(wait_for_status(fence[0], reset_timeout_ms), -EIO);
	close(fence[0]);

	/* Second fence is persistent, so should be still spinning */
	igt_assert_eq(sync_fence_wait(fence[1], 0), -ETIME);
	close(fence[1]);

	/* We have to manually clean up the orphaned spinner */
	igt_drop_caches_set(pfd, DROP_RESET_ACTIVE);

	gem_quiescent_gpu(pfd);
}

#define SATURATED_NOPREMPT	(1 << 0)

static void
test_saturated_hostile_all(int i915, const intel_ctx_t *base_ctx,
			   unsigned int engine_flags, unsigned int test_flags)
{
	const struct intel_execution_engine2 *other;
	unsigned int other_flags = 0;
	igt_spin_t *spin;
	const intel_ctx_t *ctx;
	uint64_t ahnd = get_reloc_ahnd(i915, base_ctx->id);
	int fence = -1;

	cleanup(i915);

	if (test_flags & SATURATED_NOPREMPT) {
		/*
		 * Render and compute engines have a reset dependency. If one is
		 * reset then all must be reset. Thus, if a hanging batch causes
		 * a reset, any non-preemptible batches on the other engines
		 * will be killed. So don't bother testing for the survival of
		 * non-preemptible batches when compute engines are present.
		 */
		for_each_ctx_engine(i915, base_ctx, other)
			igt_require(other->class != I915_ENGINE_CLASS_COMPUTE);

		other_flags |= IGT_SPIN_NO_PREEMPTION;
	}

	/*
	 * Check that if we have to remove a hostile request from a
	 * non-persistent context, we do so without harming any other
	 * concurrent users.
	 *
	 * We only allow non-persistent contexts if we can perform a
	 * per-engine reset, that is removal of the hostile context without
	 * impacting other users on the system. [Consider the problem of
	 * allowing the user to create a context with which they can arbitrarily
	 * reset other users whenever they chose.]
	 */

	for_each_ctx_engine(i915, base_ctx, other) {
		if (other->flags == engine_flags)
			continue;

		spin = igt_spin_new(i915, .ahnd = ahnd, .ctx = base_ctx,
				   .engine = other->flags,
				   .flags = other_flags | IGT_SPIN_FENCE_OUT);

		if (fence < 0) {
			fence = spin->out_fence;
		} else {
			int tmp;

			tmp = sync_fence_merge(fence, spin->out_fence);
			close(fence);
			close(spin->out_fence);

			fence = tmp;
		}
		spin->out_fence = -1;
	}
	put_ahnd(ahnd);
	igt_require(fence != -1);

	ctx = ctx_create_persistence(i915, &base_ctx->cfg, false);
	ahnd = get_reloc_ahnd(i915, ctx->id);
	spin = igt_spin_new(i915, .ahnd = ahnd, .ctx = ctx,
			    .engine = engine_flags,
			    .flags = (IGT_SPIN_NO_PREEMPTION |
				      IGT_SPIN_POLL_RUN |
				      IGT_SPIN_FENCE_OUT));
	igt_spin_busywait_until_started(spin);
	intel_ctx_destroy(i915, ctx);

	/* Hostile request requires a GPU reset to terminate */
	igt_assert_eq(wait_for_status(spin->out_fence, reset_timeout_ms), -EIO);

	/* All other spinners should be left unharmed */
	gem_quiescent_gpu(i915);
	igt_assert_eq(wait_for_status(fence, reset_timeout_ms), 1);
	close(fence);
	put_ahnd(ahnd);
}

static void
test_saturated_hostile_nopreempt(int i915, const intel_ctx_cfg_t *cfg,
				 unsigned int engine_flags)
{
	const intel_ctx_t *ctx = intel_ctx_create(i915, cfg);
	test_saturated_hostile_all(i915, ctx, engine_flags, SATURATED_NOPREMPT);
	intel_ctx_destroy(i915, ctx);
}

static void
test_saturated_hostile(int i915, const intel_ctx_cfg_t *cfg,
		       unsigned int engine_flags)
{
	const intel_ctx_t *ctx = intel_ctx_create(i915, cfg);
	test_saturated_hostile_all(i915, ctx, engine_flags, 0);
	intel_ctx_destroy(i915, ctx);
}

static void test_processes(int i915)
{
	struct {
		int sv[2];
	} p[2];

	cleanup(i915);

	/*
	 * If one process dies early, its nonpersistent context are cleaned up,
	 * but that should not affect a second process.
	 */

	for (int i = 0; i < ARRAY_SIZE(p); i++) {
		igt_require(socketpair(AF_UNIX, SOCK_DGRAM, 0, p[i].sv) == 0);

		igt_fork(child, 1) {
			igt_spin_t *spin;
			int pid;
			uint64_t ahnd;

			intel_allocator_init();
			i915 = gem_reopen_driver(i915);
			gem_context_set_persistence(i915, 0, i);

			ahnd = get_reloc_ahnd(i915, 0);
			spin = igt_spin_new(i915, .ahnd = ahnd,
					    .flags = IGT_SPIN_FENCE_OUT);
			/* prevent autocleanup */
			igt_list_del(&spin->link);

			sendfd(p[i].sv[0], spin->out_fence);

			/* Wait until we are told to die */
			pid = getpid();
			write(p[i].sv[0], &pid, sizeof(pid));

			pid = 0;
			read(p[i].sv[0], &pid, sizeof(pid));
			igt_assert(pid == getpid());
		}
	}

	for (int i = 0; i < ARRAY_SIZE(p); i++) {
		int fence, pid;

		/* The process is not dead yet, so the context can spin. */
		fence = recvfd(p[i].sv[1]);
		igt_assert_eq(sync_fence_wait(fence, 0), -ETIME);

		/* Kill *this* process */
		read(p[i].sv[1], &pid, sizeof(pid));
		write(p[i].sv[1], &pid, sizeof(pid));

		/*
		 * A little bit of slack required for the signal to terminate
		 * the process and for the system to cleanup the fd.
		 */
		sched_yield();
		close(p[i].sv[0]);
		close(p[i].sv[1]);
		flush_delayed_fput(i915);

		if (i == 0) {
			/* First fence is non-persistent, so should be reset */
			igt_assert_eq(wait_for_status(fence, reset_timeout_ms),
				      -EIO);
		} else {
			/* Second fence is persistent, so still spinning */
			igt_assert_eq(sync_fence_wait(fence, 0), -ETIME);
		}
		close(fence);
	}
	igt_waitchildren();

	/* We have to manually clean up the orphaned spinner */
	igt_drop_caches_set(i915, DROP_RESET_ACTIVE);
	gem_quiescent_gpu(i915);
}

static void __smoker(int i915, const intel_ctx_cfg_t *cfg,
		     unsigned int engine,
		     unsigned int timeout,
		     int expected)
{
	igt_spin_t *spin;
	int fence = -1;
	int fd, extra;
	const intel_ctx_t *ctx;
	uint64_t ahnd;

	fd = gem_reopen_driver(i915);
	ctx = ctx_create_persistence(fd, cfg, expected > 0);
	ahnd = get_reloc_ahnd(fd, ctx->id);
	spin = igt_spin_new(fd, .ahnd = ahnd, .ctx = ctx, .engine = engine,
			    .flags = IGT_SPIN_FENCE_OUT);

	extra = rand() % 8;
	while (extra--) {
		if (fence != -1)
			close(fence);
		spin->execbuf.rsvd2 = 0;
		gem_execbuf_wr(fd, &spin->execbuf);
		igt_assert(spin->execbuf.rsvd2);
		fence = spin->execbuf.rsvd2 >> 32;
	}

	intel_ctx_destroy(fd, ctx);

	close(fd);
	flush_delayed_fput(i915);

	igt_spin_end(spin);

	igt_assert_eq(wait_for_status(spin->out_fence, timeout), expected);

	if (fence != -1) {
		igt_assert_eq(wait_for_status(fence, timeout), expected);
		close(fence);
	}

	spin->handle = 0;
	igt_spin_free(fd, spin);
	put_ahnd(ahnd);
}

static void smoker(int i915, const intel_ctx_cfg_t *cfg,
		   unsigned int engine,
		   unsigned int timeout,
		   unsigned int *ctl)
{
	while (!READ_ONCE(*ctl)) {
		__smoker(i915, cfg, engine, timeout, -EIO);
		__smoker(i915, cfg, engine, timeout, 1);
	}
}

static void smoketest(int i915, const intel_ctx_cfg_t *cfg)
{
	const int SMOKE_LOAD_FACTOR = 4;
	const struct intel_execution_engine2 *e;
	uint32_t *ctl;

	cleanup(i915);

	/*
	 * All of the above! A mixture of naive and hostile processes and
	 * contexts, all trying to trick the kernel into mass slaughter.
	 */

	ctl = mmap(0, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(ctl != MAP_FAILED);

	for (int i = 1; i <= SMOKE_LOAD_FACTOR; i <<= 1) {
		*ctl = 0;

		igt_debug("Applying load factor: %d\n", i);
		for_each_ctx_cfg_engine(i915, cfg, e) {
			igt_fork(child, i)
				smoker(i915, cfg,
				       e->flags,
				       i * reset_timeout_ms,
				       ctl);
		}

		sleep(10);
		*ctl = 1;
		igt_waitchildren();
	}

	munmap(ctl, 4096);
	gem_quiescent_gpu(i915);
}

static void many_contexts(int i915, const intel_ctx_cfg_t *cfg)
{
	const struct intel_execution_engine2 *e;
	int64_t timeout = NSEC_PER_SEC;
	igt_spin_t *spin;
	uint64_t ahnd = get_reloc_ahnd(i915, 0);

	cleanup(i915);

	/*
	 * Perform many peristent kills from the same client. These should not
	 * cause the client to be banned, which in turn prevents us from
	 * creating new contexts, and submitting new execbuf.
	 */

	spin = igt_spin_new(i915, .ahnd = ahnd, .flags = IGT_SPIN_NO_PREEMPTION);
	igt_spin_end(spin);

	gem_sync(i915, spin->handle);
	igt_spin_reset(spin);

	for_each_ctx_cfg_engine(i915, cfg, e) {
		int t = 0;

		gem_engine_property_scanf(i915, e->name,
					  "preempt_timeout_ms", "%d", &t);
		timeout = max_t(int64_t, timeout, 2000000ll * t);
	}

	igt_until_timeout(30) {
		for_each_ctx_cfg_engine(i915, cfg, e) {
			const intel_ctx_t *ctx;

			ctx = ctx_create_persistence(i915, cfg, false);

			spin->execbuf.rsvd1 = ctx->id;
			spin->execbuf.flags &= ~63;
			spin->execbuf.flags |= e->flags;
			gem_execbuf(i915, &spin->execbuf);
			intel_ctx_destroy(i915, ctx);
		}
	}
	igt_debugfs_dump(i915, "i915_engine_info");
	igt_assert_eq(gem_wait(i915, spin->handle, &timeout), 0);

	/* And check we can still submit to the default context -- no bans! */
	igt_spin_reset(spin);
	spin->execbuf.rsvd1 = 0;
	spin->execbuf.flags &= ~63;
	gem_execbuf(i915, &spin->execbuf);

	igt_spin_free(i915, spin);
	gem_quiescent_gpu(i915);
	put_ahnd(ahnd);
}

static void do_test(void (*test)(int i915, const intel_ctx_cfg_t *cfg,
				 unsigned int engine),
		    int i915, const intel_ctx_cfg_t *cfg,
		    unsigned int engine, const char *name)
{
#define ATTR "preempt_timeout_ms"
	int timeout = -1;

	cleanup(i915);

	gem_engine_property_scanf(i915, name, ATTR, "%d", &timeout);
	if (timeout != -1) {
		igt_require(gem_engine_property_printf(i915, name,
						       ATTR, "%d", 50) > 0);
		reset_timeout_ms = 700;
	}

	test(i915, cfg, engine);

	if (timeout != -1) {
		gem_engine_property_printf(i915, name, ATTR, "%d", timeout);
		reset_timeout_ms = RESET_TIMEOUT_MS;
	}

	gem_quiescent_gpu(i915);
}

int i915;

static void exit_handler(int sig)
{
	enable_hangcheck(i915);
}

igt_main
{
	const intel_ctx_cfg_t empty_cfg = {};
	struct {
		const char *name;
		void (*func)(int fd, const intel_ctx_cfg_t *cfg,
			     unsigned int engine);
	} *test, tests[] = {
		{ "persistence", test_persistence },
		{ "cleanup", test_nonpersistent_cleanup },
		{ "queued", test_nonpersistent_queued },
		{ "mixed", test_nonpersistent_mixed },
		{ "mixed-process", test_process_mixed },
		{ "hostile", test_nonpersistent_hostile },
		{ "hostile-preempt", test_nonpersistent_hostile_preempt },
		{ "hang", test_nonpersistent_hang },
		{ NULL, NULL },
	};
	const intel_ctx_t *ctx;

	igt_fixture {
		i915 = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(i915);

		/* Restore the reset modparam if left clobbered */
		igt_assert(igt_params_set(i915, "reset", "%u", -1));

		enable_hangcheck(i915);
		igt_install_exit_handler(exit_handler);

		ctx = intel_ctx_create_all_physical(i915);

		igt_require(has_persistence(i915));
		igt_allow_hang(i915, ctx->id, 0);
	}

	/* Legacy execbuf engine selection flags. */

	igt_subtest("idempotent")
		test_idempotent(i915);

	igt_subtest("file")
		test_nonpersistent_file(i915);

	igt_subtest("process")
		test_process(i915);

	igt_subtest("processes")
		test_processes(i915);

	igt_subtest("userptr")
		test_userptr(i915);

	igt_subtest("hostile")
		test_nohangcheck_hostile(i915, &empty_cfg);
	igt_subtest("hang")
		test_nohangcheck_hang(i915, &empty_cfg);

	igt_subtest("heartbeat-stop")
		test_noheartbeat_many(i915, 1, 0);
	igt_subtest("heartbeat-hang")
		test_noheartbeat_many(i915, 1, IGT_SPIN_NO_PREEMPTION);
	igt_subtest("heartbeat-many")
		test_noheartbeat_many(i915, 16, 0);
	igt_subtest("heartbeat-close")
		test_noheartbeat_close(i915, 0);
	igt_subtest("heartbeat-hostile")
		test_noheartbeat_close(i915, IGT_SPIN_NO_PREEMPTION);

	igt_subtest_group {
		igt_fixture
			gem_require_contexts(i915);

		for (test = tests; test->name; test++) {
			igt_subtest_with_dynamic_f("legacy-engines-%s",
						   test->name) {
				for_each_physical_ring(e, i915) {
					igt_dynamic_f("%s", e->name) {
						do_test(test->func, i915,
							&empty_cfg, eb_ring(e),
							e->full_name);
					}
				}
			}
		}
	}

	/* New way of selecting engines. */

	igt_subtest_group {
		const struct intel_execution_engine2 *e;

		igt_fixture
			gem_require_contexts(i915);

		for (test = tests; test->name; test++) {
			igt_subtest_with_dynamic_f("engines-%s", test->name) {
				for_each_ctx_engine(i915, ctx, e) {
					igt_dynamic_f("%s", e->name) {
						do_test(test->func, i915,
							&ctx->cfg, e->flags,
							e->name);
					}
				}
			}
		}

		igt_subtest_with_dynamic_f("saturated-hostile") {
			for_each_ctx_engine(i915, ctx, e) {
				igt_dynamic_f("%s", e->name)
					do_test(test_saturated_hostile, i915, &ctx->cfg, e->flags, e->name);
			}
		}

		igt_subtest_with_dynamic_f("saturated-hostile-nopreempt") {
			for_each_ctx_engine(i915, ctx, e) {
				igt_dynamic_f("%s", e->name)
					do_test(test_saturated_hostile_nopreempt, i915, &ctx->cfg, e->flags, e->name);
			}
		}

		igt_subtest("many-contexts")
			many_contexts(i915, &ctx->cfg);
	}

	igt_subtest_group {
		igt_fixture {
			gem_require_contexts(i915);
			intel_allocator_multiprocess_start();
		}

		igt_subtest("smoketest")
			smoketest(i915, &ctx->cfg);

		igt_fixture {
			intel_allocator_multiprocess_stop();
		}

	}

	igt_fixture {
		close(i915);
	}
}
