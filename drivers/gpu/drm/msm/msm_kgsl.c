#include <linux/file.h>
#include <linux/sync_file.h>
#include <linux/uaccess.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_syncobj.h>
int drm_gem_prime_fd_to_handle(struct drm_device *dev,
				      struct drm_file *file_priv, int prime_fd,
				      uint32_t *handle);

#include "msm_drv.h"
#include "msm_gpu.h"
#include "msm_gem.h"
#include "msm_gpu_trace.h"
#include "msm_kgsl.h"

#define KGSL_MEMSTORE_SIZE SZ_32K

static const char* kgsl_fence_get_driver_name(struct dma_fence *fence)
{
	return "kgsl";
}

static const char* kgsl_fence_get_timeline_name(struct dma_fence *fence)
{
	return "kgsl";
}

static const struct dma_fence_ops kgsl_fence_ops = {
	.get_driver_name = kgsl_fence_get_driver_name,
	.get_timeline_name = kgsl_fence_get_timeline_name,
};

struct msm_gem_submit *submit_create(struct drm_device *dev,
		struct msm_gpu *gpu,
		struct msm_gpu_submitqueue *queue, uint32_t nr_bos,
		uint32_t nr_cmds);

int wait_fence(struct msm_gpu_submitqueue *queue, uint32_t fence_id, signed long timeout);

signed long drm_syncobj_array_wait_timeout(struct drm_syncobj **syncobjs,
						  void __user *user_points,
						  uint32_t count,
						  uint32_t flags,
						  signed long timeout,
						  uint32_t *idx);

uint64_t a7xx_gpu_shadow_iova(struct msm_gpu *gpu);
struct drm_gem_object *a7xx_gpu_shadow_bo(struct msm_gpu *gpu);


struct kgsl_bind_range {
	struct drm_gem_object *obj;
	s64 src_offset;
	u64 dst_offset;
	u64 length;
};

struct kgsl_bind_cmd {
	struct drm_gem_object *vbo;
	u64 num_ranges;
	struct kgsl_bind_range range[];
};

struct kgsl_bind_work {
	struct dma_fence fence;
	spinlock_t fence_lock;
	struct work_struct work;
	struct msm_gpu_submitqueue *queue;
	u32 wait_timestamp, timestamp;
	u32 num_cmds;
	struct kgsl_bind_cmd cmd[];
	/* struct kgsl_bind_range range[]; */
};

void kgsl_bind_worker(struct work_struct *_work)
{
	struct kgsl_bind_work *work = container_of(_work, struct kgsl_bind_work, work);
	struct msm_gpu_submitqueue *queue = work->queue;
	struct kgsl_bind_cmd *op = &work->cmd[0];
	struct kgsl_bind_range *r;
	u32 i, j;
	int ret;

	//printk("kgsl_bind_worker: %d %d %d %d %lx\n", work->wait_timestamp, work->timestamp, queue->timestamp, queue->timestamp_retired, queue);

	do {
		ret = wait_event_interruptible_timeout(queue->waitqueue, queue->timestamp_retired >= work->wait_timestamp, HZ * 3);
	} while (ret < 0);
	WARN_ON(queue->timestamp_retired < work->wait_timestamp);
	//printk("kgsl_bind_worker2: %d %d\n", queue->timestamp_retired, ret);

	for (i = 0; i < work->num_cmds; i++) {
		for (j = 0; j < op->num_ranges; j++) {
			r = &op->range[j];
			if (r->src_offset >= 0)
				msm_gem_vbo_bind(op->vbo, r->obj, r->src_offset, r->dst_offset, r->length);
			else
				msm_gem_vbo_unbind(op->vbo, r->obj, r->dst_offset, r->length);

			drm_gem_object_put(r->obj);
		}
		drm_gem_object_put(op->vbo);
		op = (void*) &op->range[op->num_ranges];
	}

	msm_submitqueue_retire_timestamp(queue, work->timestamp);
	msm_submitqueue_put(queue);

	dma_fence_signal(&work->fence);
	dma_fence_put(&work->fence);
}

