/*
 * \file drm_fops.c
 * File operations for DRM
 *
 * \author Rickard E. (Rik) Faith <faith@valinux.com>
 * \author Daryll Strauss <daryll@valinux.com>
 * \author Gareth Hughes <gareth@valinux.com>
 */

/*
 * Created: Mon Jan  4 08:58:31 1999 by faith@valinux.com
 *
 * Copyright 1999 Precision Insight, Inc., Cedar Park, Texas.
 * Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
 * All Rights Reserved.
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
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <sys/types.h>
#include <sys/uio.h>	/* must come first to avoid kfree() macros issues */
#include <drm/drmP.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/module.h>
#include "drm_legacy.h"
#include "drm_internal.h"
#include "drm_crtc_internal.h"

#include <sys/devfs.h>

/* from BKL pushdown */
DEFINE_MUTEX(drm_global_mutex);

/**
 * DOC: file operations
 *
 * Drivers must define the file operations structure that forms the DRM
 * userspace API entry point, even though most of those operations are
 * implemented in the DRM core. The mandatory functions are drm_open(),
 * drm_read(), drm_ioctl() and drm_compat_ioctl if CONFIG_COMPAT is enabled.
 * Drivers which implement private ioctls that require 32/64 bit compatibility
 * support must provided their onw .compat_ioctl() handler that processes
 * private ioctls and calls drm_compat_ioctl() for core ioctls.
 *
 * In addition drm_read() and drm_poll() provide support for DRM events. DRM
 * events are a generic and extensible means to send asynchronous events to
 * userspace through the file descriptor. They are used to send vblank event and
 * page flip completions by the KMS API. But drivers can also use it for their
 * own needs, e.g. to signal completion of rendering.
 *
 * The memory mapping implementation will vary depending on how the driver
 * manages memory. Legacy drivers will use the deprecated drm_legacy_mmap()
 * function, modern drivers should use one of the provided memory-manager
 * specific implementations. For GEM-based drivers this is drm_gem_mmap().
 *
 * No other file operations are supported by the DRM userspace API. Overall the
 * following is an example #file_operations structure::
 *
 *     static const example_drm_fops = {
 *             .owner = THIS_MODULE,
 *             .open = drm_open,
 *             .release = drm_release,
 *             .unlocked_ioctl = drm_ioctl,
 *     #ifdef CONFIG_COMPAT
 *             .compat_ioctl = drm_compat_ioctl,
 *     #endif
 *             .poll = drm_poll,
 *             .read = drm_read,
 *             .llseek = no_llseek,
 *             .mmap = drm_gem_mmap,
 *     };
 */

extern devclass_t drm_devclass;

static int drm_open_helper(struct cdev *kdev, int flags, int fmt, DRM_STRUCTPROC *p,
		    struct drm_device *dev, struct file *filp, struct drm_minor *minor);

static int drm_setup(struct drm_device * dev)
{
	int ret;

	if (dev->driver->firstopen &&
	    drm_core_check_feature(dev, DRIVER_LEGACY)) {
		ret = dev->driver->firstopen(dev);
		if (ret != 0)
			return ret;
	}

	dev->buf_use = 0;

	ret = drm_legacy_dma_setup(dev);
	if (ret < 0)
		return ret;

	init_waitqueue_head(&dev->lock.lock_queue);
	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		dev->irq_enabled = 0;
	dev->context_flag = 0;
	dev->last_context = 0;
	dev->if_version = 0;

	dev->buf_sigio = NULL;


	DRM_DEBUG("\n");
	return 0;
}

#define DRIVER_SOFTC(unit) \
	((struct drm_softc*)devclass_get_softc(drm_devclass, unit))

static inline int dev_minor(cdev_t x)
{
	return minor(x);
}

/**
 * drm_open - open method for DRM file
 * @inode: device inode
 * @filp: file pointer.
 *
 * This function must be used by drivers as their .open() #file_operations
 * method. It looks up the correct DRM device and instantiates all the per-file
 * resources for it.
 *
 * RETURNS:
 *
 * 0 on success or negative errno value on falure.
 */
