/*
 * Copyright (c) 2008 Benjamin Schmitz <vortex@wolpzone.de>
 * Copyright (c) 2009 Sebastian Dröge <sebastian.droege@collabora.co.uk>
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

/**
 * SECTION:element-assrender
 *
 * Renders timestamped SSA/ASS subtitles on top of a video stream.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v filesrc location=/path/to/mkv ! matroskademux name=d ! queue ! mp3parse ! mad ! audioconvert ! autoaudiosink  d. ! queue ! ffdec_h264 ! videoconvert ! r.   d. ! queue ! "application/x-ass" ! assrender name=r ! videoconvert ! autovideosink
 * ]| This pipeline demuxes a Matroska file with h.264 video, MP3 audio and embedded ASS subtitles and renders the subtitles on top of the video.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/video/gstvideometa.h>

#include "gstassrender.h"

#include <string.h>

GST_DEBUG_CATEGORY_STATIC (gst_ass_render_debug);
GST_DEBUG_CATEGORY_STATIC (gst_ass_render_lib_debug);
#define GST_CAT_DEFAULT gst_ass_render_debug

/* Filter signals and props */
enum
{
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_ENABLE,
  PROP_EMBEDDEDFONTS,
  PROP_WAIT_TEXT
};

/* FIXME: video-blend.c doesn't support formats with more than 8 bit per
 * component (which get unpacked into ARGB64 or AYUV64) yet, such as:
 *  v210, v216, UYVP, GRAY16_LE, GRAY16_BE */
#define FORMATS "{ BGRx, RGBx, xRGB, xBGR, RGBA, BGRA, ARGB, ABGR, RGB, BGR, \
    I420, YV12, AYUV, YUY2, UYVY, v308, Y41B, Y42B, Y444, \
    NV12, NV21, A420, YUV9, YVU9, IYU1, GRAY8 }"

#define ASSRENDER_CAPS GST_VIDEO_CAPS_MAKE(FORMATS)

#define ASSRENDER_ALL_CAPS ASSRENDER_CAPS ";" \
    GST_VIDEO_CAPS_MAKE_WITH_FEATURES ("ANY", GST_VIDEO_FORMATS_ALL)

static GstStaticCaps sw_template_caps = GST_STATIC_CAPS (ASSRENDER_CAPS);

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (ASSRENDER_ALL_CAPS)
    );

static GstStaticPadTemplate video_sink_factory =
GST_STATIC_PAD_TEMPLATE ("video_sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (ASSRENDER_ALL_CAPS)
    );

static GstStaticPadTemplate text_sink_factory =
    GST_STATIC_PAD_TEMPLATE ("text_sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-ass; application/x-ssa")
    );

#define GST_ASS_RENDER_GET_LOCK(ass) (&GST_ASS_RENDER (ass)->lock)
#define GST_ASS_RENDER_GET_COND(ass) (&GST_ASS_RENDER (ass)->cond)
#define GST_ASS_RENDER_LOCK(ass)     (g_mutex_lock (GST_ASS_RENDER_GET_LOCK (ass)))
#define GST_ASS_RENDER_UNLOCK(ass)   (g_mutex_unlock (GST_ASS_RENDER_GET_LOCK (ass)))
#define GST_ASS_RENDER_WAIT(ass)     (g_cond_wait (GST_ASS_RENDER_GET_COND (ass), GST_ASS_RENDER_GET_LOCK (ass)))
#define GST_ASS_RENDER_SIGNAL(ass)   (g_cond_signal (GST_ASS_RENDER_GET_COND (ass)))
#define GST_ASS_RENDER_BROADCAST(ass)(g_cond_broadcast (GST_ASS_RENDER_GET_COND (ass)))

static void gst_ass_render_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_ass_render_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static void gst_ass_render_finalize (GObject * object);

static GstStateChangeReturn gst_ass_render_change_state (GstElement * element,
    GstStateChange transition);

#define gst_ass_render_parent_class parent_class
G_DEFINE_TYPE (GstAssRender, gst_ass_render, GST_TYPE_ELEMENT);

static GstCaps *gst_ass_render_get_videosink_caps (GstPad * pad,
    GstAssRender * render, GstCaps * filter);
static GstCaps *gst_ass_render_get_src_caps (GstPad * pad,
    GstAssRender * render, GstCaps * filter);

static gboolean gst_ass_render_setcaps_video (GstPad * pad,
    GstAssRender * render, GstCaps * caps);
static gboolean gst_ass_render_setcaps_text (GstPad * pad,
    GstAssRender * render, GstCaps * caps);

static GstFlowReturn gst_ass_render_chain_video (GstPad * pad,
    GstObject * parent, GstBuffer * buf);
static GstFlowReturn gst_ass_render_chain_text (GstPad * pad,
    GstObject * parent, GstBuffer * buf);

