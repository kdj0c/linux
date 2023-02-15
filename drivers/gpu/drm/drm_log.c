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

#include <asm/unaligned.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_print.h>
#include <drm/drm_crtc.h>
#include <linux/atomic.h>
#include <linux/console.h>
#include <linux/font.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/panic_notifier.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/swab.h>
#include <linux/types.h>
#include <linux/vmalloc.h>


/**
 * DOC: drm log
 *
 * The DRM-log helpers keep an internal screen-buffer of the global kernel-log
 * and provide render functions to draw the current log-buffer into a
 * memory-mapped framebuffer. The whole subsystem is disabled if the kernel is
 * compiled with CONFIG_VT. You should use the VT layer to draw the kernel-log
 * in that case. DRM-log provides a minimal fallback if VTs are disabled.
 *
 * DRM-log is meant for debugging only! The main use-cases include:
 *   - render panic screens
 *   - render oops screens
 *   - render kernel-log for debugging
 * Rendering should be enabled for debugging only! Enabling DRM-log during boot
 * will slow everything down considerably! If you want a fast boot-log, use some
 * user-space renderer. DRM-log only makes sense to debug
 * early-boot/late-shutdown issues and oops/panics. Note that the DRM-log core
 * just provides the helpers, it does not apply any policy. It is up to the
 * users of this API and the more high-level DRM-log interfaces to render only
 * if appropriate.
 *
 * The DRM-log core keeps an internal kernel-log buffer which can be shared
 * across drivers and devices. This buffer is always kept up-to-date, allows
 * atomic updates during kernel panics/oopses and should never be accessed
 * directly from a driver.
 * The log-buffer always stays allocated so any kernel-log writes do not need to
 * allocate any memory (which might be fragile during kernel-panics). However,
 * this means that we need to limit the line-length to a maximum. A sane default
 * is provided by drm-log, but you need to call drm_log_ensure_size() to keep
 * the buffer big enough for bigger screens. Otherwise, you might end up with
 * blank margins due to lack of data. Note that the renderer is smart enough to
 * render a log-buffer of any size onto a framebuffer of any size. But if the
 * backlog is too small, you obviously will see blank margins. See
 * drm_log_ensure_size() for more.
 * Kernel-code can directly write into the DRM-log buffer via drm_log_write().
 * However, you *really* should be using printk() instead! There are only very
 * few reasons to skip the kernel-log, so you should know what you're doing.
 * DRM-log registers a console-driver to get notified of any printk() and then
 * writes it into its log via drm_log_write() itself.
 *
 * On top of the core log-buffer, the most low-level entry point for drivers is
 * drm_log_draw(). This renders the kernel log onto any memory-mapped
 * framebuffer. Any more high-level interface of DRM-log calls this at some
 * point to render the actual buffer. drm_log_draw() does no scheduling,
 * redrawing or any other fancy logic. It just renders the text-data via an
 * in-kernel font onto the framebuffer and returns. Multiple renderers can run
 * in parallel just fine (and even allow parallel writers). However, rendering
 * is slow as we need to support any pixel-format we have. We also don't want
 * any fancy (and possibly fragile) optimizations that make the code harder to
 * read. This is a debug-feature, remember? Nobody cares whether it takes 200ms
 * to render your panic-screen.
 *
 * Higher-level interfaces will follow later on.
 */

struct dlog_line {
	atomic_t length;
	atomic_t cont;
	char *cells;
};

struct dlog_buf {
	size_t width;
	size_t height;
	atomic_t pos;
	struct dlog_line lines[];
};

struct dlog_framebuffer {
	struct list_head head;
	void *kern_map;
	size_t width;
	size_t height;
	size_t stride;
	size_t cpp;
	u32 pixel_format;
	size_t columns;
};

static LIST_HEAD(dlog_fb);

/* console-buffer management */
static struct dlog_buf __rcu *dlog_buf;
static const struct font_desc *dlog_font;
static DEFINE_MUTEX(dlog_wlock);

/* caller must hold either rcu-read-lock or dlog_wlock */
static void dlog__write_line(struct dlog_buf *buf,
			     const char *data,
			     size_t len,
			     bool continuation)
{
	struct dlog_line *line;
	size_t pos;

	if (!buf || !data || !len)
		return;

	/*
	 * Get pointer to the next line and asynchronously write into it. This
	 * is *unlocked* against any readers, so:
	 *  - smp_wmb() does not protect against garbage on the screen, but
	 *    reduces conflicts slightly.
	 *  - Our buffers usually contain more lines than shown on screen, thus
	 *    garbage will only occur during huge writes.
	 *  - Caller has to redraw, so garbage will always get corrected. Any
	 *    crap we might end up with will get corrected shortly after.
	 */

	pos = atomic_read(&buf->pos) + 1;
	if (pos >= buf->height)
		pos = 0;

	line = &buf->lines[pos];
	memcpy(line->cells, data, min(buf->width, len));
	atomic_set(&line->length, len);
	atomic_set(&line->cont, continuation);

	smp_wmb();
	atomic_set(&buf->pos, pos);
}

