/*
 * Copyright (c) 2019 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>
 *
 * This code uses concepts and configuration based on 'synth', by
 * John R. Marino <draco@marino.st>, which was written in ada.
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
#include "dsynth.h"

typedef struct pinfo {
	struct pinfo *next;
	char *spath;
	int foundit;
} pinfo_t;

static void removePackagesMetaRecurse(pkg_t *pkg);
static int pinfocmp(const void *s1, const void *s2);
static void scanit(const char *path, const char *subpath,
			int *countp, pinfo_t ***list_tailp);
pinfo_t *pinfofind(pinfo_t **ary, int count, char *spath);
static void childRebuildRepo(bulk_t *bulk);
static void scandeletenew(const char *path);

static void rebuildTerminateSignal(int signo);

static char *RebuildRemovePath;

void
DoRebuildRepo(int ask)
{
	bulk_t *bulk;
	FILE *fp;
	int fd;
	char tpath[256];
	const char *sufx;

	if (ask) {
		if (askyn("Rebuild the repository? ") == 0)
			return;
	}

	/*
	 * Scan the repository for temporary .new files and delete them.
	 */
	scandeletenew(RepositoryPath);

	/*
	 * Generate temporary file
	 */
	snprintf(tpath, sizeof(tpath), "/tmp/meta.XXXXXXXX.conf");

	signal(SIGTERM, rebuildTerminateSignal);
	signal(SIGINT, rebuildTerminateSignal);
	signal(SIGHUP, rebuildTerminateSignal);

	RebuildRemovePath = tpath;

	sufx = UsePkgSufx;
	fd = mkostemps(tpath, 5, 0);
	if (fd < 0)
		dfatal_errno("Cannot create %s", tpath);
	fp = fdopen(fd, "w");
	fprintf(fp, "version = 1;\n");
	fprintf(fp, "packing_format = \"%s\";\n", sufx + 1);
	fclose(fp);

	/*
	 * Run the operation under our bulk infrastructure to
	 * get the correct environment.
	 */
	initbulk(childRebuildRepo, 1);
	queuebulk(tpath, NULL, NULL, NULL);
	bulk = getbulk();

	if (bulk->r1)
		printf("Rebuild succeeded\n");
	else
		printf("Rebuild failed\n");
	donebulk();

	remove(tpath);
}

static void
repackage(const char *basepath, const char *basefile, const char *sufx,
	  const char *comp, const char *decomp);

static void
childRebuildRepo(bulk_t *bulk)
{
	FILE *fp;
	char *ptr;
	size_t len;
	pid_t pid;
	const char *cav[MAXCAC];
	int cac;
	int repackage_needed = 1;

	cac = 0;
	cav[cac++] = PKG_BINARY;
	cav[cac++] = "repo";
	cav[cac++] = "-m";
	cav[cac++] = bulk->s1;
	cav[cac++] = "-o";
	cav[cac++] = PackagesPath;

	/*
	 * The yaml needs to generate paths relative to PackagePath
	 */
	if (strncmp(PackagesPath, RepositoryPath, strlen(PackagesPath)) == 0)
		cav[cac++] = PackagesPath;
	else
		cav[cac++] = RepositoryPath;

	printf("pkg repo -m %s -o %s %s\n", bulk->s1, cav[cac-2], cav[cac-1]);

	fp = dexec_open(cav, cac, &pid, NULL, 1, 0);
	while ((ptr = fgetln(fp, &len)) != NULL)
		fwrite(ptr, 1, len, stdout);
	if (dexec_close(fp, pid) == 0)
		bulk->r1 = strdup("");

	/*
	 * Check package version.  Pkg version 1.12 and later generates
	 * the proper repo compression format.  Prior to that version
	 * the repo directive always generated .txz files.
	 */
	cac = 0;
	cav[cac++] = PKG_BINARY;
	cav[cac++] = "-v";
	fp = dexec_open(cav, cac, &pid, NULL, 1, 0);
	if ((ptr = fgetln(fp, &len)) != NULL && len > 0) {
		int v1;
		int v2;

		ptr[len-1] = 0;
		if (sscanf(ptr, "%d.%d", &v1, &v2) == 2) {
			if (v1 > 1 || (v1 == 1 && v2 >= 12))
				repackage_needed = 0;
		}
	}
	dexec_close(fp, pid);

	/*
	 * Repackage the .txz files created by pkg repo if necessary
	 */
	if (repackage_needed && strcmp(UsePkgSufx, ".txz") != 0) {
		const char *comp;
		const char *decomp;

		printf("pkg repo - version requires repackaging\n");

		if (strcmp(UsePkgSufx, ".tar") == 0) {
			decomp = "unxz";
			comp = "cat";
		} else if (strcmp(UsePkgSufx, ".tgz") == 0) {
			decomp = "unxz";
			comp = "gzip";
		} else if (strcmp(UsePkgSufx, ".tbz") == 0) {
			decomp = "unxz";
			comp = "bzip";
		} else {
			dfatal("repackaging as %s not supported", UsePkgSufx);
			decomp = "unxz";
			comp = "cat";
		}
		repackage(PackagesPath, "digests", UsePkgSufx,
			  comp, decomp);
		repackage(PackagesPath, "packagesite", UsePkgSufx,
			  comp, decomp);
	} else if (strcmp(UsePkgSufx, ".txz") != 0) {
		printf("pkg repo - version does not require repackaging\n");
	}
}

