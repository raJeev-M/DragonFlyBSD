/*-
 * Copyright (c) 2016 Tomohiro Kusumi <tkusumi@netbsd.org>
 * Copyright (c) 2016 The DragonFly Project
 * Copyright (c) 2014 The FreeBSD Foundation
 * All rights reserved.
 *
 * This software was developed by Edward Tomasz Napierala under sponsorship
 * from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */
/*-
 * Copyright (c) 1989, 1991, 1993, 1995
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Rick Macklem at The University of Guelph.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/queue.h>
#include <sys/signalvar.h>
#include <sys/refcount.h>
#include <sys/spinlock2.h>
#include <sys/kern_syscall.h>

#include "autofs.h"
#include "autofs_ioctl.h"

MALLOC_DEFINE(M_AUTOFS, "autofs", "Automounter filesystem");

struct objcache *autofs_request_objcache = NULL;
struct objcache *autofs_node_objcache = NULL;

static d_open_t		autofs_open;
static d_close_t	autofs_close;
static d_ioctl_t	autofs_ioctl;

struct dev_ops autofs_ops = {
	{ "autofs", 0, D_MPSAFE },
	.d_open		= autofs_open,
	.d_close	= autofs_close,
	.d_ioctl	= autofs_ioctl,
};

/*
 * List of signals that can interrupt an autofs trigger.
 */
static int autofs_sig_set[] = {
	SIGINT,
	SIGTERM,
	SIGHUP,
	SIGKILL,
	SIGQUIT
};

struct autofs_softc	*autofs_softc = NULL;

SYSCTL_NODE(_vfs, OID_AUTO, autofs, CTLFLAG_RD, 0, "Automounter filesystem");
int autofs_debug = 1;
TUNABLE_INT("vfs.autofs.debug", &autofs_debug);
SYSCTL_INT(_vfs_autofs, OID_AUTO, debug, CTLFLAG_RW, &autofs_debug, 1,
    "Enable debug messages");
#if 0
int autofs_mount_on_stat = 0;
TUNABLE_INT("vfs.autofs.mount_on_stat", &autofs_mount_on_stat);
SYSCTL_INT(_vfs_autofs, OID_AUTO, mount_on_stat, CTLFLAG_RW,
    &autofs_mount_on_stat, 0, "Trigger mount on stat(2) on mountpoint");
#endif
static int autofs_timeout = 30;
TUNABLE_INT("vfs.autofs.timeout", &autofs_timeout);
SYSCTL_INT(_vfs_autofs, OID_AUTO, timeout, CTLFLAG_RW, &autofs_timeout, 30,
    "Number of seconds to wait for automountd(8)");
static int autofs_cache = 600;
TUNABLE_INT("vfs.autofs.cache", &autofs_cache);
SYSCTL_INT(_vfs_autofs, OID_AUTO, cache, CTLFLAG_RW, &autofs_cache, 600,
    "Number of seconds to wait before reinvoking automountd(8) for any given "
    "file or directory");
static int autofs_retry_attempts = 3;
TUNABLE_INT("vfs.autofs.retry_attempts", &autofs_retry_attempts);
SYSCTL_INT(_vfs_autofs, OID_AUTO, retry_attempts, CTLFLAG_RW,
    &autofs_retry_attempts, 3, "Number of attempts before failing mount");
static int autofs_retry_delay = 1;
TUNABLE_INT("vfs.autofs.retry_delay", &autofs_retry_delay);
SYSCTL_INT(_vfs_autofs, OID_AUTO, retry_delay, CTLFLAG_RW, &autofs_retry_delay,
    1, "Number of seconds before retrying");
static int autofs_interruptible = 1;
TUNABLE_INT("vfs.autofs.interruptible", &autofs_interruptible);
SYSCTL_INT(_vfs_autofs, OID_AUTO, interruptible, CTLFLAG_RW,
    &autofs_interruptible, 1, "Allow requests to be interrupted by signal");

static __inline pid_t
proc_pgid(const struct proc *p)
{
	return (p->p_pgrp->pg_id);
}

static int
autofs_node_cmp(const struct autofs_node *a, const struct autofs_node *b)
{
	return (strcmp(a->an_name, b->an_name));
}

RB_GENERATE(autofs_node_tree, autofs_node, an_link, autofs_node_cmp);

bool
autofs_ignore_thread(void)
{
	struct proc *curp = curproc;

	if (autofs_softc->sc_dev_opened == false)
		return (false);

	lwkt_gettoken(&curp->p_token);
	if (autofs_softc->sc_dev_sid == proc_pgid(curp)) {
		lwkt_reltoken(&curp->p_token);
		return (true);
	}
	lwkt_reltoken(&curp->p_token);

	return (false);
}

