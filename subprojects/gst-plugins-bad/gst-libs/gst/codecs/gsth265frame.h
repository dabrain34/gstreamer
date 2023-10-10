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

#ifndef __GTS_H265_FRAME_H__
#define __GTS_H265_FRAME_H__

#include <gst/codecs/codecs-prelude.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

#define GST_TYPE_H265_FRAME     (gst_h265_frame_get_type())
#define GST_IS_H265_FRAME(obj)  (GST_IS_MINI_OBJECT_TYPE(obj, GST_TYPE_H265_FRAME))
#define GST_H265_FRAME(obj)     ((GstH265Frame *)obj)
#define GST_H265_FRAME_CAST(obj) (GST_H265_FRAME(obj))

typedef struct _GstH265Frame GstH265Frame;

typedef enum _GstH265FrameType
{
  GST_H265_FRAME_KEY,
  GST_H265_FRAME_INTER,
} GstH265FrameType;

struct _GstH265Frame
{
  GstMiniObject parent;
  GstH265FrameType type;
  gint quality;

  GstVideoCodecFrame *frame;
};

GST_CODECS_API
GType gst_h265_frame_get_type (void);

GST_CODECS_API
GstH265Frame * gst_h265_frame_new (GstVideoCodecFrame *f);

static inline GstH265Frame *
gst_h265_frame_ref (GstH265Frame * frame)
{
  return (GstH265Frame *) gst_mini_object_ref (GST_MINI_OBJECT_CAST (frame));
}

static inline void
gst_h265_frame_unref (GstH265Frame * frame)
{
  gst_mini_object_unref (GST_MINI_OBJECT_CAST (frame));
}

G_END_DECLS

#endif /* __GTS_H265_FRAME_H__ */