static
void
repackage(const char *basepath, const char *basefile, const char *sufx,
	  const char *comp, const char *decomp)
{
	char *buf;

	asprintf(&buf, "%s < %s/%s.txz | %s > %s/%s%s",
		decomp, basepath, basefile, comp, basepath, basefile, sufx);
	if (system(buf) != 0) {
		dfatal("command failed: %s", buf);
	}
	free(buf);
}

void
DoUpgradePkgs(pkg_t *pkgs __unused, int ask __unused)
{
	dfatal("Not Implemented");
}

void
PurgeDistfiles(pkg_t *pkgs)
{
	pinfo_t *list;
	pinfo_t *item;
	pinfo_t **list_tail;
	pinfo_t **ary;
	char *dstr;
	char *buf;
	int count;
	int delcount;
	int i;

	printf("Scanning distfiles... ");
	fflush(stdout);
	count = 0;
	list = NULL;
	list_tail = &list;
	scanit(DistFilesPath, NULL, &count, &list_tail);
	printf("Checking %d distfiles\n", count);
	fflush(stdout);

	ary = calloc(count, sizeof(pinfo_t *));
	for (i = 0; i < count; ++i) {
		ary[i] = list;
		list = list->next;
	}
	ddassert(list == NULL);
	qsort(ary, count, sizeof(pinfo_t *), pinfocmp);

	for (; pkgs; pkgs = pkgs->bnext) {
		if (pkgs->distfiles == NULL || pkgs->distfiles[0] == 0)
			continue;
		ddprintf(0, "distfiles %s\n", pkgs->distfiles);
		dstr = strtok(pkgs->distfiles, " \t");
		while (dstr) {
			for (;;) {
				if (pkgs->distsubdir) {
					asprintf(&buf, "%s/%s",
						 pkgs->distsubdir, dstr);
					item = pinfofind(ary, count, buf);
					ddprintf(0, "TEST %s %p\n", buf, item);
					free(buf);
					buf = NULL;
				} else {
					item = pinfofind(ary, count, dstr);
					ddprintf(0, "TEST %s %p\n", dstr, item);
				}
				if (item) {
					item->foundit = 1;
					break;
				}
				if (strrchr(dstr, ':') == NULL)
					break;
				*strrchr(dstr, ':') = 0;
			}
			dstr = strtok(NULL, " \t");
		}
	}

	delcount = 0;
	for (i = 0; i < count; ++i) {
		item = ary[i];
		if (item->foundit == 0) {
			++delcount;
		}
	}
	if (askyn("Delete %d of %d items? ", delcount, count)) {
		printf("Deleting %d/%d obsolete source distfiles\n",
		       delcount, count);
		for (i = 0; i < count; ++i) {
			item = ary[i];
			if (item->foundit == 0) {
				asprintf(&buf, "%s/%s",
					 DistFilesPath, item->spath);
				if (remove(buf) < 0)
					printf("Cannot delete %s\n", buf);
				free(buf);
			}
		}
	}


	free(ary);
}

