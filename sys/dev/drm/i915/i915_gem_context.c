/*
 * Copyright © 2011-2012 Intel Corporation
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

/*
 * This file implements HW context support. On gen5+ a HW context consists of an
 * opaque GPU object which is referenced at times of context saves and restores.
 * With RC6 enabled, the context is also referenced as the GPU enters and exists
 * from RC6 (GPU has it's own internal power context, except on gen5). Though
 * something like a context does exist for the media ring, the code only
 * supports contexts for the render ring.
 *
 * In software, there is a distinction between contexts created by the user,
 * and the default HW context. The default HW context is used by GPU clients
 * that do not request setup of their own hardware context. The default
 * context's state is never restored to help prevent programming errors. This
 * would happen if a client ran and piggy-backed off another clients GPU state.
 * The default context only exists to give the GPU some offset to load as the
 * current to invoke a save of the context we actually care about. In fact, the
 * code could likely be constructed, albeit in a more complicated fashion, to
 * never use the default context, though that limits the driver's ability to
 * swap out, and/or destroy other contexts.
 *
 * All other contexts are created as a request by the GPU client. These contexts
 * store GPU state, and thus allow GPU clients to not re-emit state (and
 * potentially query certain state) at any time. The kernel driver makes
 * certain that the appropriate commands are inserted.
 *
 * The context life cycle is semi-complicated in that context BOs may live
 * longer than the context itself because of the way the hardware, and object
 * tracking works. Below is a very crude representation of the state machine
 * describing the context life.
 *                                         refcount     pincount     active
 * S0: initial state                          0            0           0
 * S1: context created                        1            0           0
 * S2: context is currently running           2            1           X
 * S3: GPU referenced, but not current        2            0           1
 * S4: context is current, but destroyed      1            1           0
 * S5: like S3, but destroyed                 1            0           1
 *
 * The most common (but not all) transitions:
 * S0->S1: client creates a context
 * S1->S2: client submits execbuf with context
 * S2->S3: other clients submits execbuf with context
 * S3->S1: context object was retired
 * S3->S2: clients submits another execbuf
 * S2->S4: context destroy called with current context
 * S3->S5->S0: destroy path
 * S4->S5->S0: destroy path on current context
 *
 * There are two confusing terms used above:
 *  The "current context" means the context which is currently running on the
 *  GPU. The GPU has loaded its state already and has stored away the gtt
 *  offset of the BO. The GPU is not actively referencing the data at this
 *  offset, but it will on the next context switch. The only way to avoid this
 *  is to do a GPU reset.
 *
 *  An "active context' is one which was previously the "current context" and is
 *  on the active list waiting for the next context switch to occur. Until this
 *  happens, the object must remain at the same gtt offset. It is therefore
 *  possible to destroy a context, but it is still active.
 *
 */

#include <drm/drmP.h>
#include <drm/i915_drm.h>
#include "i915_drv.h"
#include "i915_trace.h"

#define ALL_L3_SLICES(dev) (1 << NUM_L3_SLICES(dev)) - 1

/* This is a HW constraint. The value below is the largest known requirement
 * I've seen in a spec to date, and that was a workaround for a non-shipping
 * part. It should be safe to decrease this, but it's more future proof as is.
 */
#define GEN6_CONTEXT_ALIGN (64<<10)
#define GEN7_CONTEXT_ALIGN 4096

static size_t get_context_alignment(struct drm_i915_private *dev_priv)
{
	if (IS_GEN6(dev_priv))
		return GEN6_CONTEXT_ALIGN;

	return GEN7_CONTEXT_ALIGN;
}

static int get_context_size(struct drm_i915_private *dev_priv)
{
	int ret;
	u32 reg;

	switch (INTEL_GEN(dev_priv)) {
	case 6:
		reg = I915_READ(CXT_SIZE);
		ret = GEN6_CXT_TOTAL_SIZE(reg) * 64;
		break;
	case 7:
		reg = I915_READ(GEN7_CXT_SIZE);
		if (IS_HASWELL(dev_priv))
			ret = HSW_CXT_TOTAL_SIZE;
		else
			ret = GEN7_CXT_TOTAL_SIZE(reg) * 64;
		break;
	case 8:
		ret = GEN8_CXT_TOTAL_SIZE;
		break;
	default:
		BUG();
	}

	return ret;
}

