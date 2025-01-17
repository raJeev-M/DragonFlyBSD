/*
 * Copyright (c) 2012-2017 The DragonFly Project.  All rights reserved.
 * Copyright (c) 1999 Peter Wemm <peter@FreeBSD.org>.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>,
 * by Mihai Carabas <mihai.carabas@gmail.com>
 * and many others.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/queue.h>
#include <sys/proc.h>
#include <sys/rtprio.h>
#include <sys/uio.h>
#include <sys/sysctl.h>
#include <sys/resourcevar.h>
#include <sys/spinlock.h>
#include <sys/cpu_topology.h>
#include <sys/thread2.h>
#include <sys/spinlock2.h>

#include <sys/ktr.h>

#include <machine/cpu.h>
#include <machine/smp.h>

/*
 * Priorities.  Note that with 32 run queues per scheduler each queue
 * represents four priority levels.
 */

int dfly_rebalanced;

#define MAXPRI			128
#define PRIMASK			(MAXPRI - 1)
#define PRIBASE_REALTIME	0
#define PRIBASE_NORMAL		MAXPRI
#define PRIBASE_IDLE		(MAXPRI * 2)
#define PRIBASE_THREAD		(MAXPRI * 3)
#define PRIBASE_NULL		(MAXPRI * 4)

#define NQS	32			/* 32 run queues. */
#define PPQ	(MAXPRI / NQS)		/* priorities per queue */
#define PPQMASK	(PPQ - 1)

/*
 * NICE_QS	- maximum queues nice can shift the process
 * EST_QS	- maximum queues estcpu can shift the process
 *
 * ESTCPUPPQ	- number of estcpu units per priority queue
 * ESTCPUMAX	- number of estcpu units
 *
 * Remember that NICE runs over the whole -20 to +20 range.
 */
#define NICE_QS		24	/* -20 to +20 shift in whole queues */
#define EST_QS		20	/* 0-MAX shift in whole queues */
#define ESTCPUPPQ	512
#define ESTCPUMAX	(ESTCPUPPQ * EST_QS)
#define PRIO_RANGE	(PRIO_MAX - PRIO_MIN + 1)

#define ESTCPULIM(v)	min((v), ESTCPUMAX)

TAILQ_HEAD(rq, lwp);

#define lwp_priority	lwp_usdata.dfly.priority
#define lwp_forked	lwp_usdata.dfly.forked
#define lwp_rqindex	lwp_usdata.dfly.rqindex
#define lwp_estcpu	lwp_usdata.dfly.estcpu
#define lwp_estfast	lwp_usdata.dfly.estfast
#define lwp_uload	lwp_usdata.dfly.uload
#define lwp_rqtype	lwp_usdata.dfly.rqtype
#define lwp_qcpu	lwp_usdata.dfly.qcpu
#define lwp_rrcount	lwp_usdata.dfly.rrcount

static __inline int
lptouload(struct lwp *lp)
{
	int uload;

	uload = lp->lwp_estcpu / NQS;
	uload -= uload * lp->lwp_proc->p_nice / (PRIO_MAX + 1);

	return uload;
}

/*
 * DFly scheduler pcpu structure.  Note that the pcpu uload field must
 * be 64-bits to avoid overflowing in the situation where more than 32768
 * processes are on a single cpu's queue.  Since high-end systems can
 * easily run 900,000+ processes, we have to deal with it.
 */
struct usched_dfly_pcpu {
	struct spinlock spin;
	struct thread	*helper_thread;
	struct globaldata *gd;
	u_short		scancpu;
	short		upri;
	long		uload;		/* 64-bits to avoid overflow (1) */
	int		ucount;
	int		flags;
	struct lwp	*uschedcp;
	struct rq	queues[NQS];
	struct rq	rtqueues[NQS];
	struct rq	idqueues[NQS];
	u_int32_t	queuebits;
	u_int32_t	rtqueuebits;
	u_int32_t	idqueuebits;
	int		runqcount;
	int		cpuid;
	cpumask_t	cpumask;
	cpu_node_t	*cpunode;
} __cachealign;

/*
 * Reflecting bits in the global atomic masks allows us to avoid
 * a certain degree of global ping-ponging.
 */
#define DFLY_PCPU_RDYMASK	0x0001	/* reflect rdyprocmask */
#define DFLY_PCPU_CURMASK	0x0002	/* reflect curprocmask */

typedef struct usched_dfly_pcpu	*dfly_pcpu_t;

static void dfly_acquire_curproc(struct lwp *lp);
static void dfly_release_curproc(struct lwp *lp);
static void dfly_select_curproc(globaldata_t gd);
static void dfly_setrunqueue(struct lwp *lp);
static void dfly_setrunqueue_dd(dfly_pcpu_t rdd, struct lwp *lp);
static void dfly_schedulerclock(struct lwp *lp, sysclock_t period,
				sysclock_t cpstamp);
static void dfly_recalculate_estcpu(struct lwp *lp);
static void dfly_resetpriority(struct lwp *lp);
static void dfly_forking(struct lwp *plp, struct lwp *lp);
static void dfly_exiting(struct lwp *lp, struct proc *);
static void dfly_uload_update(struct lwp *lp);
static void dfly_yield(struct lwp *lp);
static void dfly_changeqcpu_locked(struct lwp *lp,
				dfly_pcpu_t dd, dfly_pcpu_t rdd);
static dfly_pcpu_t dfly_choose_best_queue(struct lwp *lp);
static dfly_pcpu_t dfly_choose_worst_queue(dfly_pcpu_t dd, int forceit);
static dfly_pcpu_t dfly_choose_queue_simple(dfly_pcpu_t dd, struct lwp *lp);
static void dfly_need_user_resched_remote(void *dummy);
static struct lwp *dfly_chooseproc_locked(dfly_pcpu_t rdd, dfly_pcpu_t dd,
					  struct lwp *chklp, int worst);
static void dfly_remrunqueue_locked(dfly_pcpu_t dd, struct lwp *lp);
static void dfly_setrunqueue_locked(dfly_pcpu_t dd, struct lwp *lp);
static void dfly_changedcpu(struct lwp *lp);

struct usched usched_dfly = {
	{ NULL },
	"dfly", "Original DragonFly Scheduler",
	NULL,			/* default registration */
	NULL,			/* default deregistration */
	dfly_acquire_curproc,
	dfly_release_curproc,
	dfly_setrunqueue,
	dfly_schedulerclock,
	dfly_recalculate_estcpu,
	dfly_resetpriority,
	dfly_forking,
	dfly_exiting,
	dfly_uload_update,
	NULL,			/* setcpumask not supported */
	dfly_yield,
	dfly_changedcpu
};

/*
 * We have NQS (32) run queues per scheduling class.  For the normal
 * class, there are 128 priorities scaled onto these 32 queues.  New
 * processes are added to the last entry in each queue, and processes
 * are selected for running by taking them from the head and maintaining
 * a simple FIFO arrangement.  Realtime and Idle priority processes have
 * and explicit 0-31 priority which maps directly onto their class queue
 * index.  When a queue has something in it, the corresponding bit is
 * set in the queuebits variable, allowing a single read to determine
 * the state of all 32 queues and then a ffs() to find the first busy
 * queue.
 *
 * curprocmask is used to publish cpus with assigned curprocs to the rest
 * of the cpus.  In certain situations curprocmask may leave a bit set
 * (e.g. a yield or a token-based yield) even though dd->uschedcp is
 * NULL'd out temporarily).
 */
					/* currently running a user process */
static cpumask_t dfly_curprocmask = CPUMASK_INITIALIZER_ALLONES;
static cpumask_t dfly_rdyprocmask;	/* ready to accept a user process */
static struct usched_dfly_pcpu dfly_pcpu[MAXCPU];
static struct sysctl_ctx_list usched_dfly_sysctl_ctx;
static struct sysctl_oid *usched_dfly_sysctl_tree;
static struct lock usched_dfly_config_lk = LOCK_INITIALIZER("usdfs", 0, 0);

/* Debug info exposed through debug.* sysctl */

static int usched_dfly_debug = -1;
SYSCTL_INT(_debug, OID_AUTO, dfly_scdebug, CTLFLAG_RW,
	   &usched_dfly_debug, 0,
	   "Print debug information for this pid");

static int usched_dfly_pid_debug = -1;
SYSCTL_INT(_debug, OID_AUTO, dfly_pid_debug, CTLFLAG_RW,
	   &usched_dfly_pid_debug, 0,
	   "Print KTR debug information for this pid");

static int usched_dfly_chooser = 0;
SYSCTL_INT(_debug, OID_AUTO, dfly_chooser, CTLFLAG_RW,
	   &usched_dfly_chooser, 0,
	   "Print KTR debug information for this pid");

/*
 * WARNING!
 *
 * The fork bias can have a large effect on the system in the face of a
 * make -j N or other high-forking applications.
 *
 * Larger values are much less invasive vs other things that
 * might be running in the system, but can cause exec chains
 * such as those typically generated by make to have higher
 * latencies in the face of modest load.
 *
 * Lower values are more invasive but have reduced latencies
 * for such exec chains.
 *
 *	make -j 10 buildkernel example, build times:
 *
 *	     +0	3:04
 *	     +1 3:14	-5.2%	<-- default
 *	     +2 3:22	-8.9%
 *
 * This issue occurs due to the way the scheduler affinity heuristics work.
 * There is no way to really 'fix' the affinity heuristics because when it
 * comes right down to it trying to instantly schedule a process on an
 * available cpu (even if it will become unavailable a microsecond later)
 * tends to cause processes to shift around between cpus and sockets too much
 * and breaks the affinity.
 *
 * NOTE: Heavily concurrent builds typically have enough things on the pan
 *	 that they remain time-efficient even with a higher bias.
 */
static int usched_dfly_forkbias = 1;
SYSCTL_INT(_debug, OID_AUTO, dfly_forkbias, CTLFLAG_RW,
	   &usched_dfly_forkbias, 0,
	   "Fork bias for estcpu in whole queues");

/*
 * Tunning usched_dfly - configurable through kern.usched_dfly.
 *
 * weight1 - Tries to keep threads on their current cpu.  If you
 *	     make this value too large the scheduler will not be
 *	     able to load-balance large loads.
 *
 *	     Generally set to a fairly low value, but high enough
 *	     such that estcpu jitter doesn't move threads around.
 *
 * weight2 - If non-zero, detects thread pairs undergoing synchronous
 *	     communications and tries to move them closer together.
 *	     The weight advantages the same package and socket and
 *	     disadvantages the same core and same cpu.
 *
 *	     WARNING!  Weight2 is a ridiculously sensitive parameter,
 *	     particularly against weight4.  change the default at your
 *	     peril.
 *
 * weight3 - Weighting based on the number of recently runnable threads
 *	     on the userland scheduling queue (ignoring their loads).
 *
 *	     A nominal value here prevents high-priority (low-load)
 *	     threads from accumulating on one cpu core when other
 *	     cores are available.
 *
 *	     This value should be left fairly small because low-load
 *	     high priority threads can still be mostly idle and too
 *	     high a value will kick cpu-bound processes off the cpu
 *	     unnecessarily.
 *
 * weight4 - Weighting based on availability of other logical cpus running
 *	     less important threads (by upri) than the thread we are trying
 *	     to schedule.
 *
 *	     This allows a thread to migrate to another nearby cpu if it
 *	     is unable to run on the current cpu based on the other cpu
 *	     being idle or running a less important (higher lwp_priority)
 *	     thread.  This value should be large enough to override weight1,
 *	     but not so large as to override weight2.
 *
 *	     This parameter generally ensures fairness at the cost of some
 *	     performance (if set to too high).  It should generally be just
 *	     a tad lower than weight2.
 *
 * weight5 - Weighting based on the relative amount of ram connected
 *	     to the node a cpu resides on.
 *
 *	     This value should remain fairly low to allow assymetric
 *	     NUMA nodes to get threads scheduled to them.  Setting a very
 *	     high level will prevent scheduling on assymetric NUMA nodes
 *	     with low amounts of directly-attached memory.
 *
 *	     Note that when testing e.g. N threads on a machine with N
 *	     cpu cores with assymtric NUMA nodes, a non-zero value will
 *	     cause some cpu threads on the low-priority NUMA nodes to remain
 *	     idle even when a few process threads are doubled-up on other
 *	     cpus.  But this is typically more ideal because it deschedules
 *	     low-priority NUMA nodes at lighter nodes.
 *
 *	     Values between 50 and 200 are recommended.  Default is 50.
 *
 * weight6 - rdd transfer weight hysteresis.  Defaults to 0, can be increased
 *	     to improve stabillity at the cost of more mis-schedules.
 *
 * ipc_smt - If enabled, advantage IPC pairing to sibling cpu threads.
 *	     If -1, automatic when load >= 1/2 ncpus (default).
 *
 * ipc_same- If enabled, advantage IPC pairing to the same logical cpu.
 *	     If -1, automatic when load >= ncpus (default).
 *
 * features - These flags can be set or cleared to enable or disable various
 *	      features.
 *
 *	      0x01	Enable idle-cpu pulling			(default)
 *	      0x02	Enable proactive pushing		(default)
 *	      0x04	Enable rebalancing rover		(default)
 *	      0x08	Enable more proactive pushing		(default)
 *	      0x10	(unassigned)
 *	      0x20	choose best cpu for forked process	(default)
 *	      0x40	choose current cpu for forked process
 *	      0x80	choose random cpu for forked process
 *
 *	     NOTE - The idea behind forking mechanic 0x20 is that most
 *		    fork()ing is either followed by an exec in the child,
 *		    or the parent wait*()s.  If the child is short-lived,
 *		    there is effectively an IPC dependency (td_wakefromcpu
 *		    is also set in kern_fork.c) and we want to implement
 *		    the weight2 behavior to reduce IPIs and to reduce CPU
 *		    cache ping-ponging.
 */
