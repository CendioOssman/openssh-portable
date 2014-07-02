/*	$OpenBSD: sshbuf.c,v 1.2 2014/06/25 14:16:09 deraadt Exp $	*/
/*
 * Copyright (c) 2011 Damien Miller
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define SSHBUF_INTERNAL
#include "includes.h"

#include <sys/types.h>
#include <sys/param.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ssherr.h"
#include "sshbuf.h"

/* XXX move to defines.h? */
#if defined(__GNUC__) && \
    ((__GNUC__ > (2)) || (__GNUC__ == (2) && __GNUC_MINOR__ >= (96)))
#define __predict_true(exp)     __builtin_expect(((exp) != 0), 1)
#define __predict_false(exp)    __builtin_expect(((exp) != 0), 0)
#else
#define __predict_true(exp)     ((exp) != 0)
#define __predict_false(exp)    ((exp) != 0)
#endif

static inline int
sshbuf_check_sanity(const struct sshbuf *buf)
{
	SSHBUF_TELL("sanity");
	if (__predict_false(buf == NULL ||
	    (!buf->readonly && buf->d != buf->cd) ||
	    buf->refcount < 1 || buf->refcount > SSHBUF_REFS_MAX ||
	    buf->cd == NULL ||
	    (buf->dont_free && (buf->readonly || buf->parent != NULL)) ||
	    buf->max_size > SSHBUF_SIZE_MAX ||
	    buf->alloc > buf->max_size ||
	    buf->size > buf->alloc ||
	    buf->off > buf->size)) {
		/* Do not try to recover from corrupted buffer internals */
		SSHBUF_DBG(("SSH_ERR_INTERNAL_ERROR"));
		signal(SIGSEGV, SIG_DFL);
		raise(SIGSEGV);
		return SSH_ERR_INTERNAL_ERROR;
	}
	return 0;
}

static void
sshbuf_maybe_pack(struct sshbuf *buf, int force)
{
	SSHBUF_DBG(("force %d", force));
	SSHBUF_TELL("pre-pack");
	if (buf->off == 0 || buf->readonly || buf->refcount > 1)
		return;
	if (force ||
	    (buf->off >= SSHBUF_PACK_MIN && buf->off >= buf->size / 2)) {
		memmove(buf->d, buf->d + buf->off, buf->size - buf->off);
		buf->size -= buf->off;
		buf->off = 0;
		SSHBUF_TELL("packed");
	}
}

struct sshbuf *
sshbuf_new(void)
{
	struct sshbuf *ret;

	if ((ret = calloc(sizeof(*ret), 1)) == NULL)
		return NULL;
	ret->alloc = SSHBUF_SIZE_INIT;
	ret->max_size = SSHBUF_SIZE_MAX;
	ret->readonly = 0;
	ret->refcount = 1;
	ret->parent = NULL;
	if ((ret->cd = ret->d = calloc(1, ret->alloc)) == NULL) {
		free(ret);
		return NULL;
	}
	return ret;
}

struct sshbuf *
sshbuf_from(const void *blob, size_t len)
{
	struct sshbuf *ret;

	if (blob == NULL || len > SSHBUF_SIZE_MAX ||
	    (ret = calloc(sizeof(*ret), 1)) == NULL)
		return NULL;
	ret->alloc = ret->size = ret->max_size = len;
	ret->readonly = 1;
	ret->refcount = 1;
	ret->parent = NULL;
	ret->cd = blob;
	ret->d = NULL;
	return ret;
}

int
sshbuf_set_parent(struct sshbuf *child, struct sshbuf *parent)
{
	int r;

	if ((r = sshbuf_check_sanity(child)) != 0 ||
	    (r = sshbuf_check_sanity(parent)) != 0)
		return r;
	child->parent = parent;
	child->parent->refcount++;
	return 0;
}

struct sshbuf *
sshbuf_fromb(struct sshbuf *buf)
{
	struct sshbuf *ret;

	if (sshbuf_check_sanity(buf) != 0)
		return NULL;
	if ((ret = sshbuf_from(sshbuf_ptr(buf), sshbuf_len(buf))) == NULL)
		return NULL;
	if (sshbuf_set_parent(ret, buf) != 0) {
		sshbuf_free(ret);
		return NULL;
	}
	return ret;
}

