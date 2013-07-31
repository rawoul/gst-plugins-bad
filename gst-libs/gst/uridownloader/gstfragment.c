/* GStreamer
 * Copyright (C) 2011 Andoni Morales Alastruey <ylatuya@gmail.com>
 *
 * gstfragment.c:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <glib.h>
#include <gst/base/gsttypefindhelper.h>
#include <gst/base/gstadapter.h>
#include "gstfragment.h"
#include "gsturidownloader_debug.h"

GST_DEBUG_CATEGORY_EXTERN (GST_CAT_PERFORMANCE);

#define GST_CAT_DEFAULT uridownloader_debug

#define GST_FRAGMENT_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_FRAGMENT, GstFragmentPrivate))

enum
{
  PROP_0,
  PROP_INDEX,
  PROP_NAME,
  PROP_DURATION,
  PROP_DISCONTINOUS,
  PROP_BUFFER,
  PROP_BUFFER_LIST,
  PROP_CAPS,
  PROP_SIZE,
  PROP_LAST
};

struct _GstFragmentPrivate
{
  GstBufferList *buffer_list;
  GstBuffer *last_buffer;
  GstCaps *caps;
  gsize size;
};

G_DEFINE_TYPE (GstFragment, gst_fragment, G_TYPE_OBJECT);

static void gst_fragment_dispose (GObject * object);
static void gst_fragment_finalize (GObject * object);

static void
gst_fragment_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec)
{
  GstFragment *fragment = GST_FRAGMENT (object);

  switch (property_id) {
    case PROP_CAPS:
      gst_fragment_set_caps (fragment, g_value_get_boxed (value));
      break;

    default:
      /* We don't have any other property... */
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gst_fragment_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec)
{
  GstFragment *fragment = GST_FRAGMENT (object);

  switch (property_id) {
    case PROP_INDEX:
      g_value_set_uint (value, fragment->index);
      break;

    case PROP_NAME:
      g_value_set_string (value, fragment->name);
      break;

    case PROP_DURATION:
      g_value_set_uint64 (value, fragment->stop_time - fragment->start_time);
      break;

    case PROP_DISCONTINOUS:
      g_value_set_boolean (value, fragment->discontinuous);
      break;

    case PROP_BUFFER:
      g_value_set_boxed (value, gst_fragment_get_buffer (fragment));
      break;

    case PROP_BUFFER_LIST:
      g_value_set_boxed (value, gst_fragment_get_buffer_list (fragment));
      break;

    case PROP_CAPS:
      g_value_set_boxed (value, gst_fragment_get_caps (fragment));
      break;

    case PROP_SIZE:
      g_value_set_uint64 (value, fragment->priv->size);
      break;

    default:
      /* We don't have any other property... */
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}



static void
gst_fragment_class_init (GstFragmentClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GstFragmentPrivate));

  gobject_class->set_property = gst_fragment_set_property;
  gobject_class->get_property = gst_fragment_get_property;
  gobject_class->dispose = gst_fragment_dispose;
  gobject_class->finalize = gst_fragment_finalize;

  g_object_class_install_property (gobject_class, PROP_INDEX,
      g_param_spec_uint ("index", "Index", "Index of the fragment", 0,
          G_MAXUINT, 0, G_PARAM_READABLE));

  g_object_class_install_property (gobject_class, PROP_NAME,
      g_param_spec_string ("name", "Name",
          "Name of the fragment (eg:fragment-12.ts)", NULL, G_PARAM_READABLE));

  g_object_class_install_property (gobject_class, PROP_DISCONTINOUS,
      g_param_spec_boolean ("discontinuous", "Discontinous",
          "Whether this fragment has a discontinuity or not",
          FALSE, G_PARAM_READABLE));

  g_object_class_install_property (gobject_class, PROP_DURATION,
      g_param_spec_uint64 ("duration", "Fragment duration",
          "Duration of the fragment", 0, G_MAXUINT64, 0, G_PARAM_READABLE));

  g_object_class_install_property (gobject_class, PROP_BUFFER,
      g_param_spec_boxed ("buffer", "Buffer",
          "The fragment's buffer", GST_TYPE_BUFFER,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_BUFFER_LIST,
      g_param_spec_boxed ("buffer-list", "Buffer List",
          "The fragment's buffer list", GST_TYPE_BUFFER_LIST,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_CAPS,
      g_param_spec_boxed ("caps", "Fragment caps",
          "The caps of the fragment's buffer. (NULL = detect)", GST_TYPE_CAPS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_BUFFER_LIST,
      g_param_spec_uint64 ("size", "Total fragment size",
          "The fragment's size in bytes", 0, G_MAXUINT64, 0, G_PARAM_READABLE));
}

static void
gst_fragment_init (GstFragment * fragment)
{
  GstFragmentPrivate *priv;

  fragment->priv = priv = GST_FRAGMENT_GET_PRIVATE (fragment);

  priv->buffer_list = gst_buffer_list_new ();
  priv->last_buffer = NULL;
  priv->size = 0;
  fragment->download_start_time = gst_util_get_timestamp ();
  fragment->start_time = 0;
  fragment->stop_time = 0;
  fragment->index = 0;
  fragment->name = g_strdup ("");
  fragment->completed = FALSE;
  fragment->discontinuous = FALSE;
}

GstFragment *
gst_fragment_new (void)
{
  return GST_FRAGMENT (g_object_new (GST_TYPE_FRAGMENT, NULL));
}

static void
gst_fragment_finalize (GObject * gobject)
{
  GstFragment *fragment = GST_FRAGMENT (gobject);

  g_free (fragment->name);

  G_OBJECT_CLASS (gst_fragment_parent_class)->finalize (gobject);
}

void
gst_fragment_dispose (GObject * object)
{
  GstFragmentPrivate *priv = GST_FRAGMENT (object)->priv;

  if (priv->last_buffer != NULL) {
    gst_buffer_unref (priv->last_buffer);
    priv->last_buffer = NULL;
  }

  if (priv->buffer_list != NULL) {
    gst_buffer_list_unref (priv->buffer_list);
    priv->buffer_list = NULL;
  }

  if (priv->caps != NULL) {
    gst_caps_unref (priv->caps);
    priv->caps = NULL;
  }

  G_OBJECT_CLASS (gst_fragment_parent_class)->dispose (object);
}

static gboolean
gst_fragment_is_completed (GstFragment * fragment)
{
  if (!fragment->completed)
    return FALSE;

  if (fragment->priv->last_buffer) {
    gst_buffer_list_add (fragment->priv->buffer_list,
        fragment->priv->last_buffer);
    fragment->priv->last_buffer = NULL;
  }

  return TRUE;
}

GstBufferList *
gst_fragment_get_buffer_list (GstFragment * fragment)
{
  g_return_val_if_fail (fragment != NULL, NULL);

  if (!gst_fragment_is_completed (fragment))
    return NULL;

  return gst_buffer_list_ref (fragment->priv->buffer_list);
}

gsize
gst_fragment_get_size (GstFragment * fragment)
{
  g_return_val_if_fail (fragment != NULL, 0);

  return fragment->priv->size;
}

void
gst_fragment_set_caps (GstFragment * fragment, GstCaps * caps)
{
  g_return_if_fail (fragment != NULL);

  gst_caps_replace (&fragment->priv->caps, caps);
}

GstCaps *
gst_fragment_get_caps (GstFragment * fragment)
{
  g_return_val_if_fail (fragment != NULL, NULL);

  if (!gst_fragment_is_completed (fragment))
    return NULL;

  if (fragment->priv->caps == NULL) {
    GstBuffer *buffer;
    GstCaps *caps;

    buffer = gst_buffer_list_get (fragment->priv->buffer_list, 0);
    if (buffer) {
      caps = gst_type_find_helper_for_buffer (NULL, buffer, NULL);
      if (caps)
        gst_caps_replace (&fragment->priv->caps, caps);
    }
  }

  return fragment->priv->caps ? gst_caps_ref (fragment->priv->caps) : NULL;
}

gboolean
gst_fragment_add_buffer (GstFragment * fragment, GstBuffer * buffer)
{
  GstBuffer *last_buffer;
  gsize size;

  g_return_val_if_fail (fragment != NULL, FALSE);
  g_return_val_if_fail (buffer != NULL, FALSE);

  if (gst_fragment_is_completed (fragment)) {
    GST_WARNING ("Fragment is completed, could not add more buffers");
    return FALSE;
  }

  size = gst_buffer_get_size (buffer);
  GST_LOG ("Adding %" G_GSIZE_FORMAT " bytes buffer to the fragment", size);

  last_buffer = fragment->priv->last_buffer;
  if (last_buffer) {
    guint n_blocks =
        gst_buffer_n_memory (last_buffer) + gst_buffer_n_memory (buffer);

    if (n_blocks <= gst_buffer_get_max_memory ()) {
      last_buffer = gst_buffer_append (last_buffer, buffer);
      GST_BUFFER_OFFSET_END (last_buffer) += size;
    } else {
      gst_buffer_list_add (fragment->priv->buffer_list, last_buffer);
      last_buffer = NULL;
    }
  }

  if (!last_buffer) {
    last_buffer = gst_buffer_make_writable (buffer);
    GST_BUFFER_OFFSET (last_buffer) = fragment->priv->size;
    GST_BUFFER_OFFSET_END (last_buffer) = fragment->priv->size + size;
  }

  fragment->priv->last_buffer = last_buffer;
  fragment->priv->size += size;

  return TRUE;
}

gsize
gst_fragment_extract (GstFragment * fragment, gsize offset, gpointer dest,
    gsize size)
{
  GstBuffer *buffer;
  guint i, length;
  gsize rd_offset;

  g_return_val_if_fail (fragment != NULL, 0);
  g_return_val_if_fail (dest != NULL, 0);

  if (!gst_fragment_is_completed (fragment))
    return 0;

  length = gst_buffer_list_length (fragment->priv->buffer_list);
  rd_offset = 0;

  for (i = 0; rd_offset < size && i < length; i++) {
    gsize buf_size;
    gsize buf_offset;

    buffer = gst_buffer_list_get (fragment->priv->buffer_list, i);
    buf_size = gst_buffer_get_size (buffer);

    if (offset > 0) {
      buf_offset = MIN (buf_size, offset);
      buf_size -= buf_offset;
      offset -= buf_offset;
    } else {
      buf_offset = 0;
    }

    if (buf_size > size - rd_offset)
      buf_size = size - rd_offset;

    if (buf_size > 0)
      rd_offset += gst_buffer_extract (buffer, buf_offset,
          (guint8 *) dest + rd_offset, buf_size);
  }

  return rd_offset;
}

GstBuffer *
gst_fragment_get_buffer (GstFragment * fragment)
{
  GstBufferList *buffer_list;
  GstBuffer *buffer;
  guint length;

  g_return_val_if_fail (fragment != NULL, NULL);

  if (!gst_fragment_is_completed (fragment))
    return NULL;

  buffer_list = fragment->priv->buffer_list;
  length = gst_buffer_list_length (buffer_list);

  if (length == 1) {
    buffer = gst_buffer_list_get (buffer_list, 0);
  } else {
    guint8 *data;
    gsize size;

    data = g_malloc (fragment->priv->size);
    if (!data)
      return NULL;

    size = gst_fragment_extract (fragment, 0, data, fragment->priv->size);

    GST_CAT_DEBUG (GST_CAT_PERFORMANCE,
        "copied %" G_GSIZE_FORMAT " bytes of data from fragment", size);

    buffer = gst_buffer_new_wrapped (data, size);
    if (!buffer) {
      g_free (data);
      return NULL;
    }

    GST_BUFFER_OFFSET (buffer) = 0;
    GST_BUFFER_OFFSET_END (buffer) = size;

    gst_buffer_list_remove (buffer_list, 0, length);
    gst_buffer_list_add (buffer_list, buffer);

    fragment->priv->size = size;
  }

  return gst_buffer_ref (buffer);
}
