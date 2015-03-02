/*
 * Copyright (C) 2015 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * Controls an individual layer of pixels being scanned out by the HVS.
 */

#include "vc4_drv.h"
#include "vc4_regs.h"
#include "drm_atomic_helper.h"
#include "drm_fb_cma_helper.h"
#include "drm_plane_helper.h"

static bool plane_enabled(struct drm_plane_state *state)
{
	return state->fb && state->crtc;
}

static int vc4_plane_prepare_fb(struct drm_plane *plane,
				struct drm_framebuffer *fb)
{
	return 0;
}

static void vc4_plane_cleanup_fb(struct drm_plane *plane,
				 struct drm_framebuffer *fb)
{
}

static int vc4_plane_atomic_check(struct drm_plane *plane,
				  struct drm_plane_state *state)
{
	return 0;
}

static void vc4_plane_atomic_update(struct drm_plane *plane,
				    struct drm_plane_state *old_state)
{
	struct drm_device *dev = plane->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_plane_state *state = plane->state;
	uint32_t __iomem *dlist = ((uint32_t __iomem *)vc4->hvs->dlist +
				   HVS_BOOTLOADER_DLIST_END);
	uint32_t dlist_count = 0;
	bool debug_dump_regs = false;

	if (debug_dump_regs) {
		DRM_INFO("HVS state before:\n");
		vc4_hvs_dump_state(dev);
	}

	if (plane_enabled(state) && plane->fb) {
		struct drm_framebuffer *fb = state->fb;
		struct drm_gem_cma_object *bo = drm_fb_cma_get_gem_obj(fb, 0);
		uint32_t __iomem *ctl0 = &dlist[dlist_count];

		dlist[dlist_count++] =
			(SCALER_CTL0_VALID |
			 (HVS_PIXEL_ORDER_ABGR << SCALER_CTL0_ORDER_SHIFT) |
			 (HVS_PIXEL_FORMAT_RGB8888 << SCALER_CTL0_PIXEL_FORMAT_SHIFT) |
			 SCALER_CTL0_UNITY);

		dlist[dlist_count++] = VC4_SET_FIELD(0xff, SCALER_POS0_FIXED_ALPHA);

		dlist[dlist_count++] =
			((1 << SCALER_POS2_ALPHA_MODE_SHIFT) |
			 (state->crtc_h << SCALER_POS2_HEIGHT_SHIFT) |
			 (state->crtc_w << SCALER_POS2_WIDTH_SHIFT));

		dlist[dlist_count++] = 0xc0c0c0c0;
		dlist[dlist_count++] = bo->paddr + fb->offsets[0];
		dlist[dlist_count++] = 0xc0c0c0c0;
		dlist[dlist_count++] = fb->pitches[0] & SCALER_SRC_PITCH_MASK;
		*ctl0 |= VC4_SET_FIELD(dlist_count, SCALER_CTL0_SIZE);
	}
	dlist[dlist_count++] = SCALER_CTL0_END;
	wmb();

	HVS_WRITE(SCALER_DISPLIST1, HVS_BOOTLOADER_DLIST_END);

	if (debug_dump_regs) {
		DRM_INFO("HVS state after:\n");
		vc4_hvs_dump_state(dev);
	}

	WARN_ON((void *)&dlist[dlist_count] - vc4->hvs->dlist >
		SCALER_DLIST_SIZE);
}

static const struct drm_plane_helper_funcs vc4_plane_helper_funcs = {
	.prepare_fb = vc4_plane_prepare_fb,
	.cleanup_fb = vc4_plane_cleanup_fb,
	.atomic_check = vc4_plane_atomic_check,
	.atomic_update = vc4_plane_atomic_update,
};

static void vc4_plane_destroy(struct drm_plane *plane)
{
	drm_plane_helper_disable(plane);
	drm_plane_cleanup(plane);
}

static const struct drm_plane_funcs vc4_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = vc4_plane_destroy,
	.set_property = NULL,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

struct drm_plane *vc4_plane_init(struct drm_device *dev)
{
	struct drm_plane *plane = NULL;
	struct vc4_plane *vc4_plane;
	static const uint32_t formats[] = {
		DRM_FORMAT_ARGB8888,
		DRM_FORMAT_XRGB8888,
	};
	int ret = 0;

	vc4_plane = devm_kzalloc(dev->dev, sizeof(*vc4_plane),
				 GFP_KERNEL);
	if (!vc4_plane) {
		ret = -ENOMEM;
		goto fail;
	}

	plane = &vc4_plane->base;
	ret = drm_universal_plane_init(dev, plane, 0xff,
				       &vc4_plane_funcs,
				       formats, ARRAY_SIZE(formats),
				       DRM_PLANE_TYPE_PRIMARY);

	drm_plane_helper_add(plane, &vc4_plane_helper_funcs);

	return plane;
fail:
	if (plane)
		vc4_plane_destroy(plane);

	return ERR_PTR(ret);
}
