/*-
 * Copyright (c) 1980, 1993
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
 *	@(#)dump.h	8.2 (Berkeley) 4/28/95
 *
 * $FreeBSD: src/sbin/dump/dump.h,v 1.7.6.4 2003/01/25 18:54:59 dillon Exp $
 */

#include <sys/param.h>

#define MAXINOPB	(MAXBSIZE / sizeof(struct ufs1_dinode))
#define MAXNINDIR	(MAXBSIZE / sizeof(daddr_t))

/*
 * Dump maps used to describe what is to be dumped.
 */
int	mapsize;	/* size of the state maps */
char	*usedinomap;	/* map of allocated inodes */
char	*dumpdirmap;	/* map of directories to be dumped */
char	*dumpinomap;	/* map of files to be dumped */
/*
 * Map manipulation macros.
 */
#define	SETINO(ino, map)	setbit(map, (u_int)((ino) - 1))
#define	CLRINO(ino, map)	clrbit(map, (u_int)((ino) - 1))
#define	TSTINO(ino, map)	isset(map, (u_int)((ino) - 1))

/*
 *	All calculations done in 0.1" units!
 */
char	*disk;		/* name of the disk file */
const char	*tape;		/* name of the tape file */
extern const char *dumpdates;	/* name of the file containing dump date information*/
extern const char *temp;		/* name of the file for doing rewrite of dumpdates */
char	lastlevel;	/* dump level of previous dump */
char	level;		/* dump level of this dump */
int	uflag;		/* update flag */
int	diskfd;		/* disk file descriptor */
int	tapefd;		/* tape file descriptor */
int	pipeout;	/* true => output to standard output */
ufs1_ino_t curino;	/* current inumber; used globally */
int	newtape;	/* new tape flag */
long	tapesize;	/* estimated tape size, blocks */
long	tsize;		/* tape size in 0.1" units */
long	asize;		/* number of 0.1" units written on current tape */
int	etapes;		/* estimated number of tapes */
int	nonodump;	/* if set, do not honor UF_NODUMP user flags */
int	unlimited;	/* if set, write to end of medium */

extern int	cachesize;	/* size of block cache */
extern int	density;	/* density in 0.1" units */
extern int	dokerberos;
#ifdef NTREC_LONG
extern long	ntrec;		/* used by sbin/restore */
#else
extern int	ntrec;		/* blocking factor on tape */
#endif
extern int	cartridge;
extern const char *host;
extern long	blocksperfile;	/* number of blocks per output file */
extern int	notify;		/* notify operator flag */
extern int	blockswritten;	/* number of blocks written on current tape */
extern long	dev_bsize;	/* block size of underlying disk device */

time_t	tstart_writing;	/* when started writing the first tape block */
time_t	tend_writing;	/* after writing the last tape block */
int	passno;		/* current dump pass number */
struct	fs *sblock;	/* the file system super block */
char	sblock_buf[MAXBSIZE];
int	dev_bshift;	/* log2(dev_bsize) */
int	tp_bshift;	/* log2(TP_BSIZE) */

/* operator interface functions */
void	broadcast(const char *);
void	infosch(int);
void	lastdump(int);		/* int should be char */
void	msg(const char *, ...) __printflike(1, 2);
void	msgtail(const char *, ...) __printflike(1, 2);
int	query(const char *);
void	quit(const char *, ...) __dead2 __printflike(1, 2);
void	timeest(void);
time_t	unctime(const char *);

/* mapping rouintes */
struct	ufs1_dinode;
long	blockest(struct ufs1_dinode *);
int	mapfiles(ufs1_ino_t maxino, long *);
int	mapdirs(ufs1_ino_t maxino, long *);

/* file dumping routines */
void	blksout(daddr_t *, int, ufs1_ino_t);
void	bread(daddr_t, char *, int);
ssize_t cread(int, void *, size_t, off_t);
void	dumpino(struct ufs1_dinode *, ufs1_ino_t);
void	dumpmap(const char *, int, ufs1_ino_t);
void	writeheader(ufs1_ino_t);

/* tape writing routines */
int	alloctape(void);
void	close_rewind(void);
void	dumpblock(daddr_t, int);
void	startnewtape(int);
void	trewind(void);
void	writerec(const void *, int);

void	Exit(int) __dead2;
void	dumpabort(int) __dead2;
void	dump_getfstab(void);

char	*rawname(char *);
struct	ufs1_dinode *getino(ufs1_ino_t);

/* rdump routines */
#if defined(RDUMP) || defined(RRESTORE)
void	rmtclose(void);
int	rmthost(const char *);
int	rmtopen(const char *, int);
int	rmtwrite(const void *, int);
#endif /* RDUMP || RRESTORE */

/* rrestore routines */
#ifdef RRESTORE
int	rmtread(char *, int);
int	rmtseek(int, int);
int	rmtioctl(int, int);
#endif /* RRESTORE */

void	interrupt(int);		/* in case operator bangs on console */

/*
 *	Exit status codes
 */
#define	X_FINOK		0	/* normal exit */
#define	X_STARTUP	1	/* startup error */
#define	X_REWRITE	2	/* restart writing from the check point */
#define	X_ABORT		3	/* abort dump; don't attempt checkpointing */

#define	OPGRENT	"operator"		/* group entry to notify */

struct	fstab *fstabsearch(const char *);	/* search fs_file and fs_spec */

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

/*
 *	The contents of the file _PATH_DUMPDATES is maintained both on
 *	a linked list, and then (eventually) arrayified.
 */
struct dumpdates {
	char	dd_name[NAME_MAX+3];
	char	dd_level;
	time_t	dd_ddate;
};
extern int	nddates;		/* number of records (might be zero) */
extern struct	dumpdates **ddatev;	/* the arrayfied version */
void	initdumptimes(void);
void	getdumptime(void);
void	putdumptime(void);
#define	ITITERATE(i, ddp) 	\
	if (ddatev != NULL)	\
		for (ddp = ddatev[i = 0]; i < nddates; ddp = ddatev[++i])

void	sig(int);