static void i915_gem_context_clean(struct i915_gem_context *ctx)
{
	struct i915_hw_ppgtt *ppgtt = ctx->ppgtt;
	struct i915_vma *vma, *next;

	if (!ppgtt)
		return;

	list_for_each_entry_safe(vma, next, &ppgtt->base.inactive_list,
				 vm_link) {
		if (WARN_ON(__i915_vma_unbind_no_wait(vma)))
			break;
	}
}

void i915_gem_context_free(struct kref *ctx_ref)
{
	struct i915_gem_context *ctx = container_of(ctx_ref, typeof(*ctx), ref);
	int i;

	lockdep_assert_held(&ctx->i915->dev->struct_mutex);
	trace_i915_context_free(ctx);

	/*
	 * This context is going away and we need to remove all VMAs still
	 * around. This is to handle imported shared objects for which
	 * destructor did not run when their handles were closed.
	 */
	i915_gem_context_clean(ctx);

	i915_ppgtt_put(ctx->ppgtt);

	for (i = 0; i < I915_NUM_ENGINES; i++) {
		struct intel_context *ce = &ctx->engine[i];

		if (!ce->state)
			continue;

		WARN_ON(ce->pin_count);
		if (ce->ringbuf)
			intel_ringbuffer_free(ce->ringbuf);

		drm_gem_object_unreference(&ce->state->base);
	}

	list_del(&ctx->link);

	ida_simple_remove(&ctx->i915->context_hw_ida, ctx->hw_id);
	kfree(ctx);
}

struct drm_i915_gem_object *
i915_gem_alloc_context_obj(struct drm_device *dev, size_t size)
{
	struct drm_i915_gem_object *obj;
	int ret;

	lockdep_assert_held(&dev->struct_mutex);

	obj = i915_gem_object_create(dev, size);
	if (IS_ERR(obj))
		return obj;

	/*
	 * Try to make the context utilize L3 as well as LLC.
	 *
	 * On VLV we don't have L3 controls in the PTEs so we
	 * shouldn't touch the cache level, especially as that
	 * would make the object snooped which might have a
	 * negative performance impact.
	 *
	 * Snooping is required on non-llc platforms in execlist
	 * mode, but since all GGTT accesses use PAT entry 0 we
	 * get snooping anyway regardless of cache_level.
	 *
	 * This is only applicable for Ivy Bridge devices since
	 * later platforms don't have L3 control bits in the PTE.
	 */
	if (IS_IVYBRIDGE(dev)) {
		ret = i915_gem_object_set_cache_level(obj, I915_CACHE_L3_LLC);
		/* Failure shouldn't ever happen this early */
		if (WARN_ON(ret)) {
			drm_gem_object_unreference(&obj->base);
			return ERR_PTR(ret);
		}
	}

	return obj;
}

static int assign_hw_id(struct drm_i915_private *dev_priv, unsigned *out)
{
	int ret;

	ret = ida_simple_get(&dev_priv->context_hw_ida,
			     0, MAX_CONTEXT_HW_ID, GFP_KERNEL);
	if (ret < 0) {
		/* Contexts are only released when no longer active.
		 * Flush any pending retires to hopefully release some
		 * stale contexts and try again.
		 */
		i915_gem_retire_requests(dev_priv);
		ret = ida_simple_get(&dev_priv->context_hw_ida,
				     0, MAX_CONTEXT_HW_ID, GFP_KERNEL);
		if (ret < 0)
			return ret;
	}

	*out = ret;
	return 0;
}

static struct i915_gem_context *
__create_hw_context(struct drm_device *dev,
		    struct drm_i915_file_private *file_priv)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct i915_gem_context *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (ctx == NULL)
		return ERR_PTR(-ENOMEM);

	ret = assign_hw_id(dev_priv, &ctx->hw_id);
	if (ret) {
		kfree(ctx);
		return ERR_PTR(ret);
	}

	kref_init(&ctx->ref);
	list_add_tail(&ctx->link, &dev_priv->context_list);
	ctx->i915 = dev_priv;

	ctx->ggtt_alignment = get_context_alignment(dev_priv);

	if (dev_priv->hw_context_size) {
		struct drm_i915_gem_object *obj =
				i915_gem_alloc_context_obj(dev, dev_priv->hw_context_size);
		if (IS_ERR(obj)) {
			ret = PTR_ERR(obj);
			goto err_out;
		}
		ctx->engine[RCS].state = obj;
	}

	/* Default context will never have a file_priv */
	if (file_priv != NULL) {
		ret = idr_alloc(&file_priv->context_idr, ctx,
				DEFAULT_CONTEXT_HANDLE, 0, GFP_KERNEL);
		if (ret < 0)
			goto err_out;
	} else
		ret = DEFAULT_CONTEXT_HANDLE;

	ctx->file_priv = file_priv;
	ctx->user_handle = ret;
	/* NB: Mark all slices as needing a remap so that when the context first
	 * loads it will restore whatever remap state already exists. If there
	 * is no remap info, it will be a NOP. */
	ctx->remap_slice = ALL_L3_SLICES(dev_priv);

	ctx->hang_stats.ban_period_seconds = DRM_I915_CTX_BAN_PERIOD;
	ctx->ring_size = 4 * PAGE_SIZE;
	ctx->desc_template = GEN8_CTX_ADDRESSING_MODE(dev_priv) <<
			     GEN8_CTX_ADDRESSING_MODE_SHIFT;
	ATOMIC_INIT_NOTIFIER_HEAD(&ctx->status_notifier);

	return ctx;

