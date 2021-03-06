/* Spa Xv Sink
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

#include <spa/type-map.h>
#include <spa/log.h>
#include <spa/node.h>
#include <spa/video/format-utils.h>
#include <lib/props.h>

typedef struct _SpaXvSink SpaXvSink;

static const char default_device[] = "/dev/video0";

typedef struct {
  char device[64];
  char device_name[128];
  int  device_fd;
} SpaXvSinkProps;

static void
reset_xv_sink_props (SpaXvSinkProps *props)
{
  strncpy (props->device, default_device, 64);
}

#define MAX_BUFFERS     256

typedef struct _XvBuffer XvBuffer;

struct _XvBuffer {
  SpaBuffer buffer;
  SpaMeta meta[1];
  SpaMetaHeader header;
  SpaData data[1];
  XvBuffer *next;
  uint32_t index;
  SpaXvSink *sink;
  bool outstanding;
};

typedef struct {
  bool opened;
  int fd;
  XvBuffer buffers[MAX_BUFFERS];
  XvBuffer *ready;
  uint32_t ready_count;
} SpaXvState;

typedef struct {
  uint32_t node;
  uint32_t props;
  uint32_t prop_device;
  uint32_t prop_device_name;
  uint32_t prop_device_fd;
  SpaTypeMediaType media_type;
  SpaTypeMediaSubtype media_subtype;
  SpaTypeFormatVideo format_video;
  SpaTypeCommandNode command_node;
} Type;

static inline void
init_type (Type *type, SpaTypeMap *map)
{
  type->node = spa_type_map_get_id (map, SPA_TYPE__Node);
  type->props = spa_type_map_get_id (map, SPA_TYPE__Props);
  type->prop_device = spa_type_map_get_id (map, SPA_TYPE_PROPS__device);
  type->prop_device_name = spa_type_map_get_id (map, SPA_TYPE_PROPS__deviceName);
  type->prop_device_fd = spa_type_map_get_id (map, SPA_TYPE_PROPS__deviceFd);
  spa_type_media_type_map (map, &type->media_type);
  spa_type_media_subtype_map (map, &type->media_subtype);
  spa_type_format_video_map (map, &type->format_video);
  spa_type_command_node_map (map, &type->command_node);
}

struct _SpaXvSink {
  SpaHandle handle;
  SpaNode   node;

  Type type;
  SpaTypeMap *map;
  SpaLog *log;

  uint8_t props_buffer[512];
  SpaXvSinkProps props;

  SpaNodeCallbacks callbacks;
  void *user_data;

  bool have_format;
  uint8_t format_buffer[1024];
  SpaVideoInfo current_format;

  SpaPortInfo info;
  SpaXvState state;

  SpaPortIO *io;
};

#define CHECK_PORT(this,d,p)  ((d) == SPA_DIRECTION_OUTPUT && (p) == 0)

#include "xv-utils.c"

#define PROP(f,key,type,...)                                                    \
          SPA_POD_PROP (f,key,0,type,1,__VA_ARGS__)
#define PROP_R(f,key,type,...)                                                  \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_READONLY,type,1,__VA_ARGS__)

static SpaResult
spa_xv_sink_node_get_props (SpaNode       *node,
                            SpaProps     **props)
{
  SpaXvSink *this;
  SpaPODBuilder b = { NULL,  };
  SpaPODFrame f[2];

  if (node == NULL || props == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaXvSink, node);

  spa_pod_builder_init (&b, this->props_buffer, sizeof (this->props_buffer));
  spa_pod_builder_props (&b, &f[0], this->type.props,
      PROP   (&f[1], this->type.prop_device,      -SPA_POD_TYPE_STRING, this->props.device, sizeof (this->props.device)),
      PROP_R (&f[1], this->type.prop_device_name, -SPA_POD_TYPE_STRING, this->props.device_name, sizeof (this->props.device_name)),
      PROP_R (&f[1], this->type.prop_device_fd,    SPA_POD_TYPE_INT,    this->props.device_fd));
  *props = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaProps);

  return SPA_RESULT_OK;
}

static SpaResult
spa_xv_sink_node_set_props (SpaNode         *node,
                            const SpaProps  *props)
{
  SpaXvSink *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaXvSink, node);

  if (props == NULL) {
    reset_xv_sink_props (&this->props);
  } else {
    spa_props_query (props,
        this->type.prop_device, -SPA_POD_TYPE_STRING, this->props.device, sizeof (this->props.device),
        0);
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_xv_sink_node_send_command (SpaNode    *node,
                               SpaCommand *command)
{
  SpaXvSink *this;

  if (node == NULL || command == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaXvSink, node);

  if (SPA_COMMAND_TYPE (command) == this->type.command_node.Start) {
    spa_xv_start (this);
  }
  else if (SPA_COMMAND_TYPE (command) == this->type.command_node.Pause) {
    spa_xv_stop (this);
  }
  else
    return SPA_RESULT_NOT_IMPLEMENTED;

  return SPA_RESULT_OK;
}

static SpaResult
spa_xv_sink_node_set_callbacks (SpaNode                *node,
                                const SpaNodeCallbacks *callbacks,
                                size_t                  callbacks_size,
                                void                   *user_data)
{
  SpaXvSink *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaXvSink, node);

  this->callbacks = *callbacks;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_xv_sink_node_get_n_ports (SpaNode       *node,
                              uint32_t      *n_input_ports,
                              uint32_t      *max_input_ports,
                              uint32_t      *n_output_ports,
                              uint32_t      *max_output_ports)
{
  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_input_ports)
    *n_input_ports = 0;
  if (n_output_ports)
    *n_output_ports = 1;
  if (max_input_ports)
    *max_input_ports = 0;
  if (max_output_ports)
    *max_output_ports = 1;

  return SPA_RESULT_OK;
}

static SpaResult
spa_xv_sink_node_get_port_ids (SpaNode       *node,
                               uint32_t       n_input_ports,
                               uint32_t      *input_ids,
                               uint32_t       n_output_ports,
                               uint32_t      *output_ids)
{
  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (n_output_ports > 0 && output_ids != NULL)
    output_ids[0] = 0;

  return SPA_RESULT_OK;
}


static SpaResult
spa_xv_sink_node_add_port (SpaNode        *node,
                           SpaDirection    direction,
                           uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_remove_port (SpaNode        *node,
                              SpaDirection    direction,
                              uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_port_enum_formats (SpaNode         *node,
                                    SpaDirection     direction,
                                    uint32_t         port_id,
                                    SpaFormat      **format,
                                    const SpaFormat *filter,
                                    uint32_t         index)
{
  //SpaXvSink *this;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  //this = SPA_CONTAINER_OF (node, SpaXvSink, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  switch (index) {
    case 0:
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  *format = NULL;

  return SPA_RESULT_OK;
}

static SpaResult
spa_xv_sink_node_port_set_format (SpaNode         *node,
                                  SpaDirection     direction,
                                  uint32_t         port_id,
                                  uint32_t         flags,
                                  const SpaFormat *format)
{
  SpaXvSink *this;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaXvSink, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (format == NULL) {
    this->have_format = false;
    return SPA_RESULT_OK;
  } else {
    SpaVideoInfo info = { SPA_FORMAT_MEDIA_TYPE (format),
                          SPA_FORMAT_MEDIA_SUBTYPE (format), };


    if (info.media_type != this->type.media_type.video &&
        info.media_subtype != this->type.media_subtype.raw)
      return SPA_RESULT_INVALID_MEDIA_TYPE;

    if (!spa_format_video_raw_parse (format, &info.info.raw, &this->type.format_video))
      return SPA_RESULT_INVALID_MEDIA_TYPE;

    if (spa_xv_set_format (this, &info, flags & SPA_PORT_FORMAT_FLAG_TEST_ONLY) < 0)
      return SPA_RESULT_INVALID_MEDIA_TYPE;

    if (!(flags & SPA_PORT_FORMAT_FLAG_TEST_ONLY)) {
      this->current_format = info;
      this->have_format = true;
    }
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_xv_sink_node_port_get_format (SpaNode          *node,
                                  SpaDirection      direction,
                                  uint32_t          port_id,
                                  const SpaFormat **format)
{
  SpaXvSink *this;

  if (node == NULL || format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaXvSink, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = NULL;

  return SPA_RESULT_OK;
}

static SpaResult
spa_xv_sink_node_port_get_info (SpaNode            *node,
                                SpaDirection        direction,
                                uint32_t            port_id,
                                const SpaPortInfo **info)
{
  SpaXvSink *this;

  if (node == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaXvSink, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  *info = &this->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_xv_sink_node_port_get_props (SpaNode       *node,
                                 SpaDirection   direction,
                                 uint32_t       port_id,
                                 SpaProps     **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_port_set_props (SpaNode         *node,
                                 SpaDirection     direction,
                                 uint32_t         port_id,
                                 const SpaProps  *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_port_use_buffers (SpaNode         *node,
                                   SpaDirection     direction,
                                   uint32_t         port_id,
                                   SpaBuffer      **buffers,
                                   uint32_t         n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_port_alloc_buffers (SpaNode         *node,
                                     SpaDirection     direction,
                                     uint32_t         port_id,
                                     SpaAllocParam  **params,
                                     uint32_t         n_params,
                                     SpaBuffer      **buffers,
                                     uint32_t        *n_buffers)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_port_set_io (SpaNode      *node,
                              SpaDirection  direction,
                              uint32_t      port_id,
                              SpaPortIO    *io)
{
  SpaXvSink *this;

  if (node == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = SPA_CONTAINER_OF (node, SpaXvSink, node);

  if (!CHECK_PORT (this, direction, port_id))
    return SPA_RESULT_INVALID_PORT;

  this->io = io;

  return SPA_RESULT_OK;
}

static SpaResult
spa_xv_sink_node_port_reuse_buffer (SpaNode         *node,
                                    uint32_t         port_id,
                                    uint32_t         buffer_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_port_send_command (SpaNode        *node,
                                    SpaDirection    direction,
                                    uint32_t        port_id,
                                    SpaCommand     *command)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_process_input (SpaNode          *node)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_xv_sink_node_process_output (SpaNode           *node)
{
  return SPA_RESULT_INVALID_PORT;
}

static const SpaNode xvsink_node = {
  sizeof (SpaNode),
  NULL,
  spa_xv_sink_node_get_props,
  spa_xv_sink_node_set_props,
  spa_xv_sink_node_send_command,
  spa_xv_sink_node_set_callbacks,
  spa_xv_sink_node_get_n_ports,
  spa_xv_sink_node_get_port_ids,
  spa_xv_sink_node_add_port,
  spa_xv_sink_node_remove_port,
  spa_xv_sink_node_port_enum_formats,
  spa_xv_sink_node_port_set_format,
  spa_xv_sink_node_port_get_format,
  spa_xv_sink_node_port_get_info,
  spa_xv_sink_node_port_get_props,
  spa_xv_sink_node_port_set_props,
  spa_xv_sink_node_port_use_buffers,
  spa_xv_sink_node_port_alloc_buffers,
  spa_xv_sink_node_port_set_io,
  spa_xv_sink_node_port_reuse_buffer,
  spa_xv_sink_node_port_send_command,
  spa_xv_sink_node_process_input,
  spa_xv_sink_node_process_output,
};

static SpaResult
spa_xv_sink_get_interface (SpaHandle               *handle,
                           uint32_t                 interface_id,
                           void                   **interface)
{
  SpaXvSink *this;

  if (handle == NULL || interface == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  this = (SpaXvSink *) handle;

  if (interface_id == this->type.node)
    *interface = &this->node;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

static SpaResult
xv_sink_clear (SpaHandle *handle)
{
  return SPA_RESULT_OK;
}

static SpaResult
xv_sink_init (const SpaHandleFactory  *factory,
              SpaHandle               *handle,
              const SpaDict           *info,
              const SpaSupport        *support,
              uint32_t                 n_support)
{
  SpaXvSink *this;
  uint32_t i;

  if (factory == NULL || handle == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  handle->get_interface = spa_xv_sink_get_interface;
  handle->clear = xv_sink_clear;

  this = (SpaXvSink *) handle;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].type, SPA_TYPE__TypeMap) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE__Log) == 0)
      this->log = support[i].data;
  }
  if (this->map == NULL) {
    spa_log_error (this->log, "a type-map is needed");
    return SPA_RESULT_ERROR;
  }
  init_type (&this->type, this->map);

  this->node = xvsink_node;
  reset_xv_sink_props (&this->props);

  this->info.flags = 0;

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo xv_sink_interfaces[] =
{
  { SPA_TYPE__Node, },
};

static SpaResult
xv_sink_enum_interface_info (const SpaHandleFactory  *factory,
                             const SpaInterfaceInfo **info,
                             uint32_t                 index)
{
  if (factory == NULL || info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  switch (index) {
    case 0:
      *info = &xv_sink_interfaces[index];
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_xv_sink_factory =
{ "xv-sink",
  NULL,
  sizeof (SpaXvSink),
  xv_sink_init,
  xv_sink_enum_interface_info,
};
