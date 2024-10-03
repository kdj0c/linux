/*
 * Copyright © 2015 Intel Corporation
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
 *    Rafael Antognolli <rafael.antognolli@intel.com>
 *
 */

#include <linux/module.h>

#include "drm_internal.h"

#if defined(CONFIG_DRM_FBDEV_EMULATION)
bool drm_fbdev_emulation = true;
module_param_named(fbdev_emulation, drm_fbdev_emulation, bool, 0600);
MODULE_PARM_DESC(fbdev_emulation,
		 "Enable legacy fbdev emulation [default=true]");

int drm_fbdev_overalloc = CONFIG_DRM_FBDEV_OVERALLOC;
module_param(drm_fbdev_overalloc, int, 0444);
MODULE_PARM_DESC(drm_fbdev_overalloc,
		 "Overallocation of the fbdev buffer (%) [default="
		 __MODULE_STRING(CONFIG_DRM_FBDEV_OVERALLOC) "]");

/*
 * In order to keep user-space compatibility, we want in certain use-cases
 * to keep leaking the fbdev physical address to the user-space program
 * handling the fbdev buffer.
 *
 * This is a bad habit, essentially kept to support closed-source OpenGL
 * drivers that should really be moved into open-source upstream projects
 * instead of using legacy physical addresses in user space to communicate
 * with other out-of-tree kernel modules.
 *
 * This module_param *should* be removed as soon as possible and be
 * considered as a broken and legacy behaviour from a modern fbdev device.
 */
bool drm_leak_fbdev_smem;
#if IS_ENABLED(CONFIG_DRM_FBDEV_LEAK_PHYS_SMEM)
module_param_unsafe(drm_leak_fbdev_smem, bool, 0600);
MODULE_PARM_DESC(drm_leak_fbdev_smem,
		 "Allow unsafe leaking fbdev physical smem address [default=false]");
#endif
#endif

MODULE_AUTHOR("David Airlie, Jesse Barnes");
MODULE_DESCRIPTION("DRM KMS helper");
MODULE_LICENSE("GPL and additional rights");
