/* GStreamer
 * Copyright (C) 2023 Igalia, S.L.
 *     Author: Stéphane Cerveau<scerveau@igalia.com>
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

#pragma once

#define GST_USE_UNSTABLE_API
#include <gst/codecs/gsth265encoder.h>
#undef GST_USE_UNSTABLE_API

G_BEGIN_DECLS

#define GST_TYPE_VULKAN_H265ENC (gst_vulkan_h265enc_get_type())
G_DECLARE_FINAL_TYPE (GstVulkanH265Enc, gst_vulkan_h265enc, GST, VULKAN_H265ENC, GstH265Encoder)

GST_ELEMENT_REGISTER_DECLARE (vulkanh265enc);

G_END_DECLS
