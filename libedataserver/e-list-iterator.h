/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Authors:
 *   Chris Lahey <clahey@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 */

#ifndef E_LIST_ITERATOR_H
#define E_LIST_ITERATOR_H

#include <stdio.h>
#include <time.h>

#include <libedataserver/e-list.h>
#include <libedataserver/e-iterator.h>

/* Standard GObject macros */
#define E_TYPE_LIST_ITERATOR \
	(e_list_iterator_get_type ())
#define E_LIST_ITERATOR(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_LIST_ITERATOR, EListIterator))
#define E_LIST_ITERATOR_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_LIST_ITERATOR, EListIteratorClass))
#define E_IS_LIST_ITERATOR(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_LIST_ITERATOR))
#define E_IS_LIST_ITERATOR_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_LIST_ITERATOR))
#define E_LIST_ITERATOR_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_LIST_ITERATOR, EListIteratorClass))

G_BEGIN_DECLS

typedef struct _EListIterator EListIterator;
typedef struct _EListIteratorClass EListIteratorClass;

struct _EListIterator {
	EIterator parent;

	EList *list;
	GList *iterator;
};

struct _EListIteratorClass {
	EIteratorClass parent_class;
};

GType		e_list_iterator_get_type	(void) G_GNUC_CONST;
EIterator *	e_list_iterator_new		(EList *list);

G_END_DECLS

#endif /* E_LIST_ITERATOR_H */