/* caller must hold either rcu-read-lock or dlog_wlock */
static void dlog__write(struct dlog_buf *buf, const char *data, size_t len)
{
	size_t i;
	char c;
	bool cont = false;

	if (!buf || !data || !len)
		return;

	for (i = 0; i < len; ) {
		c = data[i++];

		if (c == '\n' || i >= buf->width) {
			if (c == '\n') {
				dlog__write_line(buf, data, i - 1, cont);
				cont = false;
			} else {
				dlog__write_line(buf, data, i, cont);
				cont = true;
			}

			data += i;
			len -= i;
			i = 0;
		}
	}

	dlog__write_line(buf, data, i, cont);
}

#define DLOG__WRITE(_buf, _str) dlog__write((_buf), (_str), sizeof(_str) - 1)

/**
 * drm_log_write() - Write lines into log-buffer
 * @data: ASCII input data
 * @len: length of input data in bytes
 * @atomic: avoid waiting for locks
 *
 * Write the message from @data into the log-buffer. The message is put on a
 * new line and wrapped for each newline-character. Further line-wrapping is
 * performed in case a line is longer than our internal buffer-width. The
 * caller shouldn't care, though. The renderer is smart enough to draw the
 * global log-buffer onto framebuffers of any size.
 *
 * The caller can, but is not required to, hold the console-lock.
 *
 * This function is locked against other writers and may sleep to acquire a
 * mutex. In case an oops or panic is in progress, this function avoids
 * waiting for locks and allows safe parallel writes in exchange for some minor
 * drawbacks (set atomic==true to always take this fast-path).
 *
 * The drm-log subsystem registers its own console-driver, so usually there is
 * no reason to write directly into the log. Use printk()! This helper might be
 * useful for some special debugging paths, though.
 */
void drm_log_write(const char *data, size_t len, bool atomic)
{
	struct dlog_buf *buf;
	bool in_oops;

	if (!data || !len)
		return;

	/*
	 * If an oops/panic is in progress, we avoid taking the mutex and
	 * instead write data directly. Note that this is totally safe as the
	 * rcu-protected buffer has a pre-allocated static size. Things that
	 * might go wrong are:
	 *  - resize is ongoing in parallel, we might loose messages
	 *  - parallel writes might overwrite each other
	 * Both are negligible. Besides, during panics only one CPU is active
	 * and the normal message-stream is locked by console_lock. Therefore,
	 * there's currently no reason to optimize this for proper parallel
	 * writes. If this is needed at some point, we can add this.
	 */

	in_oops = atomic || READ_ONCE(oops_in_progress);
	if (in_oops) {
		if (mutex_trylock(&dlog_wlock))
			in_oops = false;
		else
			rcu_read_lock();
	} else {
		mutex_lock(&dlog_wlock);
	}

	if (in_oops)
		buf = rcu_dereference(dlog_buf);
	else
		buf = rcu_dereference_protected(dlog_buf,
						lockdep_is_held(&dlog_wlock));

	dlog__write(buf, data, len);

	if (in_oops)
		rcu_read_unlock();
	else
		mutex_unlock(&dlog_wlock);
}
EXPORT_SYMBOL(drm_log_write);

/* caller must own @buf exclusively! */
static void dlog_free_buf(struct dlog_buf *buf)
{
	size_t i;

	if (!buf)
		return;

	for (i = 0; i < buf->height; ++i)
		kfree(buf->lines[i].cells);

	kfree(buf);
}

/**
 * drm_log_ensure_size() - Try to ensure log-buffer size
 * @width_px: Width of your framebuffer in pixels
 * @width_py: Height of your framebuffer in pixels
 *
 * Whenever a new framebuffer is added, we try to make sure the log-buffer has
 * at least the required dimension. If the buffer is already big enough, there
 * is nothing to do. Otherwise, we simply allocate a new buffer and put it in
 * place. Callers should pass in the pixel-sizes of their framebuffer. We
 * calculate the cell-table dimensions internally and update the log-buffer
 * accordingly.
 * If we have to resize the buffer, we always allocate twice the required size.
 * This guarantees that adding bigger framebuffers later on avoids
 * re-allocations if they are in a suitable range. Furthermore, if a driver can,
 * in a semi-reliable manner, predict the maximum screen-size of any CRTC it
 * has, it is recommended to call drm_log_ensure_size() during device-probing to
 * get a reasonably sized buffer early on. This is not required, though.
 *
 * Note: Our renderer handles any buffer size just fine, so we can safely
 * ignore mem-alloc errors here. If we render onto a framebuffer that is
 * *bigger* than our internal log-buffer, split lines will be merged again. On
 * the other hand, if the framebuffer is *smaller* than our internal log-buffer,
 * we simply split buffer-entries into multiple screen-lines.
 *
 * This call is locked against parallel calls.
 *
 * This function may sleep.
 */