__read_mostly static int usched_dfly_smt = 0;
__read_mostly static int usched_dfly_cache_coherent = 0;
__read_mostly static int usched_dfly_weight1 = 30;  /* keep thread on cpu */
__read_mostly static int usched_dfly_weight2 = 180; /* IPC locality */
__read_mostly static int usched_dfly_weight3 = 10;  /* threads on queue */
__read_mostly static int usched_dfly_weight4 = 120; /* availability of cores */
__read_mostly static int usched_dfly_weight5 = 50;  /* node attached memory */
__read_mostly static int usched_dfly_weight6 = 0;   /* rdd transfer weight */
__read_mostly static int usched_dfly_features = 0x2f;	     /* allow pulls */
__read_mostly static int usched_dfly_fast_resched = PPQ / 2; /* delta pri */
__read_mostly static int usched_dfly_swmask = ~PPQMASK;	     /* allow pulls */
__read_mostly static int usched_dfly_rrinterval = (ESTCPUFREQ + 9) / 10;
__read_mostly static int usched_dfly_decay = 8;
__read_mostly static int usched_dfly_ipc_smt = -1;  /* IPC auto smt pair */
__read_mostly static int usched_dfly_ipc_same = -1; /* IPC auto same log cpu */
__read_mostly static long usched_dfly_node_mem;

/* KTR debug printings */

KTR_INFO_MASTER(usched);

#if !defined(KTR_USCHED_DFLY)
#define	KTR_USCHED_DFLY	KTR_ALL
#endif

KTR_INFO(KTR_USCHED_DFLY, usched, chooseproc, 0,
    "USCHED_DFLY(chooseproc: pid %d, old_cpuid %d, curr_cpuid %d)",
    pid_t pid, int old_cpuid, int curr);

/*
 * This function is called when the kernel intends to return to userland.
 * It is responsible for making the thread the current designated userland
 * thread for this cpu, blocking if necessary.
 *
 * The kernel will not depress our LWKT priority until after we return,
 * in case we have to shove over to another cpu.
 *
 * We must determine our thread's disposition before we switch away.  This
 * is very sensitive code.
 *
 * WARNING! THIS FUNCTION IS ALLOWED TO CAUSE THE CURRENT THREAD TO MIGRATE
 * TO ANOTHER CPU!  Because most of the kernel assumes that no migration will
 * occur, this function is called only under very controlled circumstances.
 */
static void
dfly_acquire_curproc(struct lwp *lp)
{
	globaldata_t gd;
	dfly_pcpu_t dd;
	dfly_pcpu_t rdd;
	thread_t td;
	int force_resched;

	/*
	 * Make sure we aren't sitting on a tsleep queue.
	 */
	td = lp->lwp_thread;
	crit_enter_quick(td);
	if (td->td_flags & TDF_TSLEEPQ)
		tsleep_remove(td);
	dfly_recalculate_estcpu(lp);

	gd = mycpu;
	dd = &dfly_pcpu[gd->gd_cpuid];

	/*
	 * Process any pending interrupts/ipi's, then handle reschedule
	 * requests.  dfly_release_curproc() will try to assign a new
	 * uschedcp that isn't us and otherwise NULL it out.
	 */
	force_resched = 0;
	if ((td->td_mpflags & TDF_MP_BATCH_DEMARC) &&
	    lp->lwp_rrcount >= usched_dfly_rrinterval / 2) {
		force_resched = 1;
	}

	if (user_resched_wanted()) {
		if (dd->uschedcp == lp)
			force_resched = 1;
		clear_user_resched();
		dfly_release_curproc(lp);
	}

	/*
	 * Loop until we are the current user thread.
	 *
	 * NOTE: dd spinlock not held at top of loop.
	 */
	if (dd->uschedcp == lp)
		lwkt_yield_quick();

	while (dd->uschedcp != lp) {
		/*
		 * Do not do a lwkt_yield_quick() here as it will prevent
		 * the lwp from being placed on the dfly_bsd runqueue for
		 * one cycle (possibly an entire round-robin), preventing
		 * it from being scheduled to another cpu.
		 */
		/* lwkt_yield_quick(); */

		if (usched_dfly_debug == lp->lwp_proc->p_pid)
			kprintf(" pid %d acquire curcpu %d (force %d) ",
				lp->lwp_proc->p_pid, gd->gd_cpuid,
				force_resched);


		spin_lock(&dd->spin);

		/* This lwp is an outcast; force reschedule. */
		if (__predict_false(
		    CPUMASK_TESTBIT(lp->lwp_cpumask, gd->gd_cpuid) == 0) &&
		    (rdd = dfly_choose_best_queue(lp)) != dd) {
			dfly_changeqcpu_locked(lp, dd, rdd);
			spin_unlock(&dd->spin);
			lwkt_deschedule(lp->lwp_thread);
			dfly_setrunqueue_dd(rdd, lp);
			lwkt_switch();
			gd = mycpu;
			dd = &dfly_pcpu[gd->gd_cpuid];
			if (usched_dfly_debug == lp->lwp_proc->p_pid)
				kprintf("SEL-A cpu %d\n", gd->gd_cpuid);
			continue;
		}

		/*
		 * We are not or are no longer the current lwp and a forced
		 * reschedule was requested.  Figure out the best cpu to
		 * run on (our current cpu will be given significant weight).
		 *
		 * Doing this on many cpus simultaneously leads to
		 * instability so pace the operation.
		 *
		 * (if a reschedule was not requested we want to move this
		 * step after the uschedcp tests).
		 */
		if (force_resched &&
		   (usched_dfly_features & 0x08) &&
		   (u_int)sched_ticks / 8 % ncpus == gd->gd_cpuid) {
			if ((rdd = dfly_choose_best_queue(lp)) != dd) {
				dfly_changeqcpu_locked(lp, dd, rdd);
				spin_unlock(&dd->spin);
				lwkt_deschedule(lp->lwp_thread);
				dfly_setrunqueue_dd(rdd, lp);
				lwkt_switch();
				gd = mycpu;
				dd = &dfly_pcpu[gd->gd_cpuid];
				if (usched_dfly_debug == lp->lwp_proc->p_pid)
					kprintf("SEL-B cpu %d\n", gd->gd_cpuid);
				continue;
			}
			if (usched_dfly_debug == lp->lwp_proc->p_pid)
				kprintf("(SEL-B same cpu) ");
		}

		/*
		 * Either no reschedule was requested or the best queue was
		 * dd, and no current process has been selected.  We can
		 * trivially become the current lwp on the current cpu.
		 */
		if (dd->uschedcp == NULL) {
			atomic_clear_int(&lp->lwp_thread->td_mpflags,
					 TDF_MP_DIDYIELD);
			if ((dd->flags & DFLY_PCPU_CURMASK) == 0) {
				ATOMIC_CPUMASK_ORBIT(dfly_curprocmask,
						     gd->gd_cpuid);
				dd->flags |= DFLY_PCPU_CURMASK;
			}
			dd->uschedcp = lp;
			dd->upri = lp->lwp_priority;
			KKASSERT(lp->lwp_qcpu == dd->cpuid);
			spin_unlock(&dd->spin);
			if (usched_dfly_debug == lp->lwp_proc->p_pid)
				kprintf("SEL-C cpu %d (same cpu)\n",
					gd->gd_cpuid);
			break;
		}

		/*
		 * Can we steal the current designated user thread?
		 *
		 * If we do the other thread will stall when it tries to
		 * return to userland, possibly rescheduling elsewhere.
		 * Set need_user_resched() to get the thread to cycle soonest.
		 *
		 * It is important to do a masked test to avoid the edge
		 * case where two near-equal-priority threads are constantly
		 * interrupting each other.
		 *
		 * In the exact match case another thread has already gained
		 * uschedcp and lowered its priority, if we steal it the
		 * other thread will stay stuck on the LWKT runq and not
		 * push to another cpu.  So don't steal on equal-priority even
		 * though it might appear to be more beneficial due to not
		 * having to switch back to the other thread's context.
		 *
		 * usched_dfly_fast_resched requires that two threads be
		 * significantly far apart in priority in order to interrupt.
		 *
		 * If better but not sufficiently far apart, the current
		 * uschedcp will be interrupted at the next scheduler clock.
		 */
		if (dd->uschedcp &&
		   (dd->upri & ~PPQMASK) >
		   (lp->lwp_priority & ~PPQMASK) + usched_dfly_fast_resched) {
			dd->uschedcp = lp;
			dd->upri = lp->lwp_priority;
			KKASSERT(lp->lwp_qcpu == dd->cpuid);
			need_user_resched();
			spin_unlock(&dd->spin);
			if (usched_dfly_debug == lp->lwp_proc->p_pid)
				kprintf("SEL-D cpu %d (same cpu)\n",
					gd->gd_cpuid);
			break;
		}

		/*
		 * Requeue us at lwp_priority, which recalculate_estcpu()
		 * set for us.  Reset the rrcount to force placement
		 * at the end of the queue.
		 *
		 * We used to move ourselves to the worst queue, but
		 * this creates a fairly serious priority inversion
		 * problem.
		 */
		if (lp->lwp_thread->td_mpflags & TDF_MP_DIDYIELD) {
			spin_unlock(&dd->spin);
			lp->lwp_rrcount = usched_dfly_rrinterval;
			lp->lwp_rqindex = (lp->lwp_priority & PRIMASK) / PPQ;

			lwkt_deschedule(lp->lwp_thread);
			dfly_setrunqueue_dd(dd, lp);
			atomic_clear_int(&lp->lwp_thread->td_mpflags,
					 TDF_MP_DIDYIELD);
			lwkt_switch();
			gd = mycpu;
			dd = &dfly_pcpu[gd->gd_cpuid];
			if (usched_dfly_debug == lp->lwp_proc->p_pid)
				kprintf("SEL-E cpu %d (requeue)\n",
					gd->gd_cpuid);
			continue;
		}

		/*
		 * We are not the current lwp, figure out the best cpu
		 * to run on (our current cpu will be given significant
		 * weight).  Loop on cpu change.
		 */
		if ((usched_dfly_features & 0x02) &&
		    force_resched == 0 &&
		    (rdd = dfly_choose_best_queue(lp)) != dd) {
			dfly_changeqcpu_locked(lp, dd, rdd);
			spin_unlock(&dd->spin);
			lwkt_deschedule(lp->lwp_thread);
			dfly_setrunqueue_dd(rdd, lp);
			lwkt_switch();
			gd = mycpu;
			dd = &dfly_pcpu[gd->gd_cpuid];
			if (usched_dfly_debug == lp->lwp_proc->p_pid)
				kprintf("SEL-F cpu %d (requeue new cpu)\n",
					gd->gd_cpuid);
			continue;
		}

		/*
		 * We cannot become the current lwp, place the lp on the
		 * run-queue of this or another cpu and deschedule ourselves.
		 *
		 * When we are reactivated we will have another chance.
		 *
		 * Reload after a switch or setrunqueue/switch possibly
		 * moved us to another cpu.
		 */
		spin_unlock(&dd->spin);
		lwkt_deschedule(lp->lwp_thread);
		dfly_setrunqueue_dd(dd, lp);
		lwkt_switch();
		gd = mycpu;
		dd = &dfly_pcpu[gd->gd_cpuid];
		if (usched_dfly_debug == lp->lwp_proc->p_pid)
			kprintf("SEL-G cpu %d (fallback setrunq)\n",
				gd->gd_cpuid);
	}
	if (usched_dfly_debug == lp->lwp_proc->p_pid)
		kprintf(" pid %d acquire DONE cpu %d\n",
			lp->lwp_proc->p_pid, gd->gd_cpuid);

	/*
	 * Make sure upri is synchronized, then yield to LWKT threads as
	 * needed before returning.  This could result in another reschedule.
	 * XXX
	 */
	crit_exit_quick(td);

	KKASSERT((lp->lwp_mpflags & LWP_MP_ONRUNQ) == 0);
}

