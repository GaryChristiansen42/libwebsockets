/*
 * libwebsockets - small server side websockets and web server implementation
 *
 * Copyright (C) 2010 - 2021 Andy Green <andy@warmcat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "private-lib-core.h"

typedef struct lws_tls_session_cache_mbedtls {
	lws_dll2_t			list;

 	mbedtls_ssl_session		session;
	lws_sorted_usec_list_t		sul_ttl;

	/* name is overallocated here */
} lws_tls_scm_t;

#define lwsl_tlssess lwsl_notice

static int
lws_tls_session_name_from_wsi(struct lws *wsi, char *buf, size_t len)
{
	size_t n;

	/*
	 * We have to include the vhost name in the session tag, since
	 * different vhosts may make connections to the same endpoint using
	 * different client certs.
	 */

	n = (size_t)lws_snprintf(buf, len, "%s.", wsi->a.vhost->name);

	buf += n;
	len = len - n;

	lws_sa46_write_numeric_address(&wsi->sa46_peer, buf, len - 8);
	lws_snprintf(buf + strlen(buf), 8, ":%u", wsi->c_port);

	return 0;
}

static void
__lws_tls_session_destroy(lws_tls_scm_t *ts)
{
	lwsl_tlssess("%s: %s (%u)\n", __func__, (const char *)&ts[1],
				     (unsigned int)(ts->list.owner->count - 1));

	lws_sul_cancel(&ts->sul_ttl);
	mbedtls_ssl_session_free(&ts->session);
	lws_dll2_remove(&ts->list);		/* vh lock */

	lws_free(ts);
}

static lws_tls_scm_t *
__lws_tls_session_lookup_by_name(struct lws_vhost *vh, const char *name)
{
	lws_start_foreach_dll(struct lws_dll2 *, p,
			      lws_dll2_get_head(&vh->tls_sessions)) {
		lws_tls_scm_t *ts = lws_container_of(p, lws_tls_scm_t, list);
		const char *ts_name = (const char *)&ts[1];

		if (!strcmp(name, ts_name))
			return ts;

	} lws_end_foreach_dll(p);

	return NULL;
}

/*
 * If possible, reuse an existing, cached session
 */

void
lws_tls_reuse_session(struct lws *wsi)
{
	char buf[16 + 48 + 1 + 8 + 1];
	mbedtls_ssl_context *msc;
	lws_tls_scm_t *ts;

	if (!wsi->a.vhost ||
	    wsi->a.vhost->options & LWS_SERVER_OPTION_DISABLE_TLS_SESSION_CACHE)
		return;

	lws_vhost_lock(wsi->a.vhost); /* -------------- vh { */

	lws_tls_session_name_from_wsi(wsi, buf, sizeof(buf));
	ts = __lws_tls_session_lookup_by_name(wsi->a.vhost, buf);

	if (!ts) {
		lwsl_tlssess("%s: no existing session for %s\n", __func__, buf);
		goto bail;
	}

	lwsl_tlssess("%s: %s\n", __func__, (const char *)&ts[1]);
	wsi->tls_session_reused = 1;

	msc = SSL_mbedtls_ssl_context_from_SSL(wsi->tls.ssl);
	mbedtls_ssl_set_session(msc, &ts->session);

	/* keep our session list sorted in lru -> mru order */

	lws_dll2_remove(&ts->list);
	lws_dll2_add_tail(&ts->list, &wsi->a.vhost->tls_sessions);

bail:
	lws_vhost_unlock(wsi->a.vhost); /* } vh --------------  */
}

static int
lws_tls_session_destroy_dll(struct lws_dll2 *d, void *user)
{
	lws_tls_scm_t *ts = lws_container_of(d, lws_tls_scm_t, list);

	__lws_tls_session_destroy(ts);

	return 0;
}

void
lws_tls_session_vh_destroy(struct lws_vhost *vh)
{
	lws_dll2_foreach_safe(&vh->tls_sessions, NULL,
			      lws_tls_session_destroy_dll);
}