// drm_open() is a file_operations function, not a dev_ops function
// int drm_open(struct inode *inode, struct file *filp)
int drm_open(struct dev_open_args *ap)
{
	struct file *filp = ap->a_fp;
	struct inode *inode = filp->f_data;	/* A Linux inode is a Unix vnode */
	struct drm_device *dev;
	struct drm_minor *minor;
	struct cdev *kdev = ap->a_head.a_dev;
	int flags = ap->a_oflags;
	int fmt = 0;
	struct thread *p = curthread;
	int retcode;
	int need_setup = 0;
#ifdef __DragonFly__
	struct drm_softc *softc = DRIVER_SOFTC(dev_minor(kdev));

	dev = softc->drm_driver_data;
#endif

	minor = drm_minor_acquire(iminor(inode));
	if (dev == NULL)
		return (ENXIO);
	if (!dev->open_count++)
		need_setup = 1;

	/* share address_space across all char-devs of a single device */
#if 0
	filp->f_mapping = dev->anon_inode->i_mapping;
#endif

	retcode = drm_open_helper(kdev, flags, fmt, p, dev, ap->a_fp, minor);
	if (retcode == 0) {
		DRM_LOCK(dev);
		device_busy(dev->dev->bsddev);
		DRM_UNLOCK(dev);
	}
	if (need_setup) {
		retcode = drm_setup(dev);
		if (retcode)
			goto err_undo;
	}
	return 0;

err_undo:
	dev->open_count--;
	drm_minor_release(minor);
	return retcode;
}
EXPORT_SYMBOL(drm_open);

/*
 * Check whether DRI will run on this CPU.
 *
 * \return non-zero if the DRI will run on this CPU, or zero otherwise.
 */

/*
 * Called whenever a process opens /dev/drm.
 *
 * \param filp file pointer.
 * \param minor acquired minor-object.
 * \return zero on success or a negative number on failure.
 *
 * Creates and initializes a drm_file structure for the file private data in \p
 * filp and add it into the double linked list in \p dev.
 */
static int drm_open_helper(struct cdev *kdev, int flags, int fmt, DRM_STRUCTPROC *p,
		    struct drm_device *dev, struct file *filp, struct drm_minor *minor)
{
	struct drm_file *priv;
	int ret;

	if (flags & O_EXCL)
		return EBUSY; /* No exclusive opens */

	DRM_DEBUG("pid = %d, device = %s\n", DRM_CURRENTPID, devtoname(kdev));

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	filp->private_data = priv;
	priv->filp = filp;
	priv->pid = p->td_proc->p_pid;
	priv->minor = minor;
	priv->dev		= dev;

	/* for compatibility root is always authenticated */
	priv->authenticated = capable(CAP_SYS_ADMIN);
	priv->lock_count = 0;

	INIT_LIST_HEAD(&priv->lhead);
	INIT_LIST_HEAD(&priv->fbs);
	lockinit(&priv->fbs_lock, "dpfl", 0, LK_CANRECURSE);
	INIT_LIST_HEAD(&priv->blobs);
	INIT_LIST_HEAD(&priv->pending_event_list);
	INIT_LIST_HEAD(&priv->event_list);
	init_waitqueue_head(&priv->event_wait);
	priv->event_space = 4096; /* set aside 4k for event buffer */

	lockinit(&priv->event_read_lock, "dperl", 0, LK_CANRECURSE);

	if (drm_core_check_feature(dev, DRIVER_GEM))
		drm_gem_open(dev, priv);

	if (dev->driver->open) {
		/* shared code returns -errno */
		ret = -dev->driver->open(dev, priv);
		if (ret != 0) {
			kfree(priv);
			return ret;
		}
	}

	/* first opener automatically becomes master */
	mutex_lock(&dev->master_mutex);
	priv->is_master = list_empty(&dev->filelist);
	mutex_unlock(&dev->master_mutex);

	mutex_lock(&dev->filelist_mutex);
	list_add(&priv->lhead, &dev->filelist);
	mutex_unlock(&dev->filelist_mutex);

	kdev->si_drv1 = dev;
	ret = devfs_set_cdevpriv(filp, priv, &drm_cdevpriv_dtor);
	if (ret != 0)
		drm_cdevpriv_dtor(priv);