char *
autofs_path(struct autofs_node *anp)
{
	struct autofs_mount *amp = anp->an_mount;
	size_t len;
	char *path, *tmp;

	path = kstrdup("", M_AUTOFS);
	for (; anp->an_parent != NULL; anp = anp->an_parent) {
		len = strlen(anp->an_name) + strlen(path) + 2;
		tmp = kmalloc(len, M_AUTOFS, M_WAITOK);
		ksnprintf(tmp, len, "%s/%s", anp->an_name, path);
		kfree(path, M_AUTOFS);
		path = tmp;
	}

	len = strlen(amp->am_on) + strlen(path) + 2;
	tmp = kmalloc(len, M_AUTOFS, M_WAITOK);
	ksnprintf(tmp, len, "%s/%s", amp->am_on, path);
	kfree(path, M_AUTOFS);
	path = tmp;

	return (path);
}

static void
autofs_task(void *context, int pending)
{
	struct autofs_request *ar = context;

	mtx_lock_ex_quick(&autofs_softc->sc_lock);
	AUTOFS_WARN("request %d for %s timed out after %d seconds",
	    ar->ar_id, ar->ar_path, autofs_timeout);

	ar->ar_error = ETIMEDOUT;
	ar->ar_wildcards = true;
	ar->ar_done = true;
	ar->ar_in_progress = false;
	cv_broadcast(&autofs_softc->sc_cv);
	mtx_unlock_ex(&autofs_softc->sc_lock);
}

bool
autofs_cached(struct autofs_node *anp, const char *component, int componentlen)
{
	struct autofs_mount *amp = anp->an_mount;

	KKASSERT(mtx_notlocked(&amp->am_lock));

	/*
	 * For root node we need to request automountd(8) assistance even
	 * if the node is marked as cached, but the requested top-level
	 * directory does not exist.  This is necessary for wildcard indirect
	 * map keys to work.  We don't do this if we know that there are
	 * no wildcards.
	 */
	if (anp->an_parent == NULL && componentlen != 0 && anp->an_wildcards) {
		int error;
		KKASSERT(amp->am_root == anp);
		mtx_lock_sh_quick(&amp->am_lock);
		error = autofs_node_find(anp, component, componentlen, NULL);
		mtx_unlock_sh(&amp->am_lock);
		if (error)
			return (false);
	}

	return (anp->an_cached);
}

static void
autofs_cache_callout(void *context)
{
	struct autofs_node *anp = context;

	autofs_node_uncache(anp);
}

void
autofs_flush(struct autofs_mount *amp)
{
	struct autofs_node *anp = amp->am_root;
	struct autofs_node *child;

	mtx_lock_ex_quick(&amp->am_lock);
	RB_FOREACH(child, autofs_node_tree, &anp->an_children)
		autofs_node_uncache(child);
	autofs_node_uncache(amp->am_root);
	mtx_unlock_ex(&amp->am_lock);

	AUTOFS_DEBUG("%s flushed", amp->am_on);
}

/*
 * The set/restore sigmask functions are used to (temporarily) overwrite
 * the thread sigmask during triggering.
 */
static void
autofs_set_sigmask(sigset_t *oldset)
{
	struct lwp *lp = curthread->td_lwp;
	sigset_t newset;
	int i;

	SIGFILLSET(newset);
	/* Remove the autofs set of signals from newset */
	lwkt_gettoken(&lp->lwp_token);
	for (i = 0; i < nitems(autofs_sig_set); i++) {
		/*
		 * But make sure we leave the ones already masked
		 * by the process, i.e. remove the signal from the
		 * temporary signalmask only if it wasn't already
		 * in sigmask.
		 */
		if (!SIGISMEMBER(lp->lwp_sigmask, autofs_sig_set[i]) &&
		    !SIGISMEMBER(lp->lwp_proc->p_sigacts->ps_sigignore,
		    autofs_sig_set[i]))
			SIGDELSET(newset, autofs_sig_set[i]);
	}
	kern_sigprocmask(SIG_SETMASK, &newset, oldset);
	lwkt_reltoken(&lp->lwp_token);
}

static void
autofs_restore_sigmask(sigset_t *set)
{
	kern_sigprocmask(SIG_SETMASK, set, NULL);
}