void drm_log_ensure_size(size_t width_px, size_t height_px)
{
	struct dlog_buf *buf, *old;
	size_t pos, i, x, y;

	mutex_lock(&dlog_wlock);

	if (!dlog_font)
		goto out_unlock;

	/* ensure 80x25 buffer size (FBs can still be smaller!) */
	x = max_t(size_t, 80, width_px / dlog_font->width);
	y = max_t(size_t, 25, height_px / dlog_font->height);

	old = rcu_dereference_protected(dlog_buf,
					lockdep_is_held(&dlog_wlock));

	/* make sure we're bigger than prev buffer, or bail out */
	if (old) {
		if (x < old->width)
			x = old->width;
		if (y < old->height)
			y = old->height;
		if (x == old->width && y == old->height)
			goto out_unlock;
	}

	/* double size to avoid repeated resizing */
	x *= 2;
	y *= 2;

	buf = kzalloc(sizeof(*buf) + y * sizeof(*buf->lines), GFP_KERNEL);
	if (!buf)
		goto out_unlock;

	buf->width = x;
	buf->height = y;

	for (i = 0; i < buf->height; ++i) {
		buf->lines[i].cells = kzalloc(buf->width, GFP_KERNEL);
		if (!buf->lines[i].cells)
			goto out_free;
	}

	/* copy over old messages */
	if (old) {
		pos = atomic_read(&old->pos);
		for (i = 0; i < old->height; ++i) {
			atomic_set(&buf->lines[i].length,
				   atomic_read(&old->lines[pos].length));
			memcpy(buf->lines[i].cells,
			       old->lines[pos].cells,
			       atomic_read(&old->lines[pos].length));
			if (++pos >= old->height)
				pos = 0;
		}
		atomic_set(&buf->pos, (i < buf->height) ? i : 0);
	}

	/* place resize message */
	DLOG__WRITE(buf, "drm: log resized");

	/* replace old buffer */
	rcu_assign_pointer(dlog_buf, buf);
	synchronize_rcu();

	/* free old buf */
	buf = old;

out_free:
	dlog_free_buf(buf);
out_unlock:
	mutex_unlock(&dlog_wlock);
}
EXPORT_SYMBOL(drm_log_ensure_size);

/*
 * Draw Pixel
 * This writes a single pixel of format @pixel_format at the given destination.
 * You can pass high-res 32bit color as input, which is downscaled to the given
 * target bitrate.
 *
 * This is horribly slow and awful but supports all known RGB formats. We might
 * wanna add fast-paths for XRGB8888 and other common formats, but why bother
 * for debug logs.. Furthermore, the compiler should be able to optimize
 * "pixel_format" for all rendering-paths if it inlines this helper. Same is
 * true for put_unaligned(), but it might not be that smart..
 */
