/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* camel-service.h : Abstract class for an email service */
/*
 *
 * Authors: Bertrand Guiheneuf <bertrand@helixcode.com>
 *          Michael Zucchi <notzed@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA
 */

#if !defined (__CAMEL_H_INSIDE__) && !defined (CAMEL_COMPILATION)
#error "Only <camel/camel.h> can be included directly."
#endif

#ifndef CAMEL_SERVICE_H
#define CAMEL_SERVICE_H

#include <camel/camel-enums.h>
#include <camel/camel-object.h>
#include <camel/camel-url.h>
#include <camel/camel-provider.h>
#include <camel/camel-operation.h>
#include <camel/camel-settings.h>

/* Standard GObject macros */
#define CAMEL_TYPE_SERVICE \
	(camel_service_get_type ())
#define CAMEL_SERVICE(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), CAMEL_TYPE_SERVICE, CamelService))
#define CAMEL_SERVICE_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), CAMEL_TYPE_SERVICE, CamelServiceClass))
#define CAMEL_IS_SERVICE(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), CAMEL_TYPE_SERVICE))
#define CAMEL_IS_SERVICE_CLASS(obj) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), CAMEL_TYPE_SERVICE))
#define CAMEL_SERVICE_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), CAMEL_TYPE_SERVICE, CamelServiceClass))

/**
 * CAMEL_SERVICE_ERROR:
 *
 * Since: 2.32
 **/
#define CAMEL_SERVICE_ERROR \
	(camel_service_error_quark ())

G_BEGIN_DECLS

struct _CamelSession;

typedef struct _CamelService CamelService;
typedef struct _CamelServiceClass CamelServiceClass;
typedef struct _CamelServicePrivate CamelServicePrivate;

/**
 * CamelServiceError:
 *
 * Since: 2.32
 **/
typedef enum {
	CAMEL_SERVICE_ERROR_INVALID,
	CAMEL_SERVICE_ERROR_URL_INVALID,
	CAMEL_SERVICE_ERROR_UNAVAILABLE,
	CAMEL_SERVICE_ERROR_CANT_AUTHENTICATE,
	CAMEL_SERVICE_ERROR_NOT_CONNECTED
} CamelServiceError;

/**
 * CamelServiceLock:
 *
 * Since: 2.32
 **/
typedef enum {
	CAMEL_SERVICE_REC_CONNECT_LOCK,
	CAMEL_SERVICE_CONNECT_OP_LOCK
} CamelServiceLock;

struct _CamelService {
	CamelObject parent;
	CamelServicePrivate *priv;
};

struct _CamelServiceClass {
	CamelObjectClass parent_class;

	GType settings_type;

	/* Non-Blocking Methods */
	gchar *		(*get_name)		(CamelService *service,
						 gboolean brief);
	void		(*cancel_connect)	(CamelService *service);

	/* Synchronous I/O Methods */
	gboolean	(*connect_sync)		(CamelService *service,
						 GCancellable *cancellable,
						 GError **error);
	gboolean	(*disconnect_sync)	(CamelService *service,
						 gboolean clean,
						 GCancellable *cancellable,
						 GError **error);
	CamelAuthenticationResult
			(*authenticate_sync)	(CamelService *service,
						 const gchar *mechanism,
						 GCancellable *cancellable,
						 GError **error);
	GList *		(*query_auth_types_sync)
						(CamelService *service,
						 GCancellable *cancellable,
						 GError **error);

	/* Asynchronous I/O Methods (all have defaults) */
	void		(*authenticate)		(CamelService *service,
						 const gchar *mechanism,
						 gint io_priority,
						 GCancellable *cancellable,
						 GAsyncReadyCallback callback,
						 gpointer user_data);
	CamelAuthenticationResult
			(*authenticate_finish)	(CamelService *service,
						 GAsyncResult *result,
						 GError **error);
	void		(*query_auth_types)	(CamelService *service,
						 gint io_priority,
						 GCancellable *cancellable,
						 GAsyncReadyCallback callback,
						 gpointer user_data);
	GList *		(*query_auth_types_finish)
						(CamelService *service,
						 GAsyncResult *result,
						 GError **error);
};

/* query_auth_types returns a GList of these */
typedef struct {
	const gchar *name;               /* user-friendly name */
	const gchar *description;
	const gchar *authproto;

	gboolean need_password;   /* needs a password to authenticate */
} CamelServiceAuthType;

GType		camel_service_get_type		(void);
GQuark		camel_service_error_quark	(void) G_GNUC_CONST;
void		camel_service_migrate_files	(CamelService *service);
CamelURL *	camel_service_new_camel_url	(CamelService *service);
const gchar *	camel_service_get_display_name	(CamelService *service);
void		camel_service_set_display_name	(CamelService *service,
						 const gchar *display_name);
const gchar *	camel_service_get_password	(CamelService *service);
void		camel_service_set_password	(CamelService *service,
						 const gchar *password);
const gchar *	camel_service_get_user_data_dir	(CamelService *service);
const gchar *	camel_service_get_user_cache_dir
						(CamelService *service);
gchar *		camel_service_get_name		(CamelService *service,
						 gboolean brief);
CamelProvider *	camel_service_get_provider	(CamelService *service);
struct _CamelSession *
		camel_service_get_session	(CamelService *service);
CamelSettings *	camel_service_get_settings	(CamelService *service);
void		camel_service_set_settings	(CamelService *service,
						 CamelSettings *settings);
const gchar *	camel_service_get_uid		(CamelService *service);
void		camel_service_cancel_connect	(CamelService *service);
gboolean	camel_service_connect_sync	(CamelService *service,
						 GError **error);
gboolean	camel_service_disconnect_sync	(CamelService *service,
						 gboolean clean,
						 GError **error);
CamelServiceConnectionStatus
		camel_service_get_connection_status
						(CamelService *service);
void		camel_service_lock		(CamelService *service,
						 CamelServiceLock lock);
void		camel_service_unlock		(CamelService *service,
						 CamelServiceLock lock);

CamelAuthenticationResult
		camel_service_authenticate_sync	(CamelService *service,
						 const gchar *mechanism,
						 GCancellable *cancellable,
						 GError **error);
void		camel_service_authenticate	(CamelService *service,
						 const gchar *mechanism,
						 gint io_priority,
						 GCancellable *cancellable,
						 GAsyncReadyCallback callback,
						 gpointer user_data);
CamelAuthenticationResult
		camel_service_authenticate_finish
						(CamelService *service,
						 GAsyncResult *result,
						 GError **error);
GList *		camel_service_query_auth_types_sync
						(CamelService *service,
						 GCancellable *cancellable,
						 GError **error);
void		camel_service_query_auth_types	(CamelService *service,
						 gint io_priority,
						 GCancellable *cancellable,
						 GAsyncReadyCallback callback,
						 gpointer user_data);
GList *		camel_service_query_auth_types_finish
						(CamelService *service,
						 GAsyncResult *result,
						 GError **error);

G_END_DECLS

#endif /* CAMEL_SERVICE_H */