err_out:
	i915_gem_context_unreference(ctx);
	return ERR_PTR(ret);
}

/**
 * The default context needs to exist per ring that uses contexts. It stores the
 * context state of the GPU for applications that don't utilize HW contexts, as
 * well as an idle case.
 */
static struct i915_gem_context *
i915_gem_create_context(struct drm_device *dev,
			struct drm_i915_file_private *file_priv)
{
	struct i915_gem_context *ctx;

	lockdep_assert_held(&dev->struct_mutex);

	ctx = __create_hw_context(dev, file_priv);
	if (IS_ERR(ctx))
		return ctx;

	if (USES_FULL_PPGTT(dev)) {
		struct i915_hw_ppgtt *ppgtt = i915_ppgtt_create(dev, file_priv);

		if (IS_ERR(ppgtt)) {
			DRM_DEBUG_DRIVER("PPGTT setup failed (%ld)\n",
					 PTR_ERR(ppgtt));
			idr_remove(&file_priv->context_idr, ctx->user_handle);
			i915_gem_context_unreference(ctx);
			return ERR_CAST(ppgtt);
		}

		ctx->ppgtt = ppgtt;
	}

	trace_i915_context_create(ctx);

	return ctx;
}

/**
 * i915_gem_context_create_gvt - create a GVT GEM context
 * @dev: drm device *
 *
 * This function is used to create a GVT specific GEM context.
 *
 * Returns:
 * pointer to i915_gem_context on success, error pointer if failed
 *
 */
struct i915_gem_context *
i915_gem_context_create_gvt(struct drm_device *dev)
{
	struct i915_gem_context *ctx;
	int ret;

	if (!IS_ENABLED(CONFIG_DRM_I915_GVT))
		return ERR_PTR(-ENODEV);

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ERR_PTR(ret);

	ctx = i915_gem_create_context(dev, NULL);
	if (IS_ERR(ctx))
		goto out;

	ctx->execlists_force_single_submission = true;
	ctx->ring_size = 512 * PAGE_SIZE; /* Max ring buffer size */
out:
	mutex_unlock(&dev->struct_mutex);
	return ctx;
}

static void i915_gem_context_unpin(struct i915_gem_context *ctx,
				   struct intel_engine_cs *engine)
{
	if (i915.enable_execlists) {
		intel_lr_context_unpin(ctx, engine);
	} else {
		struct intel_context *ce = &ctx->engine[engine->id];

		if (ce->state)
			i915_gem_object_ggtt_unpin(ce->state);

		i915_gem_context_unreference(ctx);
	}
}

void i915_gem_context_reset(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;

	lockdep_assert_held(&dev->struct_mutex);

	if (i915.enable_execlists) {
		struct i915_gem_context *ctx;

		list_for_each_entry(ctx, &dev_priv->context_list, link)
			intel_lr_context_reset(dev_priv, ctx);
	}

	i915_gem_context_lost(dev_priv);
}

