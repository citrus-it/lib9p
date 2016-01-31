/*
 * Copyright 2016 Jakub Klama <jceel@FreeBSD.org>
 * All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted providing that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 * Based on libixp code: ©2007-2010 Kris Maglione <maglione.k at Gmail>
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <dirent.h>
#include <pwd.h>
#include <grp.h>
#include <libgen.h>
#include "../lib9p.h"
#include "../lib9p_impl.h"
#include "../log.h"

static struct openfile *open_fid(const char *);
static void dostat(struct l9p_stat *, char *, struct stat *);
static void generate_qid(struct stat *, struct l9p_qid *);
static void fs_attach(void *, struct l9p_request *);
static void fs_clunk(void *, struct l9p_request *);
static void fs_create(void *, struct l9p_request *);
static void fs_flush(void *, struct l9p_request *);
static void fs_open(void *, struct l9p_request *);
static void fs_read(void *, struct l9p_request *);
static void fs_remove(void *, struct l9p_request *);
static void fs_stat(void *, struct l9p_request *);
static void fs_walk(void *, struct l9p_request *);
static void fs_write(void *, struct l9p_request *);
static void fs_wstat(void *, struct l9p_request *);

struct fs_softc
{
	const char *fs_rootpath;
	bool fs_readonly;
	TAILQ_HEAD(, fs_tree) fs_auxtrees;
};

struct fs_tree
{
	const char *fst_name;
	const char *fst_path;
	bool fst_readonly;
	TAILQ_ENTRY(fs_tree) fst_link;
};

struct openfile
{
	DIR *dir;
	int fd;
	char *name;
	uid_t uid;
	gid_t gid;
};

static struct openfile *
open_fid(const char *path)
{
	struct openfile *ret;

	ret = l9p_calloc(1, sizeof(*ret));
	ret->fd = -1;
	ret->name = strdup(path);
	return (ret);
}

static void
dostat(struct l9p_stat *s, char *name, struct stat *buf)
{
	struct passwd *user;
	struct group *group;
	
	user = getpwuid(buf->st_uid);
	group = getgrgid(buf->st_gid);

	generate_qid(buf, &s->qid);
	
	s->type = 0;
	s->dev = 0;
	s->mode = buf->st_mode & 0777;
	if (S_ISDIR(buf->st_mode))
		s->mode |= L9P_DMDIR;

	s->atime = buf->st_atime;
	s->mtime = buf->st_mtime;
	s->length = buf->st_size;
	s->name = name;
	s->uid = user != NULL ? user->pw_name : "";
	s->gid = group != NULL ? group->gr_name : "";
	s->muid = user != NULL ? user->pw_name : "";
	s->n_uid = buf->st_uid;
        s->n_gid = buf->st_gid;
        s->n_muid = buf->st_uid;
}

static void
generate_qid(struct stat *buf, struct l9p_qid *qid)
{
	qid->path = buf->st_ino;
	qid->version = 0;
	
	if (S_ISDIR(buf->st_mode))
		qid->type |= L9P_QTDIR;
}

static bool
check_access(struct stat *st, uid_t uid, int amode)
{
	struct passwd *pwd;
	int groups[NGROUPS_MAX];
	int ngroups = NGROUPS_MAX;
	int i;
	
	if (uid == 0)
		return (true);
	
	if (st->st_uid == uid) {
		if (amode == L9P_OREAD && st->st_mode & S_IRUSR)
			return (true);
		
		if (amode == L9P_OWRITE && st->st_mode & S_IWUSR)
			return (true);
		
		if (amode == L9P_OEXEC && st->st_mode & S_IXUSR)
			return (true);
	}

	/* Check for "other" access */
	if (amode == L9P_OREAD && st->st_mode & S_IROTH)
		return (true);

	if (amode == L9P_OWRITE && st->st_mode & S_IWOTH)
		return (true);

	if (amode == L9P_OEXEC && st->st_mode & S_IXOTH)
		return (true);
	
	/* Check for group access */
	pwd = getpwuid(uid);
	getgrouplist(pwd->pw_name, pwd->pw_gid, groups, &ngroups);
	
	for (i = 0; i < ngroups; i++) {
		if (st->st_gid == (gid_t)groups[i]) {
			if (amode == L9P_OREAD && st->st_mode & S_IRGRP)
				return (true);

			if (amode == L9P_OWRITE && st->st_mode & S_IWGRP)
				return (true);

			if (amode == L9P_OEXEC && st->st_mode & S_IXGRP)
				return (true);			
		}
	}
	
	return (false);
}