static gboolean gst_ass_render_event_video (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean gst_ass_render_event_text (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean gst_ass_render_event_src (GstPad * pad, GstObject * parent,
    GstEvent * event);

static gboolean gst_ass_render_query_video (GstPad * pad, GstObject * parent,
    GstQuery * query);
static gboolean gst_ass_render_query_src (GstPad * pad, GstObject * parent,
    GstQuery * query);

/* initialize the plugin's class */
static void
gst_ass_render_class_init (GstAssRenderClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_ass_render_set_property;
  gobject_class->get_property = gst_ass_render_get_property;
  gobject_class->finalize = gst_ass_render_finalize;

  g_object_class_install_property (gobject_class, PROP_ENABLE,
      g_param_spec_boolean ("enable", "Enable",
          "Enable rendering of subtitles", TRUE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_EMBEDDEDFONTS,
      g_param_spec_boolean ("embeddedfonts", "Embedded Fonts",
          "Extract and use fonts embedded in the stream", TRUE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_WAIT_TEXT,
      g_param_spec_boolean ("wait-text", "Wait Text",
          "Whether to wait for subtitles", TRUE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_ass_render_change_state);

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&video_sink_factory));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&text_sink_factory));

  gst_element_class_set_static_metadata (gstelement_class, "ASS/SSA Render",
      "Mixer/Video/Overlay/Subtitle",
      "Renders ASS/SSA subtitles with libass",
      "Benjamin Schmitz <vortex@wolpzone.de>, "
      "Sebastian Dröge <sebastian.droege@collabora.co.uk>");
}

#if defined(LIBASS_VERSION) && LIBASS_VERSION >= 0x00907000
static void
_libass_message_cb (gint level, const gchar * fmt, va_list args,
    gpointer render)
{
  gchar *message = g_strdup_vprintf (fmt, args);

  if (level < 2)
    GST_CAT_ERROR_OBJECT (gst_ass_render_lib_debug, render, "%s", message);
  else if (level < 4)
    GST_CAT_WARNING_OBJECT (gst_ass_render_lib_debug, render, "%s", message);
  else if (level < 5)
    GST_CAT_INFO_OBJECT (gst_ass_render_lib_debug, render, "%s", message);
  else if (level < 6)
    GST_CAT_DEBUG_OBJECT (gst_ass_render_lib_debug, render, "%s", message);
  else
    GST_CAT_LOG_OBJECT (gst_ass_render_lib_debug, render, "%s", message);

  g_free (message);
}
#endif

static void
gst_ass_render_init (GstAssRender * render)
{
  GST_DEBUG_OBJECT (render, "init");

  render->srcpad = gst_pad_new_from_static_template (&src_factory, "src");
  render->video_sinkpad =
      gst_pad_new_from_static_template (&video_sink_factory, "video_sink");
  render->text_sinkpad =
      gst_pad_new_from_static_template (&text_sink_factory, "text_sink");

  gst_pad_set_chain_function (render->video_sinkpad,
      GST_DEBUG_FUNCPTR (gst_ass_render_chain_video));
  gst_pad_set_chain_function (render->text_sinkpad,
      GST_DEBUG_FUNCPTR (gst_ass_render_chain_text));

  gst_pad_set_event_function (render->video_sinkpad,
      GST_DEBUG_FUNCPTR (gst_ass_render_event_video));
  gst_pad_set_event_function (render->text_sinkpad,
      GST_DEBUG_FUNCPTR (gst_ass_render_event_text));
  gst_pad_set_event_function (render->srcpad,
      GST_DEBUG_FUNCPTR (gst_ass_render_event_src));

  gst_pad_set_query_function (render->srcpad,
      GST_DEBUG_FUNCPTR (gst_ass_render_query_src));
  gst_pad_set_query_function (render->video_sinkpad,
      GST_DEBUG_FUNCPTR (gst_ass_render_query_video));

  GST_PAD_SET_PROXY_ALLOCATION (render->video_sinkpad);

  gst_element_add_pad (GST_ELEMENT (render), render->srcpad);
  gst_element_add_pad (GST_ELEMENT (render), render->video_sinkpad);
  gst_element_add_pad (GST_ELEMENT (render), render->text_sinkpad);

  gst_video_info_init (&render->info);

  g_mutex_init (&render->lock);
  g_cond_init (&render->cond);

  render->renderer_init_ok = FALSE;
  render->track_init_ok = FALSE;
  render->enable = TRUE;
  render->embeddedfonts = TRUE;
  render->wait_text = FALSE;

  gst_segment_init (&render->video_segment, GST_FORMAT_TIME);
  gst_segment_init (&render->subtitle_segment, GST_FORMAT_TIME);

  g_mutex_init (&render->ass_mutex);
  render->ass_library = ass_library_init ();
#if defined(LIBASS_VERSION) && LIBASS_VERSION >= 0x00907000
  ass_set_message_cb (render->ass_library, _libass_message_cb, render);
#endif
  ass_set_extract_fonts (render->ass_library, 1);

  render->ass_renderer = ass_renderer_init (render->ass_library);
  if (!render->ass_renderer) {
    GST_WARNING_OBJECT (render, "cannot create renderer instance");
    g_assert_not_reached ();
  }

  render->ass_track = NULL;

  GST_DEBUG_OBJECT (render, "init complete");
}

static void
gst_ass_render_finalize (GObject * object)
{
  GstAssRender *render = GST_ASS_RENDER (object);

  g_mutex_clear (&render->lock);
  g_cond_clear (&render->cond);

  if (render->ass_track) {
    ass_free_track (render->ass_track);
  }

  if (render->ass_renderer) {
    ass_renderer_done (render->ass_renderer);
  }

  if (render->ass_library) {
    ass_library_done (render->ass_library);
  }

  g_mutex_clear (&render->ass_mutex);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_ass_render_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstAssRender *render = GST_ASS_RENDER (object);

  GST_ASS_RENDER_LOCK (render);
  switch (prop_id) {
    case PROP_ENABLE:
      render->enable = g_value_get_boolean (value);
      break;
    case PROP_EMBEDDEDFONTS:
      render->embeddedfonts = g_value_get_boolean (value);
      g_mutex_lock (&render->ass_mutex);
      ass_set_extract_fonts (render->ass_library, render->embeddedfonts);
      g_mutex_unlock (&render->ass_mutex);
      break;
    case PROP_WAIT_TEXT:
      render->wait_text = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_ASS_RENDER_UNLOCK (render);
}

static void
gst_ass_render_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstAssRender *render = GST_ASS_RENDER (object);

  GST_ASS_RENDER_LOCK (render);
  switch (prop_id) {
    case PROP_ENABLE:
      g_value_set_boolean (value, render->enable);
      break;
    case PROP_EMBEDDEDFONTS:
      g_value_set_boolean (value, render->embeddedfonts);
      break;
    case PROP_WAIT_TEXT:
      g_value_set_boolean (value, render->wait_text);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_ASS_RENDER_UNLOCK (render);
}

/* Called with lock held */
static void
gst_ass_render_pop_text (GstAssRender * render)
{
  if (render->subtitle_pending) {
    GST_DEBUG_OBJECT (render, "releasing text buffer %p",
        render->subtitle_pending);
    gst_buffer_unref (render->subtitle_pending);
    render->subtitle_pending = NULL;
  }

  /* Let the text task know we used that buffer */
  GST_ASS_RENDER_BROADCAST (render);
}

static GstStateChangeReturn
gst_ass_render_change_state (GstElement * element, GstStateChange transition)
{
  GstAssRender *render = GST_ASS_RENDER (element);
  GstStateChangeReturn ret;

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      GST_ASS_RENDER_LOCK (render);
      render->subtitle_flushing = TRUE;
      render->video_flushing = TRUE;
      gst_ass_render_pop_text (render);
      GST_ASS_RENDER_UNLOCK (render);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      g_mutex_lock (&render->ass_mutex);
      if (render->ass_track)
        ass_free_track (render->ass_track);
      render->ass_track = NULL;
      if (render->composition) {
        gst_video_overlay_composition_unref (render->composition);
        render->composition = NULL;
      }
      render->track_init_ok = FALSE;
      render->renderer_init_ok = FALSE;
      g_mutex_unlock (&render->ass_mutex);
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      GST_ASS_RENDER_LOCK (render);
      render->subtitle_flushing = FALSE;
      render->video_flushing = FALSE;
      render->video_eos = FALSE;
      render->subtitle_eos = FALSE;
      gst_segment_init (&render->video_segment, GST_FORMAT_TIME);
      gst_segment_init (&render->subtitle_segment, GST_FORMAT_TIME);
      GST_ASS_RENDER_UNLOCK (render);
      break;
    default:
      break;
  }


  return ret;
}

static gboolean
gst_ass_render_query_src (GstPad * pad, GstObject * parent, GstQuery * query)
{
  gboolean res = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      GstCaps *filter, *caps;

      gst_query_parse_caps (query, &filter);
      caps = gst_ass_render_get_src_caps (pad, (GstAssRender *) parent, filter);
      gst_query_set_caps_result (query, caps);
      gst_caps_unref (caps);
      res = TRUE;
      break;
    }
    default:
      res = gst_pad_query_default (pad, parent, query);
      break;
  }

  return res;
}

static gboolean
gst_ass_render_event_src (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstAssRender *render = GST_ASS_RENDER (parent);
  gboolean ret;

  GST_DEBUG_OBJECT (render, "received src event %" GST_PTR_FORMAT, event);

  if (render->track_init_ok) {
    gst_event_ref (event);
    ret = gst_pad_push_event (render->video_sinkpad, event);
    gst_pad_push_event (render->text_sinkpad, event);
  } else {
    ret = gst_pad_push_event (render->video_sinkpad, event);
  }

  return ret;
}