int i915_gem_context_init(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct i915_gem_context *ctx;

	/* Init should only be called once per module load. Eventually the
	 * restriction on the context_disabled check can be loosened. */
	if (WARN_ON(dev_priv->kernel_context))
		return 0;

	if (intel_vgpu_active(dev_priv) &&
	    HAS_LOGICAL_RING_CONTEXTS(dev_priv)) {
		if (!i915.enable_execlists) {
			DRM_INFO("Only EXECLIST mode is supported in vgpu.\n");
			return -EINVAL;
		}
	}

	/* Using the simple ida interface, the max is limited by sizeof(int) */
	BUILD_BUG_ON(MAX_CONTEXT_HW_ID > INT_MAX);
	ida_init(&dev_priv->context_hw_ida);

	if (i915.enable_execlists) {
		/* NB: intentionally left blank. We will allocate our own
		 * backing objects as we need them, thank you very much */
		dev_priv->hw_context_size = 0;
	} else if (HAS_HW_CONTEXTS(dev_priv)) {
		dev_priv->hw_context_size =
			round_up(get_context_size(dev_priv), 4096);
		if (dev_priv->hw_context_size > (1<<20)) {
			DRM_DEBUG_DRIVER("Disabling HW Contexts; invalid size %d\n",
					 dev_priv->hw_context_size);
			dev_priv->hw_context_size = 0;
		}
	}

	ctx = i915_gem_create_context(dev, NULL);
	if (IS_ERR(ctx)) {
		DRM_ERROR("Failed to create default global context (error %ld)\n",
			  PTR_ERR(ctx));
		return PTR_ERR(ctx);
	}

	dev_priv->kernel_context = ctx;

	DRM_DEBUG_DRIVER("%s context support initialized\n",
			i915.enable_execlists ? "LR" :
			dev_priv->hw_context_size ? "HW" : "fake");
	return 0;
}

void i915_gem_context_lost(struct drm_i915_private *dev_priv)
{
	struct intel_engine_cs *engine;

	lockdep_assert_held(&dev_priv->dev->struct_mutex);

	for_each_engine(engine, dev_priv) {
		if (engine->last_context) {
			i915_gem_context_unpin(engine->last_context, engine);
			engine->last_context = NULL;
		}
	}

	/* Force the GPU state to be restored on enabling */
	if (!i915.enable_execlists) {
		struct i915_gem_context *ctx;

		list_for_each_entry(ctx, &dev_priv->context_list, link) {
			if (!i915_gem_context_is_default(ctx))
				continue;

			for_each_engine(engine, dev_priv)
				ctx->engine[engine->id].initialised = false;

			ctx->remap_slice = ALL_L3_SLICES(dev_priv);
		}

		for_each_engine(engine, dev_priv) {
			struct intel_context *kce =
				&dev_priv->kernel_context->engine[engine->id];

			kce->initialised = true;
		}
	}
}

void i915_gem_context_fini(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct i915_gem_context *dctx = dev_priv->kernel_context;

	lockdep_assert_held(&dev->struct_mutex);

	i915_gem_context_unreference(dctx);
	dev_priv->kernel_context = NULL;

	ida_destroy(&dev_priv->context_hw_ida);
}

static int context_idr_cleanup(int id, void *p, void *data)
{
	struct i915_gem_context *ctx = p;

	ctx->file_priv = ERR_PTR(-EBADF);
	i915_gem_context_unreference(ctx);
	return 0;
}

int i915_gem_context_open(struct drm_device *dev, struct drm_file *file)
{
	struct drm_i915_file_private *file_priv = file->driver_priv;
	struct i915_gem_context *ctx;

	idr_init(&file_priv->context_idr);

	mutex_lock(&dev->struct_mutex);
	ctx = i915_gem_create_context(dev, file_priv);
	mutex_unlock(&dev->struct_mutex);

	if (IS_ERR(ctx)) {
		idr_destroy(&file_priv->context_idr);
		return PTR_ERR(ctx);
	}

	return 0;
}

void i915_gem_context_close(struct drm_device *dev, struct drm_file *file)
{
	struct drm_i915_file_private *file_priv = file->driver_priv;

	lockdep_assert_held(&dev->struct_mutex);

	idr_for_each(&file_priv->context_idr, context_idr_cleanup, NULL);
	idr_destroy(&file_priv->context_idr);
}