int kgsl_work_add_bind(struct drm_file *file_priv, void **work_ptr, void *work_end, struct kgsl_gpu_aux_command_generic cmd)
{
	struct kgsl_bind_cmd *op = *work_ptr;
	struct kgsl_gpu_aux_command_bind bind;
	struct kgsl_gpumem_bind_range range;
	struct drm_gem_object *vbo;
	u32 i = 0;
	int ret = -EINVAL;

	if (cmd.size != sizeof(bind))
		return -EINVAL;

	if (copy_from_user(&bind, (void __user*)cmd.priv, sizeof(bind)))
		return -EINVAL;

	if (bind.rangesize != sizeof(range))
		return -EINVAL;

	vbo = drm_gem_object_lookup(file_priv, bind.target);
	if (!vbo)
		return -EINVAL;

	if (!msm_gem_is_vbo(vbo))
		goto fail;

	op->vbo = vbo;
	op->num_ranges = bind.numranges;

	if ((void*)&op->range[op->num_ranges] > work_end) {
		printk("not enough work size\n");
		goto fail;
	}

	for (i = 0; i < bind.numranges; i++) {
		struct drm_gem_object *obj = NULL;
		s64 src_offset;

		if (copy_from_user(&range, (void __user*)bind.rangeslist + i * sizeof(range), sizeof(range)))
			goto fail;

		src_offset = range.child_offset;

		if (range.child_id) {
			obj = drm_gem_object_lookup(file_priv, range.child_id);
			if (!obj)
				goto fail;

			if (msm_gem_is_vbo(obj)) {
				drm_gem_object_put(obj);
				goto fail;
			}
		} else if (range.op == KGSL_GPUMEM_RANGE_OP_BIND)
			goto fail;

		if (vbo->size < range.target_offset + range.length) {  // XXX overflow on addition case
			drm_gem_object_put(obj);
			goto fail;
		}

		if (range.op == KGSL_GPUMEM_RANGE_OP_BIND) {
			if (obj->size < src_offset + range.length) { // XXX overflow on addition case
				drm_gem_object_put(obj);
				goto fail;
			}
		} else {
			src_offset = -1ll; /* use negative src_offset to indicate unbind op */
		}

		op->range[i] = (struct kgsl_bind_range) {
			obj,
			src_offset,
			range.target_offset,
			range.length,
		};
	}
	*work_ptr = (void*) &op->range[op->num_ranges];

	ret = 0;
fail:
	while (i-->0)
		drm_gem_object_put(op->range[i].obj);
	drm_gem_object_put(vbo);
	return ret;
}

int kgsl_work_add_timeline(struct drm_file *file_priv, struct dma_fence *fence, struct kgsl_gpu_aux_command_generic cmd)
{
	struct kgsl_gpu_aux_command_timeline timeline;
	struct kgsl_timeline_val val;
	u32 i;

	if (cmd.size != sizeof(timeline))
		return -EINVAL;

	if (copy_from_user(&timeline, (void __user*)cmd.priv, sizeof(timeline)))
		return -EINVAL;

	if (timeline.timelines_size != sizeof(val))
		return -EINVAL;

	for (i = 0; i < timeline.count; i++) {
		struct drm_syncobj *syncobj;
		struct dma_fence_chain *chain;

		if (copy_from_user(&val, (void __user*)timeline.timelines + i * sizeof(val), sizeof(val)))
			goto fail;

		syncobj = drm_syncobj_find(file_priv, val.timeline);
		if (!syncobj)
			goto fail;

		chain = dma_fence_chain_alloc();
		if (!chain) {
			drm_syncobj_put(syncobj);
			goto fail;
		}

		drm_syncobj_add_point(syncobj, chain, fence, val.seqno);
		drm_syncobj_put(syncobj);
	}

	return 0;
fail:
	printk("cleanup installed fences on fail\n");
	return -ENOENT;
}

#define KGSL_MEMSTORE_TOKEN_ADDRESS 0xfffff000u
int kgsl_mmap(struct drm_file *file_priv, struct drm_device *dev, struct vm_area_struct *vma)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_gpu *gpu = priv->gpu;
	struct drm_gem_object *obj;
	int ret;

	if ((vma->vm_pgoff << PAGE_SHIFT) == KGSL_MEMSTORE_TOKEN_ADDRESS)
		return drm_gem_mmap_obj(a7xx_gpu_shadow_bo(gpu), KGSL_MEMSTORE_SIZE, vma);

	obj = drm_gem_object_lookup(file_priv, vma->vm_pgoff);
	if (!obj)
		return -ENOENT;

	ret = drm_gem_mmap_obj(obj, obj->size, vma);
	drm_gem_object_put(obj);
	return ret;
}