static inline void dlog__draw_px(u8 *dst,
				 u32 pixel_format,
				 u32 a,
				 u32 r,
				 u32 g,
				 u32 b)
{
	bool need_swap = false;
	u32 val32;
	u16 val16;

	switch (pixel_format) {

	/*
	 * C8
	 */

	case DRM_FORMAT_C8:
		/* We have no access to the color-palette, so use 0x00 for
		 * black, 0xff for everything else */
		*dst = 0xff * !!(r | g | b);
		break;

	/*
	 * RGB332 and friends
	 */

	case DRM_FORMAT_RGB332:
		r = (r >> 29) & 0x07;
		g = (g >> 29) & 0x07;
		b = (b >> 30) & 0x03;
		*dst = (r << 5) | (g << 2) | b;
		break;
	case DRM_FORMAT_BGR233:
		r = (r >> 29) & 0x07;
		g = (g >> 29) & 0x07;
		b = (b >> 30) & 0x03;
		*dst = (b << 6) | (g << 3) | r;
		break;

	/*
	 * XRGB4444 and friends
	 */

	case DRM_FORMAT_XRGB4444:
	case DRM_FORMAT_ARGB4444:
		a = (a >> 28) & 0x0f;
		r = (r >> 28) & 0x0f;
		g = (g >> 28) & 0x0f;
		b = (b >> 28) & 0x0f;
		val16 = (a << 12) | (r << 8) | (g << 4) | b;
		if (unlikely(need_swap))
			val16 = swab16(val16);
		put_unaligned(val16, (u16*)dst);
		break;
	case DRM_FORMAT_XBGR4444:
	case DRM_FORMAT_ABGR4444:
		a = (a >> 28) & 0x0f;
		r = (r >> 28) & 0x0f;
		g = (g >> 28) & 0x0f;
		b = (b >> 28) & 0x0f;
		val16 = (a << 12) | (b << 8) | (g << 4) | r;
		if (unlikely(need_swap))
			val16 = swab16(val16);
		put_unaligned(val16, (u16*)dst);
		break;
	case DRM_FORMAT_RGBX4444:
	case DRM_FORMAT_RGBA4444:
		a = (a >> 28) & 0x0f;
		r = (r >> 28) & 0x0f;
		g = (g >> 28) & 0x0f;
		b = (b >> 28) & 0x0f;
		val16 = (r << 12) | (g << 8) | (b << 4) | a;
		if (unlikely(need_swap))
			val16 = swab16(val16);
		put_unaligned(val16, (u16*)dst);
		break;
	case DRM_FORMAT_BGRX4444:
	case DRM_FORMAT_BGRA4444:
		a = (a >> 28) & 0x0f;
		r = (r >> 28) & 0x0f;
		g = (g >> 28) & 0x0f;
		b = (b >> 28) & 0x0f;
		val16 = (b << 12) | (g << 8) | (r << 4) | a;
		if (unlikely(need_swap))
			val16 = swab16(val16);
		put_unaligned(val16, (u16*)dst);
		break;

	/*
	 * XRGB1555 and friends
	 */

	case DRM_FORMAT_XRGB1555:
	case DRM_FORMAT_ARGB1555:
		a = (a >> 31) & 0x01;
		r = (r >> 27) & 0x1f;
		g = (g >> 27) & 0x1f;
		b = (b >> 27) & 0x1f;
		val16 = (a << 15) | (r << 10) | (g << 5) | b;
		if (unlikely(need_swap))
			val16 = swab16(val16);
		put_unaligned(val16, (u16*)dst);
		break;
	case DRM_FORMAT_XBGR1555:
	case DRM_FORMAT_ABGR1555:
		a = (a >> 31) & 0x01;
		r = (r >> 27) & 0x1f;
		g = (g >> 27) & 0x1f;
		b = (b >> 27) & 0x1f;
		val16 = (a << 15) | (b << 10) | (g << 5) | r;
		if (unlikely(need_swap))
			val16 = swab16(val16);
		put_unaligned(val16, (u16*)dst);
		break;
	case DRM_FORMAT_RGBX5551:
	case DRM_FORMAT_RGBA5551:
		a = (a >> 31) & 0x01;
		r = (r >> 27) & 0x1f;
		g = (g >> 27) & 0x1f;
		b = (b >> 27) & 0x1f;
		val16 = (r << 15) | (g << 10) | (b << 5) | a;
		if (unlikely(need_swap))
			val16 = swab16(val16);
		put_unaligned(val16, (u16*)dst);
		break;
	case DRM_FORMAT_BGRX5551:
	case DRM_FORMAT_BGRA5551:
		a = (a >> 31) & 0x01;
		r = (r >> 27) & 0x1f;
		g = (g >> 27) & 0x1f;
		b = (b >> 27) & 0x1f;
		val16 = (b << 15) | (g << 10) | (r << 5) | a;
		if (unlikely(need_swap))
			val16 = swab16(val16);
		put_unaligned(val16, (u16*)dst);
		break;

	/*
	 * RGB565 and friends
	 */

	case DRM_FORMAT_RGB565:
		r = (r >> 27) & 0x1f;
		g = (g >> 26) & 0x3f;
		b = (b >> 27) & 0x1f;
		val16 = (r << 11) | (g << 5) | b;
		if (unlikely(need_swap))
			val16 = swab16(val16);
		put_unaligned(val16, (u16*)dst);
		break;
	case DRM_FORMAT_BGR565:
		r = (r >> 27) & 0x1f;
		g = (g >> 26) & 0x3f;
		b = (b >> 27) & 0x1f;
		val16 = (b << 11) | (g << 5) | r;
		if (unlikely(need_swap))
			val16 = swab16(val16);
		put_unaligned(val16, (u16*)dst);
		break;

	/*
	 * RGB888 and friends
	 */

	case DRM_FORMAT_RGB888:
		r = (r >> 24) & 0xff;
		g = (g >> 24) & 0xff;
		b = (b >> 24) & 0xff;
		val16 = (r << 11) | (g << 5) | b;
		if (unlikely(need_swap))
			val16 = swab16(val16);
		put_unaligned(val16, (u16*)dst);
		break;
	case DRM_FORMAT_BGR888:
		r = (r >> 24) & 0xff;
		g = (g >> 24) & 0xff;
		b = (b >> 24) & 0xff;
		val16 = (b << 11) | (g << 5) | r;
		if (unlikely(need_swap))
			val16 = swab16(val16);
		put_unaligned(val16, (u16*)dst);
		break;

	/*
	 * XRGB8888 and friends
	 */

	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		a = (a >> 24) & 0xff;
		r = (r >> 24) & 0xff;
		g = (g >> 24) & 0xff;
		b = (b >> 24) & 0xff;
		val32 = (a << 24) | (r << 16) | (g << 8) | b;
		if (unlikely(need_swap))
			val32 = swab32(val32);
		put_unaligned(val32, (u32*)dst);
		break;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		a = (a >> 24) & 0xff;
		r = (r >> 24) & 0xff;
		g = (g >> 24) & 0xff;
		b = (b >> 24) & 0xff;
		val32 = (a << 24) | (b << 16) | (g << 8) | r;
		if (unlikely(need_swap))
			val32 = swab32(val32);
		put_unaligned(val32, (u32*)dst);
		break;
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_RGBA8888:
		a = (a >> 24) & 0xff;
		r = (r >> 24) & 0xff;
		g = (g >> 24) & 0xff;
		b = (b >> 24) & 0xff;
		val32 = (r << 24) | (g << 16) | (b << 8) | a;
		if (unlikely(need_swap))
			val32 = swab32(val32);
		put_unaligned(val32, (u32*)dst);
		break;
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_BGRA8888:
		a = (a >> 24) & 0xff;
		r = (r >> 24) & 0xff;
		g = (g >> 24) & 0xff;
		b = (b >> 24) & 0xff;
		val32 = (b << 24) | (g << 16) | (r << 8) | a;
		if (unlikely(need_swap))
			val32 = swab32(val32);
		put_unaligned(val32, (u32*)dst);
		break;

	/*
	 * XRGB2101010 and friends
	 */

	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
		a = (a >> 30) & 0x0003;
		r = (r >> 22) & 0x03ff;
		g = (g >> 22) & 0x03ff;
		b = (b >> 22) & 0x03ff;
		val32 = (a << 30) | (r << 20) | (g << 10) | b;
		if (unlikely(need_swap))
			val32 = swab32(val32);
		put_unaligned(val32, (u32*)dst);
		break;
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010:
		a = (a >> 30) & 0x0003;
		r = (r >> 22) & 0x03ff;
		g = (g >> 22) & 0x03ff;
		b = (b >> 22) & 0x03ff;
		val32 = (a << 30) | (b << 20) | (g << 10) | r;
		if (unlikely(need_swap))
			val32 = swab32(val32);
		put_unaligned(val32, (u32*)dst);
		break;
	case DRM_FORMAT_RGBX1010102:
	case DRM_FORMAT_RGBA1010102:
		a = (a >> 30) & 0x0003;
		r = (r >> 22) & 0x03ff;
		g = (g >> 22) & 0x03ff;
		b = (b >> 22) & 0x03ff;
		val32 = (r << 30) | (g << 20) | (b << 10) | a;
		if (unlikely(need_swap))
			val32 = swab32(val32);
		put_unaligned(val32, (u32*)dst);
		break;
	case DRM_FORMAT_BGRX1010102:
	case DRM_FORMAT_BGRA1010102:
		a = (a >> 30) & 0x0003;
		r = (r >> 22) & 0x03ff;
		g = (g >> 22) & 0x03ff;
		b = (b >> 22) & 0x03ff;
		val32 = (b << 30) | (g << 20) | (r << 10) | a;
		if (unlikely(need_swap))
			val32 = swab32(val32);
		put_unaligned(val32, (u32*)dst);
		break;
	}
}

