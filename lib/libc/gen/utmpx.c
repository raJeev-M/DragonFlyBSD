/*-
 * Copyright (c) 2002 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Christos Zoulas.
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
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "namespace.h"
#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <assert.h>
#include <db.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <utmpx.h>
#include <vis.h>

static FILE *fp;
static int readonly = 0;
static struct utmpx ut;
static char utfile[MAXPATHLEN] = _PATH_UTMPX;

static struct utmpx *utmp_update(const struct utmpx *);

static const char vers[] = "utmpx-2.00";
static utx_db_t dbtype = UTX_DB_UTMPX;
DB *lastlogx_db = NULL;

void past_getutmp(void *, void *);
void past_getutmpx(void *, void *);

static int
_open_db(const char *fname)
{
	struct stat st;

	if ((fp = fopen(fname, "re+")) == NULL)
		if ((fp = fopen(fname, "we+")) == NULL) {
			if ((fp = fopen(fname, "re")) == NULL)
				goto fail;
			else
				readonly = 1;
		}

	/* get file size in order to check if new file */
	if (fstat(fileno(fp), &st) == -1)
		goto failclose;

	if (st.st_size == 0) {
		/* new file, add signature record */
		(void)memset(&ut, 0, sizeof(ut));
		ut.ut_type = SIGNATURE;
		(void)memcpy(ut.ut_user, vers, sizeof(vers));
		if (fwrite(&ut, sizeof(ut), 1, fp) != 1)
			goto failclose;
	} else {
		/* old file, read signature record */
		if (fread(&ut, sizeof(ut), 1, fp) != 1)
			goto failclose;
		if (memcmp(ut.ut_user, vers, 5) != 0 ||
		    ut.ut_type != SIGNATURE) {
			errno = EINVAL;
			goto failclose;
		}
	}

	return 0;
failclose:
	(void)fclose(fp);
fail:
	(void)memset(&ut, 0, sizeof(ut));
	return -1;
}

int
setutxdb(utx_db_t db_type, const char *fname)
{
	switch (db_type) {
	case UTX_DB_UTMPX:
		if (fname == NULL)
			fname = _PATH_UTMPX;
		break;

	case UTX_DB_WTMPX:
		if (fname == NULL)
			fname = _PATH_WTMPX;
		break;

	default:
		errno = EINVAL;
		return -1;
	}

	/* A previous db file is still open */
	if (fp != NULL) {
		fclose(fp);
		fp = NULL;
	}
	if ((_open_db(fname)) == -1)
		return -1;

	dbtype = db_type;
	return 0;
}

void
setutxent(void)
{
	(void)memset(&ut, 0, sizeof(ut));
	if (fp == NULL)
		return;
	(void)fseeko(fp, (off_t)sizeof(ut), SEEK_SET);
}

void
endutxent(void)
{
	(void)memset(&ut, 0, sizeof(ut));

	if (fp != NULL) {
		(void)fclose(fp);
		fp = NULL;
		readonly = 0;
	}
}

struct utmpx *
getutxent(void)
{
	if (fp == NULL) {
		if ((_open_db(utfile)) == -1)
			goto fail;
	}

	if (fread(&ut, sizeof(ut), 1, fp) != 1)
		goto fail;

	return &ut;
fail:
	(void)memset(&ut, 0, sizeof(ut));
	return NULL;
}

struct utmpx *
getutxid(const struct utmpx *utx)
{

	_DIAGASSERT(utx != NULL);

	if (utx->ut_type == EMPTY)
		return NULL;

	do {
		if (ut.ut_type == EMPTY)
			continue;
		switch (utx->ut_type) {
		case EMPTY:
			return NULL;
		case RUN_LVL:
		case BOOT_TIME:
		case OLD_TIME:
		case NEW_TIME:
			if (ut.ut_type == utx->ut_type)
				return &ut;
			break;
		case INIT_PROCESS:
		case LOGIN_PROCESS:
		case USER_PROCESS:
		case DEAD_PROCESS:
			switch (ut.ut_type) {
			case INIT_PROCESS:
			case LOGIN_PROCESS:
			case USER_PROCESS:
			case DEAD_PROCESS:
				if (memcmp(ut.ut_id, utx->ut_id,
				    sizeof(ut.ut_id)) == 0)
					return &ut;
				break;
			default:
				break;
			}
			break;
		default:
			return NULL;
		}
	} while (getutxent() != NULL);
	return NULL;
}