/*
 * DFLY_RELEASE_CURPROC
 *
 * This routine detaches the current thread from the userland scheduler,
 * usually because the thread needs to run or block in the kernel (at
 * kernel priority) for a while.
 *
 * This routine is also responsible for selecting a new thread to
 * make the current thread.
 *
 * NOTE: This implementation differs from the dummy example in that
 * dfly_select_curproc() is able to select the current process, whereas
 * dummy_select_curproc() is not able to select the current process.
 * This means we have to NULL out uschedcp.
 *
 * Additionally, note that we may already be on a run queue if releasing
 * via the lwkt_switch() in dfly_setrunqueue().
 */
static void
dfly_release_curproc(struct lwp *lp)
{
	globaldata_t gd = mycpu;
	dfly_pcpu_t dd = &dfly_pcpu[gd->gd_cpuid];

	/*
	 * Make sure td_wakefromcpu is defaulted.  This will be overwritten
	 * by wakeup().
	 */
	if (dd->uschedcp == lp) {
		KKASSERT((lp->lwp_mpflags & LWP_MP_ONRUNQ) == 0);
		spin_lock(&dd->spin);
		if (dd->uschedcp == lp) {
			dd->uschedcp = NULL;	/* don't let lp be selected */
			dd->upri = PRIBASE_NULL;

			/*
			 * We're just going to set it again, avoid the global
			 * cache line ping-pong.
			 */
			if ((lp->lwp_thread->td_mpflags & TDF_MP_DIDYIELD) == 0) {
				if (dd->flags & DFLY_PCPU_CURMASK) {
					ATOMIC_CPUMASK_NANDBIT(dfly_curprocmask,
							       gd->gd_cpuid);
					dd->flags &= ~DFLY_PCPU_CURMASK;
				}
			}
			spin_unlock(&dd->spin);
			dfly_select_curproc(gd);
		} else {
			spin_unlock(&dd->spin);
		}
	}
}

/*
 * DFLY_SELECT_CURPROC
 *
 * Select a new current process for this cpu and clear any pending user
 * reschedule request.  The cpu currently has no current process.
 *
 * This routine is also responsible for equal-priority round-robining,
 * typically triggered from dfly_schedulerclock().  In our dummy example
 * all the 'user' threads are LWKT scheduled all at once and we just
 * call lwkt_switch().
 *
 * The calling process is not on the queue and cannot be selected.
 */
static
void
dfly_select_curproc(globaldata_t gd)
{
	dfly_pcpu_t dd = &dfly_pcpu[gd->gd_cpuid];
	struct lwp *nlp;
	int cpuid = gd->gd_cpuid;

	crit_enter_gd(gd);

	spin_lock(&dd->spin);
	nlp = dfly_chooseproc_locked(dd, dd, dd->uschedcp, 0);

	if (nlp) {
		if ((dd->flags & DFLY_PCPU_CURMASK) == 0) {
			ATOMIC_CPUMASK_ORBIT(dfly_curprocmask, cpuid);
			dd->flags |= DFLY_PCPU_CURMASK;
		}
		dd->upri = nlp->lwp_priority;
		dd->uschedcp = nlp;
#if 0
		dd->rrcount = 0;		/* reset round robin */
#endif
		spin_unlock(&dd->spin);
		lwkt_acquire(nlp->lwp_thread);
		lwkt_schedule(nlp->lwp_thread);
	} else {
		spin_unlock(&dd->spin);
	}
	crit_exit_gd(gd);
}

/*
 * Place the specified lwp on the user scheduler's run queue.  This routine
 * must be called with the thread descheduled.  The lwp must be runnable.
 * It must not be possible for anyone else to explicitly schedule this thread.
 *
 * The thread may be the current thread as a special case.
 */
static void
dfly_setrunqueue(struct lwp *lp)
{
	dfly_pcpu_t dd;
	dfly_pcpu_t rdd;

	/*
	 * First validate the process LWKT state.
	 */
	KASSERT(lp->lwp_stat == LSRUN, ("setrunqueue: lwp not LSRUN"));
	KASSERT((lp->lwp_mpflags & LWP_MP_ONRUNQ) == 0,
	    ("lwp %d/%d already on runq! flag %08x/%08x", lp->lwp_proc->p_pid,
	     lp->lwp_tid, lp->lwp_proc->p_flags, lp->lwp_flags));
	KKASSERT((lp->lwp_thread->td_flags & TDF_RUNQ) == 0);

	/*
	 * NOTE: dd/rdd do not necessarily represent the current cpu.
	 *	 Instead they may represent the cpu the thread was last
	 *	 scheduled on or inherited by its parent.
	 */
	dd = &dfly_pcpu[lp->lwp_qcpu];
	rdd = dd;

	/*
	 * This process is not supposed to be scheduled anywhere or assigned
	 * as the current process anywhere.  Assert the condition.
	 */
	KKASSERT(rdd->uschedcp != lp);

	/*
	 * Ok, we have to setrunqueue some target cpu and request a reschedule
	 * if necessary.
	 *
	 * We have to choose the best target cpu.  It might not be the current
	 * target even if the current cpu has no running user thread (for
	 * example, because the current cpu might be a hyperthread and its
	 * sibling has a thread assigned).
	 *
	 * If we just forked it is most optimal to run the child on the same
	 * cpu just in case the parent decides to wait for it (thus getting
	 * off that cpu).  As long as there is nothing else runnable on the
	 * cpu, that is.  If we did this unconditionally a parent forking
	 * multiple children before waiting (e.g. make -j N) leaves other
	 * cpus idle that could be working.
	 */
	if (lp->lwp_forked) {
		lp->lwp_forked = 0;
		if (usched_dfly_features & 0x20)
			rdd = dfly_choose_best_queue(lp);
		else if (usched_dfly_features & 0x40)
			rdd = &dfly_pcpu[lp->lwp_qcpu];
		else if (usched_dfly_features & 0x80)
			rdd = dfly_choose_queue_simple(rdd, lp);
		else if (dfly_pcpu[lp->lwp_qcpu].runqcount)
			rdd = dfly_choose_best_queue(lp);
		else
			rdd = &dfly_pcpu[lp->lwp_qcpu];
	} else {
		rdd = dfly_choose_best_queue(lp);
		/* rdd = &dfly_pcpu[lp->lwp_qcpu]; */
	}
	if (lp->lwp_qcpu != rdd->cpuid) {
		spin_lock(&dd->spin);
		dfly_changeqcpu_locked(lp, dd, rdd);
		spin_unlock(&dd->spin);
	}
	dfly_setrunqueue_dd(rdd, lp);
}

/*
 * Change qcpu to rdd->cpuid.  The dd the lp is CURRENTLY on must be
 * spin-locked on-call.  rdd does not have to be.
 */
static void
dfly_changeqcpu_locked(struct lwp *lp, dfly_pcpu_t dd, dfly_pcpu_t rdd)
{
	if (lp->lwp_qcpu != rdd->cpuid) {
		if (lp->lwp_mpflags & LWP_MP_ULOAD) {
			atomic_clear_int(&lp->lwp_mpflags, LWP_MP_ULOAD);
			atomic_add_long(&dd->uload, -lp->lwp_uload);
			atomic_add_int(&dd->ucount, -1);
		}
		lp->lwp_qcpu = rdd->cpuid;
	}
}

/*
 * Place lp on rdd's runqueue.  Nothing is locked on call.  This function
 * also performs all necessary ancillary notification actions.
 */
static void
dfly_setrunqueue_dd(dfly_pcpu_t rdd, struct lwp *lp)
{
	globaldata_t rgd;

	/*
	 * We might be moving the lp to another cpu's run queue, and once
	 * on the runqueue (even if it is our cpu's), another cpu can rip
	 * it away from us.
	 *
	 * TDF_MIGRATING might already be set if this is part of a
	 * remrunqueue+setrunqueue sequence.
	 */
	if ((lp->lwp_thread->td_flags & TDF_MIGRATING) == 0)
		lwkt_giveaway(lp->lwp_thread);

	rgd = rdd->gd;

	/*
	 * We lose control of the lp the moment we release the spinlock
	 * after having placed it on the queue.  i.e. another cpu could pick
	 * it up, or it could exit, or its priority could be further
	 * adjusted, or something like that.
	 *
	 * WARNING! rdd can point to a foreign cpu!
	 */
	spin_lock(&rdd->spin);
	dfly_setrunqueue_locked(rdd, lp);

	/*
	 * Potentially interrupt the currently-running thread
	 */
	if ((rdd->upri & ~PPQMASK) <= (lp->lwp_priority & ~PPQMASK)) {
		/*
		 * Currently running thread is better or same, do not
		 * interrupt.
		 */
		spin_unlock(&rdd->spin);
	} else if ((rdd->upri & ~PPQMASK) <= (lp->lwp_priority & ~PPQMASK) +
		   usched_dfly_fast_resched) {
		/*
		 * Currently running thread is not better, but not so bad
		 * that we need to interrupt it.  Let it run for one more
		 * scheduler tick.
		 */
		if (rdd->uschedcp &&
		    rdd->uschedcp->lwp_rrcount < usched_dfly_rrinterval) {
			rdd->uschedcp->lwp_rrcount = usched_dfly_rrinterval - 1;
		}
		spin_unlock(&rdd->spin);
	} else if (rgd == mycpu) {
		/*
		 * We should interrupt the currently running thread, which
		 * is on the current cpu.  However, if DIDYIELD is set we
		 * round-robin unconditionally and do not interrupt it.
		 */
		spin_unlock(&rdd->spin);
		if (rdd->uschedcp == NULL)
			wakeup_mycpu(rdd->helper_thread); /* XXX */
		if ((lp->lwp_thread->td_mpflags & TDF_MP_DIDYIELD) == 0)
			need_user_resched();
	} else {
		/*
		 * We should interrupt the currently running thread, which
		 * is on a different cpu.
		 */
		spin_unlock(&rdd->spin);
		lwkt_send_ipiq(rgd, dfly_need_user_resched_remote, NULL);
	}
}

/*
 * This routine is called from a systimer IPI.  It MUST be MP-safe and
 * the BGL IS NOT HELD ON ENTRY.  This routine is called at ESTCPUFREQ on
 * each cpu.
 */
static
void
dfly_schedulerclock(struct lwp *lp, sysclock_t period, sysclock_t cpstamp)
{
	globaldata_t gd = mycpu;
	dfly_pcpu_t dd = &dfly_pcpu[gd->gd_cpuid];

	/*
	 * Spinlocks also hold a critical section so there should not be
	 * any active.
	 */
	KKASSERT(gd->gd_spinlocks == 0 || dumping);

	/*
	 * If lp is NULL we might be contended and lwkt_switch() may have
	 * cycled into the idle thread.  Apply the tick to the current
	 * process on this cpu if it is contended.
	 */
	if (gd->gd_curthread == &gd->gd_idlethread) {
		lp = dd->uschedcp;
		if (lp && (lp->lwp_thread == NULL ||
			   lp->lwp_thread->td_contended == 0)) {
			lp = NULL;
		}
	}

	/*
	 * Dock thread for tick
	 */
	if (lp) {
		/*
		 * Do we need to round-robin?  We round-robin 10 times a
		 * second.  This should only occur for cpu-bound batch
		 * processes.
		 */
		if (++lp->lwp_rrcount >= usched_dfly_rrinterval)
			need_user_resched();

		/*
		 * Adjust estcpu upward using a real time equivalent
		 * calculation, and recalculate lp's priority.  Estcpu
		 * is increased such that it will cap-out over a period
		 * of one second.
		 */
		lp->lwp_estcpu = ESTCPULIM(lp->lwp_estcpu +
					   ESTCPUMAX / ESTCPUFREQ + 1);
		dfly_resetpriority(lp);
	}

	/*
	 * Rebalance two cpus every 8 ticks, pulling the worst thread
	 * from the worst cpu's queue into a rotating cpu number.
	 * Also require that the moving of the highest-load thread
	 * from rdd to dd does not cause the uload to cross over.
	 *
	 * This mechanic is needed because the push algorithms can
	 * steady-state in an non-optimal configuration.  We need to mix it
	 * up a little, even if it means breaking up a paired thread, so
	 * the push algorithms can rebalance the degenerate conditions.
	 * This portion of the algorithm exists to ensure stability at the
	 * selected weightings.
	 *
	 * Because we might be breaking up optimal conditions we do not want
	 * to execute this too quickly, hence we only rebalance approximately
	 * ~7-8 times per second.  The push's, on the otherhand, are capable
	 * moving threads to other cpus at a much higher rate.
	 *
	 * We choose the most heavily loaded thread from the worst queue
	 * in order to ensure that multiple heavy-weight threads on the same
	 * queue get broken up, and also because these threads are the most
	 * likely to be able to remain in place.  Hopefully then any pairings,
	 * if applicable, migrate to where these threads are.
	 */
	if ((usched_dfly_features & 0x04) &&
	    ((u_int)sched_ticks & 7) == 0 &&
	    (u_int)sched_ticks / 8 % ncpus == gd->gd_cpuid) {
		/*
		 * Our cpu is up.
		 */
		struct lwp *nlp;
		dfly_pcpu_t rdd;

		rdd = dfly_choose_worst_queue(dd, 1);
		if (rdd && dd->uload + usched_dfly_weight6 / 2 < rdd->uload) {
			spin_lock(&dd->spin);
			if (spin_trylock(&rdd->spin)) {
				nlp = dfly_chooseproc_locked(rdd, dd, NULL, 1);
				spin_unlock(&rdd->spin);
				if (nlp == NULL)
					spin_unlock(&dd->spin);
			} else {
				spin_unlock(&dd->spin);
				nlp = NULL;
			}
		} else {
			nlp = NULL;
		}
		/* dd->spin held if nlp != NULL */

		/*
		 * Either schedule it or add it to our queue.
		 */
		if (nlp &&
		    (nlp->lwp_priority & ~PPQMASK) < (dd->upri & ~PPQMASK)) {
			if ((dd->flags & DFLY_PCPU_CURMASK) == 0) {
				ATOMIC_CPUMASK_ORMASK(dfly_curprocmask,
						      dd->cpumask);
				dd->flags |= DFLY_PCPU_CURMASK;
			}
			dd->upri = nlp->lwp_priority;
			dd->uschedcp = nlp;
#if 0
			dd->rrcount = 0;	/* reset round robin */
#endif
			spin_unlock(&dd->spin);
			lwkt_acquire(nlp->lwp_thread);
			lwkt_schedule(nlp->lwp_thread);
		} else if (nlp) {
			dfly_setrunqueue_locked(dd, nlp);
			spin_unlock(&dd->spin);
		}
	}
}

