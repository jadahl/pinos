/* Simple Plugin API
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

#ifndef __SPA_NODE_H__
#define __SPA_NODE_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaNode SpaNode;

#define SPA_TYPE__Node                        SPA_TYPE_INTERFACE_BASE "Node"
#define SPA_TYPE_NODE_BASE                    SPA_TYPE__Node ":"

#include <spa/defs.h>
#include <spa/plugin.h>
#include <spa/props.h>
#include <spa/alloc-param.h>
#include <spa/event-node.h>
#include <spa/command-node.h>
#include <spa/buffer.h>
#include <spa/format.h>

typedef struct {
  uint64_t          offset;
  uint32_t          min_size;
  uint32_t          max_size;
} SpaRange;

/**
 * SpaPortIO:
 * @status: the status
 * @buffer_id: a buffer id
 * @range: requested range
 * @event: event
 *
 * IO information for a port on a node. This is allocated
 * by the host and configured on all ports for which IO is requested.
 */
typedef struct {
  uint32_t       status;
  uint32_t       buffer_id;
  SpaRange       range;
} SpaPortIO;

/**
 * SpaPortInfo
 * @flags: extra port flags
 * @rate: rate of sequence number increment per second of media data
 * @n_params: number of elements in @params;
 * @params: extra allocation parameters
 * @maxbuffering: the maximum amount of bytes that the element will keep
 *                around internally
 * @latency: latency on this port in nanoseconds
 * @extra: a dictionary of extra port info
 */
typedef struct {
#define SPA_PORT_INFO_FLAG_REMOVABLE            (1<<0)  /* port can be removed */
#define SPA_PORT_INFO_FLAG_OPTIONAL             (1<<1)  /* processing on port is optional */
#define SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS    (1<<2)  /* the port can allocate buffer data */
#define SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS      (1<<3)  /* the port can use a provided buffer */
#define SPA_PORT_INFO_FLAG_IN_PLACE             (1<<4)  /* the port can process data in-place and will need
                                                         * a writable input buffer */
#define SPA_PORT_INFO_FLAG_NO_REF               (1<<5)  /* the port does not keep a ref on the buffer */
#define SPA_PORT_INFO_FLAG_LIVE                 (1<<6)  /* output buffers from this port are timestamped against
                                                         * a live clock. */
  uint32_t            flags;
  uint32_t            rate;
  uint32_t            n_params;
  SpaAllocParam     **params;
  uint64_t            maxbuffering;
  uint64_t            latency;
  SpaDict            *extra;
} SpaPortInfo;


typedef struct {
  /**
   * SpaNodeCallbacks::event:
   * @node: a #SpaNode
   * @event: the event that was emited
   * @user_data: user data provided when registering the callbacks
   *
   * This will be called when an out-of-bound event is notified
   * on @node. the callback can be called from any thread.
   */
  void (*event)       (SpaNode  *node,
                       SpaEvent *event,
                       void     *user_data);
  /**
   * SpaNodeCallbacks::need_input:
   * @node: a #SpaNode
   * @user_data: user data provided when registering the callbacks
   *
   * The node needs more input. This callback is called from the
   * data thread.
   *
   * When this function is NULL, synchronous operation is requested
   * on the input ports.
   */
  void (*need_input)  (SpaNode  *node,
                       void     *user_data);
  /**
   * SpaNodeCallbacks::have_output:
   * @node: a #SpaNode
   * @user_data: user data provided when registering the callbacks
   *
   * The node has output input. This callback is called from the
   * data thread.
   *
   * When this function is NULL, synchronous operation is requested
   * on the output ports.
   */
  void (*have_output)  (SpaNode  *node,
                        void     *user_data);
  /**
   * SpaNodeCallbacks::reuse_buffer:
   * @node: a #SpaNode
   * @port_id: an input port_id
   * @buffer_id: the buffer id to be reused
   * @user_data: user data provided when registering the callbacks
   *
   * The node has a buffer that can be reused. This callback is called
   * from the data thread.
   *
   * When this function is NULL, the buffers to reuse will be set in
   * the io area or the input ports.
   */
  void (*reuse_buffer) (SpaNode  *node,
                        uint32_t  port_id,
                        uint32_t  buffer_id,
                        void     *user_data);
} SpaNodeCallbacks;

/**
 * SpaNode:
 *
 * A SpaNode is a component that can comsume and produce buffers.
 *
 *
 *
 */
