/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 * Copyright (C) 2006 OpenedHand Ltd
 * Copyright (C) 2009 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of version 2.1 of the GNU Lesser General Public License as
 * published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * Author: Nat Friedman <nat@ximian.com>
 * Author: Ross Burton <ross@linux.intel.com>
 */

#ifndef __E_DATA_BOOK_VIEW_H__
#define __E_DATA_BOOK_VIEW_H__

#include <gio/gio.h>
#include <libebook/e-contact.h>
#include <libebook/e-book-client-view.h>
#include <libedata-book/e-data-book-types.h>
#include <libedata-book/e-book-backend.h>
#include <libedata-book/e-book-backend-sexp.h>

G_BEGIN_DECLS

#define E_TYPE_DATA_BOOK_VIEW        (e_data_book_view_get_type ())
#define E_DATA_BOOK_VIEW(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), E_TYPE_DATA_BOOK_VIEW, EDataBookView))
#define E_DATA_BOOK_VIEW_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), E_TYPE_DATA_BOOK_VIEW, EDataBookViewClass))
#define E_IS_DATA_BOOK_VIEW(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), E_TYPE_DATA_BOOK_VIEW))
#define E_IS_DATA_BOOK_VIEW_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), E_TYPE_DATA_BOOK_VIEW))
#define E_DATA_BOOK_VIEW_GET_CLASS(k) (G_TYPE_INSTANCE_GET_CLASS ((obj), E_TYPE_DATA_BOOK_VIEW, EDataBookView))

typedef struct _EDataBookViewPrivate EDataBookViewPrivate;

struct _EDataBookView {
	GObject parent;
	EDataBookViewPrivate *priv;
};

struct _EDataBookViewClass {
	GObjectClass parent;
};

GType			e_data_book_view_get_type		(void);
EDataBookView *		e_data_book_view_new			(EDataBook *book, const gchar *card_query, EBookBackendSExp *card_sexp);
guint			e_data_book_view_register_gdbus_object	(EDataBookView *query, GDBusConnection *connection, const gchar *object_path, GError **error);

const gchar *		e_data_book_view_get_card_query		(EDataBookView *book_view);
EBookBackendSExp *	e_data_book_view_get_card_sexp		(EDataBookView *book_view);
EBookBackend *		e_data_book_view_get_backend		(EDataBookView *book_view);
EBookClientViewFlags    e_data_book_view_get_flags              (EDataBookView *book_view);
void			e_data_book_view_notify_update		(EDataBookView *book_view, const EContact *contact);

void			e_data_book_view_notify_update_vcard	(EDataBookView *book_view, const gchar *id, const gchar *vcard);
void			e_data_book_view_notify_update_prefiltered_vcard (EDataBookView *book_view, const gchar *id, const gchar *vcard);

void			e_data_book_view_notify_remove		(EDataBookView *book_view, const gchar *id);
void			e_data_book_view_notify_complete	(EDataBookView *book_view, const GError *error);
void			e_data_book_view_notify_progress        (EDataBookView *book_view, guint percent, const gchar *message);
void			e_data_book_view_ref			(EDataBookView *book_view);
void			e_data_book_view_unref			(EDataBookView *book_view);

/* const */ GHashTable *e_data_book_view_get_fields_of_interest	(EDataBookView *view);

G_END_DECLS

#endif /* __E_DATA_BOOK_VIEW_H__ */