/**
 * gst_ass_render_add_feature_and_intersect:
 *
 * Creates a new #GstCaps containing the (given caps +
 * given caps feature) + (given caps intersected by the
 * given filter).
 *
 * Returns: the new #GstCaps
 */
static GstCaps *
gst_ass_render_add_feature_and_intersect (GstCaps * caps,
    const gchar * feature, GstCaps * filter)
{
  int i, caps_size;
  GstCaps *new_caps;

  new_caps = gst_caps_copy (caps);

  caps_size = gst_caps_get_size (new_caps);
  for (i = 0; i < caps_size; i++) {
    GstCapsFeatures *features = gst_caps_get_features (new_caps, i);
    if (!gst_caps_features_is_any (features)) {
      gst_caps_features_add (features, feature);
    }
  }

  gst_caps_append (new_caps, gst_caps_intersect_full (caps,
          filter, GST_CAPS_INTERSECT_FIRST));

  return new_caps;
}

/**
 * gst_ass_render_intersect_by_feature:
 *
 * Creates a new #GstCaps based on the following filtering rule.
 *
 * For each individual caps contained in given caps, if the
 * caps uses the given caps feature, keep a version of the caps
 * with the feature and an another one without. Otherwise, intersect
 * the caps with the given filter.
 *
 * Returns: the new #GstCaps
 */
static GstCaps *
gst_ass_render_intersect_by_feature (GstCaps * caps,
    const gchar * feature, GstCaps * filter)
{
  int i, caps_size;
  GstCaps *new_caps;

  new_caps = gst_caps_new_empty ();

  caps_size = gst_caps_get_size (caps);
  for (i = 0; i < caps_size; i++) {
    GstStructure *caps_structure = gst_caps_get_structure (caps, i);
    GstCapsFeatures *caps_features =
        gst_caps_features_copy (gst_caps_get_features (caps, i));
    GstCaps *filtered_caps;
    GstCaps *simple_caps =
        gst_caps_new_full (gst_structure_copy (caps_structure), NULL);
    gst_caps_set_features (simple_caps, 0, caps_features);

    if (gst_caps_features_contains (caps_features, feature)) {
      gst_caps_append (new_caps, gst_caps_copy (simple_caps));

      gst_caps_features_remove (caps_features, feature);
      filtered_caps = gst_caps_ref (simple_caps);
    } else {
      filtered_caps = gst_caps_intersect_full (simple_caps, filter,
          GST_CAPS_INTERSECT_FIRST);
    }

    gst_caps_unref (simple_caps);
    gst_caps_append (new_caps, filtered_caps);
  }

  return new_caps;
}