struct utmpx *
getutxline(const struct utmpx *utx)
{

	_DIAGASSERT(utx != NULL);

	do {
		switch (ut.ut_type) {
		case EMPTY:
			break;
		case LOGIN_PROCESS:
		case USER_PROCESS:
			if (strncmp(ut.ut_line, utx->ut_line,
			    sizeof(ut.ut_line)) == 0)
				return &ut;
			break;
		default:
			break;
		}
	} while (getutxent() != NULL);
	return NULL;
}

struct utmpx *
getutxuser(const char *user)
{
	_DIAGASSERT(utx != NULL);

	do {
		switch (ut.ut_type) {
		case EMPTY:
			break;
		case USER_PROCESS:
			if (strncmp(ut.ut_user, user, sizeof(ut.ut_user)) == 0)
				return &ut;
			break;
		default:
			break;
		}
	} while (getutxent() != NULL);
	return NULL;
}

struct utmpx *
pututxline(const struct utmpx *utx)
{
	struct passwd *pw;
	struct lastlogx ll;
	struct utmpx temp, *u = NULL;
	int gotlock = 0;

	_DIAGASSERT(utx != NULL);

	if (utx == NULL)
		return NULL;

	if (utx->ut_type == USER_PROCESS) {
		ll.ll_tv = utx->ut_tv;
		strcpy(ll.ll_host, utx->ut_host);
		strcpy(ll.ll_line, utx->ut_line);
		pw = getpwnam(utx->ut_name);
		if (pw != NULL)
			updlastlogx(_PATH_LASTLOGX, pw->pw_uid, &ll);
	}

	if (strcmp(_PATH_UTMPX, utfile) == 0)
		if ((fp != NULL && readonly) || (fp == NULL && geteuid() != 0))
			return utmp_update(utx);


	(void)memcpy(&temp, utx, sizeof(temp));

	if (fp == NULL) {
		(void)getutxent();
		if (fp == NULL || readonly)
			return NULL;
	}

	if (getutxid(&temp) == NULL) {
		setutxent();
		if (getutxid(&temp) == NULL) {
			if (lockf(fileno(fp), F_LOCK, (off_t)0) == -1)
				return NULL;
			gotlock++;
			if (fseeko(fp, (off_t)0, SEEK_END) == -1)
				goto fail;
		}
	}

	if (!gotlock) {
		/* we are not appending */
		if (fseeko(fp, -(off_t)sizeof(ut), SEEK_CUR) == -1)
			return NULL;
	}

	if (fwrite(&temp, sizeof (temp), 1, fp) != 1)
		goto fail;

	if (fflush(fp) == -1)
		goto fail;

	u = memcpy(&ut, &temp, sizeof(ut));
fail:
	if (gotlock) {
		if (lockf(fileno(fp), F_ULOCK, (off_t)0) == -1)
			return NULL;
	}
	return u;
}

static struct utmpx *
utmp_update(const struct utmpx *utx)
{
	char buf[sizeof(*utx) * 4 + 1];
	pid_t pid;
	int status;

	_DIAGASSERT(utx != NULL);

	(void)strvisx(buf, (const char *)(const void *)utx, sizeof(*utx),
	    VIS_WHITE | VIS_NOLOCALE);
	switch (pid = fork()) {
	case 0:
		(void)execl(_PATH_UTMP_UPDATE,
		    strrchr(_PATH_UTMP_UPDATE, '/') + 1, buf, NULL);
		_exit(1);
		/*NOTREACHED*/
	case -1:
		return NULL;
	default:
		if (waitpid(pid, &status, 0) == -1)
			return NULL;
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
			return memcpy(&ut, utx, sizeof(ut));
		return NULL;
	}

}

