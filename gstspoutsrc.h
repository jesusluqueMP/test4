/* GStreamer
 * Copyright (C) 2023 Your Name <your.email@example.com>
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

/* Define GST_USE_UNSTABLE_API to avoid warnings about unstable API */
#define GST_USE_UNSTABLE_API

#pragma once

#include <gst/gst.h>
#include <gst/base/gstbasesrc.h>
#include <gst/video/video.h>
#include <gst/d3d11/gstd3d11.h>
#include <mutex>
#include <string>

G_BEGIN_DECLS

#define GST_TYPE_SPOUT_SRC (gst_spout_src_get_type())
G_DECLARE_FINAL_TYPE (GstSpoutSrc, gst_spout_src,
    GST, SPOUT_SRC, GstBaseSrc);

/* Define available format strings for templates and cap negotiation */
#define GST_SPOUT_SRC_FORMATS "{ BGRA, RGBA, RGBx, BGRx }"

G_END_DECLS