static void
fs_attach(void *softc, struct l9p_request *req)
{
	struct fs_softc *sc = (struct fs_softc *)softc;
	struct openfile *file;
	uid_t uid;
	
	assert(req->lr_fid != NULL);

	file = open_fid(sc->fs_rootpath);
	req->lr_fid->lo_qid.type = L9P_QTDIR;
	req->lr_fid->lo_qid.path = (uintptr_t)req->lr_fid;
	req->lr_fid->lo_aux = file;
	req->lr_resp.rattach.qid = req->lr_fid->lo_qid;
	
	uid = req->lr_req.tattach.n_uname;
	if (req->lr_conn->lc_version >= L9P_2000U && uid != (uid_t)-1) {
		struct passwd *pwd = getpwuid(uid);
		if (pwd == NULL) {
			l9p_respond(req, EPERM);
			return;
		}
		
		file->uid = pwd->pw_uid;
		file->gid = pwd->pw_gid;
	}

	l9p_respond(req, 0);
}

static void
fs_clunk(void *softc __unused, struct l9p_request *req)
{
	struct openfile *file;

	file = req->lr_fid->lo_aux;
	assert(file != NULL);

	if (file->dir)
		closedir(file->dir);
	else {
		close(file->fd);
		file->fd = -1;
	}

	l9p_respond(req, 0);
}

static void
fs_create(void *softc, struct l9p_request *req)
{
	struct fs_softc *sc = softc;
	struct openfile *file = req->lr_fid->lo_aux;
	struct stat st;
	char *newname;

	assert(file != NULL);
	
	if (sc->fs_readonly) {
		l9p_respond(req, EROFS);
		return;
	}
	
	asprintf(&newname, "%s/%s", file->name, req->lr_req.tcreate.name);
	
	if (stat(file->name, &st) != 0) {
		l9p_respond(req, errno);
		return;
	}
	
	if (!check_access(&st, file->uid, L9P_OWRITE)) {
		l9p_respond(req, EPERM);
		return;
	}
	
	if (req->lr_req.tcreate.perm & L9P_DMDIR) {
		mkdir(newname, 0777);
	} else {
		file->fd = open(newname, O_CREAT | O_TRUNC | req->lr_req.tcreate.mode, req->lr_req.tcreate.perm);
	}
	
	if (chown(newname, file->uid, file->gid) != 0) {
		l9p_respond(req, errno);
		return;
	}
	
	l9p_respond(req, 0);
}

static void
fs_flush(void *softc __unused, struct l9p_request *req)
{
	
	l9p_respond(req, 0);
}

static void
fs_open(void *softc __unused, struct l9p_request *req)
{
	struct l9p_connection *conn = req->lr_conn;
	struct openfile *file = req->lr_fid->lo_aux;
	struct stat st;

	assert(file != NULL);
	
	if (stat(file->name, &st) != 0) {
		l9p_respond(req, errno);
		return;
	}
	
	if (!check_access(&st, file->uid, req->lr_req.topen.mode)) {
		l9p_respond(req, EPERM);
		return;
	}
	
	if (S_ISDIR(st.st_mode))
		file->dir = opendir(file->name);
	else {
		file->fd = open(file->name, req->lr_req.topen.mode);
		if (file->fd < 0) {
			l9p_respond(req, EPERM);
			return;
		}
	}

	req->lr_resp.ropen.iounit = conn->lc_max_io_size;
	l9p_respond(req, 0);
}

static void
fs_read(void *softc __unused, struct l9p_request *req)
{
	struct openfile *file;
	struct l9p_stat l9stat;

	file = req->lr_fid->lo_aux;
	assert(file != NULL);

	if (file->dir != NULL) {
		struct dirent *d;
		struct stat st;

		for (;;) {
			d = readdir(file->dir);
			if (d) {
				stat(d->d_name, &st);
				dostat(&l9stat, d->d_name, &st);
				if (l9p_pack_stat(req, &l9stat) != 0) {
					seekdir(file->dir, -1);
					break;
				}

				continue;
			}

			break;
		}
	} else {
		size_t niov = l9p_truncate_iov(req->lr_data_iov,
                    req->lr_data_niov, req->lr_req.io.count);
		req->lr_resp.io.count = readv(file->fd, req->lr_data_iov, niov);
	}

	l9p_respond(req, 0);
}