/*
 * The following are extensions and not part of the X/Open spec.
 */
void
updwtmpx(const char *file, const struct utmpx *utx)
{
	(void)_updwtmpx(file, utx);
}

int
_updwtmpx(const char *file, const struct utmpx *utx)
{
	int fd;
	int saved_errno;
	struct stat st;

	_DIAGASSERT(file != NULL);
	_DIAGASSERT(utx != NULL);

	fd = open(file, O_WRONLY|O_APPEND|O_SHLOCK|O_CLOEXEC);

	if (fd == -1) {
		if ((fd = open(file, O_CREAT|O_WRONLY|O_EXLOCK|O_CLOEXEC, 0644)) == -1)
			goto fail;
	}
	if (fstat(fd, &st) == -1)
		goto failclose;
	if (st.st_size == 0) {
		/* new file, add signature record */
		(void)memset(&ut, 0, sizeof(ut));
		ut.ut_type = SIGNATURE;
		(void)memcpy(ut.ut_user, vers, sizeof(vers));
		if (write(fd, &ut, sizeof(ut)) == -1)
			goto failclose;
	}
	if (write(fd, utx, sizeof(*utx)) == -1)
		goto failclose;
	if (_close(fd) == -1)
		return -1;
	return 0;

failclose:
	saved_errno = errno;
	(void) _close(fd);
	errno = saved_errno;
fail:
	return -1;
}

int
utmpxname(const char *fname)
{
	size_t len;

	_DIAGASSERT(fname != NULL);

	len = strlen(fname);

	if (len >= sizeof(utfile))
		return 0;

	/* must end in x! */
	if (fname[len - 1] != 'x')
		return 0;

	(void)strlcpy(utfile, fname, sizeof(utfile));
	endutxent();
	return 1;
}


__sym_compat(getutmp, past_getutmp, DF404.0);
void
past_getutmp(void *ux __unused, void *u __unused)
{
}

__sym_compat(getutmpx, past_getutmpx, DF404.0);
void
past_getutmpx(void *u __unused, void *ux __unused)
{
}

struct lastlogx *
getlastlogx(const char *fname, uid_t uid, struct lastlogx *ll)
{
	DBT key, data;
	DB *db;

	_DIAGASSERT(fname != NULL);
	_DIAGASSERT(ll != NULL);

	db = dbopen(fname, O_RDONLY|O_SHLOCK|O_CLOEXEC, 0, DB_HASH, NULL);

	if (db == NULL)
		return NULL;

	key.data = &uid;
	key.size = sizeof(uid);

	if ((db->get)(db, &key, &data, 0) != 0)
		goto error;

	if (data.size != sizeof(*ll)) {
		errno = EFTYPE;
		goto error;
	}

	if (ll == NULL)
		if ((ll = malloc(sizeof(*ll))) == NULL)
			goto done;

	(void)memcpy(ll, data.data, sizeof(*ll));
	goto done;
error:
	ll = NULL;
done:
	(db->close)(db);
	return ll;
}

int
updlastlogx(const char *fname, uid_t uid, struct lastlogx *ll)
{
	DBT key, data;
	int error = 0;
	DB *db;

	_DIAGASSERT(fname != NULL);
	_DIAGASSERT(ll != NULL);

	db = dbopen(fname, O_RDWR|O_CREAT|O_EXLOCK|O_CLOEXEC, 0644, DB_HASH, NULL);

	if (db == NULL)
		return -1;

	key.data = &uid;
	key.size = sizeof(uid);
	data.data = ll;
	data.size = sizeof(*ll);
	if ((db->put)(db, &key, &data, 0) != 0)
		error = -1;

	(db->close)(db);
	return error;
}