static GstCaps *
gst_ass_render_get_videosink_caps (GstPad * pad, GstAssRender * render,
    GstCaps * filter)
{
  GstPad *srcpad = render->srcpad;
  GstCaps *peer_caps = NULL, *caps = NULL, *assrender_filter = NULL;

  if (filter) {
    /* filter caps + composition feature + filter caps
     * filtered by the software caps. */
    GstCaps *sw_caps = gst_static_caps_get (&sw_template_caps);
    assrender_filter = gst_ass_render_add_feature_and_intersect (filter,
        GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION, sw_caps);
    gst_caps_unref (sw_caps);

    GST_DEBUG_OBJECT (render, "assrender filter %" GST_PTR_FORMAT,
        assrender_filter);
  }

  peer_caps = gst_pad_peer_query_caps (srcpad, assrender_filter);

  if (assrender_filter)
    gst_caps_unref (assrender_filter);

  if (peer_caps) {

    GST_DEBUG_OBJECT (pad, "peer caps  %" GST_PTR_FORMAT, peer_caps);

    if (gst_caps_is_any (peer_caps)) {

      /* if peer returns ANY caps, return filtered src pad template caps */
      caps = gst_caps_copy (gst_pad_get_pad_template_caps (srcpad));
    } else {

      /* duplicate caps which contains the composition into one version with
       * the meta and one without. Filter the other caps by the software caps */
      GstCaps *sw_caps = gst_static_caps_get (&sw_template_caps);
      caps = gst_ass_render_intersect_by_feature (peer_caps,
          GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION, sw_caps);
      gst_caps_unref (sw_caps);
    }

    gst_caps_unref (peer_caps);

  } else {
    /* no peer, our padtemplate is enough then */
    caps = gst_pad_get_pad_template_caps (pad);
  }

  if (filter) {
    GstCaps *intersection = gst_caps_intersect_full (filter, caps,
        GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
    caps = intersection;
  }

  GST_DEBUG_OBJECT (render, "returning  %" GST_PTR_FORMAT, caps);

  return caps;
}

static GstCaps *
gst_ass_render_get_src_caps (GstPad * pad, GstAssRender * render,
    GstCaps * filter)
{
  GstPad *sinkpad = render->video_sinkpad;
  GstCaps *peer_caps = NULL, *caps = NULL, *assrender_filter = NULL;

  if (filter) {
    /* duplicate filter caps which contains the composition into one version
     * with the meta and one without. Filter the other caps by the software
     * caps */
    GstCaps *sw_caps = gst_static_caps_get (&sw_template_caps);
    assrender_filter =
        gst_ass_render_intersect_by_feature (filter,
        GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION, sw_caps);
    gst_caps_unref (sw_caps);
  }

  peer_caps = gst_pad_peer_query_caps (sinkpad, assrender_filter);

  if (assrender_filter)
    gst_caps_unref (assrender_filter);

  if (peer_caps) {

    GST_DEBUG_OBJECT (pad, "peer caps  %" GST_PTR_FORMAT, peer_caps);

    if (gst_caps_is_any (peer_caps)) {

      /* if peer returns ANY caps, return filtered sink pad template caps */
      caps = gst_caps_copy (gst_pad_get_pad_template_caps (sinkpad));

    } else {

      /* return upstream caps + composition feature + upstream caps
       * filtered by the software caps. */
      GstCaps *sw_caps = gst_static_caps_get (&sw_template_caps);
      caps = gst_ass_render_add_feature_and_intersect (peer_caps,
          GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION, sw_caps);
      gst_caps_unref (sw_caps);
    }

    gst_caps_unref (peer_caps);

  } else {
    /* no peer, our padtemplate is enough then */
    caps = gst_pad_get_pad_template_caps (pad);
  }

  if (filter) {
    GstCaps *intersection;

    intersection =
        gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
    caps = intersection;
  }

  GST_DEBUG_OBJECT (render, "returning  %" GST_PTR_FORMAT, caps);

  return caps;
}

static void
blit_bgra_premultiplied (GstAssRender * render, ASS_Image * ass_image,
    guint8 * data, gint width, gint height, gint stride, gint x_off, gint y_off)
{
  guint counter = 0;
  gint alpha, r, g, b, k;
  const guint8 *src;
  guint8 *dst;
  gint x, y, w, h;
  gint dst_skip;
  gint src_skip;
  gint dst_x, dst_y;

  memset (data, 0, stride * height);

  while (ass_image) {
    dst_x = ass_image->dst_x + x_off;
    dst_y = ass_image->dst_y + y_off;

    if (dst_y >= height || dst_x >= width)
      goto next;

    alpha = 255 - (ass_image->color & 0xff);
    r = ((ass_image->color) >> 24) & 0xff;
    g = ((ass_image->color) >> 16) & 0xff;
    b = ((ass_image->color) >> 8) & 0xff;
    src = ass_image->bitmap;
    dst = data + dst_y * stride + dst_x * 4;

    w = MIN (ass_image->w, width - dst_x);
    h = MIN (ass_image->h, height - dst_y);
    src_skip = ass_image->stride - w;
    dst_skip = stride - w * 4;

    for (y = 0; y < h; y++) {
      for (x = 0; x < w; x++) {
        k = src[0] * alpha / 255;
        if (dst[3] == 0) {
          dst[3] = k;
          dst[2] = (k * r) / 255;
          dst[1] = (k * g) / 255;
          dst[0] = (k * b) / 255;
        } else {
          dst[3] = k + (255 - k) * dst[3] / 255;
          dst[2] = (k * r + (255 - k) * dst[2]) / 255;
          dst[1] = (k * g + (255 - k) * dst[1]) / 255;
          dst[0] = (k * b + (255 - k) * dst[0]) / 255;
        }
        src++;
        dst += 4;
      }
      src += src_skip;
      dst += dst_skip;
    }
  next:
    counter++;
    ass_image = ass_image->next;
  }
  GST_LOG_OBJECT (render, "amount of rendered ass_image: %u", counter);
}

static gboolean
gst_ass_render_can_handle_caps (GstCaps * incaps)
{
  static GstStaticCaps static_caps = GST_STATIC_CAPS (ASSRENDER_CAPS);
  gboolean ret;
  GstCaps *caps;

  caps = gst_static_caps_get (&static_caps);
  ret = gst_caps_is_subset (incaps, caps);
  gst_caps_unref (caps);

  return ret;
}

static gboolean
gst_ass_render_setcaps_video (GstPad * pad, GstAssRender * render,
    GstCaps * caps)
{
  GstQuery *query;
  gboolean ret = FALSE;
  gint par_n = 1, par_d = 1;
  gdouble dar;
  GstVideoInfo info;
  gboolean attach = FALSE;
  gboolean caps_has_meta = TRUE;
  GstCapsFeatures *f;
  GstCaps *original_caps = caps;

  if (!gst_video_info_from_caps (&info, caps))
    goto invalid_caps;

  render->info = info;
  gst_caps_ref (caps);

  /* Try to use the overlay meta if possible */
  f = gst_caps_get_features (caps, 0);

  /* if the caps doesn't have the overlay meta, we query if downstream
   * accepts it before trying the version without the meta
   * If upstream already is using the meta then we can only use it */
  if (!f
      || !gst_caps_features_contains (f,
          GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION)) {
    GstCaps *overlay_caps;

    /* In this case we added the meta, but we can work without it
     * so preserve the original caps so we can use it as a fallback */
    overlay_caps = gst_caps_copy (caps);

    f = gst_caps_get_features (overlay_caps, 0);
    gst_caps_features_add (f,
        GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION);

    ret = gst_pad_peer_query_accept_caps (render->srcpad, overlay_caps);
    GST_DEBUG_OBJECT (render, "Downstream accepts the overlay meta: %d", ret);
    if (ret) {
      gst_caps_unref (caps);
      caps = overlay_caps;

    } else {
      /* fallback to the original */
      gst_caps_unref (overlay_caps);
      caps_has_meta = FALSE;
    }

  }
  GST_DEBUG_OBJECT (render, "Using caps %" GST_PTR_FORMAT, caps);
  ret = gst_pad_set_caps (render->srcpad, caps);
  gst_caps_unref (caps);

  if (!ret)
    goto out;

  render->width = info.width;
  render->height = info.height;

  query = gst_query_new_allocation (caps, FALSE);
  if (caps_has_meta && gst_pad_peer_query (render->srcpad, query)) {
    if (gst_query_find_allocation_meta (query,
            GST_VIDEO_OVERLAY_COMPOSITION_META_API_TYPE, NULL))
      attach = TRUE;
  }
  gst_query_unref (query);

  render->attach_compo_to_buffer = attach;

  if (!attach) {
    if (caps_has_meta) {
      /* Some elements (fakesink) claim to accept the meta on caps but won't
         put it in the allocation query result, this leads below
         check to fail. Prevent this by removing the meta from caps */
      caps = original_caps;
      ret = gst_pad_set_caps (render->srcpad, caps);
      if (!ret)
        goto out;
    }
    if (!gst_ass_render_can_handle_caps (caps))
      goto unsupported_caps;
  }

  g_mutex_lock (&render->ass_mutex);
  ass_set_frame_size (render->ass_renderer, render->width, render->height);

  dar = (((gdouble) par_n) * ((gdouble) render->width))
      / (((gdouble) par_d) * ((gdouble) render->height));
#if !defined(LIBASS_VERSION) || LIBASS_VERSION < 0x00907000
  ass_set_aspect_ratio (render->ass_renderer, dar);
#else
  ass_set_aspect_ratio (render->ass_renderer,
      dar, ((gdouble) render->width) / ((gdouble) render->height));
#endif
  ass_set_font_scale (render->ass_renderer, 1.0);
  ass_set_hinting (render->ass_renderer, ASS_HINTING_LIGHT);

#if !defined(LIBASS_VERSION) || LIBASS_VERSION < 0x00907000
  ass_set_fonts (render->ass_renderer, "Arial", "sans-serif");
  ass_set_fonts (render->ass_renderer, NULL, "Sans");
#else
  ass_set_fonts (render->ass_renderer, "Arial", "sans-serif", 1, NULL, 1);
  ass_set_fonts (render->ass_renderer, NULL, "Sans", 1, NULL, 1);
#endif
  ass_set_margins (render->ass_renderer, 0, 0, 0, 0);
  ass_set_use_margins (render->ass_renderer, 0);
  g_mutex_unlock (&render->ass_mutex);

  render->renderer_init_ok = TRUE;

  GST_DEBUG_OBJECT (render, "ass renderer setup complete");

out:

  return ret;

  /* ERRORS */
invalid_caps:
  {
    GST_ERROR_OBJECT (render, "Can't parse caps: %" GST_PTR_FORMAT, caps);
    ret = FALSE;
    goto out;
  }
unsupported_caps:
  {
    GST_ERROR_OBJECT (render, "Unsupported caps: %" GST_PTR_FORMAT, caps);
    ret = FALSE;
    goto out;
  }
}

static gboolean
gst_ass_render_setcaps_text (GstPad * pad, GstAssRender * render,
    GstCaps * caps)
{
  GstStructure *structure;
  const GValue *value;
  GstBuffer *priv;
  GstMapInfo map;
  gboolean ret = FALSE;

  structure = gst_caps_get_structure (caps, 0);

  GST_DEBUG_OBJECT (render, "text pad linked with caps:  %" GST_PTR_FORMAT,
      caps);

  value = gst_structure_get_value (structure, "codec_data");

  g_mutex_lock (&render->ass_mutex);
  if (value != NULL) {
    priv = gst_value_get_buffer (value);
    g_return_val_if_fail (priv != NULL, FALSE);

    gst_buffer_map (priv, &map, GST_MAP_READ);

    if (!render->ass_track)
      render->ass_track = ass_new_track (render->ass_library);

    ass_process_codec_private (render->ass_track, (char *) map.data, map.size);

    gst_buffer_unmap (priv, &map);

    GST_DEBUG_OBJECT (render, "ass track created");

    render->track_init_ok = TRUE;

    ret = TRUE;
  } else if (!render->ass_track) {
    render->ass_track = ass_new_track (render->ass_library);

    render->track_init_ok = TRUE;

    ret = TRUE;
  }
  g_mutex_unlock (&render->ass_mutex);

  return ret;
}


static void
gst_ass_render_process_text (GstAssRender * render, GstBuffer * buffer,
    GstClockTime running_time, GstClockTime duration)
{
  GstMapInfo map;
  gdouble pts_start, pts_end;

  pts_start = running_time;
  pts_start /= GST_MSECOND;
  pts_end = duration;
  pts_end /= GST_MSECOND;

  GST_DEBUG_OBJECT (render,
      "Processing subtitles with running time %" GST_TIME_FORMAT
      " and duration %" GST_TIME_FORMAT, GST_TIME_ARGS (running_time),
      GST_TIME_ARGS (duration));

  gst_buffer_map (buffer, &map, GST_MAP_READ);

  g_mutex_lock (&render->ass_mutex);
  ass_process_chunk (render->ass_track, (gchar *) map.data, map.size,
      pts_start, pts_end);
  g_mutex_unlock (&render->ass_mutex);

  gst_buffer_unmap (buffer, &map);
}

static GstVideoOverlayComposition *
gst_ass_render_composite_overlay (GstAssRender * render, ASS_Image * images)
{
  GstVideoOverlayComposition *composition;
  GstVideoOverlayRectangle *rectangle;
  GstVideoMeta *vmeta;
  GstMapInfo map;
  GstBuffer *buffer;
  ASS_Image *image;
  gint min_x, min_y;
  gint max_x, max_y;
  gint width, height;
  gint stride;
  gpointer data;

  min_x = G_MAXINT;
  min_y = G_MAXINT;
  max_x = 0;
  max_y = 0;

  /* find bounding box of all images, to limit the overlay rectangle size */
  for (image = images; image; image = image->next) {
    if (min_x > image->dst_x)
      min_x = image->dst_x;
    if (min_y > image->dst_y)
      min_y = image->dst_y;
    if (max_x < image->dst_x + image->w)
      max_x = image->dst_x + image->w;
    if (max_y < image->dst_y + image->h)
      max_y = image->dst_y + image->h;
  }

  width = MIN (max_x - min_x, render->width);
  height = MIN (max_y - min_y, render->height);

  GST_DEBUG_OBJECT (render, "render overlay rectangle %dx%d%+d%+d",
      width, height, min_x, min_y);

  buffer = gst_buffer_new_and_alloc (4 * width * height);
  if (!buffer) {
    GST_ERROR_OBJECT (render, "Failed to allocate overlay buffer");
    return NULL;
  }

  vmeta = gst_buffer_add_video_meta (buffer, GST_VIDEO_FRAME_FLAG_NONE,
      GST_VIDEO_OVERLAY_COMPOSITION_FORMAT_RGB, width, height);

  if (!gst_video_meta_map (vmeta, 0, &map, &data, &stride, GST_MAP_READWRITE)) {
    GST_ERROR_OBJECT (render, "Failed to map overlay buffer");
    gst_buffer_unref (buffer);
    return NULL;
  }

  blit_bgra_premultiplied (render, images, data, width, height, stride,
      -min_x, -min_y);
  gst_video_meta_unmap (vmeta, 0, &map);

  rectangle = gst_video_overlay_rectangle_new_raw (buffer, min_x, min_y,
      width, height, GST_VIDEO_OVERLAY_FORMAT_FLAG_PREMULTIPLIED_ALPHA);

  gst_buffer_unref (buffer);

  composition = gst_video_overlay_composition_new (rectangle);
  gst_video_overlay_rectangle_unref (rectangle);

  return composition;
}

static gboolean
gst_ass_render_push_frame (GstAssRender * render, GstBuffer * video_frame)
{
  GstVideoFrame frame;

  if (!render->composition)
    goto done;

  video_frame = gst_buffer_make_writable (video_frame);

  if (render->attach_compo_to_buffer) {
    gst_buffer_add_video_overlay_composition_meta (video_frame,
        render->composition);
    goto done;
  }

  if (!gst_video_frame_map (&frame, &render->info, video_frame,
          GST_MAP_READWRITE)) {
    GST_WARNING_OBJECT (render, "failed to map video frame for blending");
    goto done;
  }

  gst_video_overlay_composition_blend (render->composition, &frame);
  gst_video_frame_unmap (&frame);

done:
  return gst_pad_push (render->srcpad, video_frame);
}

static GstFlowReturn
gst_ass_render_chain_video (GstPad * pad, GstObject * parent,
    GstBuffer * buffer)
{
  GstAssRender *render = GST_ASS_RENDER (parent);
  GstFlowReturn ret = GST_FLOW_OK;
  gboolean in_seg = FALSE;
  guint64 start, stop, clip_start = 0, clip_stop = 0;
  ASS_Image *ass_image;

  if (!GST_BUFFER_TIMESTAMP_IS_VALID (buffer))
    goto missing_timestamp;

  /* ignore buffers that are outside of the current segment */
  start = GST_BUFFER_TIMESTAMP (buffer);

  if (!GST_BUFFER_DURATION_IS_VALID (buffer)) {
    stop = GST_CLOCK_TIME_NONE;
  } else {
    stop = start + GST_BUFFER_DURATION (buffer);
  }

  /* segment_clip() will adjust start unconditionally to segment_start if
   * no stop time is provided, so handle this ourselves */
  if (stop == GST_CLOCK_TIME_NONE && start < render->video_segment.start)
    goto out_of_segment;

  in_seg =
      gst_segment_clip (&render->video_segment, GST_FORMAT_TIME, start, stop,
      &clip_start, &clip_stop);

  if (!in_seg)
    goto out_of_segment;

  /* if the buffer is only partially in the segment, fix up stamps */
  if (clip_start != start || (stop != -1 && clip_stop != stop)) {
    GST_DEBUG_OBJECT (render, "clipping buffer timestamp/duration to segment");
    buffer = gst_buffer_make_writable (buffer);
    GST_BUFFER_TIMESTAMP (buffer) = clip_start;
    if (stop != -1)
      GST_BUFFER_DURATION (buffer) = clip_stop - clip_start;
  }

  /* now, after we've done the clipping, fix up end time if there's no
   * duration (we only use those estimated values internally though, we
   * don't want to set bogus values on the buffer itself) */
  if (stop == -1) {
    if (render->info.fps_n && render->info.fps_d) {
      GST_DEBUG_OBJECT (render, "estimating duration based on framerate");
      stop =
          start + gst_util_uint64_scale_int (GST_SECOND, render->info.fps_d,
          render->info.fps_n);
    } else {
      GST_WARNING_OBJECT (render, "no duration, assuming minimal duration");
      stop = start + 1;         /* we need to assume some interval */
    }
  }

wait_for_text_buf:

  GST_ASS_RENDER_LOCK (render);

  if (render->video_flushing)
    goto flushing;

  if (render->video_eos)
    goto have_eos;

  if (render->renderer_init_ok && render->track_init_ok && render->enable) {
    /* Text pad linked, check if we have a text buffer queued */
    if (render->subtitle_pending) {
      GstClockTime text_start = GST_CLOCK_TIME_NONE;
      GstClockTime text_end = GST_CLOCK_TIME_NONE;
      GstClockTime text_running_time = GST_CLOCK_TIME_NONE;
      GstClockTime text_running_time_end = GST_CLOCK_TIME_NONE;
      GstClockTime vid_running_time, vid_running_time_end;
      gdouble timestamp;
      gint changed = 0;

      /* if the text buffer isn't stamped right, pop it off the
       * queue and display it for the current video frame only */
      if (!GST_BUFFER_TIMESTAMP_IS_VALID (render->subtitle_pending) ||
          !GST_BUFFER_DURATION_IS_VALID (render->subtitle_pending)) {
        GST_WARNING_OBJECT (render,
            "Got text buffer with invalid timestamp or duration");
        gst_ass_render_pop_text (render);
        GST_ASS_RENDER_UNLOCK (render);
        goto wait_for_text_buf;
      }

      text_start = GST_BUFFER_TIMESTAMP (render->subtitle_pending);
      text_end = text_start + GST_BUFFER_DURATION (render->subtitle_pending);

      vid_running_time =
          gst_segment_to_running_time (&render->video_segment, GST_FORMAT_TIME,
          start);
      vid_running_time_end =
          gst_segment_to_running_time (&render->video_segment, GST_FORMAT_TIME,
          stop);

      /* If timestamp and duration are valid */
      text_running_time =
          gst_segment_to_running_time (&render->video_segment,
          GST_FORMAT_TIME, text_start);
      text_running_time_end =
          gst_segment_to_running_time (&render->video_segment,
          GST_FORMAT_TIME, text_end);

      GST_LOG_OBJECT (render, "T: %" GST_TIME_FORMAT " - %" GST_TIME_FORMAT,
          GST_TIME_ARGS (text_running_time),
          GST_TIME_ARGS (text_running_time_end));
      GST_LOG_OBJECT (render, "V: %" GST_TIME_FORMAT " - %" GST_TIME_FORMAT,
          GST_TIME_ARGS (vid_running_time),
          GST_TIME_ARGS (vid_running_time_end));

      /* Text too old */
      if (text_running_time_end <= vid_running_time) {
        GST_DEBUG_OBJECT (render, "text buffer too old, popping");
        gst_ass_render_pop_text (render);
        GST_ASS_RENDER_UNLOCK (render);
        goto wait_for_text_buf;
      }

      if (render->need_process) {
        GST_DEBUG_OBJECT (render, "process text buffer");
        gst_ass_render_process_text (render, render->subtitle_pending,
            text_running_time, text_running_time_end - text_running_time);
        render->need_process = FALSE;
      }

      GST_ASS_RENDER_UNLOCK (render);

      /* libass needs timestamps in ms */
      timestamp = vid_running_time / GST_MSECOND;

      g_mutex_lock (&render->ass_mutex);
      ass_image = ass_render_frame (render->ass_renderer, render->ass_track,
          timestamp, &changed);
      g_mutex_unlock (&render->ass_mutex);

      if ((!ass_image || changed) && render->composition) {
        GST_DEBUG_OBJECT (render, "release overlay (changed %d)", changed);
        gst_video_overlay_composition_unref (render->composition);
        render->composition = NULL;
      }

      if (ass_image != NULL) {
        if (!render->composition)
          render->composition = gst_ass_render_composite_overlay (render,
              ass_image);
      } else {
        GST_DEBUG_OBJECT (render, "nothing to render right now");
      }

      /* Push the video frame */
      ret = gst_ass_render_push_frame (render, buffer);

      if (text_running_time_end <= vid_running_time_end) {
        GST_ASS_RENDER_LOCK (render);
        gst_ass_render_pop_text (render);
        GST_ASS_RENDER_UNLOCK (render);
      }
    } else {
      gboolean wait_for_text_buf = TRUE;

      if (render->subtitle_eos)
        wait_for_text_buf = FALSE;

      if (!render->wait_text)
        wait_for_text_buf = FALSE;

      /* Text pad linked, but no text buffer available - what now? */
      if (render->subtitle_segment.format == GST_FORMAT_TIME) {
        GstClockTime text_start_running_time, text_last_stop_running_time;
        GstClockTime vid_running_time;

        vid_running_time =
            gst_segment_to_running_time (&render->video_segment,
            GST_FORMAT_TIME, GST_BUFFER_TIMESTAMP (buffer));
        text_start_running_time =
            gst_segment_to_running_time (&render->subtitle_segment,
            GST_FORMAT_TIME, render->subtitle_segment.start);
        text_last_stop_running_time =
            gst_segment_to_running_time (&render->subtitle_segment,
            GST_FORMAT_TIME, render->subtitle_segment.position);

        if ((GST_CLOCK_TIME_IS_VALID (text_start_running_time) &&
                vid_running_time < text_start_running_time) ||
            (GST_CLOCK_TIME_IS_VALID (text_last_stop_running_time) &&
                vid_running_time < text_last_stop_running_time)) {
          wait_for_text_buf = FALSE;
        }
      }

      if (wait_for_text_buf) {
        GST_DEBUG_OBJECT (render, "no text buffer, need to wait for one");
        GST_ASS_RENDER_WAIT (render);
        GST_DEBUG_OBJECT (render, "resuming");
        GST_ASS_RENDER_UNLOCK (render);
        goto wait_for_text_buf;
      } else {
        GST_ASS_RENDER_UNLOCK (render);
        GST_LOG_OBJECT (render, "no need to wait for a text buffer");
        ret = gst_pad_push (render->srcpad, buffer);
      }
    }
  } else {
    GST_LOG_OBJECT (render, "rendering disabled, doing buffer passthrough");

    GST_ASS_RENDER_UNLOCK (render);
    ret = gst_pad_push (render->srcpad, buffer);
    return ret;
  }

  GST_DEBUG_OBJECT (render, "leaving chain for buffer %p ret=%d", buffer, ret);

  /* Update last_stop */
  render->video_segment.position = clip_start;

  return ret;

missing_timestamp:
  {
    GST_WARNING_OBJECT (render, "buffer without timestamp, discarding");
    gst_buffer_unref (buffer);
    return GST_FLOW_OK;
  }
flushing:
  {
    GST_ASS_RENDER_UNLOCK (render);
    GST_DEBUG_OBJECT (render, "flushing, discarding buffer");
    gst_buffer_unref (buffer);
    return GST_FLOW_FLUSHING;
  }
have_eos:
  {
    GST_ASS_RENDER_UNLOCK (render);
    GST_DEBUG_OBJECT (render, "eos, discarding buffer");
    gst_buffer_unref (buffer);
    return GST_FLOW_EOS;
  }
out_of_segment:
  {
    GST_DEBUG_OBJECT (render, "buffer out of segment, discarding");
    gst_buffer_unref (buffer);
    return GST_FLOW_OK;
  }
}

static GstFlowReturn
gst_ass_render_chain_text (GstPad * pad, GstObject * parent, GstBuffer * buffer)
{
  GstFlowReturn ret = GST_FLOW_OK;
  GstAssRender *render = GST_ASS_RENDER (parent);
  gboolean in_seg = FALSE;
  guint64 clip_start = 0, clip_stop = 0;

  GST_DEBUG_OBJECT (render, "entering chain for buffer %p", buffer);

  GST_ASS_RENDER_LOCK (render);

  if (render->subtitle_flushing) {
    GST_ASS_RENDER_UNLOCK (render);
    ret = GST_FLOW_FLUSHING;
    GST_LOG_OBJECT (render, "text flushing");
    goto beach;
  }

  if (render->subtitle_eos) {
    GST_ASS_RENDER_UNLOCK (render);
    ret = GST_FLOW_EOS;
    GST_LOG_OBJECT (render, "text EOS");
    goto beach;
  }

  if (G_LIKELY (GST_BUFFER_TIMESTAMP_IS_VALID (buffer))) {
    GstClockTime stop;

    if (G_LIKELY (GST_BUFFER_DURATION_IS_VALID (buffer)))
      stop = GST_BUFFER_TIMESTAMP (buffer) + GST_BUFFER_DURATION (buffer);
    else
      stop = GST_CLOCK_TIME_NONE;

    in_seg = gst_segment_clip (&render->subtitle_segment, GST_FORMAT_TIME,
        GST_BUFFER_TIMESTAMP (buffer), stop, &clip_start, &clip_stop);
  } else {
    in_seg = TRUE;
  }

  if (in_seg) {
    if (GST_BUFFER_TIMESTAMP_IS_VALID (buffer))
      GST_BUFFER_TIMESTAMP (buffer) = clip_start;
    else if (GST_BUFFER_DURATION_IS_VALID (buffer))
      GST_BUFFER_DURATION (buffer) = clip_stop - clip_start;

    if (render->subtitle_pending
        && (!GST_BUFFER_TIMESTAMP_IS_VALID (render->subtitle_pending)
            || !GST_BUFFER_DURATION_IS_VALID (render->subtitle_pending))) {
      gst_buffer_unref (render->subtitle_pending);
      render->subtitle_pending = NULL;
      GST_ASS_RENDER_BROADCAST (render);
    } else {
      /* Wait for the previous buffer to go away */
      while (render->subtitle_pending != NULL) {
        GST_DEBUG ("Pad %s:%s has a buffer queued, waiting",
            GST_DEBUG_PAD_NAME (pad));
        GST_ASS_RENDER_WAIT (render);
        GST_DEBUG ("Pad %s:%s resuming", GST_DEBUG_PAD_NAME (pad));
        if (render->subtitle_flushing) {
          GST_ASS_RENDER_UNLOCK (render);
          ret = GST_FLOW_FLUSHING;
          goto beach;
        }
      }
    }

    if (GST_BUFFER_TIMESTAMP_IS_VALID (buffer))
      render->subtitle_segment.position = clip_start;

    GST_DEBUG_OBJECT (render,
        "New buffer arrived for timestamp %" GST_TIME_FORMAT,
        GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (buffer)));
    render->subtitle_pending = gst_buffer_ref (buffer);
    render->need_process = TRUE;

    /* in case the video chain is waiting for a text buffer, wake it up */
    GST_ASS_RENDER_BROADCAST (render);
  }

  GST_ASS_RENDER_UNLOCK (render);

beach:
  GST_DEBUG_OBJECT (render, "leaving chain for buffer %p", buffer);

  gst_buffer_unref (buffer);
  return ret;
}