static inline int
mi_set_context(struct drm_i915_gem_request *req, u32 hw_flags)
{
	struct drm_i915_private *dev_priv = req->i915;
	struct intel_engine_cs *engine = req->engine;
	u32 flags = hw_flags | MI_MM_SPACE_GTT;
	const int num_rings =
		/* Use an extended w/a on ivb+ if signalling from other rings */
		i915_semaphore_is_enabled(dev_priv) ?
		hweight32(INTEL_INFO(dev_priv)->ring_mask) - 1 :
		0;
	int len, ret;

	/* w/a: If Flush TLB Invalidation Mode is enabled, driver must do a TLB
	 * invalidation prior to MI_SET_CONTEXT. On GEN6 we don't set the value
	 * explicitly, so we rely on the value at ring init, stored in
	 * itlb_before_ctx_switch.
	 */
	if (IS_GEN6(dev_priv)) {
		ret = engine->flush(req, I915_GEM_GPU_DOMAINS, 0);
		if (ret)
			return ret;
	}

	/* These flags are for resource streamer on HSW+ */
	if (IS_HASWELL(dev_priv) || INTEL_GEN(dev_priv) >= 8)
		flags |= (HSW_MI_RS_SAVE_STATE_EN | HSW_MI_RS_RESTORE_STATE_EN);
	else if (INTEL_GEN(dev_priv) < 8)
		flags |= (MI_SAVE_EXT_STATE_EN | MI_RESTORE_EXT_STATE_EN);


	len = 4;
	if (INTEL_GEN(dev_priv) >= 7)
		len += 2 + (num_rings ? 4*num_rings + 6 : 0);

	ret = intel_ring_begin(req, len);
	if (ret)
		return ret;

	/* WaProgramMiArbOnOffAroundMiSetContext:ivb,vlv,hsw,bdw,chv */
	if (INTEL_GEN(dev_priv) >= 7) {
		intel_ring_emit(engine, MI_ARB_ON_OFF | MI_ARB_DISABLE);
		if (num_rings) {
			struct intel_engine_cs *signaller;

			intel_ring_emit(engine,
					MI_LOAD_REGISTER_IMM(num_rings));
			for_each_engine(signaller, dev_priv) {
				if (signaller == engine)
					continue;

				intel_ring_emit_reg(engine,
						    RING_PSMI_CTL(signaller->mmio_base));
				intel_ring_emit(engine,
						_MASKED_BIT_ENABLE(GEN6_PSMI_SLEEP_MSG_DISABLE));
			}
		}
	}

	intel_ring_emit(engine, MI_NOOP);
	intel_ring_emit(engine, MI_SET_CONTEXT);
	intel_ring_emit(engine,
			i915_gem_obj_ggtt_offset(req->ctx->engine[RCS].state) |
			flags);
	/*
	 * w/a: MI_SET_CONTEXT must always be followed by MI_NOOP
	 * WaMiSetContext_Hang:snb,ivb,vlv
	 */
	intel_ring_emit(engine, MI_NOOP);

	if (INTEL_GEN(dev_priv) >= 7) {
		if (num_rings) {
			struct intel_engine_cs *signaller;
			i915_reg_t last_reg = {}; /* keep gcc quiet */

			intel_ring_emit(engine,
					MI_LOAD_REGISTER_IMM(num_rings));
			for_each_engine(signaller, dev_priv) {
				if (signaller == engine)
					continue;

				last_reg = RING_PSMI_CTL(signaller->mmio_base);
				intel_ring_emit_reg(engine, last_reg);
				intel_ring_emit(engine,
						_MASKED_BIT_DISABLE(GEN6_PSMI_SLEEP_MSG_DISABLE));
			}

			/* Insert a delay before the next switch! */
			intel_ring_emit(engine,
					MI_STORE_REGISTER_MEM |
					MI_SRM_LRM_GLOBAL_GTT);
			intel_ring_emit_reg(engine, last_reg);
			intel_ring_emit(engine, engine->scratch.gtt_offset);
			intel_ring_emit(engine, MI_NOOP);
		}
		intel_ring_emit(engine, MI_ARB_ON_OFF | MI_ARB_ENABLE);
	}

	intel_ring_advance(engine);

	return ret;
}

static int remap_l3(struct drm_i915_gem_request *req, int slice)
{
	u32 *remap_info = req->i915->l3_parity.remap_info[slice];
	struct intel_engine_cs *engine = req->engine;
	int i, ret;

	if (!remap_info)
		return 0;

	ret = intel_ring_begin(req, GEN7_L3LOG_SIZE/4 * 2 + 2);
	if (ret)
		return ret;

	/*
	 * Note: We do not worry about the concurrent register cacheline hang
	 * here because no other code should access these registers other than
	 * at initialization time.
	 */
	intel_ring_emit(engine, MI_LOAD_REGISTER_IMM(GEN7_L3LOG_SIZE/4));
	for (i = 0; i < GEN7_L3LOG_SIZE/4; i++) {
		intel_ring_emit_reg(engine, GEN7_L3LOG(slice, i));
		intel_ring_emit(engine, remap_info[i]);
	}
	intel_ring_emit(engine, MI_NOOP);
	intel_ring_advance(engine);

	return 0;
}

