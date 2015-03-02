/*
 * Copyright (C) 2015 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * Controls the timings of the hardware's pixel valve.
 */

#include "drm_atomic_helper.h"
#include "drm_crtc_helper.h"
#include "linux/component.h"
#include "vc4_drv.h"
#include "vc4_regs.h"

#define CRTC_WRITE(offset, val) writel(val, vc4_crtc->regs + (offset))
#define CRTC_READ(offset) readl(vc4_crtc->regs + (offset))

#define CRTC_REG(reg) { reg, #reg }
static const struct {
	u32 reg;
	const char *name;
} crtc_regs[] = {
	CRTC_REG(PV_CONTROL),
	CRTC_REG(PV_V_CONTROL),
	CRTC_REG(PV_VSYNCD),
	CRTC_REG(PV_HORZA),
	CRTC_REG(PV_HORZB),
	CRTC_REG(PV_VERTA),
	CRTC_REG(PV_VERTB),
	CRTC_REG(PV_VERTA_EVEN),
	CRTC_REG(PV_VERTB_EVEN),
	CRTC_REG(PV_INTEN),
	CRTC_REG(PV_INTSTAT),
	CRTC_REG(PV_STAT),
};

static void
vc4_crtc_dump_regs(struct vc4_crtc *vc4_crtc)
{
	int i;

	rmb();
	for (i = 0; i < ARRAY_SIZE(crtc_regs); i++) {
		DRM_INFO("0x%04x (%s): 0x%08x\n",
			 crtc_regs[i].reg, crtc_regs[i].name,
			 CRTC_READ(crtc_regs[i].reg));
	}
}

static void vc4_crtc_destroy(struct drm_crtc *crtc)
{
	drm_crtc_cleanup(crtc);
}

static bool vc4_crtc_mode_fixup(struct drm_crtc *crtc,
				const struct drm_display_mode *mode,
				struct drm_display_mode *adjusted_mode)
{
	return true;
}

static void vc4_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
}

static void vc4_crtc_disable(struct drm_crtc *crtc)
{
}

static void vc4_crtc_enable(struct drm_crtc *crtc)
{
}

static int vc4_crtc_atomic_check(struct drm_crtc *crtc,
				 struct drm_crtc_state *state)
{
	return 0;
}

static void vc4_crtc_atomic_begin(struct drm_crtc *crtc)
{
}

static u32 vc4_get_fifo_full_level(u32 format)
{
	static const u32 fifo_len_bytes = 64;
	static const u32 hvs_latency_pix = 6;

	switch(format) {
	case PV_CONTROL_FORMAT_DSIV_16:
	case PV_CONTROL_FORMAT_DSIC_16:
		return fifo_len_bytes - 2 * hvs_latency_pix;
	case PV_CONTROL_FORMAT_DSIV_18:
		return fifo_len_bytes - 14;
	case PV_CONTROL_FORMAT_24:
	case PV_CONTROL_FORMAT_DSIV_24:
	default:
		return fifo_len_bytes - 3 * hvs_latency_pix;
	}
}

static void vc4_crtc_atomic_flush(struct drm_crtc *crtc)
{
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	struct drm_display_mode *mode = &crtc->hwmode;
	bool debug_dump_regs = false;

	if (debug_dump_regs) {
		DRM_INFO("CRTC regs before:\n");
		vc4_crtc_dump_regs(vc4_crtc);
	}

	if (0) {
		u32 verta = (VC4_SET_FIELD(mode->vtotal - mode->vdisplay,
					   PV_VERTA_VBP) |
			     VC4_SET_FIELD(mode->vsync_start, PV_VERTA_VSYNC));
		u32 vertb = (VC4_SET_FIELD(0, PV_VERTB_VFP),
			     VC4_SET_FIELD(mode->vdisplay, PV_VERTB_VACTIVE));
		u32 format = PV_CONTROL_FORMAT_24;

		CRTC_WRITE(PV_V_CONTROL,
			   CRTC_READ(PV_V_CONTROL) & ~PV_VCONTROL_VIDEN);

		do {
			/* XXX SLEEP */
		} while (CRTC_READ(PV_STAT) & (PV_STAT_RUNNING_MASK));

		CRTC_WRITE(PV_HORZA,
			   VC4_SET_FIELD(mode->htotal - mode->hdisplay,
					 PV_HORZA_HBP) |
			   VC4_SET_FIELD(mode->hsync_start, PV_HORZA_HSYNC));
		CRTC_WRITE(PV_HORZB,
			   VC4_SET_FIELD(0, PV_HORZB_HFP) |
			   VC4_SET_FIELD(mode->hdisplay, PV_HORZB_HACTIVE));

		CRTC_WRITE(PV_VERTA, verta);
		CRTC_WRITE(PV_VERTB, vertb);
		CRTC_WRITE(PV_VERTA_EVEN, verta);
		CRTC_WRITE(PV_VERTB_EVEN, vertb);

		CRTC_WRITE(PV_DSI_HACT, mode->htotal - mode->hdisplay);

		CRTC_WRITE(PV_CONTROL,
			   VC4_SET_FIELD(format, PV_CONTROL_FORMAT) |
			   VC4_SET_FIELD(vc4_get_fifo_full_level(format),
					 PV_CONTROL_FIFO_LEVEL) |
			   PV_CONTROL_CLR_AT_START |
			   PV_CONTROL_TRIGGER_UNDERFLOW |
			   PV_CONTROL_WAIT_HSTART |
			   PV_CONTROL_CLK_MUX_EN |
			   VC4_SET_FIELD(0, PV_CONTROL_CLK_SELECT) |
			   PV_CONTROL_FIFO_CLR |
			   PV_CONTROL_EN);

		CRTC_WRITE(PV_V_CONTROL,
			   PV_VCONTROL_CONTINUOUS |
			   PV_VCONTROL_VIDEN);
	}

	if (debug_dump_regs) {
		DRM_INFO("CRTC regs after:\n");
		vc4_crtc_dump_regs(vc4_crtc);
	}
}