static void
gst_ass_render_handle_tags (GstAssRender * render, GstTagList * taglist)
{
  static const gchar *mimetypes[] = {
    "application/x-font-ttf",
    "application/x-font-otf",
    "application/x-truetype-font"
  };
  static const gchar *extensions[] = {
    ".otf",
    ".ttf"
  };
  guint tag_size;

  if (!taglist)
    return;

  tag_size = gst_tag_list_get_tag_size (taglist, GST_TAG_ATTACHMENT);
  if (tag_size > 0 && render->embeddedfonts) {
    GstSample *sample;
    GstBuffer *buf;
    const GstStructure *structure;
    gboolean valid_mimetype, valid_extension;
    guint j;
    const gchar *filename;
    guint index;
    GstMapInfo map;

    GST_DEBUG_OBJECT (render, "TAG event has attachments");

    for (index = 0; index < tag_size; index++) {
      if (!gst_tag_list_get_sample_index (taglist, GST_TAG_ATTACHMENT, index,
              &sample))
        continue;
      buf = gst_sample_get_buffer (sample);
      structure = gst_sample_get_info (sample);
      if (!buf || !structure)
        continue;

      valid_mimetype = FALSE;
      valid_extension = FALSE;

      for (j = 0; j < G_N_ELEMENTS (mimetypes); j++) {
        if (gst_structure_has_name (structure, mimetypes[j])) {
          valid_mimetype = TRUE;
          break;
        }
      }
      filename = gst_structure_get_string (structure, "filename");
      if (!filename)
        continue;

      if (!valid_mimetype) {
        guint len = strlen (filename);
        const gchar *extension = filename + len - 4;
        for (j = 0; j < G_N_ELEMENTS (extensions); j++) {
          if (g_ascii_strcasecmp (extension, extensions[j]) == 0) {
            valid_extension = TRUE;
            break;
          }
        }
      }

      if (valid_mimetype || valid_extension) {
        g_mutex_lock (&render->ass_mutex);
        gst_buffer_map (buf, &map, GST_MAP_READ);
        ass_add_font (render->ass_library, (gchar *) filename,
            (gchar *) map.data, map.size);
        gst_buffer_unmap (buf, &map);
        GST_DEBUG_OBJECT (render, "registered new font %s", filename);
        g_mutex_unlock (&render->ass_mutex);
      }
    }
  }
}

