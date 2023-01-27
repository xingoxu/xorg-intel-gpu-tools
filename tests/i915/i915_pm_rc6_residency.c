/*
 * Copyright © 2012 Intel Corporation
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
 * Authors:
 *    Ben Widawsky <ben@bwidawsk.net>
 *
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include "i915/gem.h"
#include "i915/gem_create.h"
#include "igt.h"
#include "igt_perf.h"
#include "igt_power.h"
#include "igt_sysfs.h"
#include "sw_sync.h"

#define SLEEP_DURATION 3 /* in seconds */

#define RC6_ENABLED	1
#define RC6P_ENABLED	2
#define RC6PP_ENABLED	4

static int sysfs;

struct residencies {
	int rc6;
	int media_rc6;
	int rc6p;
	int rc6pp;
	int duration;
};

static unsigned long get_rc6_enabled_mask(void)
{
	unsigned long enabled;

	enabled = 0;
	igt_sysfs_scanf(sysfs, "power/rc6_enable", "%lu", &enabled);
	return enabled;
}

static bool has_rc6_residency(const char *name)
{
	unsigned long residency;
	char path[128];

	sprintf(path, "power/%s_residency_ms", name);
	return igt_sysfs_scanf(sysfs, path, "%lu", &residency) == 1;
}

static unsigned long read_rc6_residency(const char *name)
{
	unsigned long residency;
	char path[128];

	residency = 0;
	sprintf(path, "power/%s_residency_ms", name);
	igt_assert(igt_sysfs_scanf(sysfs, path, "%lu", &residency) == 1);
	return residency;
}

static void residency_accuracy(unsigned int diff,
			       unsigned int duration,
			       const char *name_of_rc6_residency)
{
	double ratio;

	ratio = (double)diff / duration;

	igt_info("Residency in %s or deeper state: %u ms (sleep duration %u ms) (%.1f%% of expected duration)\n",
		 name_of_rc6_residency, diff, duration, 100*ratio);
	igt_assert_f(ratio > 0.9 && ratio < 1.05,
		     "Sysfs RC6 residency counter is inaccurate.\n");
}

static unsigned long gettime_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void read_residencies(int devid, unsigned int mask,
			     struct residencies *res)
{
	res->duration = gettime_ms();

	if (mask & RC6_ENABLED)
		res->rc6 = read_rc6_residency("rc6");

	if ((mask & RC6_ENABLED) &&
	    (IS_VALLEYVIEW(devid) || IS_CHERRYVIEW(devid)))
		res->media_rc6 = read_rc6_residency("media_rc6");

	if (mask & RC6P_ENABLED)
		res->rc6p = read_rc6_residency("rc6p");

	if (mask & RC6PP_ENABLED)
		res->rc6pp = read_rc6_residency("rc6pp");

	res->duration += (gettime_ms() - res->duration) / 2;
}

static void measure_residencies(int devid, unsigned int mask,
				struct residencies *res)
{
	struct residencies start = { };
	struct residencies end = { };
	int retry;

	/*
	 * Retry in case of counter wrap-around. We simply re-run the
	 * measurement, since the valid counter range is different on
	 * different platforms and so fixing it up would be non-trivial.
	 */
	read_residencies(devid, mask, &end);
	igt_debug("time=%d: rc6=(%d, %d), rc6p=%d, rc6pp=%d\n",
		  end.duration, end.rc6, end.media_rc6, end.rc6p, end.rc6pp);
	for (retry = 0; retry < 2; retry++) {
		start = end;
		sleep(SLEEP_DURATION);
		read_residencies(devid, mask, &end);

		igt_debug("time=%d: rc6=(%d, %d), rc6p=%d, rc6pp=%d\n",
			  end.duration,
			  end.rc6, end.media_rc6, end.rc6p, end.rc6pp);

		if (end.rc6 >= start.rc6 &&
		    end.media_rc6 >= start.media_rc6 &&
		    end.rc6p >= start.rc6p &&
		    end.rc6pp >= start.rc6pp)
			break;
	}
	igt_assert_f(retry < 2, "residency values are not consistent\n");