/*
 * Called from acquire and from kern_synch's one-second timer (one of the
 * callout helper threads) with a critical section held.
 *
 * Adjust p_estcpu based on our single-cpu load, p_nice, and compensate for
 * overall system load.
 *
 * Note that no recalculation occurs for a process which sleeps and wakes
 * up in the same tick.  That is, a system doing thousands of context
 * switches per second will still only do serious estcpu calculations
 * ESTCPUFREQ times per second.
 */
static
void
dfly_recalculate_estcpu(struct lwp *lp)
{
	globaldata_t gd = mycpu;
	sysclock_t cpbase;
	sysclock_t ttlticks;
	int estcpu;
	int decay_factor;
	int ucount;

	/*
	 * We have to subtract periodic to get the last schedclock
	 * timeout time, otherwise we would get the upcoming timeout.
	 * Keep in mind that a process can migrate between cpus and
	 * while the scheduler clock should be very close, boundary
	 * conditions could lead to a small negative delta.
	 */
	cpbase = gd->gd_schedclock.time - gd->gd_schedclock.periodic;

	if (lp->lwp_slptime > 1) {
		/*
		 * Too much time has passed, do a coarse correction.
		 */
		lp->lwp_estcpu = lp->lwp_estcpu >> 1;
		dfly_resetpriority(lp);
		lp->lwp_cpbase = cpbase;
		lp->lwp_cpticks = 0;
		lp->lwp_estfast = 0;
	} else if (lp->lwp_cpbase != cpbase) {
		/*
		 * Adjust estcpu if we are in a different tick.  Don't waste
		 * time if we are in the same tick.
		 *
		 * First calculate the number of ticks in the measurement
		 * interval.  The ttlticks calculation can wind up 0 due to
		 * a bug in the handling of lwp_slptime  (as yet not found),
		 * so make sure we do not get a divide by 0 panic.
		 */
		ttlticks = (cpbase - lp->lwp_cpbase) /
			   gd->gd_schedclock.periodic;
		if ((ssysclock_t)ttlticks < 0) {
			ttlticks = 0;
			lp->lwp_cpbase = cpbase;
		}
		if (ttlticks < 4)
			return;
		updatepcpu(lp, lp->lwp_cpticks, ttlticks);

		/*
		 * Calculate instant estcpu based percentage of (one) cpu
		 * used and exponentially average it into the current
		 * lwp_estcpu.
		 */
		ucount = dfly_pcpu[lp->lwp_qcpu].ucount;
		estcpu = lp->lwp_cpticks * ESTCPUMAX / ttlticks;

		/*
		 * The higher ttlticks gets, the more meaning the calculation
		 * has and the smaller our decay_factor in the exponential
		 * average.
		 *
		 * The uload calculation has been removed because it actually
		 * makes things worse, causing processes which use less cpu
		 * (such as a browser) to be pumped up and treated the same
		 * as a cpu-bound process (such as a make).  The same effect
		 * can occur with sufficient load without the uload
		 * calculation, but occurs less quickly and takes more load.
		 * In addition, the less cpu a process uses the smaller the
		 * effect of the overload.
		 */
		if (ttlticks >= hz)
			decay_factor = 1;
		else
			decay_factor = hz - ttlticks;

		lp->lwp_estcpu = ESTCPULIM(
				(lp->lwp_estcpu * ttlticks + estcpu) /
				(ttlticks + 1));
		dfly_resetpriority(lp);
		lp->lwp_cpbase += ttlticks * gd->gd_schedclock.periodic;
		lp->lwp_cpticks = 0;
	}
}

/*
 * Compute the priority of a process when running in user mode.
 * Arrange to reschedule if the resulting priority is better
 * than that of the current process.
 *
 * This routine may be called with any process.
 *
 * This routine is called by fork1() for initial setup with the process of
 * the run queue, and also may be called normally with the process on or
 * off the run queue.
 */
static void
dfly_resetpriority(struct lwp *lp)
{
	dfly_pcpu_t rdd;
	int newpriority;
	u_short newrqtype;
	int rcpu;
	int checkpri;
	int estcpu;
	int delta_uload;

	crit_enter();

	/*
	 * Lock the scheduler (lp) belongs to.  This can be on a different
	 * cpu.  Handle races.  This loop breaks out with the appropriate
	 * rdd locked.
	 */
	for (;;) {
		rcpu = lp->lwp_qcpu;
		cpu_ccfence();
		rdd = &dfly_pcpu[rcpu];
		spin_lock(&rdd->spin);
		if (rcpu == lp->lwp_qcpu)
			break;
		spin_unlock(&rdd->spin);
	}

	/*
	 * Calculate the new priority and queue type
	 */
	newrqtype = lp->lwp_rtprio.type;

	switch(newrqtype) {
	case RTP_PRIO_REALTIME:
	case RTP_PRIO_FIFO:
		newpriority = PRIBASE_REALTIME +
			     (lp->lwp_rtprio.prio & PRIMASK);
		break;
	case RTP_PRIO_NORMAL:
		/*
		 * Calculate the new priority.
		 *
		 * nice contributes up to NICE_QS queues (typ 32 - full range)
		 * estcpu contributes up to EST_QS queues (typ 24)
		 *
		 * A nice +20 process receives 1/10 cpu vs nice+0.  Niced
		 * process more than 20 apart may receive no cpu, so cpu
		 * bound nice -20 can prevent a nice +5 from getting any
		 * cpu.  A nice+0, being in the middle, always gets some cpu
		 * no matter what.
		 */
		estcpu = lp->lwp_estcpu;
		newpriority = (lp->lwp_proc->p_nice - PRIO_MIN) *
			      (NICE_QS * PPQ) / PRIO_RANGE;
		newpriority += estcpu * PPQ / ESTCPUPPQ;
		if (newpriority < 0)
			newpriority = 0;
		if (newpriority >= MAXPRI)
			newpriority = MAXPRI - 1;
		newpriority += PRIBASE_NORMAL;
		break;
	case RTP_PRIO_IDLE:
		newpriority = PRIBASE_IDLE + (lp->lwp_rtprio.prio & PRIMASK);
		break;
	case RTP_PRIO_THREAD:
		newpriority = PRIBASE_THREAD + (lp->lwp_rtprio.prio & PRIMASK);
		break;
	default:
		panic("Bad RTP_PRIO %d", newrqtype);
		/* NOT REACHED */
	}

	/*
	 * The LWKT scheduler doesn't dive usched structures, give it a hint
	 * on the relative priority of user threads running in the kernel.
	 * The LWKT scheduler will always ensure that a user thread running
	 * in the kernel will get cpu some time, regardless of its upri,
	 * but can decide not to instantly switch from one kernel or user
	 * mode user thread to a kernel-mode user thread when it has a less
	 * desireable user priority.
	 *
	 * td_upri has normal sense (higher values are more desireable), so
	 * negate it (this is a different field lp->lwp_priority)
	 */
	lp->lwp_thread->td_upri = -(newpriority & usched_dfly_swmask);

	/*
	 * The newpriority incorporates the queue type so do a simple masked
	 * check to determine if the process has moved to another queue.  If
	 * it has, and it is currently on a run queue, then move it.
	 *
	 * Since uload is ~PPQMASK masked, no modifications are necessary if
	 * we end up in the same run queue.
	 *
	 * Reset rrcount if moving to a higher-priority queue, otherwise
	 * retain rrcount.
	 */
	if ((lp->lwp_priority ^ newpriority) & ~PPQMASK) {
		if (lp->lwp_priority < newpriority)
			lp->lwp_rrcount = 0;
		if (lp->lwp_mpflags & LWP_MP_ONRUNQ) {
			dfly_remrunqueue_locked(rdd, lp);
			lp->lwp_priority = newpriority;
			lp->lwp_rqtype = newrqtype;
			lp->lwp_rqindex = (newpriority & PRIMASK) / PPQ;
			dfly_setrunqueue_locked(rdd, lp);
			checkpri = 1;
		} else {
			lp->lwp_priority = newpriority;
			lp->lwp_rqtype = newrqtype;
			lp->lwp_rqindex = (newpriority & PRIMASK) / PPQ;
			checkpri = 0;
		}
	} else {
		/*
		 * In the same PPQ, uload cannot change.
		 */
		lp->lwp_priority = newpriority;
		checkpri = 1;
		rcpu = -1;
	}

	/*
	 * Adjust effective load.
	 *
	 * Calculate load then scale up or down geometrically based on p_nice.
	 * Processes niced up (positive) are less important, and processes
	 * niced downard (negative) are more important.  The higher the uload,
	 * the more important the thread.
	 */
	/* 0-511, 0-100% cpu */
	delta_uload = lptouload(lp);
	delta_uload -= lp->lwp_uload;
	if (lp->lwp_uload + delta_uload < -32767) {
		delta_uload = -32768 - lp->lwp_uload;
	} else if (lp->lwp_uload + delta_uload > 32767) {
		delta_uload = 32767 - lp->lwp_uload;
	}
	lp->lwp_uload += delta_uload;
	if (lp->lwp_mpflags & LWP_MP_ULOAD)
		atomic_add_long(&dfly_pcpu[lp->lwp_qcpu].uload, delta_uload);

	/*
	 * Determine if we need to reschedule the target cpu.  This only
	 * occurs if the LWP is already on a scheduler queue, which means
	 * that idle cpu notification has already occured.  At most we
	 * need only issue a need_user_resched() on the appropriate cpu.
	 *
	 * The LWP may be owned by a CPU different from the current one,
	 * in which case dd->uschedcp may be modified without an MP lock
	 * or a spinlock held.  The worst that happens is that the code
	 * below causes a spurious need_user_resched() on the target CPU
	 * and dd->pri to be wrong for a short period of time, both of
	 * which are harmless.
	 *
	 * If checkpri is 0 we are adjusting the priority of the current
	 * process, possibly higher (less desireable), so ignore the upri
	 * check which will fail in that case.
	 */
	if (rcpu >= 0) {
		if (CPUMASK_TESTBIT(dfly_rdyprocmask, rcpu) &&
		    (checkpri == 0 ||
		     (rdd->upri & ~PRIMASK) >
		     (lp->lwp_priority & ~PRIMASK))) {
			if (rcpu == mycpu->gd_cpuid) {
				spin_unlock(&rdd->spin);
				need_user_resched();
			} else {
				spin_unlock(&rdd->spin);
				lwkt_send_ipiq(globaldata_find(rcpu),
					       dfly_need_user_resched_remote,
					       NULL);
			}
		} else {
			spin_unlock(&rdd->spin);
		}
	} else {
		spin_unlock(&rdd->spin);
	}
	crit_exit();
}