/* fill region with given color */
static inline void dlog__fill(u8 *dst,
			      size_t width,
			      size_t height,
			      size_t stride,
			      size_t cpp,
			      u32 pixel_format,
			      u32 a, u32 r, u32 g, u32 b)
{
	size_t i;

	while (height--) {
		for (i = 0; i < width; ++i)
			dlog__draw_px(&dst[i * cpp], pixel_format, a, r, g, b);
		dst += stride;
	}
}

/* fill region with background color (opaque solid black) */
static inline void dlog__clear(u8 *dst,
			       size_t width,
			       size_t height,
			       size_t stride,
			       size_t cpp,
			       u32 pixel_format)
{
	return dlog__fill(dst, width, height, stride, cpp, pixel_format,
			  0xffffffffUL, 0, 0, 0);
}

/* draw single character at given destination */
static inline void dlog__draw_char(char ch,
				   u8 *dst,
				   size_t dst_stride,
				   size_t dst_cpp,
				   u32 pixel_format)
{
	size_t src_width, src_height, src_stride, i;
	const u8 *src;
	u32 col;

	src_width = dlog_font->width;
	src_height = dlog_font->height;
	src_stride = DIV_ROUND_UP(src_width, 8);

	src = dlog_font->data;
	src += ch * src_height * src_stride;

	while (src_height--) {
		for (i = 0; i < src_width; ++i) {
			col = src[i / 8] & (0x80 >> (i % 8));
			col = col ? 0xffffffffUL : 0;

			/* draw opaque black or white pixel */
			dlog__draw_px(&dst[dst_cpp * i], pixel_format,
				      0xffffffffUL, col, col, col);
		}

		src += src_stride;
		dst += dst_stride;
	}
}