	return ret;

#if 0
out_close:
#endif
	if (dev->driver->postclose)
		dev->driver->postclose(dev, priv);
	if (drm_core_check_feature(dev, DRIVER_GEM))
		drm_gem_release(dev, priv);
	kfree(priv);
	filp->private_data = NULL;
	return ret;
}

static void drm_events_release(struct drm_file *file_priv)
{
	struct drm_device *dev = file_priv->minor->dev;
	struct drm_pending_event *e, *et;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);

	/* Unlink pending events */
	list_for_each_entry_safe(e, et, &file_priv->pending_event_list,
				 pending_link) {
		list_del(&e->pending_link);
		e->file_priv = NULL;
	}

	/* Remove unconsumed events */
	list_for_each_entry_safe(e, et, &file_priv->event_list, link) {
		list_del(&e->link);
		kfree(e);
	}

	spin_unlock_irqrestore(&dev->event_lock, flags);
}

/*
 * drm_legacy_dev_reinit
 *
 * Reinitializes a legacy/ums drm device in it's lastclose function.
 */
static void drm_legacy_dev_reinit(struct drm_device *dev)
{
	if (dev->irq_enabled)
		drm_irq_uninstall(dev);

	mutex_lock(&dev->struct_mutex);

	drm_legacy_agp_clear(dev);

	drm_legacy_sg_cleanup(dev);
#if 0
	drm_legacy_vma_flush(dev);
#endif
	drm_legacy_dma_takedown(dev);

	mutex_unlock(&dev->struct_mutex);

	dev->sigdata.lock = NULL;

	dev->context_flag = 0;
	dev->last_context = 0;
	dev->if_version = 0;

	DRM_DEBUG("lastclose completed\n");
}

/*
 * Take down the DRM device.
 *
 * \param dev DRM device structure.
 *
 * Frees every resource in \p dev.
 *
 * \sa drm_device
 */
void drm_lastclose(struct drm_device * dev)
{
	DRM_DEBUG("\n");

	if (dev->driver->lastclose)
		dev->driver->lastclose(dev);
	DRM_DEBUG("driver lastclose completed\n");

	if (dev->irq_enabled && !drm_core_check_feature(dev, DRIVER_MODESET))
		drm_irq_uninstall(dev);

	mutex_lock(&dev->struct_mutex);

	if (dev->unique) {
		kfree(dev->unique);
		dev->unique = NULL;
		dev->unique_len = 0;
	}

	drm_legacy_agp_clear(dev);

	drm_legacy_sg_cleanup(dev);
	drm_legacy_dma_takedown(dev);

	if (dev->lock.hw_lock) {
		dev->lock.hw_lock = NULL; /* SHM removed */
		dev->lock.file_priv = NULL;
		wakeup(&dev->lock.lock_queue);
	}

	mutex_unlock(&dev->struct_mutex);

	DRM_DEBUG("lastclose completed\n");

	if (drm_core_check_feature(dev, DRIVER_LEGACY))
		drm_legacy_dev_reinit(dev);
}

/**
 * drm_release - release method for DRM file
 * @inode: device inode
 * @filp: file pointer.
 *
 * This function must be used by drivers as their .release() #file_operations
 * method. It frees any resources associated with the open file, and if this is
 * the last open file for the DRM device also proceeds to call drm_lastclose().
 *
 * RETURNS:
 *
 * Always succeeds and returns 0.
 */