	res->rc6 = end.rc6 - start.rc6;
	res->rc6p = end.rc6p - start.rc6p;
	res->rc6pp = end.rc6pp - start.rc6pp;
	res->media_rc6 = end.media_rc6 - start.media_rc6;
	res->duration = end.duration - start.duration;

	/*
	 * For the purposes of this test case we want a given residency value
	 * to include the time spent in the corresponding RC state _and_ also
	 * the time spent in any enabled deeper states. So for example if any
	 * of RC6P or RC6PP is enabled we want the time spent in these states
	 * to be also included in the RC6 residency value. The kernel reported
	 * residency values are exclusive, so add up things here.
	 */
	res->rc6p += res->rc6pp;
	res->rc6 += res->rc6p;
}

static bool wait_for_rc6(void)
{
	struct timespec tv = {};
	unsigned long start, now;

	/* First wait for roughly an RC6 Evaluation Interval */
	usleep(160 * 1000);

	/* Then poll for RC6 to start ticking */
	now = read_rc6_residency("rc6");
	do {
		start = now;
		usleep(5000);
		now = read_rc6_residency("rc6");
		if (now - start > 1)
			return true;
	} while (!igt_seconds_elapsed(&tv));

	return false;
}

static uint64_t __pmu_read_single(int fd, uint64_t *ts)
{
	uint64_t data[2];

	igt_assert_eq(read(fd, data, sizeof(data)), sizeof(data));

	if (ts)
		*ts = data[1];

	return data[0];
}

static uint64_t pmu_read_single(int fd)
{
	return __pmu_read_single(fd, NULL);
}

#define __assert_within_epsilon(x, ref, tol_up, tol_down) \
	igt_assert_f((x) <= (ref) * (1.0 + (tol_up)/100.) && \
		     (x) >= (ref) * (1.0 - (tol_down)/100.), \
		     "'%s' != '%s' (%.3g not within +%d%%/-%d%% tolerance of %.3g)\n",\
		     #x, #ref, (double)(x), (tol_up), (tol_down), (double)(ref))

#define assert_within_epsilon(x, ref, tolerance) \
	__assert_within_epsilon(x, ref, tolerance, tolerance)

static bool __pmu_wait_for_rc6(int fd)
{
	struct timespec tv = {};
	uint64_t start, now;

	/* First wait for roughly an RC6 Evaluation Interval */
	usleep(160 * 1000);

	/* Then poll for RC6 to start ticking */
	now = pmu_read_single(fd);
	do {
		start = now;
		usleep(5000);
		now = pmu_read_single(fd);
		if (now - start > 1e6)
			return true;
	} while (!igt_seconds_elapsed(&tv));

	return false;
}

static unsigned int measured_usleep(unsigned int usec)
{
	struct timespec ts = { };
	unsigned int slept;

	slept = igt_nsec_elapsed(&ts);
	igt_assert(slept == 0);
	do {
		usleep(usec - slept);
		slept = igt_nsec_elapsed(&ts) / 1000;
	} while (slept < usec);

	return igt_nsec_elapsed(&ts);
}

static uint32_t batch_create(int fd)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	uint32_t handle;

	handle = gem_create(fd, 4096);
	gem_write(fd, handle, 0, &bbe, sizeof(bbe));

	return handle;
}

static int open_pmu(int i915, uint64_t config)
{
	int fd;

	fd = perf_i915_open(i915, config);
	igt_skip_on(fd < 0 && errno == ENODEV);
	igt_assert(fd >= 0);

	return fd;
}

#define WAITBOOST 0x1
#define ONCE 0x2

static void sighandler(int sig)
{
}