static gboolean
gst_ass_render_event_video (GstPad * pad, GstObject * parent, GstEvent * event)
{
  gboolean ret = FALSE;
  GstAssRender *render = GST_ASS_RENDER (parent);

  GST_DEBUG_OBJECT (pad, "received video event %" GST_PTR_FORMAT, event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret = gst_ass_render_setcaps_video (pad, render, caps);
      gst_event_unref (event);
      break;
    }
    case GST_EVENT_SEGMENT:
    {
      GstSegment segment;

      GST_DEBUG_OBJECT (render, "received new segment");

      gst_event_copy_segment (event, &segment);

      if (segment.format == GST_FORMAT_TIME) {
        GST_DEBUG_OBJECT (render, "VIDEO SEGMENT now: %" GST_SEGMENT_FORMAT,
            &render->video_segment);

        render->video_segment = segment;

        GST_DEBUG_OBJECT (render, "VIDEO SEGMENT after: %" GST_SEGMENT_FORMAT,
            &render->video_segment);
        ret = gst_pad_event_default (pad, parent, event);
      } else {
        GST_ELEMENT_WARNING (render, STREAM, MUX, (NULL),
            ("received non-TIME newsegment event on video input"));
        ret = FALSE;
        gst_event_unref (event);
      }
      break;
    }
    case GST_EVENT_TAG:
    {
      GstTagList *taglist = NULL;

      /* tag events may contain attachments which might be fonts */
      GST_DEBUG_OBJECT (render, "got TAG event");

      gst_event_parse_tag (event, &taglist);
      gst_ass_render_handle_tags (render, taglist);
      ret = gst_pad_event_default (pad, parent, event);
      break;
    }
    case GST_EVENT_EOS:
      GST_ASS_RENDER_LOCK (render);
      GST_INFO_OBJECT (render, "video EOS");
      render->video_eos = TRUE;
      GST_ASS_RENDER_UNLOCK (render);
      ret = gst_pad_event_default (pad, parent, event);
      break;
    case GST_EVENT_FLUSH_START:
      GST_ASS_RENDER_LOCK (render);
      GST_INFO_OBJECT (render, "video flush start");
      render->video_flushing = TRUE;
      GST_ASS_RENDER_BROADCAST (render);
      GST_ASS_RENDER_UNLOCK (render);
      ret = gst_pad_event_default (pad, parent, event);
      break;
    case GST_EVENT_FLUSH_STOP:
      GST_ASS_RENDER_LOCK (render);
      GST_INFO_OBJECT (render, "video flush stop");
      render->video_flushing = FALSE;
      render->video_eos = FALSE;
      gst_segment_init (&render->video_segment, GST_FORMAT_TIME);
      GST_ASS_RENDER_UNLOCK (render);
      ret = gst_pad_event_default (pad, parent, event);
      break;
    default:
      ret = gst_pad_event_default (pad, parent, event);
      break;
  }

  return ret;
}