static int kgsl_alloc(struct drm_file *file_priv, struct kgsl_gpuobj_alloc *alloc) {
	struct drm_device *dev = file_priv->minor->dev;
	struct msm_context *ctx = file_priv->driver_priv;
	struct drm_gpuvm *vm = msm_context_vm(dev, ctx);
	struct drm_gem_object *obj;
	u32 msm_flags = MSM_BO_WC;
	unsigned size;

	union {
		struct {
			u64 bits_0_2 : 3;
			u64 secure : 1;
			u64 bits_4_7 : 4;
			u64 mem_type : 8;
			u64 mem_align : 8;
			u64 gpu_read_only : 1;
			u64 gpu_write_only : 1;
			u64 cache_mode : 2;
			u64 use_cpu_map : 1;
			u64 sparse_phys : 1;
			u64 sparse_virt : 1;
			u64 iocoherent : 1;
			u64 force_32bit : 1;
			u64 guard_page : 1;
			u64 vbo : 1;
			u64 vbo_no_map_zero : 1;
		};
		u64 bits;
	} flags;

	flags.bits = alloc->flags;

	// support only mem_type, mem_align, readonly, cache_mode, iocoherent, vbo
	if (flags.bits & ~0xc8dffff00ull) {
		DRM_DEV_ERROR(dev->dev, "unexpected alloc flags: %llx\n", alloc->flags);
		return -EINVAL;
	}

	if (flags.mem_align < 12) {
		flags.mem_align = 12;
	} else if (flags.mem_align > 20) {
		dev_warn(dev->dev, "alignment too high, clamping: %d\n", flags.mem_align);
		flags.mem_align = 20;
	}

	//if (flags.cache_mode == KGSL_CACHEMODE_WRITETHROUGH || flags.cache_mode == KGSL_CACHEMODE_WRITEBACK)
	//	msm_flags = flags.iocoherent ? MSM_BO_CACHED_COHERENT : MSM_BO_CACHED;
	msm_flags |= flags.mem_align << 20;

	if (msm_flags == MSM_BO_CACHED && 0) {
		dev_warn(dev->dev, "forced iocoherent because cache sync isn't implemented\n");
		msm_flags = MSM_BO_CACHED_COHERENT;
	}

	if (flags.gpu_read_only)
		msm_flags |= MSM_BO_GPU_READONLY;
	if (flags.vbo_no_map_zero)
		msm_flags |= MSM_VBO_NO_MAP_ZERO;

	// XXX overflow
	size = (alloc->size + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1); /* 4k aligned size */

	if (flags.vbo)
		obj = msm_gem_vbo_new(dev, size, msm_flags, vm);
	else
		obj = msm_gem_new(dev, size, msm_flags);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	int ret = drm_gem_handle_create(file_priv, obj, &alloc->id);
	drm_gem_object_put(obj);
	if (ret)
		return ret;

	alloc->va_len = size;
	alloc->mmapsize = size;

	return 0;
}

