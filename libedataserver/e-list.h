/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Authors:
 *   Chris Lahey <clahey@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 */

#ifndef E_LIST_H
#define E_LIST_H

#include <stdio.h>
#include <time.h>
#include <libedataserver/e-iterator.h>

/* Standard GObject macros */
#define E_TYPE_LIST \
	(e_list_get_type ())
#define E_LIST(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_LIST, EList))
#define E_LIST_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_LIST, EListClass))
#define E_IS_LIST(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_LIST))
#define E_IS_LIST_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_LIST))
#define E_LIST_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_LIST, EListClass))

G_BEGIN_DECLS

typedef struct _EList EList;
typedef struct _EListClass EListClass;

typedef gpointer	(*EListCopyFunc)	(gconstpointer data,
						 gpointer closure);
typedef void		(*EListFreeFunc)	(gpointer data,
						 gpointer closure);

struct _EList {
	GObject parent;

	GList *list;
	GList *iterators;
	EListCopyFunc copy;
	EListFreeFunc free;
	gpointer closure;
};

struct _EListClass {
	GObjectClass parent_class;
};

GType		e_list_get_type			(void) G_GNUC_CONST;
EList *		e_list_new			(EListCopyFunc copy,
						 EListFreeFunc free,
						 gpointer closure);
void		e_list_construct		(EList *list,
						 EListCopyFunc copy,
						 EListFreeFunc free,
						 gpointer closure);
EList *		e_list_duplicate		(EList *list);
EIterator *	e_list_get_iterator		(EList *list);
void		e_list_append			(EList *list,
						 gconstpointer data);
void		e_list_remove			(EList *list,
						 gconstpointer data);
gint		e_list_length			(EList *list);

/* For iterators to call. */
void		e_list_remove_link		(EList *list,
						 GList *link);
void		e_list_remove_iterator		(EList *list,
						 EIterator *iterator);
void		e_list_invalidate_iterators	(EList *list,
						 EIterator *skip);

G_END_DECLS

#endif /* E_LIST_H */
