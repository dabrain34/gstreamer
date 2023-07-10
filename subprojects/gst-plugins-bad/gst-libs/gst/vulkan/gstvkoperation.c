/*
 * GStreamer
 * Copyright (C) 2023 Igalia, S.L.
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
#include "config.h"
#endif

#include "gstvkoperation.h"

/**
 * SECTION:vkoperation
 * @title: GstVulkanOperation
 * @short_description: Vulkan Operation
 * @see_also: #GstVulkanCommandPool
 *
 * A #GstVulkanOperation abstract a queue operation for images adding
 * automatically semaphores and barriers. It uses Synchronization2 extension if
 * available. Also it enables a VkQueryPool if it's possible and it's requested.
 */

typedef struct _GstVulkanDependencyFrame GstVulkanDependencyFrame;

struct _GstVulkanDependencyFrame
{
  GstVulkanImageMemory *mem[GST_VIDEO_MAX_PLANES];
  gboolean updated;
  gboolean semaphored;
  guint64 dst_stage;
  guint64 new_access;
  VkImageLayout new_layout;
  GstVulkanQueue *new_queue;
};

struct _GstVulkanOperationPrivate
{
  GstVulkanCommandPool *cmd_pool;
  GstVulkanTrashList *trash_list;

  VkQueryPool query_pool;
  VkQueryType query_type;
  guint n_queries;
  gsize query_data_size;
  gsize query_data_stride;
  gpointer query_data;

  gboolean has_sync2;
  gboolean has_video;
  gboolean has_timeline;

  GArray *barriers;

  struct
  {
    GArray *frames;
    GArray *wait_semaphores;
    GArray *signal_semaphores;

    /* if sync2 isn't supported but timeline is */
    GArray *wait_dst_stage_mask;
    GArray *wait_semaphore_values;
    GArray *signal_semaphore_values;
  } deps;

#if defined(VK_KHR_synchronization2)
  PFN_vkQueueSubmit2KHR QueueSubmit2;
  PFN_vkCmdPipelineBarrier2KHR CmdPipelineBarrier2;
#endif
};

enum
{
  PROP_COMMAND_POOL = 1,
  N_PROPERTIES,
};

static GParamSpec *g_properties[N_PROPERTIES];

GST_DEBUG_CATEGORY_STATIC (GST_CAT_VULKAN_OPERATION);
#define GST_CAT_DEFAULT GST_CAT_VULKAN_OPERATION

#define GET_PRIV(operation) ((GstVulkanOperationPrivate *)              \
  gst_vulkan_operation_get_instance_private (GST_VULKAN_OPERATION (operation)))

#define gst_vulkan_operation_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstVulkanOperation, gst_vulkan_operation,
    GST_TYPE_OBJECT, G_ADD_PRIVATE (GstVulkanOperation)
    GST_DEBUG_CATEGORY_INIT (GST_CAT_VULKAN_OPERATION,
        "vulkanoperation", 0, "Vulkan Operation"));

