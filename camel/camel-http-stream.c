/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 *  Authors: Jeffrey Stedfast <fejj@ximian.com>
 *
 *  Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib/gi18n-lib.h>

#include "camel-http-stream.h"
#include "camel-mime-utils.h"
#include "camel-net-utils.h"
#include "camel-service.h" /* for hostname stuff */
#include "camel-session.h"
#include "camel-stream-buffer.h"
#include "camel-tcp-stream-raw.h"

#include "camel-tcp-stream-ssl.h"

#define SSL_FLAGS (CAMEL_TCP_STREAM_SSL_ENABLE_SSL2 | CAMEL_TCP_STREAM_SSL_ENABLE_SSL3)

#ifdef G_OS_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#define d(x)

static void camel_http_stream_seekable_init (GSeekableIface *interface);

G_DEFINE_TYPE_WITH_CODE (CamelHttpStream, camel_http_stream, CAMEL_TYPE_STREAM,
	G_IMPLEMENT_INTERFACE (G_TYPE_SEEKABLE, camel_http_stream_seekable_init))

static CamelStream *
http_connect (CamelHttpStream *http,
              CamelURL *url,
              GCancellable *cancellable,
              GError **error)
{
	CamelTcpStream *tcp_stream;
	CamelStream *stream = NULL;
	gint errsave;
	gchar *serv;

	d(printf("connecting to http stream @ '%s'\n", url->host));

	if (!g_ascii_strcasecmp (url->protocol, "https")) {
		stream = camel_tcp_stream_ssl_new (http->session, url->host, SSL_FLAGS);
	} else {
		stream = camel_tcp_stream_raw_new ();
	}

	if (stream == NULL) {
		errno = EINVAL;
		g_set_error (
			error, G_IO_ERROR,
			g_io_error_from_errno (errno),
			"%s", g_strerror (errno));
		return NULL;
	}

	if (url->port) {
		serv = g_alloca (16);
		sprintf(serv, "%d", url->port);
	} else {
		serv = url->protocol;
	}

	tcp_stream = CAMEL_TCP_STREAM (stream);

	if (camel_tcp_stream_connect (
		tcp_stream, url->host, serv, 0, cancellable, error) == -1) {
		errsave = errno;
		g_object_unref (stream);
		errno = errsave;
		return NULL;
	}

	http->raw = stream;
	http->read = camel_stream_buffer_new (stream, CAMEL_STREAM_BUFFER_READ);

	return stream;
}

static void
http_disconnect (CamelHttpStream *http)
{
	if (http->raw) {
		g_object_unref (http->raw);
		http->raw = NULL;
	}

	if (http->read) {
		g_object_unref (http->read);
		http->read = NULL;
	}

	if (http->parser) {
		g_object_unref (http->parser);
		http->parser = NULL;
	}
}

static gint
http_method_invoke (CamelHttpStream *http,
                    GCancellable *cancellable,
                    GError **error)
{
	GString *buffer;
	const gchar *method = NULL, *use_url;
	const gchar *user_agent;
	gchar *url;

	buffer = g_string_sized_new (1024);

	switch (http->method) {
	case CAMEL_HTTP_METHOD_GET:
		method = "GET";
		break;
	case CAMEL_HTTP_METHOD_HEAD:
		method = "HEAD";
		break;
	default:
		g_assert_not_reached ();
	}

	url = camel_url_to_string (http->url, 0);

	if (http->proxy) {
		use_url = url;
	} else if (http->url->host && *http->url->host) {
		use_url = strstr (url, http->url->host) + strlen (http->url->host);
	} else {
		use_url = http->url->path;
	}

	user_agent = http->user_agent;
	if (user_agent == NULL)
		user_agent = "CamelHttpStream/1.0";

	g_string_append_printf (
		buffer, "%s %s HTTP/1.0\r\n", method, use_url);

	g_string_append_printf (
		buffer, "User-Agent: %s\r\n", user_agent);

	g_string_append_printf (
		buffer, "Host: %s\r\n", http->url->host);

	g_free (url);

	if (http->authrealm != NULL)
		g_string_append_printf (
			buffer, "WWW-Authenticate: %s\r\n",
			http->authrealm);

	if (http->authpass != NULL && http->proxy != NULL)
		g_string_append_printf (
			buffer, "Proxy-Authorization: Basic %s\r\n",
			http->authpass);

	g_string_append (buffer, "\r\n");

	/* end the headers */
	if (camel_stream_write (
		http->raw, buffer->str, buffer->len, cancellable, error) == -1 ||
		camel_stream_flush (http->raw, cancellable, error) == -1) {
		g_string_free (buffer, TRUE);
		http_disconnect (http);
		return -1;
	}

	g_string_free (buffer, TRUE);

	return 0;
}