static
void
dfly_yield(struct lwp *lp)
{
	if (lp->lwp_qcpu != mycpu->gd_cpuid)
		return;
	KKASSERT(lp == curthread->td_lwp);

	/*
	 * Don't set need_user_resched() or mess with rrcount or anything.
	 * the TDF flag will override everything as long as we release.
	 */
	atomic_set_int(&lp->lwp_thread->td_mpflags, TDF_MP_DIDYIELD);
	dfly_release_curproc(lp);
}

/*
 * Thread was forcefully migrated to another cpu.  Normally forced migrations
 * are used for iterations and the kernel returns to the original cpu before
 * returning and this is not needed.  However, if the kernel migrates a
 * thread to another cpu and wants to leave it there, it has to call this
 * scheduler helper.
 *
 * Note that the lwkt_migratecpu() function also released the thread, so
 * we don't have to worry about that.
 */
static
void
dfly_changedcpu(struct lwp *lp)
{
	dfly_pcpu_t dd = &dfly_pcpu[lp->lwp_qcpu];
	dfly_pcpu_t rdd = &dfly_pcpu[mycpu->gd_cpuid];

	if (dd != rdd) {
		spin_lock(&dd->spin);
		dfly_changeqcpu_locked(lp, dd, rdd);
		spin_unlock(&dd->spin);
	}
}

/*
 * Called from fork1() when a new child process is being created.
 *
 * Give the child process an initial estcpu that is more batch then
 * its parent and dock the parent for the fork (but do not
 * reschedule the parent).
 *
 * fast
 *
 * XXX lwp should be "spawning" instead of "forking"
 */
static void
dfly_forking(struct lwp *plp, struct lwp *lp)
{
	int estcpu;

	/*
	 * Put the child 4 queue slots (out of 32) higher than the parent
	 * (less desireable than the parent).
	 */
	lp->lwp_estcpu = ESTCPULIM(plp->lwp_estcpu +
				   ESTCPUPPQ * usched_dfly_forkbias);
	lp->lwp_forked = 1;
	lp->lwp_estfast = 0;

	/*
	 * Even though the lp will be scheduled specially the first time
	 * due to lp->lwp_forked, it is important to initialize lwp_qcpu
	 * to avoid favoring a fixed cpu.
	 */
#if 0
	static uint16_t save_cpu;
	lp->lwp_qcpu = ++save_cpu % ncpus;
#else
	lp->lwp_qcpu = plp->lwp_qcpu;
	if (CPUMASK_TESTBIT(lp->lwp_cpumask, lp->lwp_qcpu) == 0)
		lp->lwp_qcpu = BSFCPUMASK(lp->lwp_cpumask);
#endif

	/*
	 * Dock the parent a cost for the fork, protecting us from fork
	 * bombs.  If the parent is forking quickly this makes both the
	 * parent and child more batchy.
	 */
	estcpu = plp->lwp_estcpu + ESTCPUPPQ / 16;
	plp->lwp_estcpu = ESTCPULIM(estcpu);
}

/*
 * Called when a lwp is being removed from this scheduler, typically
 * during lwp_exit().  We have to clean out any ULOAD accounting before
 * we can let the lp go.  The dd->spin lock is not needed for uload
 * updates.
 *
 * Scheduler dequeueing has already occurred, no further action in that
 * regard is needed.
 */
static void
dfly_exiting(struct lwp *lp, struct proc *child_proc)
{
	dfly_pcpu_t dd = &dfly_pcpu[lp->lwp_qcpu];

	if (lp->lwp_mpflags & LWP_MP_ULOAD) {
		atomic_clear_int(&lp->lwp_mpflags, LWP_MP_ULOAD);
		atomic_add_long(&dd->uload, -lp->lwp_uload);
		atomic_add_int(&dd->ucount, -1);
	}
}

/*
 * This function cannot block in any way, but spinlocks are ok.
 *
 * Update the uload based on the state of the thread (whether it is going
 * to sleep or running again).  The uload is meant to be a longer-term
 * load and not an instantanious load.
 */
static void
dfly_uload_update(struct lwp *lp)
{
	dfly_pcpu_t dd = &dfly_pcpu[lp->lwp_qcpu];

	if (lp->lwp_thread->td_flags & TDF_RUNQ) {
		if ((lp->lwp_mpflags & LWP_MP_ULOAD) == 0) {
			spin_lock(&dd->spin);
			if ((lp->lwp_mpflags & LWP_MP_ULOAD) == 0) {
				atomic_set_int(&lp->lwp_mpflags,
					       LWP_MP_ULOAD);
				atomic_add_long(&dd->uload, lp->lwp_uload);
				atomic_add_int(&dd->ucount, 1);
			}
			spin_unlock(&dd->spin);
		}
	} else if (lp->lwp_slptime > 0) {
		if (lp->lwp_mpflags & LWP_MP_ULOAD) {
			spin_lock(&dd->spin);
			if (lp->lwp_mpflags & LWP_MP_ULOAD) {
				atomic_clear_int(&lp->lwp_mpflags,
						 LWP_MP_ULOAD);
				atomic_add_long(&dd->uload, -lp->lwp_uload);
				atomic_add_int(&dd->ucount, -1);
			}
			spin_unlock(&dd->spin);
		}
	}
}

/*
 * chooseproc() is called when a cpu needs a user process to LWKT schedule,
 * it selects a user process and returns it.  If chklp is non-NULL and chklp
 * has a better or equal priority then the process that would otherwise be
 * chosen, NULL is returned.
 *
 * Until we fix the RUNQ code the chklp test has to be strict or we may
 * bounce between processes trying to acquire the current process designation.
 *
 * Must be called with rdd->spin locked.  The spinlock is left intact through
 * the entire routine.  dd->spin does not have to be locked.
 *
 * If worst is non-zero this function finds the worst thread instead of the
 * best thread (used by the schedulerclock-based rover).
 */
static
struct lwp *
dfly_chooseproc_locked(dfly_pcpu_t rdd, dfly_pcpu_t dd,
		       struct lwp *chklp, int worst)
{
	struct lwp *lp;
	struct rq *q;
	u_int32_t *which;
	u_int32_t pri;
	u_int32_t rtqbits;
	u_int32_t tsqbits;
	u_int32_t idqbits;

	/*
	 * Select best or worst process.  Once selected, clear the bit
	 * in our local variable (idqbits, tsqbits, or rtqbits) just
	 * in case we have to loop.
	 */
	rtqbits = rdd->rtqueuebits;
	tsqbits = rdd->queuebits;
	idqbits = rdd->idqueuebits;

loopfar:
	if (worst) {
		if (idqbits) {
			pri = bsrl(idqbits);
			idqbits &= ~(1U << pri);
			q = &rdd->idqueues[pri];
			which = &rdd->idqueuebits;
		} else if (tsqbits) {
			pri = bsrl(tsqbits);
			tsqbits &= ~(1U << pri);
			q = &rdd->queues[pri];
			which = &rdd->queuebits;
		} else if (rtqbits) {
			pri = bsrl(rtqbits);
			rtqbits &= ~(1U << pri);
			q = &rdd->rtqueues[pri];
			which = &rdd->rtqueuebits;
		} else {
			return (NULL);
		}
		lp = TAILQ_LAST(q, rq);
	} else {
		if (rtqbits) {
			pri = bsfl(rtqbits);
			rtqbits &= ~(1U << pri);
			q = &rdd->rtqueues[pri];
			which = &rdd->rtqueuebits;
		} else if (tsqbits) {
			pri = bsfl(tsqbits);
			tsqbits &= ~(1U << pri);
			q = &rdd->queues[pri];
			which = &rdd->queuebits;
		} else if (idqbits) {
			pri = bsfl(idqbits);
			idqbits &= ~(1U << pri);
			q = &rdd->idqueues[pri];
			which = &rdd->idqueuebits;
		} else {
			return (NULL);
		}
		lp = TAILQ_FIRST(q);
	}
	KASSERT(lp, ("chooseproc: no lwp on busy queue"));

loopnear:
	/*
	 * If the passed lwp <chklp> is reasonably close to the selected
	 * lwp <lp>, return NULL (indicating that <chklp> should be kept).
	 *
	 * Note that we must error on the side of <chklp> to avoid bouncing
	 * between threads in the acquire code.
	 */
	if (chklp) {
		if (chklp->lwp_priority < lp->lwp_priority + PPQ)
			return(NULL);
	}

	/*
	 * When rdd != dd, we have to make sure that the process we
	 * are pulling is allow to run on our cpu.  This alternative
	 * path is a bit more expensive but its not considered to be
	 * in the critical path.
	 */
	if (rdd != dd && CPUMASK_TESTBIT(lp->lwp_cpumask, dd->cpuid) == 0) {
		if (worst)
			lp = TAILQ_PREV(lp, rq, lwp_procq);
		else
			lp = TAILQ_NEXT(lp, lwp_procq);
		if (lp)
			goto loopnear;
		goto loopfar;
	}

	KTR_COND_LOG(usched_chooseproc,
	    lp->lwp_proc->p_pid == usched_dfly_pid_debug,
	    lp->lwp_proc->p_pid,
	    lp->lwp_thread->td_gd->gd_cpuid,
	    mycpu->gd_cpuid);

	KASSERT((lp->lwp_mpflags & LWP_MP_ONRUNQ) != 0, ("not on runq6!"));
	atomic_clear_int(&lp->lwp_mpflags, LWP_MP_ONRUNQ);
	TAILQ_REMOVE(q, lp, lwp_procq);
	--rdd->runqcount;
	if (TAILQ_EMPTY(q))
		*which &= ~(1 << pri);

	/*
	 * If we are choosing a process from rdd with the intent to
	 * move it to dd, lwp_qcpu must be adjusted while rdd's spinlock
	 * is still held.
	 */
	if (rdd != dd) {
		if (lp->lwp_mpflags & LWP_MP_ULOAD) {
			atomic_add_long(&rdd->uload, -lp->lwp_uload);
			atomic_add_int(&rdd->ucount, -1);
		}
		lp->lwp_qcpu = dd->cpuid;
		atomic_add_long(&dd->uload, lp->lwp_uload);
		atomic_add_int(&dd->ucount, 1);
		atomic_set_int(&lp->lwp_mpflags, LWP_MP_ULOAD);
	}
	return lp;
}

/*
 * USED TO PUSH RUNNABLE LWPS TO THE LEAST LOADED CPU.
 *
 * Choose a cpu node to schedule lp on, hopefully nearby its current
 * node.
 *
 * We give the current node a modest advantage for obvious reasons.
 *
 * We also give the node the thread was woken up FROM a slight advantage
 * in order to try to schedule paired threads which synchronize/block waiting
 * for each other fairly close to each other.  Similarly in a network setting
 * this feature will also attempt to place a user process near the kernel
 * protocol thread that is feeding it data.  THIS IS A CRITICAL PART of the
 * algorithm as it heuristically groups synchronizing processes for locality
 * of reference in multi-socket systems.
 *
 * We check against running processes and give a big advantage if there
 * are none running.
 *
 * The caller will normally dfly_setrunqueue() lp on the returned queue.
 *
 * When the topology is known choose a cpu whos group has, in aggregate,
 * has the lowest weighted load.
 */