void
sshbuf_init(struct sshbuf *ret)
{
	bzero(ret, sizeof(*ret));
	ret->alloc = SSHBUF_SIZE_INIT;
	ret->max_size = SSHBUF_SIZE_MAX;
	ret->readonly = 0;
	ret->dont_free = 1;
	ret->refcount = 1;
	if ((ret->cd = ret->d = calloc(1, ret->alloc)) == NULL)
		ret->alloc = 0;
}

void
sshbuf_free(struct sshbuf *buf)
{
	int dont_free = 0;

	if (buf == NULL)
		return;
	/*
	 * The following will leak on insane buffers, but this is the safest
	 * course of action - an invalid pointer or already-freed pointer may
	 * have been passed to us and continuing to scribble over memory would
	 * be bad.
	 */
	if (sshbuf_check_sanity(buf) != 0)
		return;
	/*
	 * If we are a child, the free our parent to decrement its reference
	 * count and possibly free it.
	 */
	if (buf->parent != NULL) {
		sshbuf_free(buf->parent);
		buf->parent = NULL;
	}
	/*
	 * If we are a parent with still-extant children, then don't free just
	 * yet. The last child's call to sshbuf_free should decrement our
	 * refcount to 0 and trigger the actual free.
	 */
	buf->refcount--;
	if (buf->refcount > 0)
		return;
	dont_free = buf->dont_free;
	if (!buf->readonly) {
		bzero(buf->d, buf->alloc);
		free(buf->d);
	}
	bzero(buf, sizeof(*buf));
	if (!dont_free)
		free(buf);
}

void
sshbuf_reset(struct sshbuf *buf)
{
	u_char *d;

	if (buf->readonly || buf->refcount > 1) {
		/* Nonsensical. Just make buffer appear empty */
		buf->off = buf->size;
		return;
	}
	if (sshbuf_check_sanity(buf) == 0)
		bzero(buf->d, buf->alloc);
	buf->off = buf->size = 0;
	if (buf->alloc != SSHBUF_SIZE_INIT) {
		if ((d = realloc(buf->d, SSHBUF_SIZE_INIT)) != NULL) {
			buf->cd = buf->d = d;
			buf->alloc = SSHBUF_SIZE_INIT;
		}
	}
}

size_t
sshbuf_max_size(const struct sshbuf *buf)
{
	return buf->max_size;
}

size_t
sshbuf_alloc(const struct sshbuf *buf)
{
	return buf->alloc;
}

const struct sshbuf *
sshbuf_parent(const struct sshbuf *buf)
{
	return buf->parent;
}

u_int
sshbuf_refcount(const struct sshbuf *buf)
{
	return buf->refcount;
}

int
sshbuf_set_max_size(struct sshbuf *buf, size_t max_size)
{
	size_t rlen;
	u_char *dp;
	int r;

	SSHBUF_DBG(("set max buf = %p len = %zu", buf, max_size));
	if ((r = sshbuf_check_sanity(buf)) != 0)
		return r;
	if (max_size == buf->max_size)
		return 0;
	if (buf->readonly || buf->refcount > 1)
		return SSH_ERR_BUFFER_READ_ONLY;
	if (max_size > SSHBUF_SIZE_MAX)
		return SSH_ERR_NO_BUFFER_SPACE;
	/* pack and realloc if necessary */
	sshbuf_maybe_pack(buf, max_size < buf->size);
	if (max_size < buf->alloc && max_size > buf->size) {
		if (buf->size < SSHBUF_SIZE_INIT)
			rlen = SSHBUF_SIZE_INIT;
		else
			rlen = roundup(buf->size, SSHBUF_SIZE_INC);
		if (rlen > max_size)
			rlen = max_size;
		bzero(buf->d + buf->size, buf->alloc - buf->size);
		SSHBUF_DBG(("new alloc = %zu", rlen));
		if ((dp = realloc(buf->d, rlen)) == NULL)
			return SSH_ERR_ALLOC_FAIL;
		buf->cd = buf->d = dp;
		buf->alloc = rlen;
	}
	SSHBUF_TELL("new-max");
	if (max_size < buf->alloc)
		return SSH_ERR_NO_BUFFER_SPACE;
	buf->max_size = max_size;
	return 0;
}

