#ifndef __DRM_LOG_H__
#define __DRM_LOG_H__

/*
 * Copyright (c) 2013-2014 David Herrmann <dh.herrmann@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <linux/fs.h>
#include <linux/module.h>
#include <linux/types.h>

#ifdef CONFIG_DRM_LOG

void drm_log_init(void);
void drm_log_exit(void);

void drm_log_write(const char *data, size_t len, bool atomic);
void drm_log_ensure_size(size_t width_px, size_t height_px);
void drm_log_draw(void *kern_map,
		  size_t width,
		  size_t height,
		  size_t stride,
		  size_t cpp,
		  u32 pixel_format,
		  size_t columns);
void *drm_log_register_panic_fb(void);
void drm_log_update_panic_fb(void * panic_fb,
		  void *kern_map,
		  size_t width,
		  size_t height,
		  size_t stride,
		  size_t cpp,
		  u32 pixel_format);
#else

static inline void drm_log_init(void) { }
static inline void drm_log_exit(void) { }

static inline void drm_log_write(const char *data, size_t len, bool atomic) { }
static inline void drm_log_ensure_size(size_t x, size_t y) { }
static inline void drm_log_draw(void *kern_map,
				size_t width,
				size_t height,
				size_t stride,
				size_t cpp,
				u32 pixel_format,
				size_t columns) { }

static inline void *drm_log_register_panic(void) { return NULL;}

static inline void drm_log_update_panic_fb(void * panic_fb,
		  void *kern_map,
		  size_t width,
		  size_t height,
		  size_t stride,
		  size_t cpp,
		  u32 pixel_format) {}
#endif

#endif /* __DRM_LOG_H__ */