static
dfly_pcpu_t
dfly_choose_best_queue(struct lwp *lp)
{
	cpumask_t wakemask;
	cpumask_t mask;
	cpu_node_t *cpup;
	cpu_node_t *cpun;
	cpu_node_t *cpub;
	dfly_pcpu_t dd = &dfly_pcpu[lp->lwp_qcpu];
	dfly_pcpu_t rdd;
	int wakecpu;
	int cpuid;
	int n;
	int loadav;
	long load;
	long lowest_load;

	/*
	 * When the topology is unknown choose a random cpu that is hopefully
	 * idle.
	 */
	if (dd->cpunode == NULL)
		return (dfly_choose_queue_simple(dd, lp));

	loadav = (averunnable.ldavg[0] + FSCALE / 2) >> FSHIFT;

	/*
	 * Pairing mask
	 */
	if ((wakecpu = lp->lwp_thread->td_wakefromcpu) >= 0)
		wakemask = dfly_pcpu[wakecpu].cpumask;
	else
		CPUMASK_ASSZERO(wakemask);

	if (usched_dfly_debug == lp->lwp_proc->p_pid)
		kprintf("choosebest wakefromcpu %d:\n",
			lp->lwp_thread->td_wakefromcpu);

	/*
	 * When the topology is known choose a cpu whos group has, in
	 * aggregate, has the lowest weighted load.
	 */
	cpup = root_cpu_node;
	rdd = dd;

	while (cpup) {
		/*
		 * Degenerate case super-root
		 */
		if (cpup->child_no == 1) {
			cpup = cpup->child_node[0];
			continue;
		}

		/*
		 * Terminal cpunode
		 */
		if (cpup->child_no == 0) {
			rdd = &dfly_pcpu[BSFCPUMASK(cpup->members)];
			if (usched_dfly_debug == lp->lwp_proc->p_pid)
				kprintf("  last cpu %d\n", rdd->cpuid);
			break;
		}

		cpub = NULL;
		lowest_load = 0x7FFFFFFFFFFFFFFFLL;
		if (usched_dfly_debug == lp->lwp_proc->p_pid)
			kprintf("  reset lowest_load for scan\n");

		for (n = 0; n < cpup->child_no; ++n) {
			/*
			 * Accumulate load information for all cpus
			 * which are members of this node.
			 */
			int count;

			cpun = cpup->child_node[n];
			mask = cpun->members;
			CPUMASK_ANDMASK(mask, usched_global_cpumask);
			CPUMASK_ANDMASK(mask, smp_active_mask);
			CPUMASK_ANDMASK(mask, lp->lwp_cpumask);
			if (CPUMASK_TESTZERO(mask))
				continue;

			load = 0;
			count = 0;

			if (usched_dfly_debug == lp->lwp_proc->p_pid)
				kprintf("  mask:");
			while (CPUMASK_TESTNZERO(mask)) {
				cpuid = BSFCPUMASK(mask);
				rdd = &dfly_pcpu[cpuid];

				if (usched_dfly_debug == lp->lwp_proc->p_pid)
					kprintf(" %d", cpuid);

				/*
				 * Cumulative load for members.  Note that
				 * if (lp) is part of the group, lp's
				 * contribution will be backed out later.
				 */
				load += rdd->uload;
				load += rdd->ucount *
					usched_dfly_weight3;

				/*
				 * If the node is running a less important
				 * thread than our thread, give it an
				 * advantage.  Witha high-enough weighting
				 * this can override most other considerations
				 * to provide ultimate priority fairness at
				 * the cost of localization.
				 */
				if ((rdd->upri & ~PPQMASK) >
				    (lp->lwp_priority & ~PPQMASK)) {
					load -= usched_dfly_weight4;
				}

#if 0
				if (rdd->uschedcp == NULL &&
				    rdd->runqcount == 0 &&
				    rdd->gd->gd_tdrunqcount == 0
				) {
					load += rdd->uload / 2;
					load += rdd->ucount *
						usched_dfly_weight3 / 2;
				} else {
					load += rdd->uload;
					load += rdd->ucount *
						usched_dfly_weight3;
				}
#endif
				CPUMASK_NANDBIT(mask, cpuid);
				++count;
			}

			/*
			 * Compensate if the lp is already accounted for in
			 * the aggregate uload for this mask set.  We want
			 * to calculate the loads as if lp were not present,
			 * otherwise the calculation is bogus.
			 */
			if ((lp->lwp_mpflags & LWP_MP_ULOAD) &&
			    CPUMASK_TESTMASK(dd->cpumask, cpun->members)) {
				load -= lp->lwp_uload;
				load -= usched_dfly_weight3;	/* ucount */
			}

			if (usched_dfly_debug == lp->lwp_proc->p_pid)
				kprintf("\n  accum_start c=%d ld=%ld "
					"cpu=%d ld/cnt=%ld ",
					count, load, rdd->cpuid,
					load / count);

			/*
			 * load is the aggregate load of count CPUs in the
			 * group.  For the weightings to work as intended,
			 * we want an average per-cpu load.
			 */
			load = load / count;

			/*
			 * Advantage the cpu group (lp) is already on.
			 */
			if (CPUMASK_TESTMASK(cpun->members, dd->cpumask))
				load -= usched_dfly_weight1;

			if (usched_dfly_debug == lp->lwp_proc->p_pid)
				kprintf("B:%ld ", load);

			/*
			 * Advantage nodes with more memory
			 */
			if (usched_dfly_node_mem) {
				load -= cpun->phys_mem * usched_dfly_weight5 /
					usched_dfly_node_mem;
			}

			if (usched_dfly_debug == lp->lwp_proc->p_pid)
				kprintf("C:%ld ", load);

			/*
			 * Advantage the cpu group we desire to pair (lp)
			 * to, but Disadvantage hyperthreads on the same
			 * core, or the same thread as the ipc peer.
			 *
			 * Under very heavy loads it is usually beneficial
			 * to set kern.usched_dfly.ipc_smt to 1, and under
			 * extreme loads it might be beneficial to also set
			 * kern.usched_dfly.ipc_same to 1.
			 *
			 * load+    disadvantage
			 * load-    advantage
			 */
			if (CPUMASK_TESTMASK(cpun->members, wakemask)) {
				if (cpun->child_no) {
					if (cpun->type == CORE_LEVEL &&
					    usched_dfly_ipc_smt < 0 &&
					    loadav >= (ncpus >> 1)) {
						/*
						 * Advantage at higher levels
						 * of the topology.
						 */
						load -= usched_dfly_weight2;
					} else if (cpun->type == CORE_LEVEL &&
						   usched_dfly_ipc_smt == 0) {
						/*
						 * Disadvantage the same core
						 * when there are hyperthreads.
						 */
						load += usched_dfly_weight2;
					} else {
						/*
						 * Advantage at higher levels
						 * of the topology.
						 */
						load -= usched_dfly_weight2;
					}
				} else {
					/*
					 * Disadvantage the last level (core
					 * or hyperthread).  Try to schedule
					 * the ipc
					 */
					if (usched_dfly_ipc_same < 0 &&
					    loadav >= ncpus) {
						load -= usched_dfly_weight2;
					} else if (usched_dfly_ipc_same) {
						load -= usched_dfly_weight2;
					} else {
						load += usched_dfly_weight2;
					}
				}
#if 0
				if (cpun->child_no != 0) {
					/* advantage */
					load -= usched_dfly_weight2;
				} else {
					/*
					 * 0x10 (disadvantage)
					 * 0x00 (advantage)   - default
					 */
					if (usched_dfly_features & 0x10)
						load += usched_dfly_weight2;
					else
						load -= usched_dfly_weight2;
				}
#endif
			}

			if (usched_dfly_debug == lp->lwp_proc->p_pid)
				kprintf("D:%ld ", load);

			/*
			 * Calculate the best load
			 */
			if (cpub == NULL || lowest_load > load ||
			    (lowest_load == load &&
			     CPUMASK_TESTMASK(cpun->members, dd->cpumask))
			) {
				lowest_load = load;
				cpub = cpun;
			}

			if (usched_dfly_debug == lp->lwp_proc->p_pid)
				kprintf("low=%ld]\n", lowest_load);
		}
		cpup = cpub;
	}
	/* Dispatch this outcast to a proper CPU. */
	if (__predict_false(CPUMASK_TESTBIT(lp->lwp_cpumask, rdd->cpuid) == 0))
		rdd = &dfly_pcpu[BSFCPUMASK(lp->lwp_cpumask)];
	if (usched_dfly_chooser > 0) {
		--usched_dfly_chooser;		/* only N lines */
		kprintf("lp %02d->%02d %s\n",
			lp->lwp_qcpu, rdd->cpuid, lp->lwp_proc->p_comm);
	}
	if (usched_dfly_debug == lp->lwp_proc->p_pid)
		kprintf("final cpu %d\n", rdd->cpuid);
	return (rdd);
}

/*
 * USED TO PULL RUNNABLE LWPS FROM THE MOST LOADED CPU.
 *
 * Choose the worst queue close to dd's cpu node with a non-empty runq
 * that is NOT dd.
 *
 * This is used by the thread chooser when the current cpu's queues are
 * empty to steal a thread from another cpu's queue.  We want to offload
 * the most heavily-loaded queue.
 *
 * However, we do not want to steal from far-away nodes who themselves
 * have idle cpu's that are more suitable to distribute the far-away
 * thread to.
 */
static
dfly_pcpu_t
dfly_choose_worst_queue(dfly_pcpu_t dd, int forceit)
{
	cpumask_t mask;
	cpu_node_t *cpup;
	cpu_node_t *cpun;
	cpu_node_t *cpub;
	dfly_pcpu_t rdd;
	int cpuid;
	int n;
	long load;
	long highest_load;
#if 0
	int pri;
	int hpri;
#endif

	/*
	 * When the topology is unknown choose a random cpu that is hopefully
	 * idle.
	 */
	if (dd->cpunode == NULL) {
		return (NULL);
	}

	/*
	 * When the topology is known choose a cpu whos group has, in
	 * aggregate, has the highest weighted load.
	 */
	cpup = root_cpu_node;
	rdd = dd;
	while (cpup) {
		/*
		 * Degenerate case super-root
		 */
		if (cpup->child_no == 1) {
			cpup = cpup->child_node[0];
			continue;
		}

		/*
		 * Terminal cpunode
		 */
		if (cpup->child_no == 0) {
			rdd = &dfly_pcpu[BSFCPUMASK(cpup->members)];
			break;
		}

		cpub = NULL;
		highest_load = -0x7FFFFFFFFFFFFFFFLL;

		for (n = 0; n < cpup->child_no; ++n) {
			/*
			 * Accumulate load information for all cpus
			 * which are members of this node.
			 */
			int count;

			cpun = cpup->child_node[n];
			mask = cpun->members;
			CPUMASK_ANDMASK(mask, usched_global_cpumask);
			CPUMASK_ANDMASK(mask, smp_active_mask);
			if (CPUMASK_TESTZERO(mask))
				continue;

			load = 0;
			count = 0;

			while (CPUMASK_TESTNZERO(mask)) {
				cpuid = BSFCPUMASK(mask);
				rdd = &dfly_pcpu[cpuid];

				load += rdd->uload;
				load += rdd->ucount * usched_dfly_weight3;

#if 0
				if (rdd->uschedcp == NULL &&
				    rdd->runqcount == 0 &&
				    rdd->gd->gd_tdrunqcount == 0
				) {
					load += rdd->uload / 2;
					load += rdd->ucount *
						usched_dfly_weight3 / 2;
				} else {
					load += rdd->uload;
					load += rdd->ucount *
						usched_dfly_weight3;
				}
#endif
				CPUMASK_NANDBIT(mask, cpuid);
				++count;
			}
			load /= count;

			/*
			 * Advantage the cpu group (dd) is already on.
			 *
			 * When choosing the worst queue we reverse the
			 * sign, but only count half the weight.
			 *
			 * weight1 needs to be high enough to be stable,
			 * but this can also cause it to be too sticky,
			 * so the iterator which rebalances the load sets
			 * forceit to ignore it.
			 */
			if (forceit == 0 &&
			    CPUMASK_TESTMASK(dd->cpumask, cpun->members)) {
				load += usched_dfly_weight1 / 2;
			}

			/*
			 * Disadvantage nodes with more memory (same sign).
			 */
			if (usched_dfly_node_mem) {
				load -= cpun->phys_mem * usched_dfly_weight5 /
					usched_dfly_node_mem;
			}


			/*
			 * The best candidate is the one with the worst
			 * (highest) load.
			 */
			if (cpub == NULL || highest_load < load ||
			    (highest_load == load &&
			     CPUMASK_TESTMASK(cpun->members, dd->cpumask))) {
				highest_load = load;
				cpub = cpun;
			}
		}
		cpup = cpub;
	}

	/*
	 * We never return our own node (dd), and only return a remote
	 * node if it's load is significantly worse than ours (i.e. where
	 * stealing a thread would be considered reasonable).
	 *
	 * This also helps us avoid breaking paired threads apart which
	 * can have disastrous effects on performance.
	 */
	if (rdd == dd)
		return(NULL);

#if 0
	hpri = 0;
	if (rdd->rtqueuebits && hpri < (pri = bsrl(rdd->rtqueuebits)))
		hpri = pri;
	if (rdd->queuebits && hpri < (pri = bsrl(rdd->queuebits)))
		hpri = pri;
	if (rdd->idqueuebits && hpri < (pri = bsrl(rdd->idqueuebits)))
		hpri = pri;
	hpri *= PPQ;
	if (rdd->uload - hpri < dd->uload + hpri)
		return(NULL);
#endif
	return (rdd);
}