static gint
http_get_headers (CamelHttpStream *http,
                  GError **error)
{
	struct _camel_header_raw *headers, *node, *tail;
	const gchar *type;
	gchar *buf;
	gsize len;
	gint err;

	if (http->parser)
		g_object_unref (http->parser);

	http->parser = camel_mime_parser_new ();
	camel_mime_parser_init_with_stream (http->parser, http->read, NULL);

	switch (camel_mime_parser_step (http->parser, &buf, &len)) {
	case CAMEL_MIME_PARSER_STATE_MESSAGE:
	case CAMEL_MIME_PARSER_STATE_HEADER:
		headers = camel_mime_parser_headers_raw (http->parser);
		if (http->content_type)
			camel_content_type_unref (http->content_type);
		type = camel_header_raw_find (&headers, "Content-Type", NULL);
		if (type)
			http->content_type = camel_content_type_decode (type);
		else
			http->content_type = NULL;

		if (http->headers)
			camel_header_raw_clear (&http->headers);

		http->headers = NULL;
		tail = (struct _camel_header_raw *) &http->headers;

		d(printf("HTTP Headers:\n"));
		while (headers) {
			d(printf(" %s:%s\n", headers->name, headers->value));
			node = g_new (struct _camel_header_raw, 1);
			node->next = NULL;
			node->name = g_strdup (headers->name);
			node->value = g_strdup (headers->value);
			node->offset = headers->offset;
			tail->next = node;
			tail = node;
			headers = headers->next;
		}

		break;
	default:
		g_warning ("Invalid state encountered???: %u", camel_mime_parser_state (http->parser));
	}

	err = camel_mime_parser_errno (http->parser);

	if (err != 0) {
		g_set_error (
			error, G_IO_ERROR,
			g_io_error_from_errno (err),
			"%s", g_strerror (err));
		g_object_unref (http->parser);
		http->parser = NULL;
		goto exception;
	}

	camel_mime_parser_drop_step (http->parser);

	return 0;

 exception:
	http_disconnect (http);

	return -1;
}

static const gchar *
http_next_token (const guchar *in)
{
	const guchar *inptr = in;

	while (*inptr && !isspace ((gint) *inptr))
		inptr++;

	while (*inptr && isspace ((gint) *inptr))
		inptr++;

	return (const gchar *) inptr;
}

static gint
http_get_statuscode (CamelHttpStream *http,
                     GCancellable *cancellable,
                     GError **error)
{
	const gchar *token;
	gchar buffer[4096];

	if (camel_stream_buffer_gets (
		CAMEL_STREAM_BUFFER (http->read), buffer,
		sizeof (buffer), cancellable, error) <= 0)
		return -1;

	d(printf("HTTP Status: %s\n", buffer));

	/* parse the HTTP status code */
	if (!g_ascii_strncasecmp (buffer, "HTTP/", 5)) {
		token = http_next_token ((const guchar *) buffer);
		http->statuscode = camel_header_decode_int (&token);
		return http->statuscode;
	}

	http_disconnect (http);

	return -1;
}

static void
http_stream_dispose (GObject *object)
{
	CamelHttpStream *http = CAMEL_HTTP_STREAM (object);

	if (http->parser != NULL) {
		g_object_unref (http->parser);
		http->parser = NULL;
	}

	if (http->content_type != NULL) {
		camel_content_type_unref (http->content_type);
		http->content_type = NULL;
	}

	if (http->session != NULL) {
		g_object_unref (http->session);
		http->session = NULL;
	}

	if (http->raw != NULL) {
		g_object_unref (http->raw);
		http->raw = NULL;
	}

	if (http->read != NULL) {
		g_object_unref (http->read);
		http->read = NULL;
	}

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (camel_http_stream_parent_class)->dispose (object);
}