static void
gst_vulkan_operation_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVulkanOperationPrivate *priv = GET_PRIV (object);

  switch (prop_id) {
    case PROP_COMMAND_POOL:
      g_assert (!priv->cmd_pool);
      priv->cmd_pool = g_value_dup_object (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vulkan_operation_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstVulkanOperationPrivate *priv = GET_PRIV (object);

  switch (prop_id) {
    case PROP_COMMAND_POOL:
      g_value_set_object (value, priv->cmd_pool);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vulkan_operation_constructed (GObject * object)
{
#if defined(VK_KHR_timeline_semaphore) || defined(VK_KHR_synchronization2)
  GstVulkanOperation *self = GST_VULKAN_OPERATION (object);
  GstVulkanOperationPrivate *priv = GET_PRIV (self);
  GstVulkanDevice *device = priv->cmd_pool->queue->device;

#if defined(VK_KHR_synchronization2)
  priv->has_sync2 = gst_vulkan_device_is_extension_enabled (device,
      VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);

  if (priv->has_sync2) {
    priv->QueueSubmit2 = gst_vulkan_instance_get_proc_address (device->instance,
        "vkQueueSubmit2");
    if (!priv->QueueSubmit2) {
      priv->QueueSubmit2 =
          gst_vulkan_instance_get_proc_address (device->instance,
          "vkQueueSubmit2KHR");
    }

    if (!priv->CmdPipelineBarrier2) {
      priv->CmdPipelineBarrier2 =
          gst_vulkan_instance_get_proc_address (device->instance,
          "vkCmdPipelineBarrier2");
      if (!priv->CmdPipelineBarrier2) {
        priv->CmdPipelineBarrier2 =
            gst_vulkan_instance_get_proc_address (device->instance,
            "vkCmdPipelineBarrier2KHR");
      }
    }

    priv->has_sync2 = (priv->QueueSubmit2 && priv->CmdPipelineBarrier2);
  }
#endif

#if GST_VULKAN_HAVE_VIDEO_EXTENSIONS
  priv->has_video = gst_vulkan_device_is_extension_enabled (device,
      VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
#endif

#if defined(VK_KHR_timeline_semaphore)
  priv->has_timeline = gst_vulkan_device_is_extension_enabled (device,
      VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
#endif
#endif

  G_OBJECT_CLASS (parent_class)->constructed (object);
}

static void
gst_vulkan_operation_finalize (GObject * object)
{
  GstVulkanOperationPrivate *priv = GET_PRIV (object);

  gst_vulkan_operation_reset (GST_VULKAN_OPERATION (object));

  g_clear_pointer (&priv->query_data, g_free);

  if (priv->query_pool) {
    vkDestroyQueryPool (priv->cmd_pool->queue->device->device, priv->query_pool,
        NULL);
    priv->query_pool = VK_NULL_HANDLE;
  }

  gst_clear_object (&priv->trash_list);
  gst_clear_object (&priv->cmd_pool);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_vulkan_operation_init (GstVulkanOperation * self)
{
  GstVulkanOperationPrivate *priv = GET_PRIV (self);

  priv->trash_list = gst_vulkan_trash_fence_list_new ();
}

static void
gst_vulkan_operation_class_init (GstVulkanOperationClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  gobject_class->set_property = gst_vulkan_operation_set_property;
  gobject_class->get_property = gst_vulkan_operation_get_property;
  gobject_class->constructed = gst_vulkan_operation_constructed;
  gobject_class->finalize = gst_vulkan_operation_finalize;

  g_properties[PROP_COMMAND_POOL] =
      g_param_spec_object ("command-pool", "GstVulkanCommandPool",
      "Vulkan Command Pool", GST_TYPE_VULKAN_COMMAND_POOL,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (gobject_class, N_PROPERTIES, g_properties);
}

static void
_dependency_frame_free (gpointer data)
{
  GstVulkanDependencyFrame *frame = data;
  guint i;

  for (i = 0; i < GST_VIDEO_MAX_PLANES && frame->mem[i] != NULL; i++) {
    gst_memory_unref (GST_MEMORY_CAST (frame->mem[i]));
    frame->mem[i] = NULL;
  }
  gst_clear_object (&frame->new_queue);
}

static GArray *
_get_dependency_frames (GstVulkanOperation * self)
{
  GstVulkanOperationPrivate *priv = GET_PRIV (self);

  if (!priv->deps.frames) {
    priv->deps.frames =
        g_array_new (FALSE, FALSE, sizeof (GstVulkanDependencyFrame));
    g_array_set_clear_func (priv->deps.frames, _dependency_frame_free);
  }

  return priv->deps.frames;
}

/**
 * gst_vulkan_operation_reset:
 * @self: a #GstVulkanOperation
 *
 * Resets the operation to a clean state.
 *
 * Since: 1.24
 */
void
gst_vulkan_operation_reset (GstVulkanOperation * self)
{
  GstVulkanOperationPrivate *priv;

  g_return_if_fail (GST_IS_VULKAN_OPERATION (self));

  priv = GET_PRIV (self);

  g_clear_pointer (&priv->barriers, g_array_unref);

  gst_vulkan_trash_list_gc (priv->trash_list);
  gst_vulkan_operation_discard_dependencies (self);
  gst_clear_vulkan_command_buffer (&self->cmd_buf);
}

/**
 * gst_vulkan_operation_begin:
 * @self: a #GstVulkanOperation
 * @error: a #GError
 *
 * Attempts to set the operation ready to work. It instantiates the common
 * command buffer in @self.
 *
 * Returns: whether the operation started. It might fill @error.
 *
 * Since: 1.24
 */
gboolean
gst_vulkan_operation_begin (GstVulkanOperation * self, GError ** error)
{
  GstVulkanOperationPrivate *priv;
  VkCommandBufferBeginInfo cmd_buf_info = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  VkResult err;

  g_return_val_if_fail (GST_IS_VULKAN_OPERATION (self), FALSE);

  priv = GET_PRIV (self);

  if (self->cmd_buf) {
    if (!gst_vulkan_operation_wait (self))
      GST_WARNING_OBJECT (self, "previous operation timed-out");
  }

  if (!(self->cmd_buf = gst_vulkan_command_pool_create (priv->cmd_pool, error)))
    return FALSE;

  gst_vulkan_command_buffer_lock (self->cmd_buf);
  err = vkBeginCommandBuffer (self->cmd_buf->cmd, &cmd_buf_info);
  if (gst_vulkan_error_to_g_error (err, error, "vkBeginCommandBuffer") < 0)
    goto error;

  if (priv->query_pool)
    vkCmdResetQueryPool (self->cmd_buf->cmd, priv->query_pool, 0, 1);

  return TRUE;

error:
  {
    if (self->cmd_buf)
      gst_vulkan_command_buffer_unlock (self->cmd_buf);
    gst_clear_vulkan_command_buffer (&self->cmd_buf);
    return FALSE;
  }
}

static gboolean
gst_vulkan_operation_submit2 (GstVulkanOperation * self, GstVulkanFence * fence,
    GError ** error)
{
#if defined(VK_KHR_synchronization2)
  GstVulkanOperationPrivate *priv = GET_PRIV (self);
  VkCommandBufferSubmitInfoKHR cmd_buf_info = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO_KHR,
    .commandBuffer = self->cmd_buf->cmd,
  };
  VkSubmitInfo2KHR submit_info;
  VkResult err;

  GST_OBJECT_LOCK (self);
  /* *INDENT-OFF* */
  submit_info = (VkSubmitInfo2KHR) {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2_KHR,
    .pCommandBufferInfos = &cmd_buf_info,
    .commandBufferInfoCount = 1,
    .pWaitSemaphoreInfos = priv->deps.wait_semaphores ?
        (const VkSemaphoreSubmitInfoKHR *) priv->deps.wait_semaphores->data : NULL,
    .waitSemaphoreInfoCount = priv->deps.wait_semaphores ?
        priv->deps.wait_semaphores->len : 0,
    .pSignalSemaphoreInfos = priv->deps.signal_semaphores ?
        (const VkSemaphoreSubmitInfoKHR *) priv->deps.signal_semaphores->data : NULL,
    .signalSemaphoreInfoCount = priv->deps.signal_semaphores ?
        priv->deps.signal_semaphores->len : 0,
  };
  /* *INDENT-ON* */

  gst_vulkan_queue_submit_lock (priv->cmd_pool->queue);
  err = priv->QueueSubmit2 (priv->cmd_pool->queue->queue, 1, &submit_info,
      GST_VULKAN_FENCE_FENCE (fence));
  gst_vulkan_queue_submit_unlock (priv->cmd_pool->queue);
  GST_OBJECT_UNLOCK (self);
  if (gst_vulkan_error_to_g_error (err, error, "vkQueueSubmit2KHR") < 0)
    goto error;

  return TRUE;

error:
#endif
  return FALSE;
}

static gboolean
gst_vulkan_operation_submit1 (GstVulkanOperation * self, GstVulkanFence * fence,
    GError ** error)
{
  GstVulkanOperationPrivate *priv = GET_PRIV (self);
#if defined(VK_KHR_timeline_semaphore)
  VkTimelineSemaphoreSubmitInfoKHR semaphore_submit_info;
#endif
  gpointer pnext = NULL;
  VkSubmitInfo submit_info;
  VkResult err;

  GST_OBJECT_LOCK (self);
  /* *INDENT-OFF* */
#if defined(VK_KHR_timeline_semaphore)
  if (priv->has_timeline) {
    semaphore_submit_info = (VkTimelineSemaphoreSubmitInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR,
      .waitSemaphoreValueCount = priv->deps.wait_semaphore_values ?
         priv->deps.wait_semaphore_values->len : 0,
      .pWaitSemaphoreValues = priv->deps.wait_semaphore_values ?
          (const guint64 *) priv->deps.wait_semaphore_values->data : NULL,
      .signalSemaphoreValueCount = priv->deps.signal_semaphore_values ?
          priv->deps.signal_semaphore_values->len : 0,
      .pSignalSemaphoreValues = priv->deps.signal_semaphore_values ?
          (const guint64 *) priv->deps.signal_semaphore_values->data : NULL,
    };

    pnext = &semaphore_submit_info;
  }
#endif
  submit_info = (VkSubmitInfo) {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .pNext = pnext,
    .commandBufferCount = 1,
    .pCommandBuffers = &self->cmd_buf->cmd,
    .pWaitSemaphores = priv->deps.wait_semaphores ?
        (const VkSemaphore *) priv->deps.wait_semaphores->data : NULL,
    .waitSemaphoreCount = priv->deps.wait_semaphores ?
        priv->deps.wait_semaphores->len : 0,
    .pSignalSemaphores = priv->deps.signal_semaphores ?
        (const VkSemaphore *) priv->deps.signal_semaphores->data : NULL,
    .signalSemaphoreCount = priv->deps.signal_semaphores ?
        priv->deps.signal_semaphores->len : 0,
    .pWaitDstStageMask = priv->deps.wait_dst_stage_mask ?
        (const VkPipelineStageFlags *)
        priv->deps.wait_dst_stage_mask->data : NULL,
  };
  /* *INDENT-ON* */

  gst_vulkan_queue_submit_lock (priv->cmd_pool->queue);
  err = vkQueueSubmit (priv->cmd_pool->queue->queue, 1, &submit_info,
      GST_VULKAN_FENCE_FENCE (fence));
  gst_vulkan_queue_submit_unlock (priv->cmd_pool->queue);
  GST_OBJECT_UNLOCK (self);
  if (gst_vulkan_error_to_g_error (err, error, "vkQueueSubmit") < 0)
    return FALSE;

  return TRUE;
}

/**
 * gst_vulkan_operation_end:
 * @self: a #GstVulkanOperation
 * @error: a #GError
 *
 * It calls either vkQueueSubmit or vkQueueSubmit2 filling up the semaphores
 * from images declared as dependencies.
 *
 * Returns: whether the operation failed. It might fill @error.
 *
 * Since: 1.24
 */
gboolean
gst_vulkan_operation_end (GstVulkanOperation * self, GError ** error)
{
  GstVulkanOperationPrivate *priv;
  GstVulkanFence *fence;
  GstVulkanDevice *device;
  VkResult err;
  guint i, j;

  g_return_val_if_fail (GST_IS_VULKAN_OPERATION (self), FALSE);

  if (!self->cmd_buf)
    return FALSE;

  priv = GET_PRIV (self);

  device = priv->cmd_pool->queue->device;
  fence = gst_vulkan_device_create_fence (device, error);
  if (!fence)
    return FALSE;

  err = vkEndCommandBuffer (self->cmd_buf->cmd);
  gst_vulkan_command_buffer_unlock (self->cmd_buf);
  if (gst_vulkan_error_to_g_error (err, error, "vkEndCommandBuffer") < 0)
    return FALSE;

  if (priv->has_sync2) {
    if (!gst_vulkan_operation_submit2 (self, fence, error))
      return FALSE;
  } else if (!gst_vulkan_operation_submit1 (self, fence, error)) {
    return FALSE;
  }

  gst_vulkan_trash_list_add (priv->trash_list,
      gst_vulkan_trash_list_acquire (priv->trash_list, fence,
          gst_vulkan_trash_mini_object_unref,
          GST_MINI_OBJECT_CAST (self->cmd_buf)));

  gst_vulkan_fence_unref (fence);

  GST_OBJECT_LOCK (self);
  for (i = 0; priv->deps.frames && i < priv->deps.frames->len; i++) {
    GstVulkanDependencyFrame *frame =
        &g_array_index (priv->deps.frames, GstVulkanDependencyFrame, i);

    for (j = 0; j < GST_VIDEO_MAX_PLANES && frame->mem[j] != NULL; j++) {
      if (frame->updated) {
        frame->mem[j]->barrier.parent.pipeline_stages = frame->dst_stage;
        frame->mem[j]->barrier.parent.access_flags = frame->new_access;
        frame->mem[j]->barrier.parent.queue = frame->new_queue;
        frame->mem[j]->barrier.image_layout = frame->new_layout;
      }

      if (frame->semaphored)
        frame->mem[j]->barrier.parent.semaphore_value++;
    }

    frame->updated = FALSE;
    frame->semaphored = FALSE;
  }
  GST_OBJECT_UNLOCK (self);

  g_clear_pointer (&priv->barriers, g_array_unref);

  return TRUE;
}

/**
 * gst_vulkan_operation_wait:
 * @self: a #GstVulkanOperation
 *
 * Waits for the operation to end and cleans up the state.
 *
 * Returns: whether the operation succeed.
 *
 * Since: 1.24
 */
gboolean
gst_vulkan_operation_wait (GstVulkanOperation * self)
{
  GstVulkanOperationPrivate *priv;
  gboolean ret;

  g_return_val_if_fail (GST_IS_VULKAN_OPERATION (self), FALSE);

  priv = GET_PRIV (self);
  ret = gst_vulkan_trash_list_wait (priv->trash_list, G_MAXUINT64);
  gst_vulkan_operation_discard_dependencies (self);
  self->cmd_buf = NULL;

  return ret;
}

static gboolean
_dep_is_frame (GstVulkanDependencyFrame * dep, GstBuffer * buffer)
{
  guint i, n_mems;
  GstMemory *mem;

  n_mems = gst_buffer_n_memory (buffer);
  g_assert (n_mems <= GST_VIDEO_MAX_PLANES);

  for (i = 0; i < n_mems; i++) {
    mem = gst_buffer_peek_memory (buffer, i);
    if (!(mem && gst_is_vulkan_image_memory (mem)))
      return FALSE;

    if (dep->mem[i] != (GstVulkanImageMemory *) mem)
      return FALSE;
  }

  return TRUE;
}

static void
_dep_set_frame (GstVulkanDependencyFrame * dep, GstBuffer * buffer)
{
  guint i, n_mems;
  GstMemory *mem;

  n_mems = gst_buffer_n_memory (buffer);
  g_assert (n_mems <= GST_VIDEO_MAX_PLANES);

  for (i = 0; i < n_mems; i++) {
    mem = gst_buffer_peek_memory (buffer, i);
    if (!(mem && gst_is_vulkan_image_memory (mem)))
      return;

    dep->mem[i] = (GstVulkanImageMemory *) gst_memory_ref (mem);
  }

  for (; i < GST_VIDEO_MAX_PLANES; i++)
    dep->mem[i] = NULL;
}

/**
 * gst_vulkan_operation_update_frame:
 * @self: a #GstVulkanOperation
 * @frame: a #GstBuffer to update after submit
 * @dst_stage: the Vulkan destination stage
 * @new_access: the new access flags
 * @new_layout: the new vulkan image layout
 * @new_queue: the #GstVulkanQueue to transfer the @frame
 *
 * Add or update the internal list of the future state of @frame. This state
 * will be set after gst_vulkan_operation_end().
 *
 * This method is useful when new barriers are added to the array without using
 * gst_vulkan_operation_add_frame_barrier().
 *
 * Since: 1.24
 */
void
gst_vulkan_operation_update_frame (GstVulkanOperation * self, GstBuffer * frame,
    guint64 dst_stage, guint64 new_access, VkImageLayout new_layout,
    GstVulkanQueue * new_queue)
{
  GArray *frames;
  guint i;
  GstVulkanDependencyFrame *dep_frame = NULL;

  g_return_if_fail (GST_IS_VULKAN_OPERATION (self));

  GST_OBJECT_LOCK (self);

  frames = _get_dependency_frames (self);
  for (i = 0; i < frames->len; i++) {
    dep_frame = &g_array_index (frames, GstVulkanDependencyFrame, i);

    if (_dep_is_frame (dep_frame, frame))
      break;
  }

  if (i >= frames->len) {
    GstVulkanDependencyFrame dframe;

    _dep_set_frame (&dframe, frame);
    g_array_append_val (frames, dframe);
    dep_frame =
        &g_array_index (frames, GstVulkanDependencyFrame, frames->len - 1);
  }

  dep_frame->updated = TRUE;
  dep_frame->dst_stage = dst_stage;
  dep_frame->new_access = new_access;
  dep_frame->new_layout = new_layout;
  dep_frame->new_queue = new_queue ? gst_object_ref (new_queue) : NULL;

  GST_OBJECT_UNLOCK (self);
}

static GArray *
_get_barriers (GstVulkanOperation * self)
{
  GstVulkanOperationPrivate *priv;
  priv = GET_PRIV (self);

  if (priv->barriers)
    return priv->barriers;

#if defined(VK_KHR_synchronization2)
  if (priv->has_sync2) {
    priv->barriers = g_array_sized_new (FALSE, FALSE,
        sizeof (VkImageMemoryBarrier2KHR), GST_VIDEO_MAX_PLANES);
  } else
#endif
  {
    priv->barriers = g_array_sized_new (FALSE, FALSE,
        sizeof (VkImageMemoryBarrier), GST_VIDEO_MAX_PLANES);
  }

  return priv->barriers;
}

/**
 * gst_vulkan_operation_retrieve_barriers:
 * @self: a #GstVulkanOperation
 *
 * Retrieves the current defined barriers, which will be used by
 * vkCmdPipelineBarrier or vkCmdPipelineBarrier2KHR.
 *
 * The element type of the array might be, depending on if synchronization2
 * extension is used, either VkImageMemoryBarrier or VkImageMemoryBarrier2KHR.
 *
 * If you add custom frame barriers, remember to call
 * gst_vulkan_operation_update_frame() after adding it.
 *
 * Returns: (transfer full): Current barriers array. Call g_array_unref() after
 *    the operation is using.
 *
 * Since: 1.24
 */
GArray *
gst_vulkan_operation_retrieve_barriers (GstVulkanOperation * self)
{
  return g_array_ref (_get_barriers (self));
}

/**
 * gst_vulkan_operation_add_frame_barrier:
 * @self: a #GstVulkanOperation
 * @frame: a Vulkan Image #GstBuffer
 * @dst_stage: the Vulkan destination stage
 * @new_access: the new access flags
 * @new_layout: the new layout
 * @new_queue: the #GstVulkanQueue to transfer the @frame
 *
 * Adds an image memory barrier per memory in @frame with its future state. And
 * updates the @frame barrier state.
 *
 * Returns: whether the @frame barriers were appended
 *
 * Since: 1.24
 */
gboolean
gst_vulkan_operation_add_frame_barrier (GstVulkanOperation * self,
    GstBuffer * frame, guint64 dst_stage, guint64 new_access,
    VkImageLayout new_layout, GstVulkanQueue * new_queue)
{
  guint i, n_mems;
#if defined(VK_KHR_synchronization2)
  GstVulkanOperationPrivate *priv;
#endif
  GstVulkanDependencyFrame *dep_frame = NULL;
  GArray *frames, *barriers;

  g_return_val_if_fail (GST_IS_VULKAN_OPERATION (self), FALSE);
  g_return_val_if_fail (GST_IS_BUFFER (frame), FALSE);

#if defined(VK_KHR_synchronization2)
  priv = GET_PRIV (self);
#endif
  n_mems = gst_buffer_n_memory (frame);

  barriers = _get_barriers (self);

  GST_OBJECT_LOCK (self);

  frames = _get_dependency_frames (self);
  for (i = 0; i < frames->len; i++) {
    dep_frame = &g_array_index (frames, GstVulkanDependencyFrame, i);
    if (_dep_is_frame (dep_frame, frame))
      break;
  }

  if (i >= frames->len || !(dep_frame && dep_frame->updated))
    dep_frame = NULL;

  for (i = 0; i < n_mems; i++) {
    guint32 queue_familiy_index = VK_QUEUE_FAMILY_IGNORED;
    GstVulkanImageMemory *vkmem;
    GstMemory *mem = gst_buffer_peek_memory (frame, i);

    if (!gst_is_vulkan_image_memory (mem))
      return FALSE;

    vkmem = (GstVulkanImageMemory *) mem;

    if (dep_frame && dep_frame->new_queue)
      queue_familiy_index = dep_frame->new_queue->family;
    else if (vkmem->barrier.parent.queue)
      queue_familiy_index = vkmem->barrier.parent.queue->family;

    /* *INDENT-OFF* */
#if defined(VK_KHR_synchronization2)
    if (priv->has_sync2) {
      VkImageMemoryBarrier2KHR barrier2  = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2_KHR,
        .pNext = NULL,
        .srcStageMask = dep_frame ?
            dep_frame->dst_stage : vkmem->barrier.parent.pipeline_stages,
        .dstStageMask = dst_stage,
        .srcAccessMask = dep_frame ?
            dep_frame->new_access : vkmem->barrier.parent.access_flags,
        .dstAccessMask = new_access,
        .oldLayout = dep_frame ?
            dep_frame->new_layout : vkmem->barrier.image_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = queue_familiy_index,
        .dstQueueFamilyIndex = new_queue ?
            new_queue->family : VK_QUEUE_FAMILY_IGNORED,
        .image = vkmem->image,
        .subresourceRange = vkmem->barrier.subresource_range,
      };

      g_array_append_val (barriers, barrier2);
    } else
#endif
    {
      VkImageMemoryBarrier barrier;

      /* this might overflow */
      if (new_access > VK_ACCESS_FLAG_BITS_MAX_ENUM) {
        GST_OBJECT_UNLOCK (self);
        GST_ERROR_OBJECT (self, "Invalid new access value: %" G_GUINT64_FORMAT,
            new_access);
        return FALSE;
      }

      barrier = (VkImageMemoryBarrier) {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = NULL,
        .srcAccessMask = vkmem->barrier.parent.access_flags,
        .dstAccessMask = (VkAccessFlags) new_access,
        .oldLayout = vkmem->barrier.image_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = queue_familiy_index,
        .dstQueueFamilyIndex = new_queue ?
            new_queue->family : VK_QUEUE_FAMILY_IGNORED,
        .image = vkmem->image,
        .subresourceRange = vkmem->barrier.subresource_range,
      };

      g_array_append_val (barriers, barrier);
    }
    /* *INDENT-ON* */
  }

  GST_OBJECT_UNLOCK (self);
  gst_vulkan_operation_update_frame (self, frame, dst_stage, new_access,
      new_layout, new_queue);

  return TRUE;
}

/**
 * gst_vulkan_operation_add_dependecy_frame:
 * @self: a #GstVulkanOperation
 * @frame: a Vulkan Image #GstBuffer
 * @wait_stage: the Vulkan stage to wait
 * @signal_stage: the Vulkan stage to signal
 *
 * Add @frame as an operation dependency by adding the timeline semaphores in
 * each memory of @frame into either the wait semaphore array. The signal array
 * hold the same semaphores but increasing their current value.
 *
 * Returns: whether the @frame was added as dependency.
 *
 * Since: 1.24
 */
gboolean
gst_vulkan_operation_add_dependecy_frame (GstVulkanOperation * self,
    GstBuffer * frame, guint64 wait_stage, guint64 signal_stage)
{
  GstVulkanOperationPrivate *priv;
  guint i, n_mems;
  GArray *frames;
  GstVulkanDependencyFrame *dep_frame = NULL;

  g_return_val_if_fail (GST_IS_VULKAN_OPERATION (self), FALSE);
  g_return_val_if_fail (GST_IS_BUFFER (frame), FALSE);

#if defined(VK_KHR_timeline_semaphore)
  priv = GET_PRIV (self);

  GST_OBJECT_LOCK (self);
  frames = _get_dependency_frames (self);

  for (i = 0; i < frames->len; i++) {
    dep_frame = &g_array_index (frames, GstVulkanDependencyFrame, i);
    if (_dep_is_frame (dep_frame, frame) && dep_frame->semaphored) {
      GST_OBJECT_UNLOCK (self);
      return TRUE;
    }
  }

  if (i >= frames->len) {
    GstVulkanDependencyFrame dframe = {
      .semaphored = TRUE,
    };
    _dep_set_frame (&dframe, frame);
    g_array_append_val (frames, dframe);
  } else if (dep_frame) {
    dep_frame->semaphored = TRUE;
  }
#if defined(VK_KHR_synchronization2)
  if (priv->has_sync2 && priv->has_timeline) {
    if (!priv->deps.signal_semaphores) {
      priv->deps.signal_semaphores =
          g_array_new (FALSE, FALSE, sizeof (VkSemaphoreSubmitInfoKHR));
    }

    if (!priv->deps.wait_semaphores) {
      priv->deps.wait_semaphores =
          g_array_new (FALSE, FALSE, sizeof (VkSemaphoreSubmitInfoKHR));
    }

    n_mems = gst_buffer_n_memory (frame);
    for (i = 0; i < n_mems; i++) {
      GstVulkanImageMemory *vkmem;
      GstMemory *mem = gst_buffer_peek_memory (frame, i);

      if (!gst_is_vulkan_image_memory (mem)) {
        GST_OBJECT_UNLOCK (self);
        return FALSE;
      }

      vkmem = (GstVulkanImageMemory *) mem;

      if (vkmem->barrier.parent.semaphore == VK_NULL_HANDLE)
        break;

      /* *INDENT-OFF* */
      g_array_append_vals (priv->deps.wait_semaphores, &(VkSemaphoreSubmitInfoKHR) {
          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR,
          .semaphore = vkmem->barrier.parent.semaphore,
          .value = vkmem->barrier.parent.semaphore_value,
          .stageMask = wait_stage,
        }, 1);
      g_array_append_vals (priv->deps.signal_semaphores, &(VkSemaphoreSubmitInfoKHR) {
          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO_KHR,
          .semaphore = vkmem->barrier.parent.semaphore,
          .value = vkmem->barrier.parent.semaphore_value + 1,
          .stageMask = signal_stage,
        }, 1);
      /* *INDENT-ON* */
    }

    GST_OBJECT_UNLOCK (self);
    return TRUE;
  }
#else
  if (priv->has_timeline && wait_stage <= G_MAXUINT32) {
    if (!priv->deps.signal_semaphores) {
      priv->deps.signal_semaphores =
          g_array_new (FALSE, FALSE, sizeof (VkSemaphore));
    }

    if (!priv->deps.wait_semaphores) {
      priv->deps.wait_semaphores =
          g_array_new (FALSE, FALSE, sizeof (VkSemaphore));
    }

    if (!priv->deps.wait_dst_stage_mask) {
      priv->deps.wait_dst_stage_mask =
          g_array_new (FALSE, FALSE, sizeof (VkPipelineStageFlags));
    }

    if (!priv->deps.wait_semaphore_values) {
      priv->deps.wait_semaphore_values =
          g_array_new (FALSE, FALSE, sizeof (guint64));
    }
    if (!priv->deps.signal_semaphore_values) {
      priv->deps.signal_semaphore_values =
          g_array_new (FALSE, FALSE, sizeof (guint64));
    }

    n_mems = gst_buffer_n_memory (frame);
    for (i = 0; i < n_mems; i++) {
      GstVulkanImageMemory *vkmem;
      GstMemory *mem = gst_buffer_peek_memory (frame, i);
      VkPipelineStageFlags wait_stage1 = (VkPipelineStageFlags) wait_stage;
      guint64 signal_value;

      if (!gst_is_vulkan_image_memory (mem)) {
        GST_OBJECT_UNLOCK (self);
        return FALSE;
      }

      vkmem = (GstVulkanImageMemory *) mem;

      if (vkmem->barrier.parent.semaphore == VK_NULL_HANDLE)
        break;

      g_array_append_val (priv->deps.wait_semaphores,
          vkmem->barrier.parent.semaphore);
      g_array_append_val (priv->deps.signal_semaphores,
          vkmem->barrier.parent.semaphore);
      g_array_append_val (priv->deps.wait_semaphore_values,
          vkmem->barrier.parent.semaphore_value);
      signal_value = vkmem->barrier.parent.semaphore_value + 1;
      g_array_append_val (priv->deps.signal_semaphore_values, signal_value);
      g_array_append_val (priv->deps.wait_dst_stage_mask, wait_stage1);
    }

    GST_OBJECT_UNLOCK (self);
    return TRUE;
  }
#endif /* synchronization2 */
#endif /* timeline semaphore */

  GST_INFO_OBJECT (self, "No dependencies added because of missing extensions");
  return FALSE;
}

/**
 * gst_vulkan_operation_discard_dependencies:
 * @self: a #GstVulkanOperation
 *
 * Discards all the semaphore arrays populated by added frames as dependencies.
 */
void
gst_vulkan_operation_discard_dependencies (GstVulkanOperation * self)
{
  GstVulkanOperationPrivate *priv;

  g_return_if_fail (GST_IS_VULKAN_OPERATION (self));

  priv = GET_PRIV (self);

  GST_OBJECT_LOCK (self);

  g_clear_pointer (&priv->deps.frames, g_array_unref);
  g_clear_pointer (&priv->deps.signal_semaphores, g_array_unref);
  g_clear_pointer (&priv->deps.wait_semaphores, g_array_unref);

  g_clear_pointer (&priv->deps.wait_dst_stage_mask, g_array_unref);
  g_clear_pointer (&priv->deps.wait_semaphore_values, g_array_unref);
  g_clear_pointer (&priv->deps.signal_semaphore_values, g_array_unref);

  GST_OBJECT_UNLOCK (self);
}

/**
 * gst_vulkan_operation_enable_query:
 * @self: a #GstVulkanOperation
 * @query_type: (type guint32): the query type to enable
 * @n_queries: number of queries to enable
 * @pnext: the structure pointer to use as pNext
 * @error: a #GError
 *
 * Tries to enable the query pool for the current operation.
 *
 * Returns: whether the query pool was enabled. It might populate @error in case
 *    of error.
 *
 * Since: 1.24
 */
gboolean
gst_vulkan_operation_enable_query (GstVulkanOperation * self,
    VkQueryType query_type, guint n_queries, gpointer pnext, GError ** error)
{
  GstVulkanOperationPrivate *priv;
  GstVulkanPhysicalDevice *device;
  guint32 queue_family;
  VkQueryPoolCreateInfo query_pool_info = {
    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
    .pNext = pnext,
    .queryType = query_type,
    .queryCount = n_queries,
  };
  VkResult res;
  size_t stride = 0;

  g_return_val_if_fail (GST_IS_VULKAN_OPERATION (self), FALSE);
  g_return_val_if_fail (n_queries > 0, FALSE);

  priv = GET_PRIV (self);

  if (priv->query_pool)
    return TRUE;

  queue_family = priv->cmd_pool->queue->family;
  device = priv->cmd_pool->queue->device->physical_device;
  if (!device->queue_family_ops[queue_family].query) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_FEATURE_NOT_PRESENT,
        "Queue %" GST_PTR_FORMAT " doesn't support query operations",
        priv->cmd_pool->queue);
    return FALSE;
  }

  res = vkCreateQueryPool (priv->cmd_pool->queue->device->device,
      &query_pool_info, NULL, &priv->query_pool);
  if (gst_vulkan_error_to_g_error (res, error,
          "vkCreateQueryPool") != VK_SUCCESS)
    return FALSE;

  priv->query_type = query_type;
  priv->n_queries = n_queries;

  /* @TODO:
   * + support 64bit
   * + result support other structures besides a guint32 array
   */
  switch (query_type) {
#if GST_VULKAN_HAVE_VIDEO_EXTENSIONS
    case VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR:
      if (priv->has_video)
        stride = sizeof (guint32);
      break;
#if VK_ENABLE_BETA_EXTENSIONS
    case VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR:
      if (priv->has_video)
        stride = sizeof (GstVulkanEncodeQueryResult);
     break;
#endif
#endif

    default:
      break;
  }

  priv->query_data_size = n_queries * stride;
  priv->query_data_stride = stride;
  priv->query_data = g_malloc0 (priv->query_data_size);

  return TRUE;
}

/**
 * gst_vulkan_operation_get_query:
 * @self: a #GstVulkanOperation
 * @data: (out callee-allocates) (transfer none): result of all queries
 * @error: a #GError
 *
 * Gets the latest operation results of all the queries in @data. API users have
 * to parse the binary array of @data according of their needs (usually is a
 * guint32 array of size of n_query).
 *
 * Don't free @data.
 *
 * Returns: whether a status was fetched. If not, it might populate @error
 *
 * Since: 1.24
 */
gboolean
gst_vulkan_operation_get_query (GstVulkanOperation * self, gpointer * result,
    GError ** error)
{
  GstVulkanOperationPrivate *priv;
  VkResult res;
  VkQueryResultFlagBits flags = 0;

  g_return_val_if_fail (GST_IS_VULKAN_OPERATION (self), FALSE);

  priv = GET_PRIV (self);
  if (!priv->query_pool || !priv->query_data)
    return TRUE;

#if GST_VULKAN_HAVE_VIDEO_EXTENSIONS
  if (priv->has_video
      && (priv->query_type == VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR
#if VK_ENABLE_BETA_EXTENSIONS
          || priv->query_type == VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR
#endif
      )) {
    flags |= VK_QUERY_RESULT_WITH_STATUS_BIT_KHR;
  }
#endif

  /* grab the results of all the queries */
  res = vkGetQueryPoolResults (priv->cmd_pool->queue->device->device,
      priv->query_pool, 0, priv->n_queries, priv->query_data_size,
      priv->query_data, priv->query_data_stride, flags);
  if (res != VK_SUCCESS && res != VK_NOT_READY) {
    gst_vulkan_error_to_g_error (res, error, "vkGetQueryPoolResults");
    return FALSE;
  }

  if (result)
    *result = priv->query_data;

  return TRUE;
}

/**
 * gst_vulkan_operation_begin_query:
 * @self: a #GstVulkanOperation
 *
 * Begins a query operation in the current command buffer.
 *
 * Returns: whether the begin command was set
 */
gboolean
gst_vulkan_operation_begin_query (GstVulkanOperation * self, guint32 id)
{
  GstVulkanOperationPrivate *priv;

  g_return_val_if_fail (GST_IS_VULKAN_OPERATION (self), FALSE);

  priv = GET_PRIV (self);
  if (!priv->query_pool)
    return TRUE;

  if (!self->cmd_buf)
    return FALSE;

  vkCmdBeginQuery (self->cmd_buf->cmd, priv->query_pool, id, 0);

  return TRUE;
}

/**
 * gst_vulkan_operation_end_query:
 * @self: a #GstVulkanOperation
 *
 * Ends a query operation in the current command buffer.
 *
 * Returns: whether the end command was set
 *
 * Since: 1.24
 */
gboolean
gst_vulkan_operation_end_query (GstVulkanOperation * self, guint32 id)
{
  GstVulkanOperationPrivate *priv;

  g_return_val_if_fail (GST_IS_VULKAN_OPERATION (self), FALSE);

  priv = GET_PRIV (self);
  if (!priv->query_pool)
    return TRUE;

  if (!self->cmd_buf)
    return FALSE;

  vkCmdEndQuery (self->cmd_buf->cmd, priv->query_pool, id);

  return TRUE;
}

/**
 * gst_vulkan_operation_use_sync2:
 * @self: a #GstVulkanOperation
 *
 * Returns: whether the operations are using synchronization2 extension.
 *
 * Since: 1.24
 */
gboolean
gst_vulkan_operation_use_sync2 (GstVulkanOperation * self)
{
  g_return_val_if_fail (GST_IS_VULKAN_OPERATION (self), FALSE);

  return GET_PRIV (self)->has_sync2;
}

/**
 * gst_vulkan_operation_pipeline_barrier2:
 * @self: a #GstVulkanOperation
 * @dependency_info: a pointer to VkDependencyInfo
 *
 * It's a wrapper to vkCmdPipelineBarrier2{KHR} if it's available.
 *
 * Returns: %TRUE if vkCmdPipelineBarrier2{KHR} it's available. %FALSE,
 *   otherwise.
 *
 * Since: 1.24
 */
gboolean
gst_vulkan_operation_pipeline_barrier2 (GstVulkanOperation * self,
    gpointer dependency_info)
{
#if defined(VK_KHR_synchronization2)
  GstVulkanOperationPrivate *priv;

  g_return_val_if_fail (GST_IS_VULKAN_OPERATION (self), FALSE);

  priv = GET_PRIV (self);

  if (priv->has_sync2) {
    VkDependencyInfoKHR *info = dependency_info;

    g_return_val_if_fail (info
        && info->sType == VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR, FALSE);

    priv->CmdPipelineBarrier2 (self->cmd_buf->cmd, info);
    return TRUE;
  }
#endif
  return FALSE;
}


/**
 * gst_vulkan_operation_new:
 * @cmd_pool: a #GstVulkanCommandPool
 *
 * Returns: (transfer full): a newly allocated #GstVulkanOperation
 *
 * Since: 1.24
 */
GstVulkanOperation *
gst_vulkan_operation_new (GstVulkanCommandPool * cmd_pool)
{
  g_return_val_if_fail (GST_IS_VULKAN_COMMAND_POOL (cmd_pool), NULL);

  return g_object_new (GST_TYPE_VULKAN_OPERATION, "command-pool", cmd_pool,
      NULL);
}