/* draw line of text at given destination */
static inline void dlog__draw_line(const char *log,
				   size_t log_len,
				   size_t row_len,
				   u8 *dst,
				   size_t dst_stride,
				   size_t dst_cpp,
				   u32 pixel_format)
{
	size_t i;

	for (i = 0; i < log_len; ++i)
		dlog__draw_char(log[i],
				dst + i * dlog_font->width * dst_cpp,
				dst_stride,
				dst_cpp,
				pixel_format);

	/* clear remaining line */
	if (i < row_len)
		dlog__clear(dst + i * dlog_font->width * dst_cpp,
			    (row_len - i) * dlog_font->width,
			    dlog_font->height,
			    dst_stride,
			    dst_cpp,
			    pixel_format);
}

/*
 * Internal Rendering Entry-Point
 * This is the main internal entry-point for rendering. The caller must
 * guarantee the lifetime of @buf and @kern_map (see drm_log_draw() for
 * parameter-descriptions).
 *
 * This helper supports multi-column layouts. On modern high-res wide displays,
 * you can render the log into multiple columns to increase the backlog size.
 * This is very useful for long backtraces during kernel panics and alike.
 * Furthermore, we support splitting lines if the target is too small and
 * merging lines if they are marked as "continuation" if the target is wider
 * than the internal log.
 *
 * Our algorithm basically is:
 *   - calculate column/screen/.. dimensions
 *   - as long as there are free screen-lines on the framebuffer:
 *      - get the next log-entry and calculate the line-length including all
 *        continuation lines
 *      - calculate how many screen-lines are needed for that log-entry
 *      - for each needed line:
 *         - get next free screen-line
 *         - if it's the last line, render possibly trailing blanks
 *         - for each char in this line:
 *            - try to get the next char from the log-buffer
 *            - parallel-writers might have changed lenghts, break if so
 *            - render the character
 *         - if column is full, advance column
 *         - if out of lines, break
 *   - fill remaining space with black
 *   - fill any margins with black
 *   - draw column-separators
 *
 * The renderer works bottom-up, not top-down! So we start at the lower right
 * corner and render the newest message first. This allows us to skip
 * line-calculations (as log-entries might occupy multiple-lines depending on
 * the screen-width).
 *
 * Note that this renderer is not optimized for speed! It is suprisingly fast
 * and works well for moderate refresh-rates. However, it is *not* suitable for
 * terminal-emulators etc. It is meant for panic-screens, oops-logs and
 * boot-debugging. Users are highly inclined to call this on vblanks, instead
 * of on each change (or use some 16ms delayed workqueue). Furthermore,
 * production systems should disable any verbose logging during normal
 * operation.
 *
 * To support continuation lines, we always need to calculate the real length
 * of the line before rendering it (because we are bottom-up!). When rendering
 * the characters, we need to advance the logs one-by-one again, but parallel
 * writers might have changed the line-lengths in between. If we detect such
 * a situation (i.e., running out of chars), we skip rendering as the writer
 * must have rescheduled a new render-pass already.
 */