static void
http_stream_finalize (GObject *object)
{
	CamelHttpStream *http = CAMEL_HTTP_STREAM (object);

	camel_header_raw_clear (&http->headers);

	if (http->url != NULL)
		camel_url_free (http->url);

	if (http->proxy)
		camel_url_free (http->proxy);

	if (http->user_agent)
		g_free (http->user_agent);

	g_free (http->authrealm);
	g_free (http->authpass);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (camel_http_stream_parent_class)->finalize (object);
}

static gssize
http_stream_read (CamelStream *stream,
                  gchar *buffer,
                  gsize n,
                  GCancellable *cancellable,
                  GError **error)
{
	CamelHttpStream *http = CAMEL_HTTP_STREAM (stream);
	const gchar *parser_buf;
	gssize nread;

	if (http->method != CAMEL_HTTP_METHOD_GET &&
		http->method != CAMEL_HTTP_METHOD_HEAD) {
		errno = EIO;
		g_set_error (
			error, G_IO_ERROR,
			g_io_error_from_errno (errno),
			"%s", g_strerror (errno));
		return -1;
	}

 redirect:

	if (!http->raw) {
		if (http_connect (
			http, http->proxy ? http->proxy :
			http->url, cancellable, error) == NULL)
			return -1;

		if (http_method_invoke (http, cancellable, error) == -1) {
			http_disconnect (http);
			return -1;
		}

		if (http_get_statuscode (http, cancellable, error) == -1) {
			http_disconnect (http);
			return -1;
		}

		if (http_get_headers (http, error) == -1) {
			http_disconnect (http);
			return -1;
		}

		switch (http->statuscode) {
		case 200:
		case 206:
			/* we are OK to go... */
			break;
		case 301:
		case 302: {
			gchar *loc;
			CamelURL *url;

			camel_content_type_unref (http->content_type);
			http->content_type = NULL;
			http_disconnect (http);

			loc = g_strdup(camel_header_raw_find(&http->headers, "Location", NULL));
			if (loc == NULL) {
				camel_header_raw_clear (&http->headers);
				return -1;
			}

			/* redirect... */
			g_strstrip (loc);
			d(printf("HTTP redirect, location = %s\n", loc));
			url = camel_url_new_with_base (http->url, loc);
			camel_url_free (http->url);
			http->url = url;
			if (url == NULL)
				http->url = camel_url_new (loc, NULL);
			g_free (loc);
			if (http->url == NULL) {
				camel_header_raw_clear (&http->headers);
				return -1;
			}
			d(printf(" redirect url = %p\n", http->url));
			camel_header_raw_clear (&http->headers);

			goto redirect;
			break; }
		case 407:
			/* failed proxy authentication? */
		default:
			/* unknown error */
			http_disconnect (http);
			return -1;
		}
	}

	if (n == 0)
		return 0;

	nread = camel_mime_parser_read (http->parser, &parser_buf, n, error);

	if (nread > 0)
		memcpy (buffer, parser_buf, nread);
	else if (nread == 0)
		stream->eos = TRUE;

	return nread;
}

static gssize
http_stream_write (CamelStream *stream,
                   const gchar *buffer,
                   gsize n,
                   GCancellable *cancellable,
                   GError **error)
{
	return -1;
}

static gint
http_stream_flush (CamelStream *stream,
                   GCancellable *cancellable,
                   GError **error)
{
	CamelHttpStream *http = (CamelHttpStream *) stream;

	if (http->raw)
		return camel_stream_flush (http->raw, cancellable, error);
	else
		return 0;
}

static gint
http_stream_close (CamelStream *stream,
                   GCancellable *cancellable,
                   GError **error)
{
	CamelHttpStream *http = (CamelHttpStream *) stream;

	if (http->raw) {
		if (camel_stream_close (http->raw, cancellable, error) == -1)
			return -1;

		http_disconnect (http);
	}

	return 0;
}

static goffset
http_stream_tell (GSeekable *seekable)
{
	return 0;
}

static gboolean
http_stream_can_seek (GSeekable *seekable)
{
	return TRUE;
}