static gboolean
gst_ass_render_query_video (GstPad * pad, GstObject * parent, GstQuery * query)
{
  gboolean res = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      GstCaps *filter, *caps;

      gst_query_parse_caps (query, &filter);
      caps =
          gst_ass_render_get_videosink_caps (pad, (GstAssRender *) parent,
          filter);
      gst_query_set_caps_result (query, caps);
      gst_caps_unref (caps);
      res = TRUE;
      break;
    }
    default:
      res = gst_pad_query_default (pad, parent, query);
      break;
  }

  return res;
}

static gboolean
gst_ass_render_event_text (GstPad * pad, GstObject * parent, GstEvent * event)
{
  gint i;
  gboolean ret = FALSE;
  GstAssRender *render = GST_ASS_RENDER (parent);

  GST_DEBUG_OBJECT (pad, "received text event %" GST_PTR_FORMAT, event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret = gst_ass_render_setcaps_text (pad, render, caps);
      gst_event_unref (event);
      break;
    }
    case GST_EVENT_SEGMENT:
    {
      GstSegment segment;

      GST_ASS_RENDER_LOCK (render);
      render->subtitle_eos = FALSE;
      GST_ASS_RENDER_UNLOCK (render);

      gst_event_copy_segment (event, &segment);

      GST_ASS_RENDER_LOCK (render);
      if (segment.format == GST_FORMAT_TIME) {
        GST_DEBUG_OBJECT (render, "TEXT SEGMENT now: %" GST_SEGMENT_FORMAT,
            &render->subtitle_segment);

        render->subtitle_segment = segment;

        GST_DEBUG_OBJECT (render,
            "TEXT SEGMENT after: %" GST_SEGMENT_FORMAT,
            &render->subtitle_segment);
      } else {
        GST_ELEMENT_WARNING (render, STREAM, MUX, (NULL),
            ("received non-TIME newsegment event on subtitle input"));
      }

      gst_event_unref (event);
      ret = TRUE;

      /* wake up the video chain, it might be waiting for a text buffer or
       * a text segment update */
      GST_ASS_RENDER_BROADCAST (render);
      GST_ASS_RENDER_UNLOCK (render);
      break;
    }
    case GST_EVENT_GAP:{
      GstClockTime start, duration;

      gst_event_parse_gap (event, &start, &duration);
      if (GST_CLOCK_TIME_IS_VALID (duration))
        start += duration;
      /* we do not expect another buffer until after gap,
       * so that is our position now */
      GST_ASS_RENDER_LOCK (render);
      render->subtitle_segment.position = start;

      /* wake up the video chain, it might be waiting for a text buffer or
       * a text segment update */
      GST_ASS_RENDER_BROADCAST (render);
      GST_ASS_RENDER_UNLOCK (render);
      break;
    }
    case GST_EVENT_FLUSH_STOP:
      GST_ASS_RENDER_LOCK (render);
      GST_INFO_OBJECT (render, "text flush stop");
      render->subtitle_flushing = FALSE;
      render->subtitle_eos = FALSE;
      gst_ass_render_pop_text (render);
      gst_segment_init (&render->subtitle_segment, GST_FORMAT_TIME);
      GST_ASS_RENDER_UNLOCK (render);
      gst_event_unref (event);
      ret = TRUE;
      break;
    case GST_EVENT_FLUSH_START:
      GST_DEBUG_OBJECT (render, "text flush start");
      g_mutex_lock (&render->ass_mutex);
      if (render->ass_track) {
        /* delete any events on the ass_track */
        for (i = 0; i < render->ass_track->n_events; i++) {
          GST_DEBUG_OBJECT (render, "deleted event with eid %i", i);
          ass_free_event (render->ass_track, i);
        }
        render->ass_track->n_events = 0;
        GST_DEBUG_OBJECT (render, "done flushing");
      }
      g_mutex_unlock (&render->ass_mutex);
      GST_ASS_RENDER_LOCK (render);
      render->subtitle_flushing = TRUE;
      GST_ASS_RENDER_BROADCAST (render);
      GST_ASS_RENDER_UNLOCK (render);
      gst_event_unref (event);
      ret = TRUE;
      break;
    case GST_EVENT_EOS:
      GST_ASS_RENDER_LOCK (render);
      render->subtitle_eos = TRUE;
      GST_INFO_OBJECT (render, "text EOS");
      /* wake up the video chain, it might be waiting for a text buffer or
       * a text segment update */
      GST_ASS_RENDER_BROADCAST (render);
      GST_ASS_RENDER_UNLOCK (render);
      gst_event_unref (event);
      ret = TRUE;
      break;
    case GST_EVENT_TAG:
    {
      GstTagList *taglist = NULL;

      /* tag events may contain attachments which might be fonts */
      GST_DEBUG_OBJECT (render, "got TAG event");

      gst_event_parse_tag (event, &taglist);
      gst_ass_render_handle_tags (render, taglist);
      ret = gst_pad_event_default (pad, parent, event);
      break;
    }
    default:
      ret = gst_pad_event_default (pad, parent, event);
      break;
  }

  return ret;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gst_ass_render_debug, "assrender",
      0, "ASS/SSA subtitle renderer");
  GST_DEBUG_CATEGORY_INIT (gst_ass_render_lib_debug, "assrender_library",
      0, "ASS/SSA subtitle renderer library");

  return gst_element_register (plugin, "assrender",
      GST_RANK_PRIMARY, GST_TYPE_ASS_RENDER);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    assrender,
    "ASS/SSA subtitle renderer",
    plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