struct _SpaNode {
  /* the total size of this node. This can be used to expand this
   * structure in the future */
  size_t size;
  /**
   * SpaNode::info
   *
   * Extra information about the node
   */
  const SpaDict * info;
  /**
   * SpaNode::get_props:
   * @node: a #SpaNode
   * @props: a location for a #SpaProps pointer
   *
   * Get the configurable properties of @node.
   *
   * The returned @props is a snapshot of the current configuration and
   * can be modified. The modifications will take effect after a call
   * to SpaNode::set_props.
   *
   * This function must be called from the main thread.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node or props are %NULL
   *          #SPA_RESULT_NOT_IMPLEMENTED when there are no properties
   *                 implemented on @node
   */
  SpaResult   (*get_props)            (SpaNode          *node,
                                       SpaProps        **props);
  /**
   * SpaNode::set_props:
   * @node: a #SpaNode
   * @props: a #SpaProps
   *
   * Set the configurable properties in @node.
   *
   * Usually, @props will be obtained from SpaNode::get_props and then
   * modified but it is also possible to set another #SpaProps object
   * as long as its keys and types match those of SpaProps::get_props.
   *
   * Properties with keys that are not known are ignored.
   *
   * If @props is NULL, all the properties are reset to their defaults.
   *
   * This function must be called from the main thread.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
   *          #SPA_RESULT_NOT_IMPLEMENTED when no properties can be
   *                 modified on @node.
   *          #SPA_RESULT_WRONG_PROPERTY_TYPE when a property has the wrong
   *                 type.
   */
  SpaResult   (*set_props)           (SpaNode         *node,
                                      const SpaProps  *props);
  /**
   * SpaNode::send_command:
   * @node: a #SpaNode
   * @command: a #SpaCommand
   *
   * Send a command to @node.
   *
   * Upon completion, a command might change the state of a node.
   *
   * This function must be called from the main thread.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node or command is %NULL
   *          #SPA_RESULT_NOT_IMPLEMENTED when this node can't process commands
   *          #SPA_RESULT_INVALID_COMMAND @command is an invalid command
   *          #SPA_RESULT_ASYNC @command is executed asynchronously
   */
  SpaResult   (*send_command)         (SpaNode    *node,
                                       SpaCommand *command);
  /**
   * SpaNode::set_event_callback:
   * @node: a #SpaNode
   * @callbacks: callbacks to set
   * @callbacks_size: size of the callbacks structure
   * @user_data: user data passed to the callback functions
   *
   * Set callbacks to receive events and scheduling callbacks from @node.
   * if @callbacks is %NULL, the current callbacks are removed.
   *
   * This function must be called from the main thread.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
   */
  SpaResult   (*set_callbacks)   (SpaNode                *node,
                                  const SpaNodeCallbacks *callbacks,
                                  size_t                  callbacks_size,
                                  void                   *user_data);
  /**
   * SpaNode::get_n_ports:
   * @node: a #SpaNode
   * @n_input_ports: location to hold the number of input ports or %NULL
   * @max_input_ports: location to hold the maximum number of input ports or %NULL
   * @n_output_ports: location to hold the number of output ports or %NULL
   * @max_output_ports: location to hold the maximum number of output ports or %NULL
   *
   * Get the current number of input and output ports and also the maximum
   * number of ports.
   *
   * This function must be called from the main thread.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
   */
  SpaResult   (*get_n_ports)          (SpaNode          *node,
                                       uint32_t         *n_input_ports,
                                       uint32_t         *max_input_ports,
                                       uint32_t         *n_output_ports,
                                       uint32_t         *max_output_ports);
  /**
   * SpaNode::get_port_ids:
   * @node: a #SpaNode
   * @n_input_ports: size of the @input_ids array
   * @input_ids: array to store the input stream ids
   * @n_output_ports: size of the @output_ids array
   * @output_ids: array to store the output stream ids
   *
   * Get the ids of the currently available ports.
   *
   * This function must be called from the main thread.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
   */
  SpaResult   (*get_port_ids)         (SpaNode          *node,
                                       uint32_t          n_input_ports,
                                       uint32_t         *input_ids,
                                       uint32_t          n_output_ports,
                                       uint32_t         *output_ids);