static
dfly_pcpu_t
dfly_choose_queue_simple(dfly_pcpu_t dd, struct lwp *lp)
{
	dfly_pcpu_t rdd;
	cpumask_t tmpmask;
	cpumask_t mask;
	int cpubase;
	int cpuid;

	/*
	 * Fallback to the original heuristic, select random cpu,
	 * first checking the cpus not currently running a user thread.
	 *
	 * Use cpuid as the base cpu in our scan, first checking
	 * cpuid...(ncpus-1), then 0...(cpuid-1).  This avoid favoring
	 * lower-numbered cpus.
	 */
	++dd->scancpu;		/* SMP race ok */
	mask = dfly_rdyprocmask;
	CPUMASK_NANDMASK(mask, dfly_curprocmask);
	CPUMASK_ANDMASK(mask, lp->lwp_cpumask);
	CPUMASK_ANDMASK(mask, smp_active_mask);
	CPUMASK_ANDMASK(mask, usched_global_cpumask);

	cpubase = (int)(dd->scancpu % ncpus);
	CPUMASK_ASSBMASK(tmpmask, cpubase);
	CPUMASK_INVMASK(tmpmask);
	CPUMASK_ANDMASK(tmpmask, mask);
	while (CPUMASK_TESTNZERO(tmpmask)) {
		cpuid = BSFCPUMASK(tmpmask);
		rdd = &dfly_pcpu[cpuid];

		if ((rdd->upri & ~PPQMASK) >= (lp->lwp_priority & ~PPQMASK))
			goto found;
		CPUMASK_NANDBIT(tmpmask, cpuid);
	}

	CPUMASK_ASSBMASK(tmpmask, cpubase);
	CPUMASK_ANDMASK(tmpmask, mask);
	while (CPUMASK_TESTNZERO(tmpmask)) {
		cpuid = BSFCPUMASK(tmpmask);
		rdd = &dfly_pcpu[cpuid];

		if ((rdd->upri & ~PPQMASK) >= (lp->lwp_priority & ~PPQMASK))
			goto found;
		CPUMASK_NANDBIT(tmpmask, cpuid);
	}

	/*
	 * Then cpus which might have a currently running lp
	 */
	mask = dfly_rdyprocmask;
	CPUMASK_ANDMASK(mask, dfly_curprocmask);
	CPUMASK_ANDMASK(mask, lp->lwp_cpumask);
	CPUMASK_ANDMASK(mask, smp_active_mask);
	CPUMASK_ANDMASK(mask, usched_global_cpumask);

	CPUMASK_ASSBMASK(tmpmask, cpubase);
	CPUMASK_INVMASK(tmpmask);
	CPUMASK_ANDMASK(tmpmask, mask);
	while (CPUMASK_TESTNZERO(tmpmask)) {
		cpuid = BSFCPUMASK(tmpmask);
		rdd = &dfly_pcpu[cpuid];

		if ((rdd->upri & ~PPQMASK) > (lp->lwp_priority & ~PPQMASK))
			goto found;
		CPUMASK_NANDBIT(tmpmask, cpuid);
	}

	CPUMASK_ASSBMASK(tmpmask, cpubase);
	CPUMASK_ANDMASK(tmpmask, mask);
	while (CPUMASK_TESTNZERO(tmpmask)) {
		cpuid = BSFCPUMASK(tmpmask);
		rdd = &dfly_pcpu[cpuid];

		if ((rdd->upri & ~PPQMASK) > (lp->lwp_priority & ~PPQMASK))
			goto found;
		CPUMASK_NANDBIT(tmpmask, cpuid);
	}

	/*
	 * If we cannot find a suitable cpu we round-robin using scancpu.
	 * Other cpus will pickup as they release their current lwps or
	 * become ready.
	 *
	 * Avoid a degenerate system lockup case if usched_global_cpumask
	 * is set to 0 or otherwise does not cover lwp_cpumask.
	 *
	 * We only kick the target helper thread in this case, we do not
	 * set the user resched flag because
	 */
	cpuid = cpubase;
	if (CPUMASK_TESTBIT(lp->lwp_cpumask, cpuid) == 0)
		cpuid = BSFCPUMASK(lp->lwp_cpumask);
	else if (CPUMASK_TESTBIT(usched_global_cpumask, cpuid) == 0)
		cpuid = 0;
	rdd = &dfly_pcpu[cpuid];
found:
	return (rdd);
}

static
void
dfly_need_user_resched_remote(void *dummy)
{
	globaldata_t gd = mycpu;
	dfly_pcpu_t  dd = &dfly_pcpu[gd->gd_cpuid];

	/*
	 * Flag reschedule needed
	 */
	need_user_resched();

	/*
	 * If no user thread is currently running we need to kick the helper
	 * on our cpu to recover.  Otherwise the cpu will never schedule
	 * anything again.
	 *
	 * We cannot schedule the process ourselves because this is an
	 * IPI callback and we cannot acquire spinlocks in an IPI callback.
	 *
	 * Call wakeup_mycpu to avoid sending IPIs to other CPUs
	 */
	if (dd->uschedcp == NULL && (dd->flags & DFLY_PCPU_RDYMASK)) {
		ATOMIC_CPUMASK_NANDBIT(dfly_rdyprocmask, gd->gd_cpuid);
		dd->flags &= ~DFLY_PCPU_RDYMASK;
		wakeup_mycpu(dd->helper_thread);
	}
}

/*
 * dfly_remrunqueue_locked() removes a given process from the run queue
 * that it is on, clearing the queue busy bit if it becomes empty.
 *
 * Note that user process scheduler is different from the LWKT schedule.
 * The user process scheduler only manages user processes but it uses LWKT
 * underneath, and a user process operating in the kernel will often be
 * 'released' from our management.
 *
 * uload is NOT adjusted here.  It is only adjusted if the lwkt_thread goes
 * to sleep or the lwp is moved to a different runq.
 */
static void
dfly_remrunqueue_locked(dfly_pcpu_t rdd, struct lwp *lp)
{
	struct rq *q;
	u_int32_t *which;
	u_int8_t pri;

	KKASSERT(rdd->runqcount >= 0);

	pri = lp->lwp_rqindex;

	switch(lp->lwp_rqtype) {
	case RTP_PRIO_NORMAL:
		q = &rdd->queues[pri];
		which = &rdd->queuebits;
		break;
	case RTP_PRIO_REALTIME:
	case RTP_PRIO_FIFO:
		q = &rdd->rtqueues[pri];
		which = &rdd->rtqueuebits;
		break;
	case RTP_PRIO_IDLE:
		q = &rdd->idqueues[pri];
		which = &rdd->idqueuebits;
		break;
	default:
		panic("remrunqueue: invalid rtprio type");
		/* NOT REACHED */
	}
	KKASSERT(lp->lwp_mpflags & LWP_MP_ONRUNQ);
	atomic_clear_int(&lp->lwp_mpflags, LWP_MP_ONRUNQ);
	TAILQ_REMOVE(q, lp, lwp_procq);
	--rdd->runqcount;
	if (TAILQ_EMPTY(q)) {
		KASSERT((*which & (1 << pri)) != 0,
			("remrunqueue: remove from empty queue"));
		*which &= ~(1 << pri);
	}
}

/*
 * dfly_setrunqueue_locked()
 *
 * Add a process whos rqtype and rqindex had previously been calculated
 * onto the appropriate run queue.   Determine if the addition requires
 * a reschedule on a cpu and return the cpuid or -1.
 *
 * NOTE: 	  Lower priorities are better priorities.
 *
 * NOTE ON ULOAD: This variable specifies the aggregate load on a cpu, the
 *		  sum of the rough lwp_priority for all running and runnable
 *		  processes.  Lower priority processes (higher lwp_priority
 *		  values) actually DO count as more load, not less, because
 *		  these are the programs which require the most care with
 *		  regards to cpu selection.
 */
static void
dfly_setrunqueue_locked(dfly_pcpu_t rdd, struct lwp *lp)
{
	u_int32_t *which;
	struct rq *q;
	int pri;

	KKASSERT(lp->lwp_qcpu == rdd->cpuid);

	if ((lp->lwp_mpflags & LWP_MP_ULOAD) == 0) {
		atomic_set_int(&lp->lwp_mpflags, LWP_MP_ULOAD);
		atomic_add_long(&rdd->uload, lp->lwp_uload);
		atomic_add_int(&rdd->ucount, 1);
	}

	pri = lp->lwp_rqindex;

	switch(lp->lwp_rqtype) {
	case RTP_PRIO_NORMAL:
		q = &rdd->queues[pri];
		which = &rdd->queuebits;
		break;
	case RTP_PRIO_REALTIME:
	case RTP_PRIO_FIFO:
		q = &rdd->rtqueues[pri];
		which = &rdd->rtqueuebits;
		break;
	case RTP_PRIO_IDLE:
		q = &rdd->idqueues[pri];
		which = &rdd->idqueuebits;
		break;
	default:
		panic("remrunqueue: invalid rtprio type");
		/* NOT REACHED */
	}

	/*
	 * Place us on the selected queue.  Determine if we should be
	 * placed at the head of the queue or at the end.
	 *
	 * We are placed at the tail if our round-robin count has expired,
	 * or is about to expire and the system thinks its a good place to
	 * round-robin, or there is already a next thread on the queue
	 * (it might be trying to pick up where it left off and we don't
	 * want to interfere).
	 */
	KKASSERT((lp->lwp_mpflags & LWP_MP_ONRUNQ) == 0);
	atomic_set_int(&lp->lwp_mpflags, LWP_MP_ONRUNQ);
	++rdd->runqcount;

	if (lp->lwp_rrcount >= usched_dfly_rrinterval ||
	    (lp->lwp_rrcount >= usched_dfly_rrinterval / 2 &&
	     (lp->lwp_thread->td_mpflags & TDF_MP_BATCH_DEMARC))
	) {
		/*
		 * Place on tail
		 */
		atomic_clear_int(&lp->lwp_thread->td_mpflags,
				 TDF_MP_BATCH_DEMARC);
		lp->lwp_rrcount = 0;
		TAILQ_INSERT_TAIL(q, lp, lwp_procq);
	} else {
		/*
		 * Retain rrcount and place on head.  Count is retained
		 * even if the queue is empty.
		 */
		TAILQ_INSERT_HEAD(q, lp, lwp_procq);
	}
	*which |= 1 << pri;
}

/*
 * For SMP systems a user scheduler helper thread is created for each
 * cpu and is used to allow one cpu to wakeup another for the purposes of
 * scheduling userland threads from setrunqueue().
 *
 * UP systems do not need the helper since there is only one cpu.
 *
 * We can't use the idle thread for this because we might block.
 * Additionally, doing things this way allows us to HLT idle cpus
 * on MP systems.
 */
static void
dfly_helper_thread(void *dummy)
{
    globaldata_t gd;
    dfly_pcpu_t dd;
    dfly_pcpu_t rdd;
    struct lwp *nlp;
    cpumask_t mask;
    int cpuid;

    gd = mycpu;
    cpuid = gd->gd_cpuid;	/* doesn't change */
    mask = gd->gd_cpumask;	/* doesn't change */
    dd = &dfly_pcpu[cpuid];

    /*
     * Initial interlock, make sure all dfly_pcpu[] structures have
     * been initialized before proceeding.
     */
    lockmgr(&usched_dfly_config_lk, LK_SHARED);
    lockmgr(&usched_dfly_config_lk, LK_RELEASE);

    /*
     * Since we only want to be woken up only when no user processes
     * are scheduled on a cpu, run at an ultra low priority.
     */
    lwkt_setpri_self(TDPRI_USER_SCHEDULER);

    for (;;) {
	/*
	 * We use the LWKT deschedule-interlock trick to avoid racing
	 * dfly_rdyprocmask.  This means we cannot block through to the
	 * manual lwkt_switch() call we make below.
	 */
	crit_enter_gd(gd);
	tsleep_interlock(dd->helper_thread, 0);

	spin_lock(&dd->spin);
	if ((dd->flags & DFLY_PCPU_RDYMASK) == 0) {
		ATOMIC_CPUMASK_ORMASK(dfly_rdyprocmask, mask);
		dd->flags |= DFLY_PCPU_RDYMASK;
	}
	clear_user_resched();	/* This satisfied the reschedule request */
#if 0
	dd->rrcount = 0;	/* Reset the round-robin counter */
#endif

	if (dd->runqcount || dd->uschedcp != NULL) {
		/*
		 * Threads are available.  A thread may or may not be
		 * currently scheduled.  Get the best thread already queued
		 * to this cpu.
		 */
		nlp = dfly_chooseproc_locked(dd, dd, dd->uschedcp, 0);
		if (nlp) {
			if ((dd->flags & DFLY_PCPU_CURMASK) == 0) {
				ATOMIC_CPUMASK_ORMASK(dfly_curprocmask, mask);
				dd->flags |= DFLY_PCPU_CURMASK;
			}
			dd->upri = nlp->lwp_priority;
			dd->uschedcp = nlp;
#if 0
			dd->rrcount = 0;	/* reset round robin */
#endif
			spin_unlock(&dd->spin);
			lwkt_acquire(nlp->lwp_thread);
			lwkt_schedule(nlp->lwp_thread);
		} else {
			/*
			 * This situation should not occur because we had
			 * at least one thread available.
			 */
			spin_unlock(&dd->spin);
		}
	} else if (usched_dfly_features & 0x01) {
		/*
		 * This cpu is devoid of runnable threads, steal a thread
		 * from another cpu.  Since we're stealing, might as well
		 * load balance at the same time.
		 *
		 * We choose the highest-loaded thread from the worst queue.
		 *
		 * NOTE! This function only returns a non-NULL rdd when
		 *	 another cpu's queue is obviously overloaded.  We
		 *	 do not want to perform the type of rebalancing
		 *	 the schedclock does here because it would result
		 *	 in insane process pulling when 'steady' state is
		 *	 partially unbalanced (e.g. 6 runnables and only
		 *	 4 cores).
		 */
		rdd = dfly_choose_worst_queue(dd, 0);
		if (rdd && dd->uload + usched_dfly_weight6 < rdd->uload &&
		    spin_trylock(&rdd->spin)) {
			nlp = dfly_chooseproc_locked(rdd, dd, NULL, 1);
			spin_unlock(&rdd->spin);
		} else {
			nlp = NULL;
		}
		if (nlp) {
			if ((dd->flags & DFLY_PCPU_CURMASK) == 0) {
				ATOMIC_CPUMASK_ORMASK(dfly_curprocmask, mask);
				dd->flags |= DFLY_PCPU_CURMASK;
			}
			dd->upri = nlp->lwp_priority;
			dd->uschedcp = nlp;
#if 0
			dd->rrcount = 0;	/* reset round robin */
#endif
			spin_unlock(&dd->spin);
			lwkt_acquire(nlp->lwp_thread);
			lwkt_schedule(nlp->lwp_thread);
		} else {
			/*
			 * Leave the thread on our run queue.  Another
			 * scheduler will try to pull it later.
			 */
			spin_unlock(&dd->spin);
		}
	} else {
		/*
		 * devoid of runnable threads and not allowed to steal
		 * any.
		 */
		spin_unlock(&dd->spin);
	}

	/*
	 * We're descheduled unless someone scheduled us.  Switch away.
	 * Exiting the critical section will cause splz() to be called
	 * for us if interrupts and such are pending.
	 */
	crit_exit_gd(gd);
	tsleep(dd->helper_thread, PINTERLOCKED, "schslp", 0);
    }
}

