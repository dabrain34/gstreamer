/* GStreamer
 * Copyright (C) 2023 Stéphane Cerveau <scerveau@igalia.com>
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
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gsth264frame.h"

GST_DEBUG_CATEGORY_EXTERN (gst_h264_encoder_debug);
#define GST_CAT_DEFAULT gst_h264_encoder_debug

GST_DEFINE_MINI_OBJECT_TYPE (GstH264Frame, gst_h264_frame);

static void
_gst_h264_frame_free (GstH264Frame * frame)
{
  GST_TRACE ("Free frame %p", frame);

  gst_video_codec_frame_unref (frame->frame);

  g_free (frame);
}

/**
 * gst_h264_frame_new:
 *
 * Create new #GstH264Frame
 *
 * Returns: a new #GstH264Frame
 */
GstH264Frame *
gst_h264_frame_new (GstVideoCodecFrame * f)
{
  GstH264Frame *frame;

  if (!f)
    return NULL;

  frame = g_new0 (GstH264Frame, 1);

  gst_mini_object_init (GST_MINI_OBJECT_CAST (frame), 0,
      GST_TYPE_H264_FRAME, NULL, NULL,
      (GstMiniObjectFreeFunction) _gst_h264_frame_free);

  frame->frame = gst_video_codec_frame_ref (f);

  GST_TRACE ("New frame %p", frame);

  return frame;
}