int drm_release(struct inode *inode, struct file *filp)
{
	struct drm_file *file_priv = filp->private_data;
	struct drm_minor *minor = file_priv->minor;
	struct drm_device *dev = minor->dev;

#ifdef __DragonFly__
	/* dev is not correctly set yet */
	return 0;
#endif

	mutex_lock(&drm_global_mutex);

	DRM_DEBUG("open_count = %d\n", dev->open_count);

	mutex_lock(&dev->filelist_mutex);
	list_del(&file_priv->lhead);
	mutex_unlock(&dev->filelist_mutex);

	if (dev->driver->preclose)
		dev->driver->preclose(dev, file_priv);

	/* ========================================================
	 * Begin inline drm_release
	 */

	DRM_DEBUG("\n");

	if (drm_core_check_feature(dev, DRIVER_LEGACY))
		drm_legacy_lock_release(dev, filp);

	if (drm_core_check_feature(dev, DRIVER_HAVE_DMA))
		drm_legacy_reclaim_buffers(dev, file_priv);

	drm_events_release(file_priv);

	if (drm_core_check_feature(dev, DRIVER_MODESET)) {
		drm_fb_release(file_priv);
		drm_property_destroy_user_blobs(dev, file_priv);
	}

	if (drm_core_check_feature(dev, DRIVER_GEM))
		drm_gem_release(dev, file_priv);

	drm_legacy_ctxbitmap_flush(dev, file_priv);

	if (drm_is_primary_client(file_priv))
		drm_master_release(file_priv);

	if (dev->driver->postclose)
		dev->driver->postclose(dev, file_priv);

	if (drm_core_check_feature(dev, DRIVER_PRIME))
		drm_prime_destroy_file_private(&file_priv->prime);

	WARN_ON(!list_empty(&file_priv->event_list));

#if 0	/* XXX: put_pid() not implemented */
	put_pid(file_priv->pid);
#endif
	kfree(file_priv);

	/* ========================================================
	 * End inline drm_release
	 */

	if (!--dev->open_count) {
		drm_lastclose(dev);
#if 0	/* XXX: drm_put_dev() not implemented */
		if (drm_device_is_unplugged(dev))
			drm_put_dev(dev);
#endif
	}
	mutex_unlock(&drm_global_mutex);

	drm_minor_release(minor);

	return 0;
}
EXPORT_SYMBOL(drm_release);

/**
 * drm_read - read method for DRM file
 * @filp: file pointer
 * @buffer: userspace destination pointer for the read
 * @count: count in bytes to read
 * @offset: offset to read
 *
 * This function must be used by drivers as their .read() #file_operations
 * method iff they use DRM events for asynchronous signalling to userspace.
 * Since events are used by the KMS API for vblank and page flip completion this
 * means all modern display drivers must use it.
 *
 * @offset is ignore, DRM events are read like a pipe. Therefore drivers also
 * must set the .llseek() #file_operation to no_llseek(). Polling support is
 * provided by drm_poll().
 *
 * This function will only ever read a full event. Therefore userspace must
 * supply a big enough buffer to fit any event to ensure forward progress. Since
 * the maximum event space is currently 4K it's recommended to just use that for
 * safety.
 *
 * RETURNS:
 *
 * Number of bytes read (always aligned to full events, and can be 0) or a
 * negative error code on failure.
 */
/*
ssize_t drm_read(struct file *filp, char __user *buffer,
		 size_t count, loff_t *offset)
*/
int drm_read(struct dev_read_args *ap)
{
	struct file *filp = ap->a_fp;
	struct cdev *kdev = ap->a_head.a_dev;
	struct uio *uio = ap->a_uio;
	size_t count = uio->uio_resid;
	struct drm_file *file_priv = filp->private_data;
	struct drm_device *dev = drm_get_device_from_kdev(kdev);
	int ret = 0;	/* drm_read() returns int in DragonFly */

	ret = mutex_lock_interruptible(&file_priv->event_read_lock);
	if (ret)
		return ret;

	for (;;) {
		struct drm_pending_event *e = NULL;

		spin_lock_irq(&dev->event_lock);
		if (!list_empty(&file_priv->event_list)) {
			e = list_first_entry(&file_priv->event_list,
					struct drm_pending_event, link);
			file_priv->event_space += e->event->length;
			list_del(&e->link);
		}
		spin_unlock_irq(&dev->event_lock);

		if (e == NULL) {
			if (ret) {
				ret = 0;	/* DragonFly expects a zero return value on success */
				break;
			}

			if (filp->f_flag & O_NONBLOCK) {
				ret = -EAGAIN;
				break;
			}

			mutex_unlock(&file_priv->event_read_lock);
			ret = wait_event_interruptible(file_priv->event_wait,
						       !list_empty(&file_priv->event_list));
			if (ret >= 0)
				ret = mutex_lock_interruptible(&file_priv->event_read_lock);
			if (ret)
				return ret;
		} else {
			unsigned length = e->event->length;

			if (length > count - ret) {
put_back_event:
				spin_lock_irq(&dev->event_lock);
				file_priv->event_space -= length;
				list_add(&e->link, &file_priv->event_list);
				spin_unlock_irq(&dev->event_lock);
				break;
			}

			if (uiomove((caddr_t)e->event, length, uio)) {
				if (ret == 0)
					ret = -EFAULT;
				goto put_back_event;
			}

			ret += length;
			kfree(e);
		}
	}
	mutex_unlock(&file_priv->event_read_lock);

	return ret;
}
EXPORT_SYMBOL(drm_read);

