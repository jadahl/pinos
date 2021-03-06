/* Spa V4l2 Source
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <linux/videodev2.h>

#include <spa/node.h>
#include <spa/video/format-utils.h>
#include <spa/clock.h>
#include <spa/list.h>
#include <spa/log.h>
#include <spa/loop.h>
#include <spa/type-map.h>
#include <spa/format-builder.h>
#include <lib/debug.h>
#include <lib/props.h>

typedef struct _SpaV4l2Source SpaV4l2Source;

static const char default_device[] = "/dev/video0";

typedef struct {
  char device[64];
  char device_name[128];
  int  device_fd;
} SpaV4l2SourceProps;

static void
reset_v4l2_source_props (SpaV4l2SourceProps *props)
{
  strncpy (props->device, default_device, 64);
}

#define MAX_BUFFERS     64

typedef struct _V4l2Buffer V4l2Buffer;

struct _V4l2Buffer {
  SpaBuffer *outbuf;
  SpaMetaHeader *h;
  bool outstanding;
  bool allocated;
  struct v4l2_buffer v4l2_buffer;
};

typedef struct {
  uint32_t node;
  uint32_t clock;
  uint32_t format;
  uint32_t props;
  uint32_t prop_device;
  uint32_t prop_device_name;
  uint32_t prop_device_fd;
  SpaTypeMediaType media_type;
  SpaTypeMediaSubtype media_subtype;
  SpaTypeMediaSubtypeVideo media_subtype_video;
  SpaTypeFormatVideo format_video;
  SpaTypeVideoFormat video_format;
  SpaTypeEventNode event_node;
  SpaTypeCommandNode command_node;
  SpaTypeAllocParamBuffers alloc_param_buffers;
  SpaTypeAllocParamMetaEnable alloc_param_meta_enable;
  SpaTypeMeta meta;
  SpaTypeData data;
} Type;

static inline void
init_type (Type *type, SpaTypeMap *map)
{
  type->node = spa_type_map_get_id (map, SPA_TYPE__Node);
  type->clock = spa_type_map_get_id (map, SPA_TYPE__Clock);
  type->format = spa_type_map_get_id (map, SPA_TYPE__Format);
  type->props = spa_type_map_get_id (map, SPA_TYPE__Props);
  type->prop_device = spa_type_map_get_id (map, SPA_TYPE_PROPS__device);
  type->prop_device_name = spa_type_map_get_id (map, SPA_TYPE_PROPS__deviceName);
  type->prop_device_fd = spa_type_map_get_id (map, SPA_TYPE_PROPS__deviceFd);
  spa_type_media_type_map (map, &type->media_type);
  spa_type_media_subtype_map (map, &type->media_subtype);
  spa_type_media_subtype_video_map (map, &type->media_subtype_video);
  spa_type_format_video_map (map, &type->format_video);
  spa_type_video_format_map (map, &type->video_format);
  spa_type_event_node_map (map, &type->event_node);
  spa_type_command_node_map (map, &type->command_node);
  spa_type_alloc_param_buffers_map (map, &type->alloc_param_buffers);
  spa_type_alloc_param_meta_enable_map (map, &type->alloc_param_meta_enable);
  spa_type_meta_map (map, &type->meta);
  spa_type_data_map (map, &type->data);
}

typedef struct {
  SpaLog *log;
  SpaLoop *main_loop;
  SpaLoop *data_loop;

  bool export_buf;
  bool started;

  bool next_fmtdesc;
  struct v4l2_fmtdesc fmtdesc;
  bool next_frmsize;
  struct v4l2_frmsizeenum frmsize;
  struct v4l2_frmivalenum frmival;

  bool have_format;
  SpaVideoInfo current_format;
  uint8_t format_buffer[1024];

  int fd;
  bool opened;
  struct v4l2_capability cap;
  struct v4l2_format fmt;
  enum v4l2_buf_type type;
  enum v4l2_memory memtype;

  V4l2Buffer   buffers[MAX_BUFFERS];
  uint32_t     n_buffers;

  bool source_enabled;
  SpaSource source;

  SpaPortInfo info;
  SpaAllocParam *params[2];
  uint8_t params_buffer[1024];
  SpaPortIO *io;

  int64_t last_ticks;
  int64_t last_monotonic;
} SpaV4l2State;

struct _SpaV4l2Source {
  SpaHandle handle;
  SpaNode node;
  SpaClock clock;

  SpaTypeMap *map;
  SpaLog *log;
  Type type;

  uint32_t seq;

  uint8_t props_buffer[512];
  SpaV4l2SourceProps props;

  SpaNodeCallbacks callbacks;
  void *user_data;

  SpaV4l2State state[1];
};

#define CHECK_PORT(this,direction,port_id)  ((direction) == SPA_DIRECTION_OUTPUT && (port_id) == 0)


#define PROP(f,key,type,...)                                                    \
          SPA_POD_PROP (f,key,0,type,1,__VA_ARGS__)
#define PROP_R(f,key,type,...)                                                  \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_READONLY,type,1,__VA_ARGS__)
#define PROP_MM(f,key,type,...)                                                 \
          SPA_POD_PROP (f,key,SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_U_MM(f,key,type,...)                                               \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_EN(f,key,type,n,...)                                               \
          SPA_POD_PROP (f,key,SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)
#define PROP_U_EN(f,key,type,n,...)                                             \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)
#include "v4l2-utils.c"

static SpaResult
spa_v4l2_source_node_get_props (SpaNode       *node,
                                SpaProps     **props)
{
  SpaV4l2Source *this;
  SpaPODBuilder b = { NULL,  };
  SpaPODFrame f[2];

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (props != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  spa_pod_builder_init (&b, this->props_buffer, sizeof (this->props_buffer));
  spa_pod_builder_props (&b, &f[0], this->type.props,
      PROP   (&f[1], this->type.prop_device,      -SPA_POD_TYPE_STRING, this->props.device, sizeof (this->props.device)),
      PROP_R (&f[1], this->type.prop_device_name, -SPA_POD_TYPE_STRING, this->props.device_name, sizeof (this->props.device_name)),
      PROP_R (&f[1], this->type.prop_device_fd,    SPA_POD_TYPE_INT,    this->props.device_fd));
  *props = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaProps);

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_set_props (SpaNode         *node,
                                const SpaProps  *props)
{
  SpaV4l2Source *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  if (props == NULL) {
    reset_v4l2_source_props (&this->props);
    return SPA_RESULT_OK;
  } else {
    spa_props_query (props,
        this->type.prop_device, -SPA_POD_TYPE_STRING, this->props.device, sizeof (this->props.device),
        0);
  }
  return SPA_RESULT_OK;
}

static SpaResult
do_pause_done (SpaLoop        *loop,
               bool            async,
               uint32_t        seq,
               size_t          size,
               void           *data,
               void           *user_data)
{
  SpaV4l2Source *this = user_data;
  SpaEventNodeAsyncComplete *ac = data;

  if (SPA_RESULT_IS_OK (ac->body.res.value))
    ac->body.res.value = spa_v4l2_stream_off (this);

  this->callbacks.event (&this->node, (SpaEvent *)ac, this->user_data);

  return SPA_RESULT_OK;
}

static SpaResult
do_pause (SpaLoop        *loop,
          bool            async,
          uint32_t        seq,
          size_t          size,
          void           *data,
          void           *user_data)
{
  SpaV4l2Source *this = user_data;
  SpaResult res;
  SpaCommand *cmd = data;

  res = spa_node_port_send_command (&this->node,
                                    SPA_DIRECTION_OUTPUT,
                                    0,
                                    cmd);

  if (async) {
    SpaEventNodeAsyncComplete ac = SPA_EVENT_NODE_ASYNC_COMPLETE_INIT (this->type.event_node.AsyncComplete,
                                                                       seq, res);
    spa_loop_invoke (this->state[0].main_loop,
                     do_pause_done,
                     seq,
                     sizeof (ac),
                     &ac,
                     this);
  }
  return res;
}

static SpaResult
do_start_done (SpaLoop        *loop,
               bool            async,
               uint32_t        seq,
               size_t          size,
               void           *data,
               void           *user_data)
{
  SpaV4l2Source *this = user_data;
  SpaEventNodeAsyncComplete *ac = data;

  this->callbacks.event (&this->node, (SpaEvent *)ac, this->user_data);

  return SPA_RESULT_OK;
}

static SpaResult
do_start (SpaLoop        *loop,
          bool            async,
          uint32_t        seq,
          size_t          size,
          void           *data,
          void           *user_data)
{
  SpaV4l2Source *this = user_data;
  SpaResult res;
  SpaCommand *cmd = data;

  res = spa_node_port_send_command (&this->node,
                                    SPA_DIRECTION_OUTPUT,
                                    0,
                                    cmd);

  if (async) {
    SpaEventNodeAsyncComplete ac = SPA_EVENT_NODE_ASYNC_COMPLETE_INIT (this->type.event_node.AsyncComplete,
                                                                       seq, res);
    spa_loop_invoke (this->state[0].main_loop,
                     do_start_done,
                     seq,
                     sizeof (ac),
                     &ac,
                     this);
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_send_command (SpaNode    *node,
                                   SpaCommand *command)
{
  SpaV4l2Source *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (command != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  if (SPA_COMMAND_TYPE (command) == this->type.command_node.Start) {
    SpaV4l2State *state = &this->state[0];
    SpaResult res;

    if (!state->have_format)
      return SPA_RESULT_NO_FORMAT;

    if (state->n_buffers == 0)
      return SPA_RESULT_NO_BUFFERS;

    if ((res = spa_v4l2_stream_on (this)) < 0)
      return res;

    return spa_loop_invoke (this->state[0].data_loop,
                            do_start,
                            ++this->seq,
                            SPA_POD_SIZE (command),
                            command,
                            this);
  }
  else if (SPA_COMMAND_TYPE (command) == this->type.command_node.Pause) {
    SpaV4l2State *state = &this->state[0];

    if (!state->have_format)
      return SPA_RESULT_NO_FORMAT;

    if (state->n_buffers == 0)
      return SPA_RESULT_NO_BUFFERS;

    return spa_loop_invoke (this->state[0].data_loop,
                            do_pause,
                            ++this->seq,
                            SPA_POD_SIZE (command),
                            command,
                            this);
  }
  else if (SPA_COMMAND_TYPE (command) == this->type.command_node.ClockUpdate) {
    return SPA_RESULT_OK;
  }
  else
    return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_set_callbacks (SpaNode                *node,
                                    const SpaNodeCallbacks *callbacks,
                                    size_t                  callbacks_size,
                                    void                   *user_data)
{
  SpaV4l2Source *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  this->callbacks = *callbacks;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_get_n_ports (SpaNode       *node,
                                  uint32_t      *n_input_ports,
                                  uint32_t      *max_input_ports,
                                  uint32_t      *n_output_ports,
                                  uint32_t      *max_output_ports)
{
  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  if (n_input_ports)
    *n_input_ports = 0;
  if (max_input_ports)
    *max_input_ports = 0;
  if (n_output_ports)
    *n_output_ports = 1;
  if (max_output_ports)
    *max_output_ports = 1;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_get_port_ids (SpaNode       *node,
                                   uint32_t       n_input_ports,
                                   uint32_t      *input_ids,
                                   uint32_t       n_output_ports,
                                   uint32_t      *output_ids)
{
  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  if (n_output_ports > 0 && output_ids != NULL)
    output_ids[0] = 0;

  return SPA_RESULT_OK;
}


static SpaResult
spa_v4l2_source_node_add_port (SpaNode        *node,
                               SpaDirection    direction,
                               uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_remove_port (SpaNode        *node,
                                  SpaDirection    direction,
                                  uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_port_enum_formats (SpaNode         *node,
                                        SpaDirection     direction,
                                        uint32_t         port_id,
                                        SpaFormat      **format,
                                        const SpaFormat *filter,
                                        uint32_t         index)
{
  SpaV4l2Source *this;
  SpaResult res;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  res = spa_v4l2_enum_format (this, format, filter, index);

  return res;
}

static SpaResult
spa_v4l2_source_node_port_set_format (SpaNode         *node,
                                      SpaDirection     direction,
                                      uint32_t         port_id,
                                      uint32_t         flags,
                                      const SpaFormat *format)
{
  SpaV4l2Source *this;
  SpaV4l2State *state;
  SpaVideoInfo info;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  state = &this->state[port_id];

  if (format == NULL) {
    spa_v4l2_stream_off (this);
    spa_v4l2_clear_buffers (this);
    spa_v4l2_close (this);
    state->have_format = false;
    return SPA_RESULT_OK;
  } else {
    info.media_type = SPA_FORMAT_MEDIA_TYPE (format);
    info.media_subtype = SPA_FORMAT_MEDIA_SUBTYPE (format);

    if (info.media_type != this->type.media_type.video) {
      spa_log_error (this->log, "media type must be video");
      return SPA_RESULT_INVALID_MEDIA_TYPE;
    }

    if (info.media_subtype == this->type.media_subtype.raw) {
      if (!spa_format_video_raw_parse (format, &info.info.raw, &this->type.format_video)) {
        spa_log_error (this->log, "can't parse video raw");
        return SPA_RESULT_INVALID_MEDIA_TYPE;
      }

      if (state->have_format && info.media_type == state->current_format.media_type &&
          info.media_subtype == state->current_format.media_subtype &&
          info.info.raw.format == state->current_format.info.raw.format &&
          info.info.raw.size.width == state->current_format.info.raw.size.width &&
          info.info.raw.size.height == state->current_format.info.raw.size.height)
        return SPA_RESULT_OK;
    }
    else if (info.media_subtype == this->type.media_subtype_video.mjpg) {
      if (!spa_format_video_mjpg_parse (format, &info.info.mjpg, &this->type.format_video))
        return SPA_RESULT_INVALID_MEDIA_TYPE;

      if (state->have_format && info.media_type == state->current_format.media_type &&
          info.media_subtype == state->current_format.media_subtype &&
          info.info.mjpg.size.width == state->current_format.info.mjpg.size.width &&
          info.info.mjpg.size.height == state->current_format.info.mjpg.size.height)
        return SPA_RESULT_OK;
    }
    else if (info.media_subtype == this->type.media_subtype_video.h264) {
      if (!spa_format_video_h264_parse (format, &info.info.h264, &this->type.format_video))
        return SPA_RESULT_INVALID_MEDIA_TYPE;

      if (state->have_format && info.media_type == state->current_format.media_type &&
          info.media_subtype == state->current_format.media_subtype &&
          info.info.h264.size.width == state->current_format.info.h264.size.width &&
          info.info.h264.size.height == state->current_format.info.h264.size.height)
        return SPA_RESULT_OK;
    }
  }

  if (state->have_format && !(flags & SPA_PORT_FORMAT_FLAG_TEST_ONLY)) {
    spa_v4l2_use_buffers (this, NULL, 0);
    state->have_format = false;
  }

  if (spa_v4l2_set_format (this, &info, flags & SPA_PORT_FORMAT_FLAG_TEST_ONLY) < 0)
    return SPA_RESULT_INVALID_MEDIA_TYPE;

  if (!(flags & SPA_PORT_FORMAT_FLAG_TEST_ONLY)) {
    state->current_format = info;
    state->have_format = true;
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_get_format (SpaNode          *node,
                                      SpaDirection      direction,
                                      uint32_t          port_id,
                                      const SpaFormat **format)
{
  SpaV4l2Source *this;
  SpaV4l2State *state;
  SpaPODBuilder b = { NULL, };
  SpaPODFrame f[2];

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  state = &this->state[port_id];

  if (!state->have_format)
    return SPA_RESULT_NO_FORMAT;

  b.data = state->format_buffer;
  b.size = sizeof (state->format_buffer);

  spa_pod_builder_push_format (&b, &f[0], this->type.format,
                               state->current_format.media_type,
                               state->current_format.media_subtype);

  if (state->current_format.media_subtype == this->type.media_subtype.raw) {
    spa_pod_builder_add (&b,
        PROP (&f[1], this->type.format_video.format,     SPA_POD_TYPE_ID,        state->current_format.info.raw.format),
        PROP (&f[1], this->type.format_video.size,      -SPA_POD_TYPE_RECTANGLE, &state->current_format.info.raw.size),
        PROP (&f[1], this->type.format_video.framerate, -SPA_POD_TYPE_FRACTION,  &state->current_format.info.raw.framerate),
        0);
  }
  else if (state->current_format.media_subtype == this->type.media_subtype_video.mjpg ||
           state->current_format.media_subtype == this->type.media_subtype_video.jpeg) {
    spa_pod_builder_add (&b,
        PROP (&f[1], this->type.format_video.size,      -SPA_POD_TYPE_RECTANGLE, &state->current_format.info.mjpg.size),
        PROP (&f[1], this->type.format_video.framerate, -SPA_POD_TYPE_FRACTION,  &state->current_format.info.mjpg.framerate),
        0);
  }
  else if (state->current_format.media_subtype == this->type.media_subtype_video.h264) {
    spa_pod_builder_add (&b,
        PROP (&f[1], this->type.format_video.size,      -SPA_POD_TYPE_RECTANGLE, &state->current_format.info.h264.size),
        PROP (&f[1], this->type.format_video.framerate, -SPA_POD_TYPE_FRACTION,  &state->current_format.info.h264.framerate),
        0);
  } else
    return SPA_RESULT_NO_FORMAT;

  spa_pod_builder_pop (&b, &f[0]);

  *format = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaFormat);

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_get_info (SpaNode            *node,
                                    SpaDirection        direction,
                                    uint32_t            port_id,
                                    const SpaPortInfo **info)
{
  SpaV4l2Source *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  *info = &this->state[port_id].info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_get_props (SpaNode       *node,
                                     SpaDirection   direction,
                                     uint32_t       port_id,
                                     SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_port_set_props (SpaNode         *node,
                                     SpaDirection     direction,
                                     uint32_t         port_id,
                                     const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_port_use_buffers (SpaNode         *node,
                                       SpaDirection     direction,
                                       uint32_t         port_id,
                                       SpaBuffer      **buffers,
                                       uint32_t         n_buffers)
{
  SpaV4l2Source *this;
  SpaV4l2State *state;
  SpaResult res;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  state = &this->state[port_id];

  if (!state->have_format)
    return SPA_RESULT_NO_FORMAT;

  if (state->n_buffers) {
    spa_v4l2_stream_off (this);
    if ((res = spa_v4l2_clear_buffers (this)) < 0)
      return res;
  }
  if (buffers != NULL) {
    if ((res = spa_v4l2_use_buffers (this, buffers, n_buffers)) < 0)
      return res;
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_alloc_buffers (SpaNode         *node,
                                         SpaDirection     direction,
                                         uint32_t         port_id,
                                         SpaAllocParam  **params,
                                         uint32_t         n_params,
                                         SpaBuffer      **buffers,
                                         uint32_t        *n_buffers)
{
  SpaV4l2Source *this;
  SpaV4l2State *state;
  SpaResult res;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (buffers != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  state = &this->state[port_id];

  if (!state->have_format)
    return SPA_RESULT_NO_FORMAT;

  res = spa_v4l2_alloc_buffers (this, params, n_params, buffers, n_buffers);

  return res;
}

static SpaResult
spa_v4l2_source_node_port_set_io (SpaNode       *node,
                                  SpaDirection   direction,
                                  uint32_t       port_id,
                                  SpaPortIO     *io)
{
  SpaV4l2Source *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  this->state[port_id].io = io;

  return SPA_RESULT_OK;
}

static SpaResult
spa_v4l2_source_node_port_reuse_buffer (SpaNode         *node,
                                        uint32_t         port_id,
                                        uint32_t         buffer_id)
{
  SpaV4l2Source *this;
  SpaV4l2State *state;
  SpaResult res;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (port_id == 0, SPA_RESULT_INVALID_PORT);

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);
  state = &this->state[port_id];

  spa_return_val_if_fail (state->n_buffers > 0, SPA_RESULT_NO_BUFFERS);
  spa_return_val_if_fail (buffer_id < state->n_buffers, SPA_RESULT_INVALID_BUFFER_ID);

  res = spa_v4l2_buffer_recycle (this, buffer_id);

  return res;
}

static SpaResult
spa_v4l2_source_node_port_send_command (SpaNode        *node,
                                        SpaDirection    direction,
                                        uint32_t        port_id,
                                        SpaCommand     *command)
{
  SpaV4l2Source *this;
  SpaResult res;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  if (SPA_COMMAND_TYPE (command) == this->type.command_node.Pause) {
    res = spa_v4l2_port_set_enabled (this, false);
  }
  else if (SPA_COMMAND_TYPE (command) == this->type.command_node.Start) {
    res = spa_v4l2_port_set_enabled (this, true);
  }
  else
    res = SPA_RESULT_NOT_IMPLEMENTED;

  return res;
}

static SpaResult
spa_v4l2_source_node_process_input (SpaNode *node)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_node_process_output (SpaNode *node)
{
  SpaV4l2Source *this;
  SpaResult res = SPA_RESULT_OK;
  SpaPortIO *io;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaV4l2Source, node);
  io = this->state[0].io;
  spa_return_val_if_fail (io != NULL, SPA_RESULT_WRONG_STATE);

  if (io->status == SPA_RESULT_HAVE_BUFFER)
    return SPA_RESULT_HAVE_BUFFER;

  if (io->buffer_id != SPA_ID_INVALID) {
    res = spa_v4l2_buffer_recycle (this, io->buffer_id);
    io->buffer_id = SPA_ID_INVALID;
  }
  return res;
}

static const SpaNode v4l2source_node = {
  sizeof (SpaNode),
  NULL,
  spa_v4l2_source_node_get_props,
  spa_v4l2_source_node_set_props,
  spa_v4l2_source_node_send_command,
  spa_v4l2_source_node_set_callbacks,
  spa_v4l2_source_node_get_n_ports,
  spa_v4l2_source_node_get_port_ids,
  spa_v4l2_source_node_add_port,
  spa_v4l2_source_node_remove_port,
  spa_v4l2_source_node_port_enum_formats,
  spa_v4l2_source_node_port_set_format,
  spa_v4l2_source_node_port_get_format,
  spa_v4l2_source_node_port_get_info,
  spa_v4l2_source_node_port_get_props,
  spa_v4l2_source_node_port_set_props,
  spa_v4l2_source_node_port_use_buffers,
  spa_v4l2_source_node_port_alloc_buffers,
  spa_v4l2_source_node_port_set_io,
  spa_v4l2_source_node_port_reuse_buffer,
  spa_v4l2_source_node_port_send_command,
  spa_v4l2_source_node_process_input,
  spa_v4l2_source_node_process_output,
};

static SpaResult
spa_v4l2_source_clock_get_props (SpaClock  *clock,
                                 SpaProps **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_clock_set_props (SpaClock       *clock,
                                 const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_v4l2_source_clock_get_time (SpaClock         *clock,
                                int32_t          *rate,
                                int64_t          *ticks,
                                int64_t          *monotonic_time)
{
  SpaV4l2Source *this;
  SpaV4l2State *state;

  spa_return_val_if_fail (clock != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (clock, SpaV4l2Source, clock);
  state = &this->state[0];

  if (rate)
    *rate = SPA_USEC_PER_SEC;
  if (ticks)
    *ticks = state->last_ticks;
  if (monotonic_time)
    *monotonic_time = state->last_monotonic;

  return SPA_RESULT_OK;
}

static const SpaClock v4l2source_clock = {
  sizeof (SpaClock),
  NULL,
  SPA_CLOCK_STATE_STOPPED,
  spa_v4l2_source_clock_get_props,
  spa_v4l2_source_clock_set_props,
  spa_v4l2_source_clock_get_time,
};

static SpaResult
spa_v4l2_source_get_interface (SpaHandle               *handle,
                               uint32_t                 interface_id,
                               void                   **interface)
{
  SpaV4l2Source *this;

  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (interface != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = (SpaV4l2Source *) handle;

  if (interface_id == this->type.node)
    *interface = &this->node;
  else if (interface_id == this->type.clock)
    *interface = &this->clock;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

static SpaResult
v4l2_source_clear (SpaHandle *handle)
{
  return SPA_RESULT_OK;
}

static SpaResult
v4l2_source_init (const SpaHandleFactory  *factory,
                  SpaHandle               *handle,
                  const SpaDict           *info,
                  const SpaSupport        *support,
                  uint32_t                 n_support)
{
  SpaV4l2Source *this;
  uint32_t i;
  const char *str;

  spa_return_val_if_fail (factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  handle->get_interface = spa_v4l2_source_get_interface;
  handle->clear = v4l2_source_clear,

  this = (SpaV4l2Source *) handle;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].type, SPA_TYPE__TypeMap) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE__Log) == 0)
      this->log = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE_LOOP__MainLoop) == 0)
      this->state[0].main_loop = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE_LOOP__DataLoop) == 0)
      this->state[0].data_loop = support[i].data;
  }
  if (this->map == NULL) {
    spa_log_error (this->log, "a type-map is needed");
    return SPA_RESULT_ERROR;
  }
  if (this->state[0].main_loop == NULL) {
    spa_log_error (this->log, "a main_loop is needed");
    return SPA_RESULT_ERROR;
  }
  if (this->state[0].data_loop == NULL) {
    spa_log_error (this->log, "a data_loop is needed");
    return SPA_RESULT_ERROR;
  }
  init_type (&this->type, this->map);

  this->node = v4l2source_node;
  this->clock = v4l2source_clock;

  reset_v4l2_source_props (&this->props);

  this->state[0].log = this->log;
  this->state[0].info.flags = SPA_PORT_INFO_FLAG_LIVE;

  this->state[0].export_buf = true;

  if (info && (str = spa_dict_lookup (info, "device.path"))) {
    strncpy (this->props.device, str, 63);
  }

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo v4l2_source_interfaces[] =
{
  { SPA_TYPE__Node, },
  { SPA_TYPE__Clock, },
};

static SpaResult
v4l2_source_enum_interface_info (const SpaHandleFactory  *factory,
                                 const SpaInterfaceInfo **info,
                                 uint32_t                 index)
{
  spa_return_val_if_fail (factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  if (index < 0 || index >= SPA_N_ELEMENTS (v4l2_source_interfaces))
    return SPA_RESULT_ENUM_END;

  *info = &v4l2_source_interfaces[index];
  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_v4l2_source_factory =
{ "v4l2-source",
  NULL,
  sizeof (SpaV4l2Source),
  v4l2_source_init,
  v4l2_source_enum_interface_info,
};