static gboolean
http_stream_seek (GSeekable *seekable,
                  goffset offset,
                  GSeekType type,
                  GCancellable *cancellable,
                  GError **error)
{
	CamelHttpStream *http;

	http = CAMEL_HTTP_STREAM (seekable);

	if (type != G_SEEK_SET || offset != 0) {
		g_set_error_literal (
			error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Only reset to beginning is supported with CamelHttpStream"));
		return FALSE;
	}

	if (http->raw)
		http_disconnect (http);

	return TRUE;
}

static gboolean
http_stream_can_truncate (GSeekable *seekable)
{
	return FALSE;
}

static gboolean
http_stream_truncate_fn (GSeekable *seekable,
                         goffset offset,
                         GCancellable *cancellable,
                         GError **error)
{
	/* XXX Don't bother translating this.  Camel never calls it. */
	g_set_error_literal (
		error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		"Truncation is not supported");

	return FALSE;
}

static void
camel_http_stream_class_init (CamelHttpStreamClass *class)
{
	GObjectClass *object_class;
	CamelStreamClass *stream_class;

	object_class = G_OBJECT_CLASS (class);
	object_class->dispose = http_stream_dispose;
	object_class->finalize = http_stream_finalize;

	stream_class = CAMEL_STREAM_CLASS (class);
	stream_class->read = http_stream_read;
	stream_class->write = http_stream_write;
	stream_class->flush = http_stream_flush;
	stream_class->close = http_stream_close;
}

static void
camel_http_stream_seekable_init (GSeekableIface *interface)
{
	interface->tell = http_stream_tell;
	interface->can_seek = http_stream_can_seek;
	interface->seek = http_stream_seek;
	interface->can_truncate = http_stream_can_truncate;
	interface->truncate_fn = http_stream_truncate_fn;
}

static void
camel_http_stream_init (CamelHttpStream *http)
{
}

/**
 * camel_http_stream_new:
 * @method: HTTP method
 * @session: active session
 * @url: URL to act upon
 *
 * Returns: a http stream
 **/
CamelStream *
camel_http_stream_new (CamelHttpMethod method,
                       CamelSession *session,
                       CamelURL *url)
{
	CamelHttpStream *stream;
	gchar *str;

	g_return_val_if_fail (CAMEL_IS_SESSION (session), NULL);
	g_return_val_if_fail (url != NULL, NULL);

	stream = g_object_new (CAMEL_TYPE_HTTP_STREAM, NULL);

	stream->method = method;
	stream->session = g_object_ref (session);

	str = camel_url_to_string (url, 0);
	stream->url = camel_url_new (str, NULL);
	g_free (str);

	return (CamelStream *) stream;
}

CamelContentType *
camel_http_stream_get_content_type (CamelHttpStream *http_stream)
{
	g_return_val_if_fail (CAMEL_IS_HTTP_STREAM (http_stream), NULL);

	if (!http_stream->content_type && !http_stream->raw) {
		CamelStream *stream = CAMEL_STREAM (http_stream);

		if (http_stream_read (stream, NULL, 0, NULL, NULL) == -1)
			return NULL;
	}

	if (http_stream->content_type)
		camel_content_type_ref (http_stream->content_type);

	return http_stream->content_type;
}

void
camel_http_stream_set_user_agent (CamelHttpStream *http_stream,
                                  const gchar *user_agent)
{
	g_return_if_fail (CAMEL_IS_HTTP_STREAM (http_stream));

	g_free (http_stream->user_agent);
	http_stream->user_agent = g_strdup (user_agent);
}

void
camel_http_stream_set_proxy_authrealm (CamelHttpStream *http_stream,
                                       const gchar *proxy_authrealm)
{
	g_return_if_fail (CAMEL_IS_HTTP_STREAM (http_stream));

	g_free (http_stream->authrealm);
	http_stream->authrealm = g_strdup (proxy_authrealm);
}

void
camel_http_stream_set_proxy_authpass (CamelHttpStream *http_stream,
                                      const gchar *proxy_authpass)
{
	g_return_if_fail (CAMEL_IS_HTTP_STREAM (http_stream));

	g_free (http_stream->authpass);
	http_stream->authpass = g_strdup (proxy_authpass);
}