/**
 * drm_poll - poll method for DRM file
 * @filp: file pointer
 * @wait: poll waiter table
 *
 * This function must be used by drivers as their .read() #file_operations
 * method iff they use DRM events for asynchronous signalling to userspace.
 * Since events are used by the KMS API for vblank and page flip completion this
 * means all modern display drivers must use it.
 *
 * See also drm_read().
 *
 * RETURNS:
 *
 * Mask of POLL flags indicating the current status of the file.
 */

static int
drmfilt(struct knote *kn, long hint)
{
	struct drm_file *file_priv = (struct drm_file *)kn->kn_hook;
	int ready = 0;

//	poll_wait(filp, &file_priv->event_wait, wait);

	if (!list_empty(&file_priv->event_list))
		ready = 1;

	return (ready);
}

static void
drmfilt_detach(struct knote *kn)
{
	struct drm_file *file_priv;
	struct drm_device *dev;
	struct klist *klist;

	file_priv = (struct drm_file *)kn->kn_hook;
	dev = file_priv->dev;

	klist = &file_priv->dkq.ki_note;
	knote_remove(klist, kn);
}

static struct filterops drmfiltops =
        { FILTEROP_MPSAFE | FILTEROP_ISFD, NULL, drmfilt_detach, drmfilt };

int
drm_kqfilter(struct dev_kqfilter_args *ap)
{
	struct file *filp = ap->a_fp;
	struct drm_file *file_priv = filp->private_data;
	struct knote *kn = ap->a_kn;
	struct klist *klist;

	ap->a_result = 0;

	switch (kn->kn_filter) {
	case EVFILT_READ:
	case EVFILT_WRITE:
		kn->kn_fop = &drmfiltops;
		kn->kn_hook = (caddr_t)file_priv;
		break;
	default:
		ap->a_result = EOPNOTSUPP;
		return (0);
	}

	klist = &file_priv->dkq.ki_note;
	knote_insert(klist, kn);

	return (0);
}

/**
 * drm_event_reserve_init_locked - init a DRM event and reserve space for it
 * @dev: DRM device
 * @file_priv: DRM file private data
 * @p: tracking structure for the pending event
 * @e: actual event data to deliver to userspace
 *
 * This function prepares the passed in event for eventual delivery. If the event
 * doesn't get delivered (because the IOCTL fails later on, before queuing up
 * anything) then the even must be cancelled and freed using
 * drm_event_cancel_free(). Successfully initialized events should be sent out
 * using drm_send_event() or drm_send_event_locked() to signal completion of the
 * asynchronous event to userspace.
 *
 * If callers embedded @p into a larger structure it must be allocated with
 * kmalloc and @p must be the first member element.
 *
 * This is the locked version of drm_event_reserve_init() for callers which
 * already hold dev->event_lock.
 *
 * RETURNS:
 *
 * 0 on success or a negative error code on failure.
 */
int drm_event_reserve_init_locked(struct drm_device *dev,
				  struct drm_file *file_priv,
				  struct drm_pending_event *p,
				  struct drm_event *e)
{
	if (file_priv->event_space < e->length)
		return -ENOMEM;

	file_priv->event_space -= e->length;

	p->event = e;
	list_add(&p->pending_link, &file_priv->pending_event_list);
	p->file_priv = file_priv;

	return 0;
}
EXPORT_SYMBOL(drm_event_reserve_init_locked);

