/*
 * Cogl
 *
 * An object oriented GL/GLES Abstraction/Utility Layer
 *
 * Copyright (C) 2009 Intel Corporation.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */

#ifndef __COGL_FEATURE_PRIVATE_H
#define __COGL_FEATURE_PRIVATE_H

#include <gst/gst.h>

#include "gstgldisplay.h"

#define GST_GL_CHECK_GL_VERSION(driver_major, driver_minor, \
                                target_major, target_minor) \
  ((driver_major) > (target_major) || \
   ((driver_major) == (target_major) && (driver_minor) >= (target_minor)))

typedef struct _GstGLFeatureFunction GstGLFeatureFunction;

struct _GstGLFeatureFunction
{
  /* The name of the function without the "EXT" or "ARB" suffix */
  const char *name;
  /* The offset in the context of where to store the function pointer */
  unsigned int pointer_offset;
};

typedef struct _GstGLFeatureData GstGLFeatureData;

struct _GstGLFeatureData
{
  /* A minimum GL version which the functions should be defined in
     without needing an extension. Set to 255,255 if it's only
     provided in an extension */
  int min_gl_major, min_gl_minor;
  /* Flags specifying which versions of GL the feature is available
     in core in */
  GstGLAPI gl_availability;
  /* \0 separated list of namespaces to try. Eg "EXT\0ARB\0" */
  const char *namespaces;
  /* \0 separated list of required extension names without the GL_EXT
     or GL_ARB prefix. Any of the extensions must be available for the
     feature to be considered available. If the suffix for an
     extension is different from the namespace, you can specify it
     with a ':' after the namespace */
  const char *extension_names;
  /* A list of functions required for this feature. Terminated with a
     NULL name */
  const GstGLFeatureFunction *functions;
};

gboolean
_gst_gl_feature_check (GstGLDisplay *display,
                     const char *driver_prefix,
                     const GstGLFeatureData *data,
                     int gl_major,
                     int gl_minor,
                     const char *extensions_string);

void
_gst_gl_feature_check_ext_functions (GstGLDisplay *display,
                                   int gl_major,
                                   int gl_minor,
                                   const char *gl_extensions);

#endif /* __COGL_FEATURE_PRIVATE_H */