size_t
sshbuf_len(const struct sshbuf *buf)
{
	if (sshbuf_check_sanity(buf) != 0)
		return 0;
	return buf->size - buf->off;
}

size_t
sshbuf_avail(const struct sshbuf *buf)
{
	if (sshbuf_check_sanity(buf) != 0 || buf->readonly || buf->refcount > 1)
		return 0;
	return buf->max_size - (buf->size - buf->off);
}

const u_char *
sshbuf_ptr(const struct sshbuf *buf)
{
	if (sshbuf_check_sanity(buf) != 0)
		return NULL;
	return buf->cd + buf->off;
}

u_char *
sshbuf_mutable_ptr(const struct sshbuf *buf)
{
	if (sshbuf_check_sanity(buf) != 0 || buf->readonly || buf->refcount > 1)
		return NULL;
	return buf->d + buf->off;
}

int
sshbuf_check_reserve(const struct sshbuf *buf, size_t len)
{
	int r;

	if ((r = sshbuf_check_sanity(buf)) != 0)
		return r;
	if (buf->readonly || buf->refcount > 1)
		return SSH_ERR_BUFFER_READ_ONLY;
	SSHBUF_TELL("check");
	/* Check that len is reasonable and that max_size + available < len */
	if (len > buf->max_size || buf->max_size - len < buf->size - buf->off)
		return SSH_ERR_NO_BUFFER_SPACE;
	return 0;
}

int
sshbuf_reserve(struct sshbuf *buf, size_t len, u_char **dpp)
{
	size_t rlen, need;
	u_char *dp;
	int r;

	if (dpp != NULL)
		*dpp = NULL;

	SSHBUF_DBG(("reserve buf = %p len = %zu", buf, len));
	if ((r = sshbuf_check_reserve(buf, len)) != 0)
		return r;
	/*
	 * If the requested allocation appended would push us past max_size
	 * then pack the buffer, zeroing buf->off.
	 */
	sshbuf_maybe_pack(buf, buf->size + len > buf->max_size);
	SSHBUF_TELL("reserve");
	if (len + buf->size > buf->alloc) {
		/*
		 * Prefer to alloc in SSHBUF_SIZE_INC units, but
		 * allocate less if doing so would overflow max_size.
		 */
		need = len + buf->size - buf->alloc;
		rlen = roundup(buf->alloc + need, SSHBUF_SIZE_INC);
		SSHBUF_DBG(("need %zu initial rlen %zu", need, rlen));
		if (rlen > buf->max_size)
			rlen = buf->alloc + need;
		SSHBUF_DBG(("adjusted rlen %zu", rlen));
		if ((dp = realloc(buf->d, rlen)) == NULL) {
			SSHBUF_DBG(("realloc fail"));
			if (dpp != NULL)
				*dpp = NULL;
			return SSH_ERR_ALLOC_FAIL;
		}
		buf->alloc = rlen;
		buf->cd = buf->d = dp;
		if ((r = sshbuf_check_reserve(buf, len)) < 0) {
			/* shouldn't fail */
			if (dpp != NULL)
				*dpp = NULL;
			return r;
		}
	}
	dp = buf->d + buf->size;
	buf->size += len;
	SSHBUF_TELL("done");
	if (dpp != NULL)
		*dpp = dp;
	return 0;
}

int
sshbuf_consume(struct sshbuf *buf, size_t len)
{
	int r;

	SSHBUF_DBG(("len = %zu", len));
	if ((r = sshbuf_check_sanity(buf)) != 0)
		return r;
	if (len == 0)
		return 0;
	if (len > sshbuf_len(buf))
		return SSH_ERR_MESSAGE_INCOMPLETE;
	buf->off += len;
	SSHBUF_TELL("done");
	return 0;
}

int
sshbuf_consume_end(struct sshbuf *buf, size_t len)
{
	int r;

	SSHBUF_DBG(("len = %zu", len));
	if ((r = sshbuf_check_sanity(buf)) != 0)
		return r;
	if (len == 0)
		return 0;
	if (len > sshbuf_len(buf))
		return SSH_ERR_MESSAGE_INCOMPLETE;
	buf->size -= len;
	SSHBUF_TELL("done");
	return 0;
}