  /**
   * SpaNode::add_port:
   * @node: a #SpaNode
   * @direction: a #SpaDirection
   * @port_id: an unused port id
   *
   * Make a new port with @port_id. The called should use get_port_ids() to
   * find an unused id for the given @direction.
   *
   * Port ids should be between 0 and max_ports as obtained from get_n_ports().
   *
   * This function must be called from the main thread.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
   */
  SpaResult   (*add_port)             (SpaNode          *node,
                                       SpaDirection      direction,
                                       uint32_t          port_id);
  SpaResult   (*remove_port)          (SpaNode          *node,
                                       SpaDirection      direction,
                                       uint32_t          port_id);

  /**
   * SpaNode::port_enum_formats:
   * @node: a #SpaNode
   * @direction: a #SpaDirection
   * @port_id: the port to query
   * @format: pointer to a format
   * @filter: a format filter
   * @index: an index variable, 0 to get the first item
   *
   * Enumerate all possible formats on @port_id of @node that are compatible
   * with @filter. When @port_id is #SPA_ID_INVALID, the enumeration will
   * list all the formats possible on a port that would be added with
   * add_port().
   *
   * Use @index to retrieve the formats one by one until the function
   * returns #SPA_RESULT_ENUM_END.
   *
   * The result format can be queried and modified and ultimately be used
   * to call SpaNode::port_set_format.
   *
   * This function must be called from the main thread.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node or format is %NULL
   *          #SPA_RESULT_INVALID_PORT when port_id is not valid
   *          #SPA_RESULT_ENUM_END when no format exists
   */
  SpaResult   (*port_enum_formats)    (SpaNode          *node,
                                       SpaDirection      direction,
                                       uint32_t          port_id,
                                       SpaFormat       **format,
                                       const SpaFormat  *filter,
                                       uint32_t          index);
  /**
   * SpaNode::port_set_format:
   * @node: a #SpaNode
   * @direction: a #SpaDirection
   * @port_id: the port to configure
   * @flags: flags
   * @format: a #SpaFormat with the format
   *
   * Set a format on @port_id of @node.
   *
   * When @format is %NULL, the current format will be removed.
   *
   * This function takes a copy of the format.
   *
   * Upon completion, this function might change the state of a node to
   * the READY state or to CONFIGURE when @format is NULL.
   *
   * This function must be called from the main thread.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_OK_RECHECK on success, the value of @format might have been
   *                 changed depending on @flags and the final value can be found by
   *                 doing SpaNode::get_format.
   *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
   *          #SPA_RESULT_INVALID_PORT when port_id is not valid
   *          #SPA_RESULT_INVALID_MEDIA_TYPE when the media type is not valid
   *          #SPA_RESULT_INVALID_FORMAT_PROPERTIES when one of the mandatory format
   *                 properties is not specified and #SPA_PORT_FORMAT_FLAG_FIXATE was
   *                 not set in @flags.
   *          #SPA_RESULT_WRONG_PROPERTY_TYPE when the type or size of a property
   *                 is not correct.
   *          #SPA_RESULT_ASYNC the function is executed asynchronously
   */
#define SPA_PORT_FORMAT_FLAG_TEST_ONLY        (1 << 0)      /* just check if the format is accepted */
#define SPA_PORT_FORMAT_FLAG_FIXATE           (1 << 1)      /* fixate the non-optional unset fields */
#define SPA_PORT_FORMAT_FLAG_NEAREST          (1 << 2)      /* allow set fields to be rounded to the
                                                             * nearest allowed field value. */
  SpaResult   (*port_set_format)      (SpaNode         *node,
                                       SpaDirection     direction,
                                       uint32_t         port_id,
                                       uint32_t         flags,
                                       const SpaFormat *format);
  /**
   * SpaNode::port_get_format:
   * @node: a #SpaNode
   * @direction: a #SpaDirection
   * @port_id: the port to query
   * @format: a pointer to a location to hold the #SpaFormat
   *
   * Get the format on @port_id of @node. The result #SpaFormat can
   * not be modified.
   *
   * This function must be called from the main thread.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when @node or @format is %NULL
   *          #SPA_RESULT_INVALID_PORT when @port_id is not valid
   *          #SPA_RESULT_INVALID_NO_FORMAT when no format was set
   */
  SpaResult   (*port_get_format)      (SpaNode              *node,
                                       SpaDirection          direction,
                                       uint32_t              port_id,
                                       const SpaFormat     **format);

  SpaResult   (*port_get_info)        (SpaNode              *node,
                                       SpaDirection          direction,
                                       uint32_t              port_id,
                                       const SpaPortInfo   **info);

