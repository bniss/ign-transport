/*
 * Copyright (C) 2019 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#ifndef IGN_TRANSPORT_CIFACE_HH_
#define IGN_TRANSPORT_CIFACE_HH_

#ifdef __cplusplus
extern "C" {
#endif
  /// \brief Id of a transport node.
  typedef int32_t IgnTransportNode;

  /// \brief Initialize a transport node.
  IgnTransportNode ignTransportInit(void);

  /// \brief Publishe a message on a topic.
  /// \param[in] _node Id of a node.
  /// \param[in] _topic Topic on which to publish the message.
  /// \param[in] _data Byte array of serialized data to publish.
  /// \param[in] _msgType Name of the message type.
  /// \return 0 on success.
  int ignTransportPublish(IgnTransportNode _node, const char *_topic,
                         const void *_data, const char *_msgType);

  /// \brief Subscribe to a topic, and register a callback.
  /// \param[in] _node Id of a node.
  /// \param[in] _topic Name of the topic.
  /// \param[in] _callback The function to call when a message is received.
  /// \return 0 on success.
  int ignTransportSubscribe(IgnTransportNode _node, const char *_topic,
      void (*_callback)(const char *, const size_t, const char *));

  /// \brief Block the current thread until a SIGINT or SIGTERM is received.
  /// Note that this function registers a signal handler. Do not use this
  /// function if you want to manage yourself SIGINT/SIGTERM.
  void ignTransportWaitForShutdown();

#ifdef __cplusplus
}
#endif

#endif