#if 0
static int
sysctl_usched_dfly_stick_to_level(SYSCTL_HANDLER_ARGS)
{
	int error, new_val;

	new_val = usched_dfly_stick_to_level;

	error = sysctl_handle_int(oidp, &new_val, 0, req);
        if (error != 0 || req->newptr == NULL)
		return (error);
	if (new_val > cpu_topology_levels_number - 1 || new_val < 0)
		return (EINVAL);
	usched_dfly_stick_to_level = new_val;
	return (0);
}
#endif

/*
 * Setup the queues and scheduler helpers (scheduler helpers are SMP only).
 * Note that curprocmask bit 0 has already been cleared by rqinit() and
 * we should not mess with it further.
 */
static void
usched_dfly_cpu_init(void)
{
	int i;
	int j;
	int smt_not_supported = 0;
	int cache_coherent_not_supported = 0;

	if (bootverbose)
		kprintf("Start usched_dfly helpers on cpus:\n");

	sysctl_ctx_init(&usched_dfly_sysctl_ctx);
	usched_dfly_sysctl_tree =
		SYSCTL_ADD_NODE(&usched_dfly_sysctl_ctx,
				SYSCTL_STATIC_CHILDREN(_kern), OID_AUTO,
				"usched_dfly", CTLFLAG_RD, 0, "");

	usched_dfly_node_mem = get_highest_node_memory();

	lockmgr(&usched_dfly_config_lk, LK_EXCLUSIVE);

	for (i = 0; i < ncpus; ++i) {
		dfly_pcpu_t dd = &dfly_pcpu[i];
		cpumask_t mask;

		CPUMASK_ASSBIT(mask, i);
		if (CPUMASK_TESTMASK(mask, smp_active_mask) == 0)
		    continue;

		spin_init(&dd->spin, "uschedcpuinit");
		dd->cpunode = get_cpu_node_by_cpuid(i);
		dd->cpuid = i;
		dd->gd = globaldata_find(i);
		CPUMASK_ASSBIT(dd->cpumask, i);
		for (j = 0; j < NQS; j++) {
			TAILQ_INIT(&dd->queues[j]);
			TAILQ_INIT(&dd->rtqueues[j]);
			TAILQ_INIT(&dd->idqueues[j]);
		}
		ATOMIC_CPUMASK_NANDBIT(dfly_curprocmask, 0);
		if (i == 0)
			dd->flags &= ~DFLY_PCPU_CURMASK;

		if (dd->cpunode == NULL) {
			smt_not_supported = 1;
			cache_coherent_not_supported = 1;
			if (bootverbose)
				kprintf ("    cpu%d - WARNING: No CPU NODE "
					 "found for cpu\n", i);
		} else {
			switch (dd->cpunode->type) {
			case THREAD_LEVEL:
				if (bootverbose)
					kprintf ("    cpu%d - HyperThreading "
						 "available. Core siblings: ",
						 i);
				break;
			case CORE_LEVEL:
				smt_not_supported = 1;

				if (bootverbose)
					kprintf ("    cpu%d - No HT available, "
						 "multi-core/physical "
						 "cpu. Physical siblings: ",
						 i);
				break;
			case CHIP_LEVEL:
				smt_not_supported = 1;

				if (bootverbose)
					kprintf ("    cpu%d - No HT available, "
						 "single-core/physical cpu. "
						 "Package siblings: ",
						 i);
				break;
			default:
				/* Let's go for safe defaults here */
				smt_not_supported = 1;
				cache_coherent_not_supported = 1;
				if (bootverbose)
					kprintf ("    cpu%d - Unknown cpunode->"
						 "type=%u. siblings: ",
						 i,
						 (u_int)dd->cpunode->type);
				break;
			}

			if (bootverbose) {
				if (dd->cpunode->parent_node != NULL) {
					kprint_cpuset(&dd->cpunode->
							parent_node->members);
					kprintf("\n");
				} else {
					kprintf(" no siblings\n");
				}
			}
		}

		lwkt_create(dfly_helper_thread, NULL, &dd->helper_thread, NULL,
			    0, i, "usched %d", i);

		/*
		 * Allow user scheduling on the target cpu.  cpu #0 has already
		 * been enabled in rqinit().
		 */
		if (i) {
			ATOMIC_CPUMASK_NANDMASK(dfly_curprocmask, mask);
			dd->flags &= ~DFLY_PCPU_CURMASK;
		}
		if ((dd->flags & DFLY_PCPU_RDYMASK) == 0) {
			ATOMIC_CPUMASK_ORMASK(dfly_rdyprocmask, mask);
			dd->flags |= DFLY_PCPU_RDYMASK;
		}
		dd->upri = PRIBASE_NULL;

	}

	/* usched_dfly sysctl configurable parameters */

	SYSCTL_ADD_INT(&usched_dfly_sysctl_ctx,
		       SYSCTL_CHILDREN(usched_dfly_sysctl_tree),
		       OID_AUTO, "rrinterval", CTLFLAG_RW,
		       &usched_dfly_rrinterval, 0, "");
	SYSCTL_ADD_INT(&usched_dfly_sysctl_ctx,
		       SYSCTL_CHILDREN(usched_dfly_sysctl_tree),
		       OID_AUTO, "decay", CTLFLAG_RW,
		       &usched_dfly_decay, 0, "Extra decay when not running");
	SYSCTL_ADD_INT(&usched_dfly_sysctl_ctx,
		       SYSCTL_CHILDREN(usched_dfly_sysctl_tree),
		       OID_AUTO, "ipc_smt", CTLFLAG_RW,
		       &usched_dfly_ipc_smt, 0, "Pair IPC on hyper-threads");
	SYSCTL_ADD_INT(&usched_dfly_sysctl_ctx,
		       SYSCTL_CHILDREN(usched_dfly_sysctl_tree),
		       OID_AUTO, "ipc_same", CTLFLAG_RW,
		       &usched_dfly_ipc_same, 0, "Pair IPC on same thread");

	/* Add enable/disable option for SMT scheduling if supported */
	if (smt_not_supported) {
		usched_dfly_smt = 0;
		SYSCTL_ADD_STRING(&usched_dfly_sysctl_ctx,
				  SYSCTL_CHILDREN(usched_dfly_sysctl_tree),
				  OID_AUTO, "smt", CTLFLAG_RD,
				  "NOT SUPPORTED", 0, "SMT NOT SUPPORTED");
	} else {
		usched_dfly_smt = 1;
		SYSCTL_ADD_INT(&usched_dfly_sysctl_ctx,
			       SYSCTL_CHILDREN(usched_dfly_sysctl_tree),
			       OID_AUTO, "smt", CTLFLAG_RW,
			       &usched_dfly_smt, 0, "Enable SMT scheduling");
	}

	/*
	 * Add enable/disable option for cache coherent scheduling
	 * if supported
	 */
	if (cache_coherent_not_supported) {
		usched_dfly_cache_coherent = 0;
		SYSCTL_ADD_STRING(&usched_dfly_sysctl_ctx,
				  SYSCTL_CHILDREN(usched_dfly_sysctl_tree),
				  OID_AUTO, "cache_coherent", CTLFLAG_RD,
				  "NOT SUPPORTED", 0,
				  "Cache coherence NOT SUPPORTED");
	} else {
		usched_dfly_cache_coherent = 1;
		SYSCTL_ADD_INT(&usched_dfly_sysctl_ctx,
			       SYSCTL_CHILDREN(usched_dfly_sysctl_tree),
			       OID_AUTO, "cache_coherent", CTLFLAG_RW,
			       &usched_dfly_cache_coherent, 0,
			       "Enable/Disable cache coherent scheduling");

		SYSCTL_ADD_INT(&usched_dfly_sysctl_ctx,
			       SYSCTL_CHILDREN(usched_dfly_sysctl_tree),
			       OID_AUTO, "weight1", CTLFLAG_RW,
			       &usched_dfly_weight1, 200,
			       "Weight selection for current cpu");

		SYSCTL_ADD_INT(&usched_dfly_sysctl_ctx,
			       SYSCTL_CHILDREN(usched_dfly_sysctl_tree),
			       OID_AUTO, "weight2", CTLFLAG_RW,
			       &usched_dfly_weight2, 180,
			       "Weight selection for wakefrom cpu");

		SYSCTL_ADD_INT(&usched_dfly_sysctl_ctx,
			       SYSCTL_CHILDREN(usched_dfly_sysctl_tree),
			       OID_AUTO, "weight3", CTLFLAG_RW,
			       &usched_dfly_weight3, 40,
			       "Weight selection for num threads on queue");

		SYSCTL_ADD_INT(&usched_dfly_sysctl_ctx,
			       SYSCTL_CHILDREN(usched_dfly_sysctl_tree),
			       OID_AUTO, "weight4", CTLFLAG_RW,
			       &usched_dfly_weight4, 160,
			       "Availability of other idle cpus");

		SYSCTL_ADD_INT(&usched_dfly_sysctl_ctx,
			       SYSCTL_CHILDREN(usched_dfly_sysctl_tree),
			       OID_AUTO, "weight5", CTLFLAG_RW,
			       &usched_dfly_weight5, 50,
			       "Memory attached to node");

		SYSCTL_ADD_INT(&usched_dfly_sysctl_ctx,
			       SYSCTL_CHILDREN(usched_dfly_sysctl_tree),
			       OID_AUTO, "weight6", CTLFLAG_RW,
			       &usched_dfly_weight6, 150,
			       "Transfer weight");

		SYSCTL_ADD_INT(&usched_dfly_sysctl_ctx,
			       SYSCTL_CHILDREN(usched_dfly_sysctl_tree),
			       OID_AUTO, "fast_resched", CTLFLAG_RW,
			       &usched_dfly_fast_resched, 0,
			       "Availability of other idle cpus");

		SYSCTL_ADD_INT(&usched_dfly_sysctl_ctx,
			       SYSCTL_CHILDREN(usched_dfly_sysctl_tree),
			       OID_AUTO, "features", CTLFLAG_RW,
			       &usched_dfly_features, 0x8F,
			       "Allow pulls into empty queues");

		SYSCTL_ADD_INT(&usched_dfly_sysctl_ctx,
			       SYSCTL_CHILDREN(usched_dfly_sysctl_tree),
			       OID_AUTO, "swmask", CTLFLAG_RW,
			       &usched_dfly_swmask, ~PPQMASK,
			       "Queue mask to force thread switch");

#if 0
		SYSCTL_ADD_PROC(&usched_dfly_sysctl_ctx,
				SYSCTL_CHILDREN(usched_dfly_sysctl_tree),
				OID_AUTO, "stick_to_level",
				CTLTYPE_INT | CTLFLAG_RW,
				NULL, sizeof usched_dfly_stick_to_level,
				sysctl_usched_dfly_stick_to_level, "I",
				"Stick a process to this level. See sysctl"
				"paremter hw.cpu_topology.level_description");
#endif
	}
	lockmgr(&usched_dfly_config_lk, LK_RELEASE);
}

SYSINIT(uschedtd, SI_BOOT2_USCHED, SI_ORDER_SECOND,
	usched_dfly_cpu_init, NULL);