long kgsl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct drm_file *file_priv = file->private_data;
	struct drm_device *dev = file_priv->minor->dev;
	struct msm_context *ctx = file_priv->driver_priv;
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_gpu *gpu = priv->gpu;
	struct msm_gpu_submitqueue *queue;
	struct drm_gem_object *obj;
	struct drm_gpuvm *vm = msm_context_vm(dev, ctx);
	unsigned long flags;
	int ret;

	char __user *argp = (char __user *)arg;

	switch (cmd) {
	case IOCTL_KGSL_DRAWCTXT_CREATE: {
		struct kgsl_drawctxt_create create;

		if (copy_from_user(&create, argp, sizeof(create)))
			return -EFAULT;

		// XXX: look at create.flags
		ret = msm_submitqueue_create(dev, ctx, 0, 0, &create.drawctxt_id);

		queue = msm_submitqueue_get(ctx, create.drawctxt_id);
		queue->kgsl_flags = create.flags;

		if (copy_to_user(argp, &create, sizeof(create)))
			WARN_ON(1);
		return 0;
	}
	case IOCTL_KGSL_DRAWCTXT_DESTROY: {
		struct kgsl_drawctxt_destroy destroy;

		if (copy_from_user(&destroy, argp, sizeof(destroy)))
			return -EFAULT;

		return msm_submitqueue_remove(ctx, destroy.drawctxt_id);
	}
	case IOCTL_KGSL_DEVICE_GETPROPERTY: {
		struct kgsl_device_getproperty get_prop;

		if (copy_from_user(&get_prop, argp, sizeof(get_prop)))
			return -EFAULT;

		union {
			u32 val;
			u64 gmem_vaddr;
			struct kgsl_ucode_version ucode;
			struct kgsl_qtimer_prop qtimer;
			struct kgsl_qdss_stm_prop qdss;
			struct kgsl_shadowprop shadow;
			struct kgsl_gpu_model model;
		} prop = {};

		switch (get_prop.type) {
		case KGSL_PROP_DEVICE_INFO: {
			static const struct kgsl_devinfo prop = {
				.device_id = 1, /* KGSL_DEVICE_3D0 + 1 ? */
				.chip_id = 0x43051401,
				.mmu_enabled = 1,
				.gmem_gpubaseaddr = 0,
				.gpu_id = 750,
				.gmem_sizebytes = SZ_1M * 3,
			};
			if (get_prop.sizebytes != sizeof(prop))
				return -EINVAL;
			if (copy_to_user(get_prop.value, &prop, sizeof(prop)))
				return -EFAULT;
		} return 0;
		case KGSL_PROP_DEVICE_SHADOW: {
			prop.shadow.gpuaddr = KGSL_MEMSTORE_TOKEN_ADDRESS; //a7xx_gpu_shadow_iova(gpu);
			prop.shadow.size = KGSL_MEMSTORE_SIZE;
			prop.shadow.flags = KGSL_FLAGS_INITIALIZED | KGSL_FLAGS_PER_CONTEXT_TIMESTAMPS;
		} break;
		case KGSL_PROP_UCHE_GMEM_VADDR: {
			prop.gmem_vaddr = 0;
		} break;
		case KGSL_PROP_UCODE_VERSION: {
			prop.ucode.pfp = 0x01740159; // XXX read from FW
			prop.ucode.pm4 = 0;
		} break;
		case KGSL_PROP_HIGHEST_BANK_BIT: { prop.val = 16; } break;
		case KGSL_PROP_DEVICE_BITNESS: { prop.val = 48; } break;
		case KGSL_PROP_DEVICE_QDSS_STM: {
		} break;
		case KGSL_PROP_MIN_ACCESS_LENGTH: { prop.val = 32; } break;
		case KGSL_PROP_UBWC_MODE: { prop.val = 4; } break;
		case KGSL_PROP_DEVICE_QTIMER: {
			prop.qtimer.gpuaddr = 0;
			prop.qtimer.size = 0;
		} break;
		case KGSL_PROP_SECURE_BUFFER_ALIGNMENT: { prop.val = SZ_4K; } break;
		case KGSL_PROP_SECURE_CTXT_SUPPORT: { prop.val = 0; } break;
		case KGSL_PROP_SPEED_BIN: { prop.val = 0; } break;
		case KGSL_PROP_GAMING_BIN: { prop.val = 0; } break;
		case KGSL_PROP_GPU_MODEL: { strcpy(prop.model.gpu_model, "Adreno750v2"); } break;
		case KGSL_PROP_VK_DEVICE_ID: { prop.val = 0x43051401; } break;
		case KGSL_PROP_IS_LPAC_ENABLED: { prop.val = 0; } break;
		case KGSL_PROP_IS_RAYTRACING_ENABLED: { prop.val = 1; } break;
		case KGSL_PROP_IS_FASTBLEND_ENABLED: { prop.val = 1; } break;
		case KGSL_PROP_GPU_RESET_STAT: {
			prop.val = to_msm_vm(ctx->vm)->faults < to_msm_vm(ctx->vm)->kgsl_last_queried_faults ?
				KGSL_CTX_STAT_NO_ERROR : KGSL_CTX_STAT_INNOCENT_CONTEXT_RESET_EXT;
			to_msm_vm(ctx->vm)->kgsl_last_queried_faults = to_msm_vm(ctx->vm)->faults;
		} break;
		default:
			DRM_DEV_ERROR(dev->dev, "TODO: get prop %d\n", get_prop.type);
			return -ENODEV;
		}

		if (get_prop.sizebytes > sizeof(prop))
				return -EINVAL;
		if (copy_to_user(get_prop.value, &prop, get_prop.sizebytes))
				return -EFAULT;
		return 0;
	}
	case IOCTL_KGSL_TIMESTAMP_EVENT: {
		struct kgsl_timestamp_event event;
		struct kgsl_fence *fence;
		struct sync_file *sync_file;
		int out_fence_fd;

		if (copy_from_user(&event, argp, sizeof(event)))
			return -EFAULT;

		if (event.type != KGSL_TIMESTAMP_EVENT_FENCE || event.len != sizeof(int))
			return -EINVAL;

		queue = msm_submitqueue_get(ctx, event.context_id);
		if (!queue)
			return -EINVAL;

#if 0
		/* note: userspace expects 0 for already signaled fence (or not?) */
		if (event.timestamp <= queue->timestamp_retired) {
			msm_submitqueue_put(queue);
			return 0;
		}
#endif

		fence = kzalloc(sizeof(*fence), GFP_KERNEL);
		if (!fence)
			return -ENOMEM;

		dma_fence_init(&fence->base, &kgsl_fence_ops, &fence->lock,
			dma_fence_context_alloc(1), 0);

		out_fence_fd = get_unused_fd_flags(O_CLOEXEC);
		if (out_fence_fd < 0) {
			kfree(fence);
			return out_fence_fd;
		}

		sync_file = sync_file_create(&fence->base);
		if (!sync_file) {
			put_unused_fd(out_fence_fd);
			kfree(fence);
			return -ENOMEM;
		}

		fd_install(out_fence_fd, sync_file->file);

		fence->timestamp = event.timestamp;

		spin_lock_irqsave(&queue->fence_lock, flags);
		if (event.timestamp <= queue->timestamp_retired) {
			/* timestamp was retired while creating the fence: */
			dma_fence_signal(&fence->base);
			dma_fence_put(&fence->base);
		} else {
			list_add_tail(&fence->node, &queue->fences);
		}
		spin_unlock_irqrestore(&queue->fence_lock, flags);

		if (copy_to_user(event.priv, &out_fence_fd, sizeof(out_fence_fd))) {
			WARN_ON(1); // XXX close fd
			return -EFAULT;
		}

		//printk("event.timestamp=%d %d %d\n", event.timestamp, queue->timestamp_retired, out_fence_fd);

		msm_submitqueue_put(queue);
		return 0;
	}

case IOCTL_KGSL_GPUOBJ_ALLOC: {
	struct kgsl_gpuobj_alloc alloc;

	if (copy_from_user(&alloc, argp, sizeof(alloc)))
		return -EFAULT;

	if (alloc.size >> 32)
		return -EINVAL;

	kgsl_alloc(file_priv, &alloc);

	if (copy_to_user(argp, &alloc, sizeof(alloc)))
		WARN_ON(1);

	return 0;
}

case IOCTL_KGSL_GPUOBJ_IMPORT: {
	struct kgsl_gpuobj_import import;
	struct kgsl_gpuobj_import_dma_buf dma_buf;

	if (copy_from_user(&import, argp, sizeof(import)))
		return -EFAULT;

	if (import.type != KGSL_USER_MEM_TYPE_DMABUF)
		return -ENOENT;

	if (copy_from_user(&dma_buf, (void __user*)import.priv, sizeof(dma_buf)))
		return -EFAULT;

	ret = drm_gem_prime_fd_to_handle(dev, file_priv, dma_buf.fd, &import.id);
	if (ret)
		return ret;

	if (copy_to_user(argp, &import, sizeof(import)))
		WARN_ON(1);
	return 0;
}

case IOCTL_KGSL_GPUOBJ_INFO: {
	struct kgsl_gpuobj_info info;
	int ret;

	if (copy_from_user(&info, argp, sizeof(info)))
		return -EFAULT;

	obj = drm_gem_object_lookup(file_priv, info.id);
	if (!obj)
		return -EINVAL;

	ret = drm_gem_create_mmap_offset(obj);
	if (ret != 0) {
		DRM_DEV_ERROR(dev->dev, "failed to create mmap offset: %d\n", ret);
		drm_gem_object_put(obj);
		return -EINVAL;
	}

	msm_gem_get_and_pin_iova(obj, vm, &info.gpuaddr);
	info.flags = 0; // XXX
	info.size = obj->size;
	info.va_len = obj->size;
	info.va_addr = drm_vma_node_offset_addr(&obj->vma_node);

	drm_gem_object_put(obj);

	if (copy_to_user(argp, &info, sizeof(info)))
		return -EINVAL;
	return 0;
}

case IOCTL_KGSL_GPUMEM_ALLOC_ID: {
	struct kgsl_gpumem_alloc_id alloc;
	struct kgsl_gpuobj_alloc c_alloc;

	if (copy_from_user(&alloc, argp, sizeof(alloc)))
		return -EFAULT;

	if (alloc.size >> 32)
		return -EINVAL;

	c_alloc.id = alloc.id;
	c_alloc.flags = alloc.flags;
	c_alloc.size = alloc.size;
	c_alloc.metadata_len = 0;
	c_alloc.metadata = 0;

	kgsl_alloc(file_priv, &c_alloc);

	alloc.mmapsize = c_alloc.mmapsize;
	alloc.id = c_alloc.id;

	obj = drm_gem_object_lookup(file_priv, alloc.id);
	if (!obj) {
		//XXX: we are not deallocating in case of failure
		WARN_ON(1);
		return -EINVAL;
	}

	msm_gem_get_and_pin_iova(obj, ctx->vm, (uint64_t *)&alloc.gpuaddr);

	drm_gem_object_put(obj);

	if (copy_to_user(argp, &alloc, sizeof(alloc))) {
		//XXX: we are not deallocating in case of failure
		WARN_ON(1);
		return -EINVAL;
	}

	return 0;
}

case IOCTL_KGSL_GPUOBJ_FREE: {
	struct kgsl_gpuobj_free free;

	if (copy_from_user(&free, argp, sizeof(free)))
		return -EFAULT;

	if (free.flags) {
		DRM_DEV_ERROR(dev->dev, "unexpected free flags: %llx\n", free.flags);
		return -EINVAL;
	}

	return drm_gem_handle_delete(file_priv, free.id);
}

case IOCTL_KGSL_GPUMEM_FREE_ID: {
	struct kgsl_gpumem_free_id free;

	if (copy_from_user(&free, argp, sizeof(free)))
		return -EFAULT;

	obj = drm_gem_object_lookup(file_priv, free.id);
	if (!obj)
		return -EINVAL;

	if (!msm_gem_is_vbo(obj))
		msm_gem_unpin_iova(obj, ctx->vm);

	drm_gem_object_put(obj);

	return drm_gem_handle_delete(file_priv, free.id);
}

case IOCTL_KGSL_GPU_COMMAND: {
	struct msm_gem_submit *submit;
	struct kgsl_gpu_command cmd;
	struct kgsl_command_object obj;
	struct kgsl_command_syncpoint sync;
	u32 i;
	int fence_fd, ret;
	struct dma_fence *in_fence;

	if (copy_from_user(&cmd, argp, sizeof(cmd)))
		return -EFAULT;

	if (cmd.cmdsize != sizeof(obj))
		return -EINVAL;

	if (!access_ok((void __user*)cmd.cmdlist, cmd.numcmds * sizeof(obj)))
		return -EFAULT;

	// XXX cmd.flags

	queue = msm_submitqueue_get(ctx, cmd.context_id);
	if (!queue)
		return -EINVAL;

	submit = submit_create(dev, gpu, queue, 0, cmd.numcmds);
	if (IS_ERR(submit))
		return PTR_ERR(submit);

	mutex_lock(&queue->lock);

	for (i = 0; i < cmd.numsyncs; i++) {
		__copy_from_user(&sync, (void __user*)cmd.synclist + i * sizeof(sync), sizeof(sync));
		if (sync.type != KGSL_CMD_SYNCPOINT_TYPE_FENCE) {
			DRM_DEV_ERROR(dev->dev, "TODO: sync type %d", sync.type);
			continue;
		}
		__copy_from_user(&fence_fd, (void __user*)sync.priv, sizeof(fence_fd));

		in_fence = sync_file_get_fence(fence_fd);
		if (!in_fence) {
			DRM_DEV_ERROR(dev->dev, "TODO: sync_file_get_fence failed\n");
			continue;
		}

		ret = drm_sched_job_add_dependency(&submit->base, in_fence);
		if (ret)
			DRM_DEV_ERROR(dev->dev, "TODO: drm_sched_job_add_dependency failed\n");
	}

	submit->nr_cmds = cmd.numcmds;
	for (i = 0; i < cmd.numcmds; i++) {
		__copy_from_user(&obj, (void __user*)cmd.cmdlist + i * sizeof(obj), sizeof(obj));
		submit->cmd[i].type = MSM_SUBMIT_CMD_BUF;
		submit->cmd[i].size = obj.size >> 2;
		submit->cmd[i].iova = obj.gpuaddr;
	}

	drm_sched_job_arm(&submit->base);

	submit->user_fence = dma_fence_get(&submit->base.s_fence->finished);

	if (queue->kgsl_flags & KGSL_CONTEXT_USER_GENERATED_TS) {
		WARN_ON(cmd.timestamp < queue->timestamp);

		submit->timestamp = cmd.timestamp;
		queue->timestamp = cmd.timestamp;
	} else {
		queue->timestamp++;
		cmd.timestamp = queue->timestamp;
		submit->timestamp = cmd.timestamp;
	}

	msm_gem_submit_get(submit);
	drm_sched_entity_push_job(&submit->base);

	mutex_unlock(&queue->lock);
	msm_gem_submit_put(submit);

	if (copy_to_user(argp, &cmd, sizeof(cmd)))
		WARN_ON(1);

	return 0;
}

case IOCTL_KGSL_PERFCOUNTER_GET: {
	struct kgsl_perfcounter_get get;

	if (copy_from_user(&get, argp, sizeof(get)))
		return -EFAULT;

	// DRM_DEV_ERROR(dev->dev, "TODO: perfcounter get\n");
	return -EBUSY;
}

case IOCTL_KGSL_PERFCOUNTER_PUT: {
	struct kgsl_perfcounter_put put;

	if (copy_from_user(&put, argp, sizeof(put)))
		return -EFAULT;

	DRM_DEV_ERROR(dev->dev, "TODO: perfcounter put\n");
	return 0;
}
case IOCTL_KGSL_SYNCSOURCE_CREATE:
	// DRM_DEV_ERROR(dev->dev, "TODO: syncsource create\n");
	return 0;
case IOCTL_KGSL_SYNCSOURCE_DESTROY:
	// DRM_DEV_ERROR(dev->dev, "TODO: syncsource destroy\n");
	return 0;

case IOCTL_KGSL_GPUOBJ_SYNC:
	// TODO: cache sync for cached objects (worked around by always allocating coherent)
	// DRM_DEV_ERROR(dev->dev, "TODO: GPUOBJ_SYNC\n");
	return 0;

case IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID: {
	struct kgsl_device_waittimestamp_ctxtid wait;
	long ret;

	if (copy_from_user(&wait, argp, sizeof(wait)))
		return -EFAULT;

	queue = msm_submitqueue_get(ctx, wait.context_id);
	if (!queue)
		return -EINVAL;

	ret = wait_event_interruptible_timeout(queue->waitqueue,
			queue->timestamp_retired >= wait.timestamp,
			msecs_to_jiffies(wait.timeout));

	msm_submitqueue_put(queue);

	if (ret > 0)
		return 0;
	return ret == 0 ? -ETIMEDOUT : ret;
}

case IOCTL_KGSL_GPU_AUX_COMMAND: {
	struct kgsl_gpu_aux_command aux;
	struct kgsl_bind_work *work;
	void *work_ptr, *work_end;
	u32 i;

	if (copy_from_user(&aux, argp, sizeof(aux)))
		return -EFAULT;

	if (aux.flags & KGSL_GPU_AUX_COMMAND_SYNC) {
		DRM_DEV_ERROR(dev->dev, "TODO: aux command syncs\n");
		return -EINVAL;
	}

	if (aux.cmdsize != sizeof(struct kgsl_gpu_aux_command_generic))
		return -EINVAL;

	queue = msm_submitqueue_get(ctx, aux.context_id);
	if (!queue)
		return -EINVAL;

#define BIND_WORK_SIZE 65536
	work = kmalloc(BIND_WORK_SIZE, GFP_KERNEL);
	if (!work) {
		msm_submitqueue_put(queue);
		return -ENOMEM;
	}

	INIT_WORK(&work->work, kgsl_bind_worker);
	work->queue = queue;
	work->wait_timestamp = queue->timestamp;
	work->timestamp = aux.timestamp;
	work->num_cmds = 0;
	work_ptr = &work->cmd[0];
	work_end = (void*) work + BIND_WORK_SIZE;

	spin_lock_init(&work->fence_lock);
	dma_fence_init(&work->fence, &kgsl_fence_ops, &work->fence_lock,
		dma_fence_context_alloc(1), 0);

	for (i = 0; i < aux.numcmds; i++) {
		struct kgsl_gpu_aux_command_generic cmd;

		if (copy_from_user(&cmd, (void __user*)aux.cmdlist + i * sizeof(cmd), sizeof(cmd)))
			goto fail;

		if (cmd.type == KGSL_GPU_AUX_COMMAND_BIND) {
			ret = kgsl_work_add_bind(file_priv, &work_ptr, work_end, cmd);
			work->num_cmds++;
		} else if (cmd.type == KGSL_GPU_AUX_COMMAND_TIMELINE) {
			ret = kgsl_work_add_timeline(file_priv, &work->fence, cmd);
		} else {
			goto fail;
		}

		if (ret != 0)
			goto fail;
	}

	queue->timestamp = work->timestamp;
	schedule_work(&work->work);

#if 1
	//XXX: blob appears to not wait on fences from aux command
	// I guess IOCTL_KGSL_GPU_COMMAND is supposed to implicitly sync with aux commands?
	ret = wait_event_interruptible_timeout(queue->waitqueue,
		queue->timestamp_retired >= aux.timestamp,
		5*HZ);
#endif

	return 0;
fail:
	DRM_DEV_ERROR(dev->dev, "TODO: drop references on fail\n");
	msm_submitqueue_put(queue);
	kfree(work);
	return -EINVAL;
};

case IOCTL_KGSL_TIMELINE_CREATE: {
	struct kgsl_timeline_create create;
	struct drm_syncobj *syncobj;
	struct dma_fence_chain *chain;
	struct dma_fence *fence;

	if (copy_from_user(&create, argp, sizeof(create)))
		return -EFAULT;

	ret = drm_syncobj_create(&syncobj, 0, NULL);
	if (ret)
		return ret;

	chain = dma_fence_chain_alloc();
	if (!chain) {
		drm_syncobj_put(syncobj);
		return -ENOMEM;
	}

	fence = dma_fence_get_stub();
	drm_syncobj_add_point(syncobj, chain, fence, create.seqno);
	dma_fence_put(fence);

	ret = drm_syncobj_get_handle(file_priv, syncobj, &create.id);
	drm_syncobj_put(syncobj);
	if (ret)
		return ret;

	if (copy_to_user(argp, &create, sizeof(create)))
		WARN_ON(1);

	return 0;
}

case IOCTL_KGSL_TIMELINE_WAIT: {
	struct kgsl_timeline_wait wait;
	struct kgsl_timeline_val val;
	signed long timeout;
	uint32_t first = ~0;
	struct drm_syncobj *syncobjs[16];
	u64 points[16];
	u32 i;

	if (copy_from_user(&wait, argp, sizeof(wait)))
		return -EFAULT;

	if (wait.timelines_size != sizeof(val))
		return -EINVAL;

	if (wait.flags != KGSL_TIMELINE_WAIT_ALL && wait.flags != KGSL_TIMELINE_WAIT_ANY)
		return -EINVAL;

	if (wait.count > 16) // XXX
		return -EINVAL;

	for (i = 0; i < wait.count; i++) {
		if (copy_from_user(&val, (void __user*)wait.timelines + i * sizeof(val), sizeof(val))) {
			ret = -EFAULT;
			goto fail_timeline_wait;
		}
		syncobjs[i] = drm_syncobj_find(file_priv, val.timeline);
		points[i] = val.seqno;

		if (!syncobjs[i]) {
			ret = -ENOENT;
			goto fail_timeline_wait;
		}
	}

	timeout = min_t(u64, MAX_JIFFY_OFFSET, nsecs_to_jiffies64((u64) wait.tv_sec * 1000000000ull + (u64) wait.tv_nsec));
	timeout = drm_syncobj_array_wait_timeout(syncobjs, points, wait.count,
		(wait.flags == KGSL_TIMELINE_WAIT_ALL ? DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL : 0) |
		DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, timeout, &first);
	ret = timeout < 0 ? timeout : 0; // XXX check this;
fail_timeline_wait:
	while (i-->0)
		drm_syncobj_put(syncobjs[i]);
	return ret;
}

case IOCTL_KGSL_TIMELINE_QUERY: {
	struct kgsl_timeline_val val;
	struct drm_syncobj *syncobj;

	if (copy_from_user(&val, argp, sizeof(val)))
		return -EFAULT;

	syncobj = drm_syncobj_find(file_priv, val.timeline);
	if (!syncobj)
		return -ENOENT;

	{ // from drm_syncobj_query_ioctl:
		struct dma_fence_chain *chain;
		struct dma_fence *fence;
		uint64_t point;

		fence = drm_syncobj_fence_get(syncobj);
		chain = to_dma_fence_chain(fence);
		if (chain) {
			struct dma_fence *iter, *last_signaled =
				dma_fence_get(fence);

			/* if (args->flags &
			    DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED) {
				point = fence->seqno;
			} else */ {
				dma_fence_chain_for_each(iter, fence) {
					if (iter->context != fence->context) {
						dma_fence_put(iter);
						/* It is most likely that timeline has
						* unorder points. */
						break;
					}
					dma_fence_put(last_signaled);
					last_signaled = dma_fence_get(iter);
				}
				point = dma_fence_is_signaled(last_signaled) ?
					last_signaled->seqno :
					to_dma_fence_chain(last_signaled)->prev_seqno;
			}
			dma_fence_put(last_signaled);
		} else {
			point = 0;
		}
		dma_fence_put(fence);
		val.seqno = point;
	}

	drm_syncobj_put(syncobj);

	if (copy_to_user(argp, &val, sizeof(val)))
		WARN_ON(1);

	return 0;
};

default:
	DRM_DEV_ERROR(dev->dev, "TODO: ioctl %.8x\n", cmd);
	return 0;
	}
}