static void bg_load(int i915, uint32_t ctx_id, uint64_t engine_flags, unsigned int flags, unsigned long *ctl)
{
	const bool has_execlists = intel_gen(intel_get_drm_devid(i915)) >= 8;
	struct drm_i915_gem_exec_object2 obj = {
		.handle = batch_create(i915),
	};
	struct drm_i915_gem_execbuffer2 execbuf = {
		.buffers_ptr = to_user_pointer(&obj),
		.buffer_count = 1,
		.flags = engine_flags,
		.rsvd1 = ctx_id,
	};
	struct sigaction act = {
		.sa_handler = sighandler
	};

	sigaction(SIGINT, &act, NULL);
	do {
		uint64_t submit, wait, elapsed;
		struct timespec tv = {};

		igt_nsec_elapsed(&tv);

		gem_execbuf(i915, &execbuf);
		submit = igt_nsec_elapsed(&tv);
		if (flags & WAITBOOST) {
			gem_sync(i915, obj.handle);
			if (flags & ONCE)
				flags &= ~WAITBOOST;
		} else  {
			while (gem_bo_busy(i915, obj.handle))
				usleep(0);
		}
		wait = igt_nsec_elapsed(&tv);

		/*
		 * The legacy ringbuffer submission lacks a fast soft-rc6
		 * mechanism as we have no interrupt for an idle ring. As such
		 * we are at the mercy of HW RC6... which is not quite as
		 * precise as we need to pass this test. Oh well.
		 *
		 * Fake it until we make it.
		 */
		if (!has_execlists)
			igt_drop_caches_set(i915, DROP_IDLE);

		elapsed = igt_nsec_elapsed(&tv);
		igt_debug("Pulse took %.3fms (submit %.1fus, wait %.1fus, idle %.1fus)\n",
			  1e-6 * elapsed,
			  1e-3 * submit,
			  1e-3 * (wait - submit),
			  1e-3 * (elapsed - wait));
		ctl[1]++;

		/* aim for ~1% busy */
		usleep(min_t(elapsed, elapsed / 10, 50 * 1000));
	} while (!READ_ONCE(*ctl));
}

static void kill_children(int sig)
{
	void (*old)(int);

	old = signal(sig, SIG_IGN);
	kill(-getpgrp(), sig);
	signal(sig, old);
}