static inline bool skip_rcs_switch(struct i915_hw_ppgtt *ppgtt,
				   struct intel_engine_cs *engine,
				   struct i915_gem_context *to)
{
	if (to->remap_slice)
		return false;

	if (!to->engine[RCS].initialised)
		return false;

	if (ppgtt && (intel_engine_flag(engine) & ppgtt->pd_dirty_rings))
		return false;

	return to == engine->last_context;
}

static bool
needs_pd_load_pre(struct i915_hw_ppgtt *ppgtt,
		  struct intel_engine_cs *engine,
		  struct i915_gem_context *to)
{
	if (!ppgtt)
		return false;

	/* Always load the ppgtt on first use */
	if (!engine->last_context)
		return true;

	/* Same context without new entries, skip */
	if (engine->last_context == to &&
	    !(intel_engine_flag(engine) & ppgtt->pd_dirty_rings))
		return false;

	if (engine->id != RCS)
		return true;

	if (INTEL_GEN(engine->i915) < 8)
		return true;

	return false;
}

static bool
needs_pd_load_post(struct i915_hw_ppgtt *ppgtt,
		   struct i915_gem_context *to,
		   u32 hw_flags)
{
	if (!ppgtt)
		return false;

	if (!IS_GEN8(to->i915))
		return false;

	if (hw_flags & MI_RESTORE_INHIBIT)
		return true;

	return false;
}

static int do_rcs_switch(struct drm_i915_gem_request *req)
{
	struct i915_gem_context *to = req->ctx;
	struct intel_engine_cs *engine = req->engine;
	struct i915_hw_ppgtt *ppgtt = to->ppgtt ?: req->i915->mm.aliasing_ppgtt;
	struct i915_gem_context *from;
	u32 hw_flags;
	int ret, i;

	if (skip_rcs_switch(ppgtt, engine, to))
		return 0;

	/* Trying to pin first makes error handling easier. */
	ret = i915_gem_obj_ggtt_pin(to->engine[RCS].state,
				    to->ggtt_alignment,
				    0);
	if (ret)
		return ret;

	/*
	 * Pin can switch back to the default context if we end up calling into
	 * evict_everything - as a last ditch gtt defrag effort that also
	 * switches to the default context. Hence we need to reload from here.
	 *
	 * XXX: Doing so is painfully broken!
	 */
	from = engine->last_context;

	/*
	 * Clear this page out of any CPU caches for coherent swap-in/out. Note
	 * that thanks to write = false in this call and us not setting any gpu
	 * write domains when putting a context object onto the active list
	 * (when switching away from it), this won't block.
	 *
	 * XXX: We need a real interface to do this instead of trickery.
	 */
	ret = i915_gem_object_set_to_gtt_domain(to->engine[RCS].state, false);
	if (ret)
		goto unpin_out;

	if (needs_pd_load_pre(ppgtt, engine, to)) {
		/* Older GENs and non render rings still want the load first,
		 * "PP_DCLV followed by PP_DIR_BASE register through Load
		 * Register Immediate commands in Ring Buffer before submitting
		 * a context."*/
		trace_switch_mm(engine, to);
		ret = ppgtt->switch_mm(ppgtt, req);
		if (ret)
			goto unpin_out;
	}

	if (!to->engine[RCS].initialised || i915_gem_context_is_default(to))
		/* NB: If we inhibit the restore, the context is not allowed to
		 * die because future work may end up depending on valid address
		 * space. This means we must enforce that a page table load
		 * occur when this occurs. */
		hw_flags = MI_RESTORE_INHIBIT;
	else if (ppgtt && intel_engine_flag(engine) & ppgtt->pd_dirty_rings)
		hw_flags = MI_FORCE_RESTORE;
	else
		hw_flags = 0;

	if (to != from || (hw_flags & MI_FORCE_RESTORE)) {
		ret = mi_set_context(req, hw_flags);
		if (ret)
			goto unpin_out;
	}

	/* The backing object for the context is done after switching to the
	 * *next* context. Therefore we cannot retire the previous context until
	 * the next context has already started running. In fact, the below code
	 * is a bit suboptimal because the retiring can occur simply after the
	 * MI_SET_CONTEXT instead of when the next seqno has completed.
	 */
	if (from != NULL) {
		from->engine[RCS].state->base.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		i915_vma_move_to_active(i915_gem_obj_to_ggtt(from->engine[RCS].state), req);
		/* As long as MI_SET_CONTEXT is serializing, ie. it flushes the
		 * whole damn pipeline, we don't need to explicitly mark the
		 * object dirty. The only exception is that the context must be
		 * correct in case the object gets swapped out. Ideally we'd be
		 * able to defer doing this until we know the object would be
		 * swapped, but there is no way to do that yet.
		 */
		from->engine[RCS].state->dirty = 1;

		/* obj is kept alive until the next request by its active ref */
		i915_gem_object_ggtt_unpin(from->engine[RCS].state);
		i915_gem_context_unreference(from);
	}
	i915_gem_context_reference(to);
	engine->last_context = to;

	/* GEN8 does *not* require an explicit reload if the PDPs have been
	 * setup, and we do not wish to move them.
	 */
	if (needs_pd_load_post(ppgtt, to, hw_flags)) {
		trace_switch_mm(engine, to);
		ret = ppgtt->switch_mm(ppgtt, req);
		/* The hardware context switch is emitted, but we haven't
		 * actually changed the state - so it's probably safe to bail
		 * here. Still, let the user know something dangerous has
		 * happened.
		 */
		if (ret)
			return ret;
	}

	if (ppgtt)
		ppgtt->pd_dirty_rings &= ~intel_engine_flag(engine);

	for (i = 0; i < MAX_L3_SLICES; i++) {
		if (!(to->remap_slice & (1<<i)))
			continue;

		ret = remap_l3(req, i);
		if (ret)
			return ret;

		to->remap_slice &= ~(1<<i);
	}

	if (!to->engine[RCS].initialised) {
		if (engine->init_context) {
			ret = engine->init_context(req);
			if (ret)
				return ret;
		}
		to->engine[RCS].initialised = true;
	}

	return 0;

unpin_out:
	i915_gem_object_ggtt_unpin(to->engine[RCS].state);
	return ret;
}

