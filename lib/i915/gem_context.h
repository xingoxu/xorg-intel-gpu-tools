/*
 * Copyright © 2017 Intel Corporation
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

#ifndef GEM_CONTEXT_H
#define GEM_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>

struct drm_i915_gem_context_param;

uint32_t gem_context_create(int fd);
uint32_t gem_context_create_ext(int fd, uint32_t flags, uint64_t extensions);
int __gem_context_create(int fd, uint32_t *ctx_id);
int __gem_context_create_ext(int fd, uint32_t flags, uint64_t extensions,
			     uint32_t *ctx_id);
void gem_context_destroy(int fd, uint32_t ctx_id);
int __gem_context_destroy(int fd, uint32_t ctx_id);

uint32_t gem_context_create_for_engine(int fd, unsigned int class, unsigned int inst);
uint32_t gem_context_create_for_class(int i915, unsigned int class, unsigned int *count);

bool gem_contexts_has_shared_gtt(int i915);
bool gem_has_queues(int i915);

bool gem_has_contexts(int fd);
void gem_require_contexts(int fd);
bool gem_context_has_single_timeline(int i915);
void gem_context_require_bannable(int fd);
void gem_context_require_param(int fd, uint64_t param);

void gem_context_get_param(int fd, struct drm_i915_gem_context_param *p);
void gem_context_set_param(int fd, struct drm_i915_gem_context_param *p);
int __gem_context_set_param(int fd, struct drm_i915_gem_context_param *p);
int __gem_context_get_param(int fd, struct drm_i915_gem_context_param *p);

int __gem_context_set_priority(int fd, uint32_t ctx, int prio);
void gem_context_set_priority(int fd, uint32_t ctx, int prio);

bool gem_context_has_persistence(int i915);
int __gem_context_set_persistence(int i915, uint32_t ctx, bool state);
void gem_context_set_persistence(int i915, uint32_t ctx, bool state);

bool gem_context_has_engine(int fd, uint32_t ctx, uint64_t engine);

#endif /* GEM_CONTEXT_H */