/**
 * drm_event_reserve_init - init a DRM event and reserve space for it
 * @dev: DRM device
 * @file_priv: DRM file private data
 * @p: tracking structure for the pending event
 * @e: actual event data to deliver to userspace
 *
 * This function prepares the passed in event for eventual delivery. If the event
 * doesn't get delivered (because the IOCTL fails later on, before queuing up
 * anything) then the even must be cancelled and freed using
 * drm_event_cancel_free(). Successfully initialized events should be sent out
 * using drm_send_event() or drm_send_event_locked() to signal completion of the
 * asynchronous event to userspace.
 *
 * If callers embedded @p into a larger structure it must be allocated with
 * kmalloc and @p must be the first member element.
 *
 * Callers which already hold dev->event_lock should use
 * drm_event_reserve_init() instead.
 *
 * RETURNS:
 *
 * 0 on success or a negative error code on failure.
 */
int drm_event_reserve_init(struct drm_device *dev,
			   struct drm_file *file_priv,
			   struct drm_pending_event *p,
			   struct drm_event *e)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&dev->event_lock, flags);
	ret = drm_event_reserve_init_locked(dev, file_priv, p, e);
	spin_unlock_irqrestore(&dev->event_lock, flags);

	return ret;
}
EXPORT_SYMBOL(drm_event_reserve_init);

/**
 * drm_event_cancel_free - free a DRM event and release it's space
 * @dev: DRM device
 * @p: tracking structure for the pending event
 *
 * This function frees the event @p initialized with drm_event_reserve_init()
 * and releases any allocated space.
 */
void drm_event_cancel_free(struct drm_device *dev,
			   struct drm_pending_event *p)
{
	unsigned long flags;
	spin_lock_irqsave(&dev->event_lock, flags);
	if (p->file_priv) {
		p->file_priv->event_space += p->event->length;
		list_del(&p->pending_link);
	}
	spin_unlock_irqrestore(&dev->event_lock, flags);
	kfree(p);
}
EXPORT_SYMBOL(drm_event_cancel_free);

/**
 * drm_send_event_locked - send DRM event to file descriptor
 * @dev: DRM device
 * @e: DRM event to deliver
 *
 * This function sends the event @e, initialized with drm_event_reserve_init(),
 * to its associated userspace DRM file. Callers must already hold
 * dev->event_lock, see drm_send_event() for the unlocked version.
 */
void drm_send_event_locked(struct drm_device *dev, struct drm_pending_event *e)
{
	assert_spin_locked(&dev->event_lock);

	if (e->completion) {
		/* ->completion might disappear as soon as it signalled. */
		complete_all(e->completion);
		e->completion = NULL;
	}

	if (e->fence) {
		fence_signal(e->fence);
		fence_put(e->fence);
	}

	if (!e->file_priv) {
		kfree(e);
		return;
	}

	list_del(&e->pending_link);
	list_add_tail(&e->link,
		      &e->file_priv->event_list);
	wake_up_interruptible(&e->file_priv->event_wait);
#ifdef __DragonFly__
	KNOTE(&e->file_priv->dkq.ki_note, 0);
#endif

}
EXPORT_SYMBOL(drm_send_event_locked);

/**
 * drm_send_event - send DRM event to file descriptor
 * @dev: DRM device
 * @e: DRM event to deliver
 *
 * This function sends the event @e, initialized with drm_event_reserve_init(),
 * to its associated userspace DRM file. This function acquires dev->event_lock,
 * see drm_send_event_locked() for callers which already hold this lock.
 *
 * Note that the core will take care of unlinking and disarming events when the
 * corresponding DRM file is closed. Drivers need not worry about whether the
 * DRM file for this event still exists and can call this function upon
 * completion of the asynchronous work unconditionally.
 */
void drm_send_event(struct drm_device *dev, struct drm_pending_event *e)
{
	unsigned long irqflags;

	spin_lock_irqsave(&dev->event_lock, irqflags);
	drm_send_event_locked(dev, e);
	spin_unlock_irqrestore(&dev->event_lock, irqflags);
}
EXPORT_SYMBOL(drm_send_event);