void
RemovePackages(pkg_t *list)
{
	pkg_t *scan;
	char *path;

	for (scan = list; scan; scan = scan->bnext) {
		if ((scan->flags & PKGF_MANUALSEL) == 0)
			continue;
		if (scan->pkgfile) {
			scan->flags &= ~PKGF_PACKAGED;
			scan->pkgfile_size = 0;
			asprintf(&path, "%s/%s", RepositoryPath, scan->pkgfile);
			if (remove(path) == 0)
				printf("Removed: %s\n", path);
			free(path);
		}
		if (scan->pkgfile == NULL ||
		    (scan->flags & (PKGF_DUMMY | PKGF_META))) {
			removePackagesMetaRecurse(scan);
		}
	}
}

static void
removePackagesMetaRecurse(pkg_t *pkg)
{
	pkglink_t *link;
	pkg_t *scan;
	char *path;

	PKGLIST_FOREACH(link, &pkg->idepon_list) {
		scan = link->pkg;
		if (scan == NULL)
			continue;
		if (scan->pkgfile == NULL ||
		    (scan->flags & (PKGF_DUMMY | PKGF_META))) {
			removePackagesMetaRecurse(scan);
			continue;
		}
		scan->flags &= ~PKGF_PACKAGED;
		scan->pkgfile_size = 0;

		asprintf(&path, "%s/%s", RepositoryPath, scan->pkgfile);
		if (remove(path) == 0)
			printf("Removed: %s\n", path);
		free(path);
	}
}

static int
pinfocmp(const void *s1, const void *s2)
{
	const pinfo_t *item1 = *(const pinfo_t *const*)s1;
	const pinfo_t *item2 = *(const pinfo_t *const*)s2;

	return (strcmp(item1->spath, item2->spath));
}

pinfo_t *
pinfofind(pinfo_t **ary, int count, char *spath)
{
	pinfo_t *item;
	int res;
	int b;
	int e;
	int m;

	b = 0;
	e = count;
	while (b != e) {
		m = b + (e - b) / 2;
		item = ary[m];
		res = strcmp(spath, item->spath);
		if (res == 0)
			return item;
		if (res < 0) {
			e = m;
		} else {
			b = m + 1;
		}
	}
	return NULL;
}

void
scanit(const char *path, const char *subpath,
       int *countp, pinfo_t ***list_tailp)
{
	struct dirent *den;
	pinfo_t *item;
	char *npath;
	char *spath;
	DIR *dir;
	struct stat st;

	if ((dir = opendir(path)) != NULL) {
		while ((den = readdir(dir)) != NULL) {
			if (den->d_namlen == 1 && den->d_name[0] == '.')
				continue;
			if (den->d_namlen == 2 && den->d_name[0] == '.' &&
			    den->d_name[1] == '.')
				continue;
			asprintf(&npath, "%s/%s", path, den->d_name);
			if (lstat(npath, &st) < 0) {
				free(npath);
				continue;
			}
			if (S_ISDIR(st.st_mode)) {
				if (subpath) {
					asprintf(&spath, "%s/%s",
						 subpath, den->d_name);
					scanit(npath, spath,
					       countp, list_tailp);
					free(spath);
				} else {
					scanit(npath, den->d_name,
					       countp, list_tailp);
				}
			} else if (S_ISREG(st.st_mode)) {
				item = calloc(1, sizeof(*item));
				if (subpath) {
					asprintf(&item->spath, "%s/%s",
						 subpath, den->d_name);
				} else {
					item->spath = strdup(den->d_name);
				}
				**list_tailp = item;
				*list_tailp = &item->next;
				++*countp;
				ddprintf(0, "scan   %s\n", item->spath);
			}
			free(npath);
		}
		closedir(dir);
	}
}

/*
 * This removes any .new files left over in the repo.  These can wind
 * being left around when dsynth is killed.
 */
static void
scandeletenew(const char *path)
{
	struct dirent *den;
	const char *ptr;
	DIR *dir;
	char *buf;

	if ((dir = opendir(path)) == NULL)
		dfatal_errno("Cannot scan directory %s", path);
	while ((den = readdir(dir)) != NULL) {
		if ((ptr = strrchr(den->d_name, '.')) != NULL &&
		    strcmp(ptr, ".new") == 0) {
			asprintf(&buf, "%s/%s", path, den->d_name);
			if (remove(buf) < 0)
				dfatal_errno("remove: Garbage %s\n", buf);
			printf("Deleted Garbage %s\n", buf);
			free(buf);
		}
	}
	closedir(dir);
}

static void
rebuildTerminateSignal(int signo __unused)
{
	if (RebuildRemovePath)
		remove(RebuildRemovePath);
	exit(1);

}