static void dlog__draw(struct dlog_buf *buf,
		       void *kern_map,
		       size_t width,
		       size_t height,
		       size_t stride,
		       size_t cpp,
		       u32 pixel_format,
		       size_t columns)
{
	const size_t col_padding = 5;
	size_t log_pos, log_i, t, pos;
	size_t line_i, col_i, col_line_i;
	size_t col_width, lines_per_col, lines_per_screen, chars_per_line;
	size_t col_offset, lines_needed, k, l;
	size_t entry_pos, entry_len, entry_overlen, entry_cnt;
	struct dlog_line *entry;
	u8 *map, *line, *ch;

	if (!columns)
		columns = 1;

	t = (width - col_padding * (columns - 1)) / columns;
	chars_per_line = t / dlog_font->width;
	col_width = chars_per_line * dlog_font->width;
	col_offset = col_width + col_padding;

	lines_per_col = height / dlog_font->height;
	lines_per_screen = lines_per_col * columns;

	log_i = 0;
	line_i = 0;
	col_i = 0;
	col_line_i = 0;
	log_pos = atomic_read(&buf->pos);

	/* safety checks for invalid divisors */
	if (!chars_per_line)
		goto skip;

	/* draw as long as screen-lines are left */
	while (line_i < lines_per_screen) {

		/* get next log-entry, combine all continuation entries */
		entry = NULL;
		entry_cnt = 0;
		entry_len = 0;
		entry_pos = log_pos;

		do {
			/* out-of-lines? (don't care for continuation) */
			if (log_i >= buf->height)
				break;

			++log_i;
			entry = &buf->lines[log_pos];
			if (!log_pos--)
				log_pos = buf->height - 1;

			entry_len += atomic_read(&entry->length);
			++entry_cnt;
		} while (atomic_read(&entry->cont));

		/* out of log-entries? */
		if (!entry_cnt)
			break;

		/* how many lines are needed for this entry? */
		lines_needed = DIV_ROUND_UP(entry_len, chars_per_line);
		/* how long's the last line? (all others are full-lines) */
		entry_overlen = entry_len % chars_per_line;

		/* draw all needed lines (bottom up) */
		entry = &buf->lines[entry_pos];
		pos = atomic_read(&entry->length);
		for (k = 0; k < lines_needed; ++k) {

			/* get next line; we know there's at least one free */
			line = kern_map;
			/* jump to current column */
			line += (columns - col_i - 1) * col_offset * cpp;
			/* jump to current line */
			line += (lines_per_col - col_line_i - 1) *
				dlog_font->height * stride;

			/* if in last line; draw blanks */
			t = chars_per_line - entry_overlen;
			if (!k && t > 0) {
				/* last line and @t blanks at the end */

				/* get char position */
				ch = line;
				ch += (chars_per_line - t) *
					dlog_font->width * cpp;

				/* clear @t chars at @ch */
				dlog__clear(ch,
					    t * dlog_font->width,
					    dlog_font->height,
					    stride,
					    cpp,
					    pixel_format);

				/* skip @t blanks */
				l = t;
			} else {
				/* not last line or no blanks; draw all */
				l = 0;
			}

			/* draw all remaining chars (right to left) */
			for ( ; l < chars_per_line; ++l) {
				/* get char position */
				ch = line;
				ch += (chars_per_line - l - 1) *
					dlog_font->width * cpp;

				/* get next char */
				while (!pos--) {
					/* There must be entry_len chars in the
					 * log-buffer to write. But parallel
					 * writers might have shortened the
					 * lines. Stop rendering as a redraw
					 * must have been scheduled already. */
					if (!--entry_cnt)
						goto skip;

					if (!entry_pos--)
						entry_pos = buf->height - 1;

					entry = &buf->lines[entry_pos];
					pos = atomic_read(&entry->length);
				}

				dlog__draw_char(entry->cells[pos],
						ch,
						stride,
						cpp,
						pixel_format);
			}

			/* if out-of-lines, go to next column */
			++col_line_i;
			if (col_line_i >= lines_per_col) {
				col_line_i = 0;
				++col_i;
			}

			/* we might be out-of-lines (or out-of-cols) */
			++line_i;
			if (line_i >= lines_per_screen)
				break;
		}
	}

skip:
	/*
	 * Any amount of data might have been drawn. Here we need to make sure
	 * to clear the remaining parts to black.
	 */

	/* clear remaining parts of partially used column */
	if (col_line_i > 0) {
		map = kern_map;
		map += (columns - col_i - 1) * col_offset * cpp;
		dlog__clear(map,
			    col_width,
			    (lines_per_col - col_line_i) * dlog_font->height,
			    stride,
			    cpp,
			    pixel_format);

		col_line_i = 0;
		++col_i;
	}

	/* clear remaining columns */
	if (col_i < columns) {
		dlog__clear(kern_map,
			    (columns - col_i) * col_offset,
			    height,
			    stride,
			    cpp,
			    pixel_format);
	}

	/* clear right margin */
	t = columns * col_offset - col_padding;
	if (t < width) {
		map = kern_map;
		map += t * cpp;
		dlog__clear(map, width - t, height, stride, cpp, pixel_format);
	}

	/* clear bottom margin */
	t = lines_per_col * dlog_font->height;
	if (t < height) {
		map = kern_map;
		map += t * stride;
		dlog__clear(map, width, height - t, stride, cpp, pixel_format);
	}

	/* draw column separators */
	for (k = 1; k < columns; ++k) {
		map = kern_map;
		map += (k * col_offset - col_padding) * cpp;
		dlog__clear(map,
			    col_padding,
			    height,
			    stride,
			    cpp,
			    pixel_format);
	}
}

/**
 * drm_log_draw: Render current kernel log into framebuffer
 * @kern_map: kernel-address of framebuffer region
 * @width: FB width in pixels
 * @height: FB height in pixels
 * @stride: FB stride in *bytes*
 * @cpp: FB chars-per-pixel (if 0, autodetected from @pixel_format)
 * @pixel_format: Pixel format of FB (four-CC)
 * @columns: Number of log-columns to render
 *
 * This is the main rendering entry-point for the kernel log. It renders the
 * current log-contents into the given memory area. It must be mapped for write
 * access from kernel-space and can be unmapped after this returns.
 *
 * @columns contains the maximum number of columns to use to render the log
 * contents. Usually you want to set this to 1, but higher values can be used
 * to increase the visible backlog on wide displays. Note that this value is
 * limited internally so columns will always have a suitable width.
 *
 * This function may be called at *any* times (even if drm-log is not
 * initialized or currently initializing). Multiple renderers can run in
 * parallel just fine.
 *
 * Note that currently only RGB formats are supported (but all of them). There
 * is currently no plan to support multi-plane YUV formats and alike.
 */
