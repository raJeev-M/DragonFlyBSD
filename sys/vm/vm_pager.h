/*
 * Copyright (c) 1990 University of Utah.
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department.
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
 *	@(#)vm_pager.h	8.4 (Berkeley) 1/12/94
 * $FreeBSD: src/sys/vm/vm_pager.h,v 1.24.2.2 2002/12/31 09:34:51 dillon Exp $
 */

/*
 * Pager routine interface definition.
 */

#ifndef	_VM_VM_PAGER_H_
#define	_VM_VM_PAGER_H_

#ifndef _SYS_QUEUE_H_
#include <sys/queue.h>
#endif
#ifndef _VM_VM_PAGE_H_
#include <vm/vm_page.h>
#endif
#ifndef _VM_VM_OBJECT_H_
#include <vm/vm_object.h>
#endif

#ifdef _KERNEL
TAILQ_HEAD(pagerlst, vm_object);

struct buf;
struct bio;

struct pagerops {
	void (*pgo_dealloc) (vm_object_t);
	int (*pgo_getpage) (vm_object_t, vm_page_t *, int);
	void (*pgo_putpages) (vm_object_t, vm_page_t *, int, int, int *);
	boolean_t (*pgo_haspage) (vm_object_t, vm_pindex_t);
};
#endif	/* _KERNEL */

/*
 * get/put return values
 * OK	 operation was successful
 * BAD	 specified data was out of the accepted range
 * FAIL	 specified data was in range, but doesn't exist
 * PEND	 operations was initiated but not completed
 * ERROR error while accessing data that is in range and exists
 * AGAIN temporary resource shortage prevented operation from happening
 */
#define	VM_PAGER_OK	0
#define	VM_PAGER_BAD	1
#define	VM_PAGER_FAIL	2
#define	VM_PAGER_PEND	3
#define	VM_PAGER_ERROR	4
#define VM_PAGER_AGAIN	5

#define	VM_PAGER_PUT_SYNC		0x0001
#define	VM_PAGER_PUT_INVAL		0x0002
#define	VM_PAGER_IGNORE_CLEANCHK	0x0004
#define	VM_PAGER_CLUSTER_OK		0x0008
#define	VM_PAGER_TRY_TO_CACHE		0x0010
#define	VM_PAGER_ALLOW_ACTIVE		0x0020

#ifdef _KERNEL

struct vnode;

extern struct vm_map pager_map;
extern int pager_map_size;
extern struct pagerops *pagertab[];

vm_object_t default_pager_alloc(void *, off_t, vm_prot_t, off_t);
vm_object_t dev_pager_alloc(void *, off_t, vm_prot_t, off_t);
vm_object_t phys_pager_alloc(void *, off_t, vm_prot_t, off_t);
vm_object_t swap_pager_alloc(void *, off_t, vm_prot_t, off_t);
vm_object_t vnode_pager_alloc (void *, off_t, vm_prot_t, off_t, int, int);
vm_object_t vnode_pager_reference (struct vnode *);

void vm_pager_deallocate (vm_object_t);
static __inline int vm_pager_get_page (vm_object_t, vm_page_t *, int);
static __inline boolean_t vm_pager_has_page (vm_object_t, vm_pindex_t);
vm_object_t vm_pager_object_lookup(struct pagerlst *, void *);
void vm_pager_sync (void);
struct buf *getchainbuf(struct buf *bp, struct vnode *vp, int flags);
void flushchainbuf(struct buf *nbp);
void waitchainbuf(struct buf *bp, int count, int done);
void autochaindone(struct buf *bp);
void swap_pager_strategy(vm_object_t object, struct bio *bio);
void swap_pager_unswapped (vm_page_t m);

/*
 * vm_page_get_pages:
 *
 * Retrieve the contents of the page from the object pager.  Note that the
 * object pager might replace the page.
 *
 * If the pagein was successful, we must fully validate it so it can be
 * memory mapped.
 */

static __inline int
vm_pager_get_page(vm_object_t object, vm_page_t *m, int seqaccess)
{
	int r;

	r = (*pagertab[object->type]->pgo_getpage)(object, m, seqaccess);
	if (r == VM_PAGER_OK && (*m)->valid != VM_PAGE_BITS_ALL) {
		vm_page_zero_invalid(*m, TRUE);
	}
	return(r);
}

static __inline void
vm_pager_put_pages(
	vm_object_t object,
	vm_page_t *m,
	int count,
	int flags,
	int *rtvals
) {
	(*pagertab[object->type]->pgo_putpages)
	    (object, m, count, flags, rtvals);
}

/*
 *	vm_pager_haspage
 *
 *	Check to see if an object's pager has the requested page.  The
 *	object's pager will also set before and after to give the caller
 *	some idea of the number of pages before and after the requested
 *	page can be I/O'd efficiently.
 *
 *	This routine does not have to be called at any particular spl.
 */

static __inline boolean_t
vm_pager_has_page(vm_object_t object, vm_pindex_t offset)
{
	return ((*pagertab[object->type]->pgo_haspage)(object, offset));
}

struct cdev_pager_ops {
	int (*cdev_pg_fault)(vm_object_t vm_obj, vm_ooffset_t offset,
	    int prot, vm_page_t *mres);
	int (*cdev_pg_ctor)(void *handle, vm_ooffset_t size, vm_prot_t prot,
	    vm_ooffset_t foff, struct ucred *cred, u_short *color);
	void (*cdev_pg_dtor)(void *handle);
};

vm_object_t cdev_pager_allocate(void *handle, enum obj_type tp,
    struct cdev_pager_ops *ops, vm_ooffset_t size, vm_prot_t prot,
    vm_ooffset_t foff, struct ucred *cred);
vm_object_t cdev_pager_lookup(void *handle);
void cdev_pager_free_page(vm_object_t object, vm_page_t m);

#endif	/* _KERNEL */

#endif	/* _VM_VM_PAGER_H_ */