static void rc6_idle(int i915, uint32_t ctx_id, uint64_t flags)
{
	const int64_t duration_ns = SLEEP_DURATION * (int64_t)NSEC_PER_SEC;
	const int tolerance = 20; /* Some RC6 is better than none! */
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	struct {
		const char *name;
		unsigned int flags;
		double power;
	} phases[] = {
		{ "normal", 0 },
		{ "boost", WAITBOOST },
		{ "once", WAITBOOST | ONCE },
	};
	struct power_sample sample[2];
	unsigned long slept, cycles;
	unsigned long *done;
	uint64_t rc6, ts[2];
	struct igt_power gpu;
	int fd;

	fd = open_pmu(i915, I915_PMU_RC6_RESIDENCY);
	igt_drop_caches_set(i915, DROP_IDLE);
	igt_require(__pmu_wait_for_rc6(fd));
	igt_power_open(i915, &gpu, "gpu");

	/* While idle check full RC6. */
	igt_power_get_energy(&gpu, &sample[0]);
	rc6 = -__pmu_read_single(fd, &ts[0]);
	slept = measured_usleep(duration_ns / 1000);
	rc6 += __pmu_read_single(fd, &ts[1]);
	igt_debug("slept=%lu perf=%"PRIu64", rc6=%"PRIu64"\n",
		  slept, ts[1] - ts[0], rc6);
	igt_power_get_energy(&gpu, &sample[1]);
	if (sample[1].energy) {
		double idle = igt_power_get_mJ(&gpu, &sample[0], &sample[1]);

		igt_log(IGT_LOG_DOMAIN,
			!gem_has_lmem(i915) && idle > 1e-3 && gen > 6 ? IGT_LOG_WARN : IGT_LOG_INFO,
			"Total energy used while idle: %.1fmJ (%.1fmW)\n",
			idle, (idle * 1e9) / slept);
	}
	assert_within_epsilon(rc6, ts[1] - ts[0], 5);

	done = mmap(0, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);

	for (int p = 0; p < ARRAY_SIZE(phases); p++) {
		memset(done, 0, 2 * sizeof(*done));
		igt_fork(child, 1) /* Setup up a very light load */
			bg_load(i915, ctx_id, flags, phases[p].flags, done);

		igt_power_get_energy(&gpu, &sample[0]);
		cycles = -READ_ONCE(done[1]);
		rc6 = -__pmu_read_single(fd, &ts[0]);
		slept = measured_usleep(duration_ns / 1000);
		rc6 += __pmu_read_single(fd, &ts[1]);
		cycles += READ_ONCE(done[1]);
		igt_debug("%s: slept=%lu perf=%"PRIu64", cycles=%lu, rc6=%"PRIu64"\n",
			  phases[p].name, slept, ts[1] - ts[0], cycles, rc6);
		igt_power_get_energy(&gpu, &sample[1]);
		if (sample[1].energy) {
			phases[p].power = igt_power_get_mJ(&gpu, &sample[0], &sample[1]);
			igt_info("Total energy used for %s: %.1fmJ (%.1fmW)\n",
				 phases[p].name,
				 phases[p].power,
				 phases[p].power * 1e9 / slept);
			phases[p].power /= slept; /* normalize */
			phases[p].power *= 1e9; /* => mW */
		}

		*done = 1;
		kill_children(SIGINT);
		igt_waitchildren();

		/* At least one wakeup/s needed for a reasonable test */
		igt_assert(cycles >= SLEEP_DURATION);

		/* While very nearly idle, expect full RC6 */
		assert_within_epsilon(rc6, ts[1] - ts[0], tolerance);
	}

	munmap(done, 4096);
	close(fd);

	igt_power_close(&gpu);

	if (phases[1].power - phases[0].power > 10) {
		igt_assert_f(2 * phases[2].power - phases[0].power <= phases[1].power,
			     "Exceeded energy expectations for single busy wait load\n"
			     "Used %.1fmW, min %.1fmW, max %.1fmW, expected less than %.1fmW\n",
			     phases[2].power, phases[0].power, phases[1].power,
			     phases[0].power + (phases[1].power - phases[0].power) / 2);
	}
}