  SpaResult   (*port_get_props)       (SpaNode              *node,
                                       SpaDirection          direction,
                                       uint32_t              port_id,
                                       SpaProps            **props);
  SpaResult   (*port_set_props)       (SpaNode              *node,
                                       SpaDirection          direction,
                                       uint32_t              port_id,
                                       const SpaProps       *props);

  /**
   * SpaNode::port_use_buffers:
   * @node: a #SpaNode
   * @direction: a #SpaDirection
   * @port_id: a port id
   * @buffers: an array of buffer pointers
   * @n_buffers: number of elements in @buffers
   *
   * Tell the port to use the given buffers
   *
   * For an input port, all the buffers will remain dequeued. Once a buffer
   * has been pushed on a port with port_push_input, it should not be reused
   * until the REUSE_BUFFER event is notified.
   *
   * For output ports, all buffers will be queued in the port. with
   * port_pull_output, a buffer can be dequeued. When a buffer can be reused,
   * port_reuse_buffer() should be called.
   *
   * Passing %NULL as @buffers will remove the reference that the port has
   * on the buffers.
   *
   * Upon completion, this function might change the state of the
   * node to PAUSED, when the node has enough buffers on all ports, or READY
   * when @buffers are %NULL.
   *
   * This function must be called from the main thread.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_ASYNC the function is executed asynchronously
   */
  SpaResult   (*port_use_buffers)     (SpaNode              *node,
                                       SpaDirection          direction,
                                       uint32_t              port_id,
                                       SpaBuffer           **buffers,
                                       uint32_t              n_buffers);
  /**
   * SpaNode::port_alloc_buffers:
   * @node: a #SpaNode
   * @direction: a #SpaDirection
   * @port_id: a port id
   * @params: allocation parameters
   * @n_params: number of elements in @params
   * @buffers: an array of buffer pointers
   * @n_buffers: number of elements in @buffers
   *
   * Tell the port to allocate memory for @buffers.
   *
   * @buffers should contain an array of pointers to buffers. The data
   * in the buffers should point to an array of at least 1 SPA_DATA_TYPE_INVALID
   * data pointers that will be filled by this function.
   *
   * For input ports, the buffers will be dequeued and ready to be filled
   * and pushed into the port. A notify should be configured so that you can
   * know when a buffer can be reused.
   *
   * For output ports, the buffers remain queued. port_reuse_buffer() should
   * be called when a buffer can be reused.
   *
   * Upon completion, this function might change the state of the
   * node to PAUSED, when the node has enough buffers on all ports.
   *
   * Once the port has allocated buffers, the memory of the buffers can be
   * released again by calling SpaNode::port_use_buffers with %NULL.
   *
   * This function must be called from the main thread.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_ERROR when the node already has allocated buffers.
   *          #SPA_RESULT_ASYNC the function is executed asynchronously
   */
  SpaResult   (*port_alloc_buffers)   (SpaNode              *node,
                                       SpaDirection          direction,
                                       uint32_t              port_id,
                                       SpaAllocParam       **params,
                                       uint32_t              n_params,
                                       SpaBuffer           **buffers,
                                       uint32_t             *n_buffers);

  /**
   * SpaNode::port_set_io:
   * @direction: a #SpaDirection
   * @port_id: a port id
   * @io: a #SpaPortIO
   *
   * Configure the given io structure on @port_id. This
   * structure is allocated by the host and is used to query the state
   * of the port and exchange buffers with the port.
   *
   * Setting an @io of %NULL will disable the port.
   *
   * Returns: #SPA_RESULT_OK on success
   */
  SpaResult   (*port_set_io)          (SpaNode       *node,
                                       SpaDirection   direction,
                                       uint32_t       port_id,
                                       SpaPortIO     *io);

  /**
   * SpaNode::port_reuse_buffer:
   * @node: a #SpaNode
   * @port_id: a port id
   * @buffer_id: a buffer id to reuse
   *
   * Tell an output port to reuse a buffer.
   *
   * This function must be called from the data thread.
   *
   * Returns: #SPA_RESULT_OK on success
   *          #SPA_RESULT_INVALID_ARGUMENTS when node is %NULL
   */
  SpaResult   (*port_reuse_buffer)    (SpaNode          *node,
                                       uint32_t          port_id,
                                       uint32_t          buffer_id);