/**
 * i915_switch_context() - perform a GPU context switch.
 * @req: request for which we'll execute the context switch
 *
 * The context life cycle is simple. The context refcount is incremented and
 * decremented by 1 and create and destroy. If the context is in use by the GPU,
 * it will have a refcount > 1. This allows us to destroy the context abstract
 * object while letting the normal object tracking destroy the backing BO.
 *
 * This function should not be used in execlists mode.  Instead the context is
 * switched by writing to the ELSP and requests keep a reference to their
 * context.
 */
int i915_switch_context(struct drm_i915_gem_request *req)
{
	struct intel_engine_cs *engine = req->engine;

	WARN_ON(i915.enable_execlists);
	lockdep_assert_held(&req->i915->dev->struct_mutex);

	if (!req->ctx->engine[engine->id].state) {
		struct i915_gem_context *to = req->ctx;
		struct i915_hw_ppgtt *ppgtt =
			to->ppgtt ?: req->i915->mm.aliasing_ppgtt;

		if (needs_pd_load_pre(ppgtt, engine, to)) {
			int ret;

			trace_switch_mm(engine, to);
			ret = ppgtt->switch_mm(ppgtt, req);
			if (ret)
				return ret;

			ppgtt->pd_dirty_rings &= ~intel_engine_flag(engine);
		}

		if (to != engine->last_context) {
			i915_gem_context_reference(to);
			if (engine->last_context)
				i915_gem_context_unreference(engine->last_context);
			engine->last_context = to;
		}

		return 0;
	}

	return do_rcs_switch(req);
}

static bool contexts_enabled(struct drm_device *dev)
{
	return i915.enable_execlists || to_i915(dev)->hw_context_size;
}

int i915_gem_context_create_ioctl(struct drm_device *dev, void *data,
				  struct drm_file *file)
{
	struct drm_i915_gem_context_create *args = data;
	struct drm_i915_file_private *file_priv = file->driver_priv;
	struct i915_gem_context *ctx;
	int ret;

	if (!contexts_enabled(dev))
		return -ENODEV;

	if (args->pad != 0)
		return -EINVAL;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ret;

	ctx = i915_gem_create_context(dev, file_priv);
	mutex_unlock(&dev->struct_mutex);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	args->ctx_id = ctx->user_handle;
	DRM_DEBUG_DRIVER("HW context %d created\n", args->ctx_id);

	return 0;
}