static void rc6_fence(int i915, const intel_ctx_t *ctx)
{
	const int64_t duration_ns = SLEEP_DURATION * (int64_t)NSEC_PER_SEC;
	const int tolerance = 20; /* Some RC6 is better than none! */
	const unsigned int gen = intel_gen(intel_get_drm_devid(i915));
	const struct intel_execution_engine2 *e;
	struct power_sample sample[2];
	unsigned long slept;
	uint64_t rc6, ts[2], ahnd;
	struct igt_power gpu;
	int fd;

	igt_require_sw_sync();

	fd = open_pmu(i915, I915_PMU_RC6_RESIDENCY);
	igt_drop_caches_set(i915, DROP_IDLE);
	igt_require(__pmu_wait_for_rc6(fd));
	igt_power_open(i915, &gpu, "gpu");

	/* While idle check full RC6. */
	igt_power_get_energy(&gpu, &sample[0]);
	rc6 = -__pmu_read_single(fd, &ts[0]);
	slept = measured_usleep(duration_ns / 1000);
	rc6 += __pmu_read_single(fd, &ts[1]);
	igt_debug("slept=%lu perf=%"PRIu64", rc6=%"PRIu64"\n",
		  slept, ts[1] - ts[0], rc6);

	igt_power_get_energy(&gpu, &sample[1]);
	if (sample[1].energy) {
		double idle = igt_power_get_mJ(&gpu, &sample[0], &sample[1]);
		igt_log(IGT_LOG_DOMAIN,
			!gem_has_lmem(i915) && idle > 1e-3 && gen > 6 ? IGT_LOG_WARN : IGT_LOG_INFO,
			"Total energy used while idle: %.1fmJ (%.1fmW)\n",
			idle, (idle * 1e9) / slept);
	}
	assert_within_epsilon(rc6, ts[1] - ts[0], 5);

	/* Submit but delay execution, we should be idle and conserving power */
	ahnd = get_reloc_ahnd(i915, ctx->id);
	for_each_ctx_engine(i915, ctx, e) {
		igt_spin_t *spin;
		int timeline;
		int fence;

		timeline = sw_sync_timeline_create();
		fence = sw_sync_timeline_create_fence(timeline, 1);
		spin = igt_spin_new(i915,
				    .ahnd = ahnd,
				    .ctx = ctx,
				    .engine = e->flags,
				    .fence = fence,
				    .flags = IGT_SPIN_FENCE_IN);
		close(fence);

		igt_power_get_energy(&gpu, &sample[0]);
		rc6 = -__pmu_read_single(fd, &ts[0]);
		slept = measured_usleep(duration_ns / 1000);
		rc6 += __pmu_read_single(fd, &ts[1]);
		igt_debug("%s: slept=%lu perf=%"PRIu64", rc6=%"PRIu64"\n",
			  e->name, slept, ts[1] - ts[0], rc6);

		igt_power_get_energy(&gpu, &sample[1]);
		if (sample[1].energy) {
			double power = igt_power_get_mJ(&gpu, &sample[0], &sample[1]);
			igt_info("Total energy used for %s: %.1fmJ (%.1fmW)\n",
				 e->name,
				 power,
				 power * 1e9 / slept);
		}

		igt_assert(gem_bo_busy(i915, spin->handle));
		igt_spin_free(i915, spin);

		close(timeline);

		assert_within_epsilon(rc6, ts[1] - ts[0], tolerance);
		gem_quiescent_gpu(i915);
	}
	put_ahnd(ahnd);

	igt_power_close(&gpu);
	close(fd);
}

igt_main
{
	int i915 = -1;
	const intel_ctx_t *ctx;

	/* Use drm_open_driver to verify device existence */
	igt_fixture {
		i915 = drm_open_driver(DRIVER_INTEL);
		ctx = intel_ctx_create_all_physical(i915);
	}

	igt_subtest_with_dynamic("rc6-idle") {
		const struct intel_execution_engine2 *e;

		igt_require_gem(i915);
		gem_quiescent_gpu(i915);

		for_each_ctx_engine(i915, ctx, e) {
			if (e->instance == 0) {
				igt_dynamic_f("%s", e->name)
					rc6_idle(i915, ctx->id, e->flags);
			}
		}
	}

	igt_subtest("rc6-fence") {
		igt_require_gem(i915);
		gem_quiescent_gpu(i915);

		rc6_fence(i915, ctx);
	}

	igt_subtest_group {
		unsigned int rc6_enabled = 0;
		unsigned int devid = 0;

		igt_fixture {
			devid = intel_get_drm_devid(i915);
			sysfs = igt_sysfs_open(i915);

			igt_require(has_rc6_residency("rc6"));

			/* Make sure rc6 counters are running */
			igt_drop_caches_set(i915, DROP_IDLE);
			igt_require(wait_for_rc6());

			rc6_enabled = get_rc6_enabled_mask();
			igt_require(rc6_enabled & RC6_ENABLED);
		}

		igt_subtest("rc6-accuracy") {
			struct residencies res;

			measure_residencies(devid, rc6_enabled, &res);
			residency_accuracy(res.rc6, res.duration, "rc6");
		}

		igt_subtest("media-rc6-accuracy") {
			struct residencies res;

			igt_require(IS_VALLEYVIEW(devid) || IS_CHERRYVIEW(devid));

			measure_residencies(devid, rc6_enabled, &res);
			residency_accuracy(res.media_rc6, res.duration, "media_rc6");
		}

		igt_fixture
			close(sysfs);
	}

	igt_fixture {
		intel_ctx_destroy(i915, ctx);
		close(i915);
	}
}