  SpaResult   (*port_send_command)    (SpaNode          *node,
                                       SpaDirection      direction,
                                       uint32_t          port_id,
                                       SpaCommand       *command);
  /**
   * SpaNode::process_input:
   * @node: a #SpaNode
   *
   * Process the input area of the node.
   *
   * For synchronous nodes, this function is called to start processing data
   * or when process_output returned SPA_RESULT_NEED_MORE_INPUT
   *
   * For Asynchronous node, this function is called when a NEED_INPUT event
   * is received from the node.
   *
   * Before calling this function, you must configure SpaPortInput structures
   * configured on the input ports.
   *
   * The node will loop through all SpaPortInput structures and will
   * process the buffers. For each port, the port info will be updated as:
   *
   *  - The buffer_id is set to SPA_ID_INVALID and the status is set to
   *    SPA_RESULT_OK when the buffer was successfully consumed
   *
   *  - The buffer_id is untouched and the status is set to an error when
   *    the buffer was invalid.
   *
   *  - The buffer_id is untouched and the status is set to SPA_RESULT_OK
   *    when no input was consumed. This can happen when the node does not
   *    need input on this port.
   *
   * Returns: #SPA_RESULT_OK on success or when the node is asynchronous
   *          #SPA_RESULT_HAVE_OUTPUT for synchronous nodes when output
   *                                  can be consumed.
   *          #SPA_RESULT_OUT_OF_BUFFERS for synchronous nodes when buffers
   *                                     should be released with port_reuse_buffer
   *          #SPA_RESULT_ERROR when one of the inputs is in error
   */
  SpaResult   (*process_input)           (SpaNode *node);

  /**
   * SpaNode::process_output:
   * @node: a #SpaNode
   *
   * Tell the node to produce more output.
   *
   * Before calling this function you must process the buffers and events
   * in the SpaPortOutput structure and set the buffer_id to SPA_ID_INVALID
   * for all consumed buffers. Buffers that you do not want to consume should
   * be returned to the node with port_reuse_buffer.
   *
   * For synchronous nodes, this function can be called when process_input
   * returned #SPA_RESULT_HAVE_ENOUGH_INPUT.
   *
   * For Asynchronous node, this function is called when a HAVE_OUTPUT event
   * is received from the node.
   *
   * Returns: #SPA_RESULT_OK on success or when the node is asynchronous
   *          #SPA_RESULT_NEED_INPUT for synchronous nodes when input
   *                                 is needed.
   *          #SPA_RESULT_ERROR when one of the outputs is in error
   */
  SpaResult   (*process_output)          (SpaNode *node);
};

#define spa_node_get_props(n,...)          (n)->get_props((n),__VA_ARGS__)
#define spa_node_set_props(n,...)          (n)->set_props((n),__VA_ARGS__)
#define spa_node_send_command(n,...)       (n)->send_command((n),__VA_ARGS__)
#define spa_node_set_callbacks(n,...)      (n)->set_callbacks((n),__VA_ARGS__)
#define spa_node_get_n_ports(n,...)        (n)->get_n_ports((n),__VA_ARGS__)
#define spa_node_get_port_ids(n,...)       (n)->get_port_ids((n),__VA_ARGS__)
#define spa_node_add_port(n,...)           (n)->add_port((n),__VA_ARGS__)
#define spa_node_remove_port(n,...)        (n)->remove_port((n),__VA_ARGS__)
#define spa_node_port_enum_formats(n,...)  (n)->port_enum_formats((n),__VA_ARGS__)
#define spa_node_port_set_format(n,...)    (n)->port_set_format((n),__VA_ARGS__)
#define spa_node_port_get_format(n,...)    (n)->port_get_format((n),__VA_ARGS__)
#define spa_node_port_get_info(n,...)      (n)->port_get_info((n),__VA_ARGS__)
#define spa_node_port_get_props(n,...)     (n)->port_get_props((n),__VA_ARGS__)
#define spa_node_port_set_props(n,...)     (n)->port_set_props((n),__VA_ARGS__)
#define spa_node_port_use_buffers(n,...)   (n)->port_use_buffers((n),__VA_ARGS__)
#define spa_node_port_alloc_buffers(n,...) (n)->port_alloc_buffers((n),__VA_ARGS__)
#define spa_node_port_set_io(n,...)        (n)->port_set_io((n),__VA_ARGS__)
#define spa_node_port_reuse_buffer(n,...)  (n)->port_reuse_buffer((n),__VA_ARGS__)
#define spa_node_port_send_command(n,...)  (n)->port_send_command((n),__VA_ARGS__)
#define spa_node_process_input(n)          (n)->process_input((n))
#define spa_node_process_output(n)         (n)->process_output((n))

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_NODE_H__ */
