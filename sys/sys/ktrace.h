/*
 * Copyright (c) 1988, 1993
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
 *	@(#)ktrace.h	8.1 (Berkeley) 6/2/93
 * $FreeBSD: src/sys/sys/ktrace.h,v 1.19.2.3 2001/01/06 09:58:23 alfred Exp $
 */

#ifndef _SYS_KTRACE_H_
#define _SYS_KTRACE_H_

#ifndef _SYS_TYPES_H_
#include <sys/types.h>
#endif
#ifndef _SYS_TIME_H_
#include <sys/time.h>
#endif
#ifndef _SYS_SIGNAL_H_
#include <sys/signal.h>
#endif

struct proc;

struct ktrace_node {
	struct vnode	*kn_vp;
	int		kn_refs;
};

typedef struct ktrace_node *ktrace_node_t;


/*
 * operations to ktrace system call  (KTROP(op))
 */
#define KTROP_SET		0	/* set trace points */
#define KTROP_CLEAR		1	/* clear trace points */
#define KTROP_CLEARFILE		2	/* stop all tracing to file */
#define	KTROP(o)		((o)&3)	/* macro to extract operation */
/*
 * flags (ORed in with operation)
 */
#define KTRFLAG_DESCEND		4	/* perform op on all children too */

/*
 * ktrace record header
 */
struct ktr_header {
	int	ktr_len;		/* length of buf */
	short	ktr_type;		/* trace record type */
	short	ktr_flags;		/* reserved for future use */
	pid_t	ktr_pid;		/* process id */
	lwpid_t ktr_tid;		/* lwp id */
	char	ktr_comm[MAXCOMLEN+1];	/* command name */
	struct	timeval ktr_time;	/* timestamp */
	caddr_t ktr_buf;
};

#define KTRH_THREADED	0x0001		/* multiple threads present */

#define KTRH_CPUID_ENCODE(cpuid)	(((cpuid) & 0xFF) << 8)
#define KTRH_CPUID_DECODE(flags)	(((flags) >> 8) & 0xFF)

/*
 * Test for kernel trace point (MP SAFE)
 */
#define KTRPOINT(td, type)	\
	((td)->td_proc && ((td)->td_proc->p_traceflag & (1<<(type))) && \
	(((td)->td_proc->p_traceflag | (td)->td_lwp->lwp_traceflag) & \
	  KTRFAC_ACTIVE) == 0)


/*
 * ktrace record types
 */

/*
 * KTR_SYSCALL - system call record
 */
#define KTR_SYSCALL	1
struct ktr_syscall {
	short	ktr_code;		/* syscall number */
	short	ktr_narg;		/* number of arguments */
	/*
	 * Followed by ktr_narg register_t (can be more than 8)
	 */
	register_t	ktr_args[8];
};

/*
 * KTR_SYSRET - return from system call record
 */
#define KTR_SYSRET	2
struct ktr_sysret {
	short	ktr_code;
	short	ktr_eosys;
	int	ktr_error;
	register_t	ktr_retval;
};

/*
 * KTR_NAMEI - namei record
 */
#define KTR_NAMEI	3
	/* record contains pathname */

/*
 * KTR_GENIO - trace generic process i/o
 */
#define KTR_GENIO	4
#if defined(_KERNEL) || defined(_KERNEL_STRUCTURES)
#include <sys/_uio.h>		/* enum uio_rw, struct uio */
struct ktr_genio {
	int	ktr_fd;
	enum	uio_rw ktr_rw;
	/*
	 * followed by data successfully read/written
	 */
};
#endif

/*
 * KTR_PSIG - trace processed signal
 */
#define	KTR_PSIG	5
struct ktr_psig {
	int	signo;
	sig_t	action;
	int	code;
	sigset_t mask;
};

/*
 * KTR_CSW - trace context switches
 */
#define KTR_CSW		6
struct ktr_csw {
	int	out;	/* 1 if switch out, 0 if switch in */
	int	user;	/* 1 if usermode (ivcsw), 0 if kernel (vcsw) */
};

/*
 * KTR_USER - data comming from userland
 */
#define	KTR_USER_MAXLEN	2048	/* maximum length of passed data */
#define KTR_USER	7

/*
 * kernel trace points (in p_traceflag)
 */
#define KTRFAC_MASK	0x00ffffff
#define KTRFAC_SYSCALL	(1<<KTR_SYSCALL)
#define KTRFAC_SYSRET	(1<<KTR_SYSRET)
#define KTRFAC_NAMEI	(1<<KTR_NAMEI)
#define KTRFAC_GENIO	(1<<KTR_GENIO)
#define	KTRFAC_PSIG	(1<<KTR_PSIG)
#define KTRFAC_CSW	(1<<KTR_CSW)
#define KTRFAC_USER	(1<<KTR_USER)
/*
 * trace flags (also in p_traceflags)
 */
#define KTRFAC_ROOT	0x80000000	/* root set this trace */
#define KTRFAC_INHERIT	0x40000000	/* pass trace flags to children */
#define KTRFAC_ACTIVE	0x20000000	/* ktrace logging in progress, ignore */

#ifdef	_KERNEL
void	ktrnamei (struct lwp *,char *);
void	ktrcsw (struct lwp *,int,int);
void	ktrpsig (struct lwp *, int, sig_t, sigset_t *, int);
void	ktrgenio (struct lwp *, int, enum uio_rw, struct uio *, int);
void	ktrsyscall (struct lwp *, int, int narg, register_t args[]);
void	ktrsysret (struct lwp *, int, int, register_t);
void	ktrdestroy (struct ktrace_node **);
struct ktrace_node *ktrinherit (struct ktrace_node *);

#else

#include <sys/cdefs.h>

__BEGIN_DECLS
int	ktrace (const char *, int, int, pid_t);
int	utrace (const void *, size_t);
__END_DECLS

#endif

#endif
