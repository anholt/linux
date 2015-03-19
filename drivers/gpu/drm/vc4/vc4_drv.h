/*
 * Copyright (C) 2015 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "drmP.h"
#include "drm_gem_cma_helper.h"

struct vc4_dev {
	struct drm_device *dev;

	struct vc4_hdmi *hdmi;
	struct vc4_hvs *hvs;
	struct vc4_v3d *v3d;
};

static inline struct vc4_dev *
to_vc4_dev(struct drm_device *dev)
{
	return (struct vc4_dev *)dev->dev_private;
}

struct vc4_bo {
	struct drm_gem_cma_object base;
};

static inline struct vc4_bo *
to_vc4_bo(struct drm_gem_object *bo)
{
	return (struct vc4_bo *)bo;
}

struct vc4_v3d {
	struct platform_device *pdev;
	void __iomem *regs;
};

struct vc4_hvs {
	struct platform_device *pdev;
	void __iomem *regs;
	void __iomem *dlist;
};

struct vc4_crtc {
	struct drm_crtc base;
	void __iomem *regs;
};

static inline struct vc4_crtc *
to_vc4_crtc(struct drm_crtc *crtc)
{
	return (struct vc4_crtc *)crtc;
}

struct vc4_plane {
	struct drm_plane base;
};

static inline struct vc4_plane *
to_vc4_plane(struct drm_plane *plane)
{
	return (struct vc4_plane *)plane;
}

#define V3D_READ(offset) readl(vc4->v3d->regs + offset)
#define V3D_WRITE(offset, val) writel(val, vc4->v3d->regs + offset)
#define HVS_READ(offset) readl(vc4->hvs->regs + offset)
#define HVS_WRITE(offset, val) writel(val, vc4->hvs->regs + offset)
#define HDMI_READ(offset) readl(vc4->hdmi->regs + offset)
#define HDMI_WRITE(offset, val) writel(val, vc4->hdmi->regs + offset)

struct exec_info {
	/* This is the array of BOs that were looked up at the start of exec.
	 * Command validation will use indices into this array.
	 */
	struct drm_gem_cma_object **bo;
	uint32_t bo_count;

	/* Current indices into @bo loaded by the non-hardware packet
	 * that passes in indices.  This can be used even without
	 * checking that we've seen one of those packets, because
	 * @bo_count is always >= 1, and this struct is initialized to
	 * 0.
	 */
	uint32_t bo_index[2];
	uint32_t max_width, max_height;

	/**
	 * This is the BO where we store the validated command lists
	 * and shader records.
	 */
	struct drm_gem_cma_object *exec_bo;

	/**
	 * This tracks the per-shader-record state (packet 64) that
	 * determines the length of the shader record and the offset
	 * it's expected to be found at.  It gets read in from the
	 * command lists.
	 */
	struct vc4_shader_state {
		uint8_t packet;
		uint32_t addr;
	} *shader_state;

	/** How many shader states the user declared they were using. */
	uint32_t shader_state_size;
	/** How many shader state records the validator has seen. */
	uint32_t shader_state_count;

	/**
	 * Computed addresses pointing into exec_bo where we start the
	 * bin thread (ct0) and render thread (ct1).
	 */
	uint32_t ct0ca, ct0ea;
	uint32_t ct1ca, ct1ea;
	uint32_t shader_paddr;
};

/* vc4_bo.c */
void vc4_free_object(struct drm_gem_object *gem_obj);
struct vc4_bo *vc4_bo_create(struct drm_device *dev, size_t size);
int vc4_dumb_create(struct drm_file *file_priv,
		    struct drm_device *dev,
		    struct drm_mode_create_dumb *args);
struct dma_buf *vc4_prime_export(struct drm_device *dev,
				 struct drm_gem_object *obj, int flags);

/* vc4_crtc.c */
void vc4_crtc_register(void);
void vc4_crtc_unregister(void);

/* vc4_debugfs.c */
int vc4_debugfs_init(struct drm_minor *minor);
void vc4_debugfs_cleanup(struct drm_minor *minor);

/* vc4_drv.c */
void __iomem *vc4_ioremap_regs(struct platform_device *dev);

/* vc4_gem.c */
int vc4_submit_cl_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv);

/* vc4_hdmi.c */
void vc4_hdmi_register(void);
void vc4_hdmi_unregister(void);
struct drm_encoder *vc4_hdmi_encoder_init(struct drm_device *dev);
struct drm_connector *vc4_hdmi_connector_init(struct drm_device *dev,
					      struct drm_encoder *encoder);
int vc4_hdmi_debugfs_regs(struct seq_file *m, void *unused);

/* vc4_hvs.c */
void vc4_hvs_register(void);
void vc4_hvs_unregister(void);
void vc4_hvs_dump_state(struct drm_device *dev);
int vc4_hvs_debugfs_regs(struct seq_file *m, void *unused);

/* vc4_kms.c */
int vc4_kms_load(struct drm_device *dev);

/* vc4_plane.c */
struct drm_plane *vc4_plane_init(struct drm_device *dev);

/* vc4_v3d.c */
void vc4_v3d_register(void);
void vc4_v3d_unregister(void);
int vc4_v3d_debugfs_ident(struct seq_file *m, void *unused);
int vc4_v3d_debugfs_regs(struct seq_file *m, void *unused);
int vc4_v3d_set_power(bool on);

/* vc4_validate.c */
int
vc4_validate_cl(struct drm_device *dev,
		void *validated,
		void *unvalidated,
		uint32_t len,
		bool is_bin,
		struct exec_info *exec);

int
vc4_validate_shader_recs(struct drm_device *dev,
			 void *validated,
			 void *unvalidated,
			 uint32_t len,
			 struct exec_info *exec);