static void
fs_remove(void *softc, struct l9p_request *req)
{
	struct fs_softc *sc = softc;
	struct openfile *file;
	struct stat st;
	
	file = req->lr_fid->lo_aux;
	assert(file);
	
	if (sc->fs_readonly) {
		l9p_respond(req, EROFS);
		return;
	}
	
	if (stat(file->name, &st) != 0) {
		l9p_respond(req, errno);
		return;
	}
	
	if (!check_access(&st, file->uid, L9P_OWRITE)) {
		l9p_respond(req, EPERM);
		return;
	}

	if (unlink(file->name) != 0) {
		l9p_respond(req, errno);
		return;
	}

	l9p_respond(req, 0);
}

static void
fs_stat(void *softc __unused, struct l9p_request *req)
{
	struct openfile *file;
	struct stat st;

	file = req->lr_fid->lo_aux;
	assert(file);
	
	stat(file->name, &st);
	dostat(&req->lr_resp.rstat.stat, file->name, &st);

	l9p_respond(req, 0);
}

static void
fs_walk(void *softc __unused, struct l9p_request *req)
{
	int i;
	struct stat buf;
	struct openfile *file = req->lr_fid->lo_aux;
	struct openfile *newfile;
	char name[MAXPATHLEN];

	strcpy(name, file->name);

	/* build full path. Stat full path. Done */
	for (i = 0; i < req->lr_req.twalk.nwname; i++) {
		strcat(name, "/");
		strcat(name, req->lr_req.twalk.wname[i]);
		if (stat(name, &buf) < 0){
			l9p_respond(req, ENOENT);
			return;
		}
		req->lr_resp.rwalk.wqid[i].type = buf.st_mode & S_IFMT >> 8;
		req->lr_resp.rwalk.wqid[i].path = buf.st_ino;
	}

	newfile = open_fid(name);
	newfile->uid = file->uid;
	req->lr_newfid->lo_aux = newfile;
	req->lr_resp.rwalk.nwqid = i;
	l9p_respond(req, 0);
}

static void
fs_write(void *softc, struct l9p_request *req)
{
	struct fs_softc *sc = softc;
	struct openfile *file;

	file = req->lr_fid->lo_aux;
	assert(file != NULL);

	if (sc->fs_readonly) {
		l9p_respond(req, EROFS);
		return;
	}
	
	size_t niov = l9p_truncate_iov(req->lr_data_iov,
            req->lr_data_niov, req->lr_req.io.count);
	
	req->lr_resp.io.count = writev(file->fd, req->lr_data_iov, niov);
	l9p_respond(req, 0);
}

static void
fs_wstat(void *softc, struct l9p_request *req)
{
	struct fs_softc *sc = softc;
	struct openfile *file;
	struct l9p_stat *l9stat = &req->lr_req.twstat.stat;
	
	file = req->lr_fid->lo_aux;
	assert(file != NULL);
	
	if (sc->fs_readonly) {
		l9p_respond(req, EROFS);
		return;
	}
	
	if (l9stat->atime != (uint32_t)~0) {
		
	}
	
	if (l9stat->dev != (uint32_t)~0) {
		l9p_respond(req, EPERM);
		return;
	}
	
	if (l9stat->length != (uint64_t)~0) {
		
	}
	
	if (l9stat->n_uid != (uid_t)~0) {
		
	}
	
	if (l9stat->n_gid != (uid_t)~0) {
		
	}
	
	if (strlen(l9stat->name) > 0) {
		char *dir = dirname(file->name);
		char *newname;
		
		asprintf(&newname, "%s/%s", dir, l9stat->name);
		rename(file->name, newname);
		
		free(newname);
	}
	
	l9p_respond(req, 0);
}

int
l9p_backend_fs_init(struct l9p_backend **backendp, const char *root)
{
	struct l9p_backend *backend;
	struct fs_softc *sc;

	backend = l9p_malloc(sizeof(*backend));
	backend->attach = fs_attach;
	backend->clunk = fs_clunk;
	backend->create = fs_create;
	backend->flush = fs_flush;
	backend->open = fs_open;
	backend->read = fs_read;
	backend->remove = fs_remove;
	backend->stat = fs_stat;
	backend->walk = fs_walk;
	backend->write = fs_write;
	backend->wstat = fs_wstat;

	sc = l9p_malloc(sizeof(*sc));
	sc->fs_rootpath = strdup(root);
	sc->fs_readonly = false;
	backend->softc = sc;

	setpassent(1);
	
	*backendp = backend;
	return (0);
}