static int
autofs_trigger_one(struct autofs_node *anp, const char *component,
    int componentlen)
{
#define _taskqueue_thread (taskqueue_thread[mycpuid])
	struct autofs_mount *amp = anp->an_mount;
	struct autofs_request *ar;
	char *key, *path;
	int error = 0, request_error;
	bool wildcards;

	KKASSERT(mtx_islocked_ex(&autofs_softc->sc_lock));

	if (anp->an_parent == NULL) {
		key = kstrndup(component, componentlen, M_AUTOFS);
	} else {
		struct autofs_node *firstanp;
		for (firstanp = anp; firstanp->an_parent->an_parent != NULL;
		    firstanp = firstanp->an_parent)
			continue;
		key = kstrdup(firstanp->an_name, M_AUTOFS);
	}

	path = autofs_path(anp);

	TAILQ_FOREACH(ar, &autofs_softc->sc_requests, ar_next) {
		if (strcmp(ar->ar_path, path) || strcmp(ar->ar_key, key))
			continue;
		KASSERT(strcmp(ar->ar_from, amp->am_from) == 0,
		    ("from changed; %s != %s", ar->ar_from, amp->am_from));
		KASSERT(strcmp(ar->ar_prefix, amp->am_prefix) == 0,
		    ("prefix changed; %s != %s",
		     ar->ar_prefix, amp->am_prefix));
		KASSERT(strcmp(ar->ar_options, amp->am_options) == 0,
		    ("options changed; %s != %s",
		     ar->ar_options, amp->am_options));
		break;
	}

	if (ar != NULL) {
		refcount_acquire(&ar->ar_refcount);
	} else {
		/*
		 * All struct fields must be initialized.
		 */
		ar = objcache_get(autofs_request_objcache, M_WAITOK);
		ar->ar_mount = amp;
		ar->ar_id = autofs_softc->sc_last_request_id++;
		ar->ar_done = false;
		ar->ar_error = 0;
		ar->ar_wildcards = false;
		ar->ar_in_progress = false;
		strlcpy(ar->ar_from, amp->am_from, sizeof(ar->ar_from));
		strlcpy(ar->ar_path, path, sizeof(ar->ar_path));
		strlcpy(ar->ar_prefix, amp->am_prefix, sizeof(ar->ar_prefix));
		strlcpy(ar->ar_key, key, sizeof(ar->ar_key));
		strlcpy(ar->ar_options, amp->am_options,
		    sizeof(ar->ar_options));
		TIMEOUT_TASK_INIT(_taskqueue_thread, &ar->ar_task, 0,
		    autofs_task, ar);
		taskqueue_enqueue_timeout(_taskqueue_thread, &ar->ar_task,
		    autofs_timeout * hz);
		refcount_init(&ar->ar_refcount, 1);
		TAILQ_INSERT_TAIL(&autofs_softc->sc_requests, ar, ar_next);
	}

	cv_broadcast(&autofs_softc->sc_cv);
	while (ar->ar_done == false) {
		if (autofs_interruptible) {
			sigset_t oldset;
			autofs_set_sigmask(&oldset);
			error = cv_mtx_wait_sig(&autofs_softc->sc_cv,
			    &autofs_softc->sc_lock);
			autofs_restore_sigmask(&oldset);
			if (error) {
				AUTOFS_WARN("cv_mtx_wait_sig for %s failed "
				    "with error %d", ar->ar_path, error);
				break;
			}
		} else {
			cv_mtx_wait(&autofs_softc->sc_cv,
			    &autofs_softc->sc_lock);
		}
	}

	request_error = ar->ar_error;
	if (request_error)
		AUTOFS_WARN("request for %s completed with error %d",
		    ar->ar_path, request_error);

	wildcards = ar->ar_wildcards;

	/*
	 * Check if this is the last reference.
	 */
	if (refcount_release(&ar->ar_refcount)) {
		TAILQ_REMOVE(&autofs_softc->sc_requests, ar, ar_next);
		mtx_unlock_ex(&autofs_softc->sc_lock);
		taskqueue_cancel_timeout(_taskqueue_thread, &ar->ar_task, NULL);
		taskqueue_drain_timeout(_taskqueue_thread, &ar->ar_task);
		objcache_put(autofs_request_objcache, ar);
		mtx_lock_ex_quick(&autofs_softc->sc_lock);
	}

	/*
	 * Note that we do not do negative caching on purpose.  This
	 * way the user can retry access at any time, e.g. after fixing
	 * the failure reason, without waiting for cache timer to expire.
	 */
	if (error == 0 && request_error == 0 && autofs_cache > 0) {
		autofs_node_cache(anp);
		anp->an_wildcards = wildcards;
		callout_reset(&anp->an_callout, autofs_cache * hz,
		    autofs_cache_callout, anp);
	}

	kfree(key, M_AUTOFS);
	kfree(path, M_AUTOFS);

	if (error)
		return (error);
	return (request_error);
}