static void
lws_tls_session_expiry_cb(lws_sorted_usec_list_t *sul)
{
	lws_tls_scm_t *ts = lws_container_of(sul, lws_tls_scm_t, sul_ttl);
	struct lws_vhost *vh = lws_container_of(ts->list.owner,
						struct lws_vhost, tls_sessions);

	lws_vhost_lock(vh); /* -------------- vh { */
	__lws_tls_session_destroy(ts);
	lws_vhost_unlock(vh); /* } vh --------------  */
}

/*
 * Called after SSL_accept on the wsi
 */

int
lws_tls_session_new_mbedtls(struct lws *wsi)
{
	char buf[16 + 48 + 1 + 8 + 1];
	mbedtls_ssl_context *msc;
	struct lws_vhost *vh;
	lws_tls_scm_t *ts;
	size_t nl;
#if !defined(LWS_WITH_NO_LOGS) && defined(_DEBUG)
	const char *disposition = "reuse";
#endif

	vh = wsi->a.vhost;
	if (vh->options & LWS_SERVER_OPTION_DISABLE_TLS_SESSION_CACHE)
		return 0;

	lws_tls_session_name_from_wsi(wsi, buf, sizeof(buf));
	nl = strlen(buf);

	msc = SSL_mbedtls_ssl_context_from_SSL(wsi->tls.ssl);

	lws_vhost_lock(vh); /* -------------- vh { */

	ts = __lws_tls_session_lookup_by_name(vh, buf);

	if (!ts) {
		/*
		 * We have to make our own, new session
		 */

		if (vh->tls_sessions.count == vh->tls_session_cache_max) {

			/*
			 * We have reached the vhost's session cache limit,
			 * prune the LRU / head
			 */
			ts = lws_container_of(vh->tls_sessions.head,
					      lws_tls_scm_t, list);

			lwsl_tlssess("%s: pruning oldest session (hit max %u)\n",
				     __func__,
				     (unsigned int)vh->tls_session_cache_max);

			lws_vhost_lock(vh); /* -------------- vh { */
			__lws_tls_session_destroy(ts);
			lws_vhost_unlock(vh); /* } vh --------------  */
		}

		ts = lws_malloc(sizeof(*ts) + nl + 1, __func__);

		if (!ts)
			goto bail;

		memset(ts, 0, sizeof(*ts));
		memcpy(&ts[1], buf, nl + 1);

		if (mbedtls_ssl_get_session(msc, &ts->session))
			/* no joy for whatever reason */
			goto bail;

		lws_dll2_add_tail(&ts->list, &vh->tls_sessions);

		lws_sul_schedule(wsi->a.context, wsi->tsi, &ts->sul_ttl,
				 lws_tls_session_expiry_cb,
				 (int64_t)vh->tls.tls_session_cache_ttl *
							 LWS_US_PER_SEC);

#if !defined(LWS_WITH_NO_LOGS) && defined(_DEBUG)
		disposition = "new";
#endif
	} else {

		mbedtls_ssl_session_free(&ts->session);

		if (mbedtls_ssl_get_session(msc, &ts->session))
			/* no joy for whatever reason */
			goto bail;

		/* keep our session list sorted in lru -> mru order */

		lws_dll2_remove(&ts->list);
		lws_dll2_add_tail(&ts->list, &vh->tls_sessions);
	}

	lws_vhost_unlock(vh); /* } vh --------------  */

	lwsl_tlssess("%s: %s: %s %s, (%s:%u)\n", __func__,
		     wsi->lc.gutag, disposition, buf, vh->name,
		     (unsigned int)vh->tls_sessions.count);

	/*
	 * indicate we will hold on to the SSL_SESSION reference, and take
	 * responsibility to call SSL_SESSION_free() on it ourselves
	 */

	return 1;

bail:
	lws_vhost_unlock(vh); /* } vh --------------  */

	return 0;
}

void
lws_tls_session_cache(struct lws_vhost *vh, uint32_t ttl)
{
	/* Default to 1hr max recommendation from RFC5246 F.1.4 */
	vh->tls.tls_session_cache_ttl = !ttl ? 3600 : ttl;
}