void drm_log_draw(void *kern_map,
		  size_t width,
		  size_t height,
		  size_t stride,
		  size_t cpp,
		  u32 pixel_format,
		  size_t columns)
{
	const struct drm_format_info *info;
	struct dlog_buf *buf;

	if (!kern_map || !width || !height || !stride)
		return;
	if (!pixel_format || !columns)
		return;

	if (!cpp) {
		info = drm_format_info(pixel_format);
		if (!info)
			return;
		cpp = info->cpp[0];
	}

	rcu_read_lock();
	buf = rcu_dereference(dlog_buf);
	if (buf) {
		/* make each column at least 80 chars wide */
		columns = min(columns, width / (dlog_font->width * 80));
		columns = columns ? : 1;

		dlog__draw(buf,
			   kern_map,
			   width,
			   height,
			   stride,
			   cpp,
			   pixel_format,
			   columns);
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL(drm_log_draw);

static void dlog_con_write(struct console *con,
			   const char *buf,
			   unsigned int size)
{
	drm_log_write(buf, size, false);
}

static struct console dlog_con_driver = {
	.name = "drmlog",
	.write = dlog_con_write,
	.flags = CON_PRINTBUFFER | CON_ENABLED,
};

static int drm_log_panic(struct notifier_block *this,
			 unsigned long event, void *ptr)
{
	struct dlog_framebuffer *fb;
	
	list_for_each_entry(fb, &dlog_fb, head) {
		if (!fb->kern_map)
			continue;
		drm_log_draw(fb->kern_map, fb->width, fb->height, fb->stride, fb->cpp, fb->pixel_format, fb->columns);
	}
	return 0;
}

struct notifier_block drm_log_panic_notifier = {
	.notifier_call = drm_log_panic,
};

void *drm_log_register_panic_fb(void)
{
	struct dlog_framebuffer *new = (struct dlog_framebuffer *) kzalloc(sizeof(*new), GFP_KERNEL);

	pr_info("Adding drmlog panic handler\n");
	if (new)
		list_add_tail(&new->head, &dlog_fb);
	return (void *) new;
}
EXPORT_SYMBOL(drm_log_register_panic_fb);

void drm_log_update_panic_fb(void *panic, void *kern_map,
		  size_t width,
		  size_t height,
		  size_t stride,
		  size_t cpp,
		  u32 pixel_format)
{
	struct dlog_framebuffer *fb = (struct dlog_framebuffer *) panic;

	pr_info("Updating drmlog panic handler\n");

	fb->kern_map = kern_map;
	fb->width = width;
	fb->height = height;
	fb->stride = stride;
	fb->cpp = cpp;
	fb->pixel_format = pixel_format;
	fb->columns = 1;
}
EXPORT_SYMBOL(drm_log_update_panic_fb);


/**
 * drm_log_init() - Initialize drm-log subsystem
 *
 * Initialize drm-log subsystem, allocate initial buffers and register a
 * console-driver. Errors are handled internally, so the caller can assume this
 * always succeeds.
 * You can safely call this helper multiple times. It's a no-op if the subsystem
 * is already initialized. However, no locking is done, so you usually have to
 * call this from within your module_init() callback.
 *
 * You must call drm_log_exit() to clean up any allocated memory.
 */
void drm_log_init(void)
{
	size_t def_width, def_height;

	if (dlog_font)
		return;

	/* use 800x600 as initial value and as global hint */
	def_width = 800;
	def_height = 600;

	/* prefer fonts with w/h multiple of 8 */
	dlog_font = get_default_font(def_width, def_height, 0x8080, 0x8080);
	if (!dlog_font) {
		DRM_ERROR("cannot get font-description, disabling drm-log");
		return;
	}

	/* provide initial buffer so we ca start logging */
	drm_log_ensure_size(def_width, def_height);

	register_console(&dlog_con_driver);

	/* register panic handler */
	atomic_notifier_chain_register(&panic_notifier_list, &drm_log_panic_notifier);
}

/**
 * drm_log_exit() - Shutdown drm-log subsystem
 *
 * Deinitialize the drm-log subsystem, unregister the console driver and free
 * allocated buffers.
 *
 * You must make sure there are no other users of this subsystem when calling
 * this! Usually, it's *not* safe to call this outside of your module_exit()
 * callback.
 *
 * You can call this helper multiple times. If the subsytem is already
 * deinitialized, this is a no-op.
 */
void drm_log_exit(void)
{
	struct dlog_buf *buf;

	if (!dlog_font)
		return;

	unregister_console(&dlog_con_driver);

	/* Make buffer-destruction safe against pending readers in case of
	 * buggy drivers and pending workqueues. */
	mutex_lock(&dlog_wlock);
	buf = rcu_dereference_protected(dlog_buf,
					lockdep_is_held(&dlog_wlock));
	if (buf) {
		rcu_assign_pointer(dlog_buf, NULL);
		synchronize_rcu();
		dlog_free_buf(buf);
	}
	dlog_font = NULL;
	mutex_unlock(&dlog_wlock);
}