int
autofs_trigger(struct autofs_node *anp, const char *component, int componentlen)
{
	for (;;) {
		int error, dummy;

		error = autofs_trigger_one(anp, component, componentlen);
		if (error == 0) {
			anp->an_retries = 0;
			return (0);
		}
		if (error == EINTR || error == ERESTART) {
			AUTOFS_DEBUG("trigger interrupted by signal, "
			    "not retrying");
			anp->an_retries = 0;
			return (error);
		}
		anp->an_retries++;
		if (anp->an_retries >= autofs_retry_attempts) {
			AUTOFS_DEBUG("trigger failed %d times; returning "
			    "error %d", anp->an_retries, error);
			anp->an_retries = 0;
			return (error);

		}
		AUTOFS_DEBUG("trigger failed with error %d; will retry in "
		    "%d seconds, %d attempts left", error, autofs_retry_delay,
		    autofs_retry_attempts - anp->an_retries);
		mtx_unlock_ex(&autofs_softc->sc_lock);
		tsleep(&dummy, 0, "autofs_retry", autofs_retry_delay * hz);
		mtx_lock_ex_quick(&autofs_softc->sc_lock);
	}
}

static int
autofs_ioctl_request(struct autofs_daemon_request *adr)
{
	struct proc *curp = curproc;
	struct autofs_request *ar;

	mtx_lock_ex_quick(&autofs_softc->sc_lock);
	for (;;) {
		int error;
		TAILQ_FOREACH(ar, &autofs_softc->sc_requests, ar_next) {
			if (ar->ar_done || ar->ar_in_progress)
				continue;
			break;
		}
		if (ar != NULL)
			break; /* found (!done && !in_progress) */

		error = cv_mtx_wait_sig(&autofs_softc->sc_cv,
		    &autofs_softc->sc_lock);
		if (error) {
			mtx_unlock_ex(&autofs_softc->sc_lock);
			return (error);
		}
	}

	ar->ar_in_progress = true;

	adr->adr_id = ar->ar_id;
	strlcpy(adr->adr_from, ar->ar_from, sizeof(adr->adr_from));
	strlcpy(adr->adr_path, ar->ar_path, sizeof(adr->adr_path));
	strlcpy(adr->adr_prefix, ar->ar_prefix, sizeof(adr->adr_prefix));
	strlcpy(adr->adr_key, ar->ar_key, sizeof(adr->adr_key));
	strlcpy(adr->adr_options, ar->ar_options, sizeof(adr->adr_options));

	mtx_unlock_ex(&autofs_softc->sc_lock);

	lwkt_gettoken(&curp->p_token);
	autofs_softc->sc_dev_sid = proc_pgid(curp);
	lwkt_reltoken(&curp->p_token);

	return (0);
}

static int
autofs_ioctl_done(struct autofs_daemon_done *add)
{
	struct autofs_request *ar;

	mtx_lock_ex_quick(&autofs_softc->sc_lock);
	TAILQ_FOREACH(ar, &autofs_softc->sc_requests, ar_next)
		if (ar->ar_id == add->add_id)
			break;

	if (ar == NULL) {
		mtx_unlock_ex(&autofs_softc->sc_lock);
		AUTOFS_DEBUG("id %d not found", add->add_id);
		return (ESRCH);
	}

	ar->ar_error = add->add_error;
	ar->ar_wildcards = add->add_wildcards;
	ar->ar_done = true;
	ar->ar_in_progress = false;
	cv_broadcast(&autofs_softc->sc_cv);

	mtx_unlock_ex(&autofs_softc->sc_lock);

	return (0);
}

static int
autofs_open(struct dev_open_args *ap)
{
	mtx_lock_ex_quick(&autofs_softc->sc_lock);
	/*
	 * We must never block automountd(8) and its descendants, and we use
	 * session ID to determine that: we store session id of the process
	 * that opened the device, and then compare it with session ids
	 * of triggering processes.  This means running a second automountd(8)
	 * instance would break the previous one.  The check below prevents
	 * it from happening.
	 */
	if (autofs_softc->sc_dev_opened) {
		mtx_unlock_ex(&autofs_softc->sc_lock);
		return (EBUSY);
	}

	autofs_softc->sc_dev_opened = true;
	mtx_unlock_ex(&autofs_softc->sc_lock);

	return (0);
}

static int
autofs_close(struct dev_close_args *ap)
{
	mtx_lock_ex_quick(&autofs_softc->sc_lock);
	KASSERT(autofs_softc->sc_dev_opened, ("not opened?"));
	autofs_softc->sc_dev_opened = false;
	mtx_unlock_ex(&autofs_softc->sc_lock);

	return (0);
}

static int
autofs_ioctl(struct dev_ioctl_args *ap)
{
	unsigned long cmd = ap->a_cmd;
	void *arg = ap->a_data;

	KASSERT(autofs_softc->sc_dev_opened, ("not opened?"));

	switch (cmd) {
	case AUTOFSREQUEST:
		return (autofs_ioctl_request(
		    (struct autofs_daemon_request *)arg));
	case AUTOFSDONE:
		return (autofs_ioctl_done((struct autofs_daemon_done *)arg));
	default:
		AUTOFS_DEBUG("invalid cmd %lx", cmd);
		return (EINVAL);
	}
	return (EINVAL);
}