static int vc4_crtc_cursor_set(struct drm_crtc *crtc,
			       struct drm_file *file_priv, uint32_t handle,
			       uint32_t width, uint32_t height)
{
	/* XXX */
	return 0;
}

static int vc4_crtc_cursor_move(struct drm_crtc *crtc, int x, int y)
{
	/* XXX */
	return 0;
}

static const struct drm_crtc_funcs vc4_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.destroy = vc4_crtc_destroy,
	.page_flip = drm_atomic_helper_page_flip,
	.set_property = NULL,
	.cursor_set = vc4_crtc_cursor_set,
	.cursor_move = vc4_crtc_cursor_move,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_crtc_helper_funcs vc4_crtc_helper_funcs = {
	.mode_fixup = vc4_crtc_mode_fixup,
	.mode_set_nofb = vc4_crtc_mode_set_nofb,
	.disable = vc4_crtc_disable,
	.enable = vc4_crtc_enable,
	.atomic_check = vc4_crtc_atomic_check,
	.atomic_begin = vc4_crtc_atomic_begin,
	.atomic_flush = vc4_crtc_atomic_flush,
};

static int vc4_crtc_bind(struct device *dev, struct device *master, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *drm = dev_get_drvdata(master);
	struct vc4_crtc *vc4_crtc;
	struct drm_crtc *crtc;
	struct drm_plane *plane;
	int ret;

	plane = vc4_plane_init(drm);
	if (IS_ERR(plane)) {
		dev_err(dev, "failed to construct plane\n");
		ret = PTR_ERR(plane);
		goto fail;
	}

	vc4_crtc = devm_kzalloc(dev, sizeof(*vc4_crtc), GFP_KERNEL);
	if (!vc4_crtc)
		return -ENOMEM;
	crtc = &vc4_crtc->base;

	vc4_crtc->regs = vc4_ioremap_regs(pdev);
	if (IS_ERR(vc4_crtc->regs))
		return PTR_ERR(vc4_crtc->regs);

	drm_crtc_init_with_planes(drm, crtc, plane, NULL, &vc4_crtc_funcs);
	drm_crtc_helper_add(crtc, &vc4_crtc_helper_funcs);
	plane->crtc = crtc;

	platform_set_drvdata(pdev, vc4_crtc);

	return 0;

fail:
	return ret;
}

static void vc4_crtc_unbind(struct device *dev, struct device *master,
			    void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct vc4_crtc *vc4_crtc = dev_get_drvdata(dev);

	vc4_crtc_destroy(&vc4_crtc->base);

	platform_set_drvdata(pdev, NULL);
}

static const struct component_ops vc4_crtc_ops = {
	.bind   = vc4_crtc_bind,
	.unbind = vc4_crtc_unbind,
};

static int vc4_crtc_dev_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &vc4_crtc_ops);
}

static int vc4_crtc_dev_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &vc4_crtc_ops);
	return 0;
}

static const struct of_device_id vc4_crtc_dt_match[] = {
	{ .compatible = "brcm,vc4-pixelvalve" },
	{}
};

static struct platform_driver vc4_crtc_driver = {
	.probe = vc4_crtc_dev_probe,
	.remove = vc4_crtc_dev_remove,
	.driver = {
		.name = "vc4_crtc",
		.of_match_table = vc4_crtc_dt_match,
	},
};

void __init vc4_crtc_register(void)
{
	platform_driver_register(&vc4_crtc_driver);
}

void __exit vc4_crtc_unregister(void)
{
	platform_driver_unregister(&vc4_crtc_driver);
}
