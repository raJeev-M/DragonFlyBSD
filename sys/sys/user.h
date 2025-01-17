/*
 * Copyright (c) 1982, 1986, 1989, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
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
 *	@(#)user.h	8.2 (Berkeley) 9/23/93
 * $FreeBSD: src/sys/sys/user.h,v 1.24.2.1 2001/10/11 08:20:18 peter Exp $
 */

#ifndef _SYS_USER_H_
#define _SYS_USER_H_

/*
 * stuff that *used* to be included by user.h, or is now needed.  The
 * expectation here is that the user program wants to mess with kernel
 * structures.  To be sure we get kernel structures we have to define
 * _KERNEL_STRUCTURES.  Otherwise we might get the user version.
 *
 * This is a really aweful hack.  Fortunately nobody includes sys/user.h
 * unless they really, really, really need kinfo_proc.
 */
#ifndef _KERNEL_STRUCTURES
#define _KERNEL_STRUCTURES
#endif

#ifndef _SYS_TYPES_H_
#include <sys/types.h>
#endif
#ifndef _SYS_ERRNO_H_
#include <sys/errno.h>
#endif
#ifndef _SYS_TIME_H_
#include <sys/time.h>
#endif
#ifndef _SYS_RESOURCE_H_
#include <sys/resource.h>
#endif
#ifndef _SYS_UCRED_H_
#include <sys/ucred.h>
#endif
#ifndef _SYS__IOVEC_H_
#include <sys/_iovec.h>
#endif
#ifndef _SYS__UIO_H_
#include <sys/_uio.h>
#endif
#ifndef _SYS_PROC_H_
#include <sys/proc.h>
#endif
#ifndef _SYS_LOCK_H_
#include <sys/lock.h>		/* XXX */
#endif
#ifndef _VM_VM_H_
#include <vm/vm.h>		/* XXX */
#endif
#ifndef _VM_VM_PARAM_H_
#include <vm/vm_param.h>	/* XXX */
#endif
#ifndef _VM_PMAP_H_
#include <vm/pmap.h>		/* XXX */
#endif
#ifndef _VM_VM_MAP_H_
#include <vm/vm_map.h>		/* XXX */
#endif
#ifndef _SYS_RESOURCEVAR_H_
#include <sys/resourcevar.h>
#endif
#ifndef _SYS_SIGNALVAR_H_
#include <sys/signalvar.h>
#endif
#ifndef _MACHINE_PCB_H_
#include <machine/pcb.h>
#endif
#include <sys/kinfo.h>

#endif