int i915_gem_context_destroy_ioctl(struct drm_device *dev, void *data,
				   struct drm_file *file)
{
	struct drm_i915_gem_context_destroy *args = data;
	struct drm_i915_file_private *file_priv = file->driver_priv;
	struct i915_gem_context *ctx;
	int ret;

	if (args->pad != 0)
		return -EINVAL;

	if (args->ctx_id == DEFAULT_CONTEXT_HANDLE)
		return -ENOENT;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ret;

	ctx = i915_gem_context_lookup(file_priv, args->ctx_id);
	if (IS_ERR(ctx)) {
		mutex_unlock(&dev->struct_mutex);
		return PTR_ERR(ctx);
	}

	idr_remove(&file_priv->context_idr, ctx->user_handle);
	i915_gem_context_unreference(ctx);
	mutex_unlock(&dev->struct_mutex);

	DRM_DEBUG_DRIVER("HW context %d destroyed\n", args->ctx_id);
	return 0;
}

int i915_gem_context_getparam_ioctl(struct drm_device *dev, void *data,
				    struct drm_file *file)
{
	struct drm_i915_file_private *file_priv = file->driver_priv;
	struct drm_i915_gem_context_param *args = data;
	struct i915_gem_context *ctx;
	int ret;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ret;

	ctx = i915_gem_context_lookup(file_priv, args->ctx_id);
	if (IS_ERR(ctx)) {
		mutex_unlock(&dev->struct_mutex);
		return PTR_ERR(ctx);
	}

	args->size = 0;
	switch (args->param) {
	case I915_CONTEXT_PARAM_BAN_PERIOD:
		args->value = ctx->hang_stats.ban_period_seconds;
		break;
	case I915_CONTEXT_PARAM_NO_ZEROMAP:
		args->value = ctx->flags & CONTEXT_NO_ZEROMAP;
		break;
	case I915_CONTEXT_PARAM_GTT_SIZE:
		if (ctx->ppgtt)
			args->value = ctx->ppgtt->base.total;
		else if (to_i915(dev)->mm.aliasing_ppgtt)
			args->value = to_i915(dev)->mm.aliasing_ppgtt->base.total;
		else
			args->value = to_i915(dev)->ggtt.base.total;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&dev->struct_mutex);

	return ret;
}

int i915_gem_context_setparam_ioctl(struct drm_device *dev, void *data,
				    struct drm_file *file)
{
	struct drm_i915_file_private *file_priv = file->driver_priv;
	struct drm_i915_gem_context_param *args = data;
	struct i915_gem_context *ctx;
	int ret;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ret;

	ctx = i915_gem_context_lookup(file_priv, args->ctx_id);
	if (IS_ERR(ctx)) {
		mutex_unlock(&dev->struct_mutex);
		return PTR_ERR(ctx);
	}

	switch (args->param) {
	case I915_CONTEXT_PARAM_BAN_PERIOD:
		if (args->size)
			ret = -EINVAL;
		else if (args->value < ctx->hang_stats.ban_period_seconds &&
			 !capable(CAP_SYS_ADMIN))
			ret = -EPERM;
		else
			ctx->hang_stats.ban_period_seconds = args->value;
		break;
	case I915_CONTEXT_PARAM_NO_ZEROMAP:
		if (args->size) {
			ret = -EINVAL;
		} else {
			ctx->flags &= ~CONTEXT_NO_ZEROMAP;
			ctx->flags |= args->value ? CONTEXT_NO_ZEROMAP : 0;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&dev->struct_mutex);

	return ret;
}

int i915_gem_context_reset_stats_ioctl(struct drm_device *dev,
				       void *data, struct drm_file *file)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_reset_stats *args = data;
	struct i915_ctx_hang_stats *hs;
	struct i915_gem_context *ctx;
	int ret;

	if (args->flags || args->pad)
		return -EINVAL;

	if (args->ctx_id == DEFAULT_CONTEXT_HANDLE && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		return ret;

	ctx = i915_gem_context_lookup(file->driver_priv, args->ctx_id);
	if (IS_ERR(ctx)) {
		mutex_unlock(&dev->struct_mutex);
		return PTR_ERR(ctx);
	}
	hs = &ctx->hang_stats;

	if (capable(CAP_SYS_ADMIN))
		args->reset_count = i915_reset_count(&dev_priv->gpu_error);
	else
		args->reset_count = 0;

	args->batch_active = hs->batch_active;
	args->batch_pending = hs->batch_pending;

	mutex_unlock(&dev->struct_mutex);

	return 0;
}
