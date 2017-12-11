/*
 * Copyright (C) 2014 Open Source Robotics Foundation
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

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <zmq.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ToDo: Remove after fixing the warnings.
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <ignition/msgs.hh>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "ignition/transport/AdvertiseOptions.hh"
#include "ignition/transport/Discovery.hh"
#include "ignition/transport/Helpers.hh"
#include "ignition/transport/NodeShared.hh"
#include "ignition/transport/Packet.hh"
#include "ignition/transport/RepHandler.hh"
#include "ignition/transport/ReqHandler.hh"
#include "ignition/transport/SubscriptionHandler.hh"
#include "ignition/transport/TransportTypes.hh"
#include "ignition/transport/Uuid.hh"

#include "NodeSharedPrivate.hh"

#ifdef _MSC_VER
# pragma warning(disable: 4503)
#endif

using namespace ignition;
using namespace transport;

const std::string kIgnAuthDomain = "ign-auth";

//////////////////////////////////////////////////
// Helper to get the username and password
bool userPass(std::string &_user, std::string &_pass)
{
  char *username = std::getenv("IGN_TRANSPORT_USERNAME");
  char *password = std::getenv("IGN_TRANSPORT_PASSWORD");

  if (!username || !password)
    return false;

  _user = username;
  _pass = password;
  return true;
}

//////////////////////////////////////////////////
// Helper to send messages
int sendHelper(zmq::socket_t &_pub, const std::string &_data, int _type)
{
  zmq::message_t msg(_data.data(), _data.size());
  return _pub.send(msg, _type);
}

//////////////////////////////////////////////////
// Helper to receive messages
std::string receiveHelper(zmq::socket_t &_socket)
{
  zmq::message_t msg(0);

  if (!_socket.recv(&msg, 0))
    return std::string();

  return std::string(reinterpret_cast<char *>(msg.data()), msg.size());
}

//////////////////////////////////////////////////
// Helper to send an authentication error. This is used by basic
// authentication.
void sendAuthErrorHelper(zmq::socket_t &_socket, const std::string &_err)
{
  std::cerr << _err << std::endl;
  sendHelper(_socket, "400", ZMQ_SNDMORE);
  sendHelper(_socket, _err, ZMQ_SNDMORE);
  sendHelper(_socket, "", ZMQ_SNDMORE);
  sendHelper(_socket, "", 0);
}

//////////////////////////////////////////////////
NodeShared *NodeShared::Instance()
{
  static NodeShared instance;
  return &instance;
}

//////////////////////////////////////////////////
NodeShared::NodeShared()
  : verbose(false),
    dataPtr(new NodeSharedPrivate)
{
  // If IGN_VERBOSE=1 enable the verbose mode.
  std::string ignVerbose;
  this->verbose = (env("IGN_VERBOSE", ignVerbose) && ignVerbose == "1");

  // My process UUID.
  Uuid uuid;
  this->pUuid = uuid.ToString();

  // Initialize my discovery services.
  this->dataPtr->msgDiscovery.reset(
      new MsgDiscovery(this->pUuid, this->kMsgDiscPort));
  this->dataPtr->srvDiscovery.reset(
      new SrvDiscovery(this->pUuid, this->kSrvDiscPort));

  // Initialize the 0MQ objects.
  if (!this->InitializeSockets())
    return;

  if (this->verbose)
  {
    std::cout << "Current host address: " << this->hostAddr << std::endl;
    std::cout << "Process UUID: " << this->pUuid << std::endl;
    std::cout << "Bind at: [" << this->myAddress << "] for pub/sub\n";
    std::cout << "Bind at: [" << this->myControlAddress << "] for control\n";
    std::cout << "Bind at: [" << this->myReplierAddress << "] for srv. calls\n";
    std::cout << "Identity for receiving srv. requests: ["
              << this->replierId.ToString() << "]" << std::endl;
    std::cout << "Identity for receiving srv. responses: ["
              << this->responseReceiverId.ToString() << "]" << std::endl;
  }

  // Start the service thread.
  this->threadReception = std::thread(&NodeShared::RunReceptionTask, this);

#ifdef _WIN32
  this->threadReceptionExiting = false;
  this->threadReception.detach();
#endif

  // Set the callback to notify discovery updates (new topics).
  this->dataPtr->msgDiscovery->ConnectionsCb(
      std::bind(&NodeShared::OnNewConnection, this, std::placeholders::_1));

  // Set the callback to notify discovery updates (invalid topics).
  this->dataPtr->msgDiscovery->DisconnectionsCb(
      std::bind(&NodeShared::OnNewDisconnection, this, std::placeholders::_1));

  // Set the callback to notify svc discovery updates (new services).
  this->dataPtr->srvDiscovery->ConnectionsCb(
      std::bind(&NodeShared::OnNewSrvConnection, this, std::placeholders::_1));

  // Set the callback to notify svc discovery updates (invalid services).
  this->dataPtr->srvDiscovery->DisconnectionsCb(
      std::bind(&NodeShared::OnNewSrvDisconnection,
        this, std::placeholders::_1));

  // Start the discovery services.
  this->dataPtr->msgDiscovery->Start();
  this->dataPtr->srvDiscovery->Start();
}

//////////////////////////////////////////////////
NodeShared::~NodeShared()
{
  // Tell the service thread to terminate.
  this->dataPtr->exitMutex.lock();
  this->dataPtr->exit = true;
  this->dataPtr->exitMutex.unlock();

  // Don't join on Windows, because it can hang when this object
  // is destructed on process exit (e.g., when it's a global static).
  // I think that it's due to this bug:
  // https://connect.microsoft.com/VisualStudio/feedback/details/747145/std-thread-join-hangs-if-called-after-main-exits-when-using-vs2012-rc
#ifndef _WIN32
  // Wait for the service thread before exit.
  if (this->threadReception.joinable())
    this->threadReception.join();

  // Wait for the authentication thread before exit.
  if (this->dataPtr->accessControlThread.joinable())
    this->dataPtr->accessControlThread.join();
#else
  bool exitLoop = false;
  while (!exitLoop)
  {
    std::lock_guard<std::mutex> lock(this->dataPtr->exitMutex);
    {
      if (this->threadReceptionExiting)
        exitLoop = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // We intentionally don't destroy the context in Windows.
  // For some reason, when MATLAB deallocates the MEX file makes the context
  // destructor to hang (probably waiting for ZMQ sockets to terminate).
  // ToDo: Fix it.
#endif
}

//////////////////////////////////////////////////
void NodeShared::RunReceptionTask()
{
  bool exitLoop = false;

  // Poll socket for a reply, with timeout.
  std::vector<zmq::pollitem_t> items =
  {
    {static_cast<void*>(*this->dataPtr->subscriber), 0, ZMQ_POLLIN, 0},
    {static_cast<void*>(*this->dataPtr->control), 0, ZMQ_POLLIN, 0},
    {static_cast<void*>(*this->dataPtr->replier), 0, ZMQ_POLLIN, 0},
    {static_cast<void*>(*this->dataPtr->responseReceiver), 0, ZMQ_POLLIN, 0}
  };

  while (!exitLoop)
  {
    try
    {
      zmq::poll(&items[0], items.size(), NodeSharedPrivate::Timeout);
    }
    catch(...)
    {
      continue;
    }

    //  If we got a reply, process it.
    if (items[0].revents & ZMQ_POLLIN)
      this->RecvMsgUpdate();
    if (items[1].revents & ZMQ_POLLIN)
      this->RecvControlUpdate();
    if (items[2].revents & ZMQ_POLLIN)
      this->RecvSrvRequest();
    if (items[3].revents & ZMQ_POLLIN)
      this->RecvSrvResponse();

    /*if (items.size() > 4 && (items[4].revents & ZMQ_POLLIN))
      this->dataPtr->AccessControlHandler(sock);
      */

    // Is it time to exit?
    {
      std::lock_guard<std::mutex> lock(this->dataPtr->exitMutex);
      if (this->dataPtr->exit)
        exitLoop = true;
    }
  }
#ifdef _WIN32
  std::lock_guard<std::mutex> lock(this->dataPtr->exitMutex);
  {
    this->threadReceptionExiting = true;
  }
#endif
}

//////////////////////////////////////////////////
bool NodeShared::Publish(const std::string &_topic, const std::string &_data,
  const std::string &_msgType)
{
  try
  {
    std::lock_guard<std::recursive_mutex> lock(this->mutex);
    zmq::message_t msg;
    msg.rebuild(_topic.size());
    memcpy(msg.data(), _topic.data(), _topic.size());
    this->dataPtr->publisher->send(msg, ZMQ_SNDMORE);

    msg.rebuild(this->myAddress.size());
    memcpy(msg.data(), this->myAddress.data(), this->myAddress.size());
    this->dataPtr->publisher->send(msg, ZMQ_SNDMORE);

    msg.rebuild(_data.size());
    memcpy(msg.data(), _data.data(), _data.size());
    this->dataPtr->publisher->send(msg, ZMQ_SNDMORE);

    msg.rebuild(_msgType.size());
    memcpy(msg.data(), _msgType.data(), _msgType.size());
    this->dataPtr->publisher->send(msg, 0);
  }
  catch(const zmq::error_t& ze)
  {
     std::cerr << "NodeShared::Publish() Error: " << ze.what() << std::endl;
     return false;
  }

  return true;
}

//////////////////////////////////////////////////
void NodeShared::RecvMsgUpdate()
{
  zmq::message_t msg(0);
  std::string topic;
  // std::string sender;
  std::string data;
  std::string msgType;
  std::map<std::string, ISubscriptionHandler_M> handlers;
  ISubscriptionHandlerPtr firstSubscriberPtr;
  bool handlersFound;
  bool firstHandlerFound;

  {
    std::lock_guard<std::recursive_mutex> lock(this->mutex);

    try
    {
      if (!this->dataPtr->subscriber->recv(&msg, 0))
        return;
      topic = std::string(reinterpret_cast<char *>(msg.data()), msg.size());

      // ToDo(caguero): Use this as extra metadata for the subscriber.
      if (!this->dataPtr->subscriber->recv(&msg, 0))
        return;
      // sender = std::string(reinterpret_cast<char *>(msg.data()), msg.size());

      if (!this->dataPtr->subscriber->recv(&msg, 0))
        return;
      data = std::string(reinterpret_cast<char *>(msg.data()), msg.size());

      if (!this->dataPtr->subscriber->recv(&msg, 0))
        return;
      msgType = std::string(reinterpret_cast<char *>(msg.data()), msg.size());
    }
    catch(const zmq::error_t &_error)
    {
      std::cerr << "Error: " << _error.what() << std::endl;
      return;
    }

    handlersFound = this->localSubscriptions.Handlers(topic, handlers);
    firstHandlerFound = this->localSubscriptions.FirstHandler(topic, msgType,
      firstSubscriberPtr);
  }

  // Execute the callbacks registered.
  if (handlersFound && firstHandlerFound)
  {
    // Create the message.
    auto recvMsg = firstSubscriberPtr->CreateMsg(data, msgType);
    if (!recvMsg)
      return;

    // Remove the partition name from the topic.
    topic.erase(0, topic.find_last_of("@") + 1);

    // Create and populate the message information object.
    MessageInfo info;
    info.SetTopic(topic);

    for (const auto &node : handlers)
    {
      for (const auto &handler : node.second)
      {
        ISubscriptionHandlerPtr subscriptionHandlerPtr = handler.second;
        if (subscriptionHandlerPtr)
        {
          if (subscriptionHandlerPtr->TypeName() == msgType ||
              subscriptionHandlerPtr->TypeName() == kGenericMessageType)
            subscriptionHandlerPtr->RunLocalCallback(*recvMsg, info);
        }
        else
          std::cerr << "Subscription handler is NULL" << std::endl;
      }
    }
  }
}

//////////////////////////////////////////////////
void NodeShared::RecvControlUpdate()
{
  zmq::message_t msg(0);
  std::string topic;
  std::string procUuid;
  std::string nodeUuid;
  std::string type;
  std::string data;

  std::lock_guard<std::recursive_mutex> lock(this->mutex);

  try
  {
    if (!this->dataPtr->control->recv(&msg, 0))
      return;
    topic = std::string(reinterpret_cast<char *>(msg.data()), msg.size());

    if (!this->dataPtr->control->recv(&msg, 0))
      return;
    procUuid = std::string(reinterpret_cast<char *>(msg.data()), msg.size());

    if (!this->dataPtr->control->recv(&msg, 0))
      return;
    nodeUuid = std::string(reinterpret_cast<char *>(msg.data()), msg.size());

    if (!this->dataPtr->control->recv(&msg, 0))
      return;
    type = std::string(reinterpret_cast<char *>(msg.data()), msg.size());

    if (!this->dataPtr->control->recv(&msg, 0))
      return;
    data = std::string(reinterpret_cast<char *>(msg.data()), msg.size());
  }
  catch(const zmq::error_t &_error)
  {
    std::cerr << "NodeShared::RecvControlUpdate() error: "
              << _error.what() << std::endl;
    return;
  }

  if (std::stoi(data) == NewConnection)
  {
    if (this->verbose)
    {
      std::cout << "Registering a new remote connection" << std::endl;
      std::cout << "\tProc UUID: [" << procUuid << "]" << std::endl;
      std::cout << "\tNode UUID: [" << nodeUuid << "]" << std::endl;
    }

    // Register that we have another remote subscriber.
    MessagePublisher remoteNode(topic, "", "", procUuid, nodeUuid, type,
      AdvertiseMessageOptions());
    this->remoteSubscribers.AddPublisher(remoteNode);
  }
  else if (std::stoi(data) == EndConnection)
  {
    if (this->verbose)
    {
      std::cout << "Registering the end of a remote connection" << std::endl;
      std::cout << "\tProc UUID: " << procUuid << std::endl;
      std::cout << "\tNode UUID: [" << nodeUuid << "]" << std::endl;
    }

    // Delete a remote subscriber.
    this->remoteSubscribers.DelPublisherByNode(topic, procUuid, nodeUuid);
  }
}

//////////////////////////////////////////////////
void NodeShared::RecvSrvRequest()
{
  if (verbose)
    std::cout << "Message received requesting a service call" << std::endl;

  zmq::message_t msg(0);
  std::string topic;
  std::string sender;
  std::string nodeUuid;
  std::string reqUuid;
  std::string req;
  std::string rep;
  std::string resultStr;
  std::string dstId;
  std::string reqType;
  std::string repType;

  IRepHandlerPtr repHandler;
  bool hasHandler;

  {
    std::lock_guard<std::recursive_mutex> lock(this->mutex);

    try
    {
      if (!this->dataPtr->replier->recv(&msg, 0))
        return;

      if (!this->dataPtr->replier->recv(&msg, 0))
        return;
      topic = std::string(reinterpret_cast<char *>(msg.data()), msg.size());

      if (!this->dataPtr->replier->recv(&msg, 0))
        return;
      sender = std::string(reinterpret_cast<char *>(msg.data()), msg.size());

      if (!this->dataPtr->replier->recv(&msg, 0))
        return;
      dstId = std::string(reinterpret_cast<char *>(msg.data()), msg.size());

      if (!this->dataPtr->replier->recv(&msg, 0))
        return;
      nodeUuid = std::string(reinterpret_cast<char *>(msg.data()), msg.size());

      if (!this->dataPtr->replier->recv(&msg, 0))
        return;
      reqUuid = std::string(reinterpret_cast<char *>(msg.data()), msg.size());

      if (!this->dataPtr->replier->recv(&msg, 0))
        return;
      req = std::string(reinterpret_cast<char *>(msg.data()), msg.size());

      if (!this->dataPtr->replier->recv(&msg, 0))
        return;
      reqType = std::string(reinterpret_cast<char *>(msg.data()), msg.size());

      if (!this->dataPtr->replier->recv(&msg, 0))
        return;
      repType = std::string(reinterpret_cast<char *>(msg.data()), msg.size());
    }
    catch(const zmq::error_t &_error)
    {
      std::cerr << "NodeShared::RecvSrvRequest() error parsing request: "
                << _error.what() << std::endl;
      return;
    }

    hasHandler =
      this->repliers.FirstHandler(topic, reqType, repType, repHandler);
  }

  // Get the REP handler.
  if (hasHandler)
  {
    bool result;
    // Run the service call and get the results.
    repHandler->RunCallback(req, rep, result);

    // If 'reptype' is msgs::Empty", this is a oneway request
    // and we don't send response
    if (repType == ignition::msgs::Empty().GetTypeName())
    {
      return;
    }

    if (result)
      resultStr = "1";
    else
      resultStr = "0";

    {
      std::lock_guard<std::recursive_mutex> lock(this->mutex);
      // I am still not connected to this address.
      if (std::find(this->srvConnections.begin(), this->srvConnections.end(),
            sender) == this->srvConnections.end())
      {
        this->dataPtr->replier->connect(sender.c_str());
        this->srvConnections.push_back(sender);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (this->verbose)
        {
          std::cout << "\t* Connected to [" << sender
                    << "] for sending a response" << std::endl;
        }
      }
    }

    // Send the reply.
    try
    {
      std::lock_guard<std::recursive_mutex> lock(this->mutex);
      zmq::message_t response;

      response.rebuild(dstId.size());
      memcpy(response.data(), dstId.data(), dstId.size());
      this->dataPtr->replier->send(response, ZMQ_SNDMORE);

      response.rebuild(topic.size());
      memcpy(response.data(), topic.data(), topic.size());
      this->dataPtr->replier->send(response, ZMQ_SNDMORE);

      response.rebuild(nodeUuid.size());
      memcpy(response.data(), nodeUuid.data(), nodeUuid.size());
      this->dataPtr->replier->send(response, ZMQ_SNDMORE);

      response.rebuild(reqUuid.size());
      memcpy(response.data(), reqUuid.data(), reqUuid.size());
      this->dataPtr->replier->send(response, ZMQ_SNDMORE);

      response.rebuild(rep.size());
      memcpy(response.data(), rep.data(), rep.size());
      this->dataPtr->replier->send(response, ZMQ_SNDMORE);

      response.rebuild(resultStr.size());
      memcpy(response.data(), resultStr.data(), resultStr.size());
      this->dataPtr->replier->send(response, 0);
    }
    catch(const zmq::error_t &_error)
    {
      std::cerr << "NodeShared::RecvSrvRequest() error sending response: "
                << _error.what() << std::endl;
      return;
    }
  }
  // else
  //   std::cerr << "I do not have a service call registered for topic ["
  //             << topic << "]\n";
}

//////////////////////////////////////////////////
void NodeShared::RecvSrvResponse()
{
  if (verbose)
    std::cout << "Message received containing a service call REP" << std::endl;

  zmq::message_t msg(0);
  std::string topic;
  std::string nodeUuid;
  std::string reqUuid;
  std::string rep;
  std::string resultStr;
  bool result;

  IReqHandlerPtr reqHandlerPtr;
  bool hasHandler;

  {
    std::lock_guard<std::recursive_mutex> lock(this->mutex);

    try
    {
      if (!this->dataPtr->responseReceiver->recv(&msg, 0))
        return;

      if (!this->dataPtr->responseReceiver->recv(&msg, 0))
        return;
      topic = std::string(reinterpret_cast<char *>(msg.data()), msg.size());

      if (!this->dataPtr->responseReceiver->recv(&msg, 0))
        return;
      nodeUuid = std::string(reinterpret_cast<char *>(msg.data()), msg.size());

      if (!this->dataPtr->responseReceiver->recv(&msg, 0))
        return;
      reqUuid = std::string(reinterpret_cast<char *>(msg.data()), msg.size());

      if (!this->dataPtr->responseReceiver->recv(&msg, 0))
        return;
      rep = std::string(reinterpret_cast<char *>(msg.data()), msg.size());

      if (!this->dataPtr->responseReceiver->recv(&msg, 0))
        return;
      resultStr = std::string(reinterpret_cast<char *>(msg.data()), msg.size());
      result = resultStr == "1";
    }
    catch(const zmq::error_t &_error)
    {
      std::cerr << "NodeShared::RecvSrvResponse() error: "
                << _error.what() << std::endl;
      return;
    }

    hasHandler =
      this->requests.Handler(topic, nodeUuid, reqUuid, reqHandlerPtr);
  }

  if (hasHandler)
  {
    // Notify the result.
    reqHandlerPtr->NotifyResult(rep, result);

    // Remove the handler.
    std::lock_guard<std::recursive_mutex> lock(this->mutex);
    {
      if (!this->requests.RemoveHandler(topic, nodeUuid, reqUuid))
      {
        std::cerr << "NodeShare::RecvSrvResponse(): "
                  << "Error removing request handler" << std::endl;
      }
    }
  }
  else
  {
    std::cerr << "Received a service call response but I don't have a handler"
              << " for it" << std::endl;
  }
}

//////////////////////////////////////////////////
void NodeShared::SendPendingRemoteReqs(const std::string &_topic,
  const std::string &_reqType, const std::string &_repType)
{
  std::string responserAddr;
  std::string responserId;
  SrvAddresses_M addresses;
  this->dataPtr->srvDiscovery->Publishers(_topic, addresses);
  if (addresses.empty())
    return;

  // Find a publisher that offers this service with a particular pair of REQ/REP
  // types.
  bool found = false;
  for (auto &proc : addresses)
  {
    auto &v = proc.second;
    for (auto &pub : v)
    {
      if (pub.ReqTypeName() == _reqType && pub.RepTypeName() == _repType)
      {
        found = true;
        responserAddr = pub.Addr();
        responserId = pub.SocketId();
        break;
      }
    }
    if (found)
      break;
  }

  if (!found)
    return;

  if (verbose)
  {
    std::cout << "Found a service call responser at ["
              << responserAddr << "]" << std::endl;
  }

  std::lock_guard<std::recursive_mutex> lock(this->mutex);

  // I am still not connected to this address.
  if (std::find(this->srvConnections.begin(), this->srvConnections.end(),
        responserAddr) == this->srvConnections.end())
  {
    this->dataPtr->requester->connect(responserAddr.c_str());
    this->srvConnections.push_back(responserAddr);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (this->verbose)
    {
      std::cout << "\t* Connected to [" << responserAddr
                << "] for service requests" << std::endl;
    }
  }

  // Send all the pending REQs.
  IReqHandler_M reqs;
  if (!this->requests.Handlers(_topic, reqs))
    return;

  for (auto &node : reqs)
  {
    for (auto &req : node.second)
    {
      // Check if this service call has been already requested.
      if (req.second->Requested())
        continue;

      // Check that the pending service call has types that match the responser.
      if (req.second->ReqTypeName() != _reqType ||
          req.second->RepTypeName() != _repType)
      {
        continue;
      }

      // Mark the handler as requested.
      req.second->Requested(true);

      std::string data;
      if (!req.second->Serialize(data))
        continue;

      auto nodeUuid = req.second->NodeUuid();
      auto reqUuid = req.second->HandlerUuid();

      try
      {
        zmq::message_t msg;

        msg.rebuild(responserId.size());
        memcpy(msg.data(), responserId.data(), responserId.size());
        this->dataPtr->requester->send(msg, ZMQ_SNDMORE);

        msg.rebuild(_topic.size());
        memcpy(msg.data(), _topic.data(), _topic.size());
        this->dataPtr->requester->send(msg, ZMQ_SNDMORE);

        msg.rebuild(this->myRequesterAddress.size());
        memcpy(msg.data(), this->myRequesterAddress.data(),
          this->myRequesterAddress.size());
        this->dataPtr->requester->send(msg, ZMQ_SNDMORE);

        std::string myId = this->responseReceiverId.ToString();
        msg.rebuild(myId.size());
        memcpy(msg.data(), myId.data(), myId.size());
        this->dataPtr->requester->send(msg, ZMQ_SNDMORE);

        msg.rebuild(nodeUuid.size());
        memcpy(msg.data(), nodeUuid.data(), nodeUuid.size());
        this->dataPtr->requester->send(msg, ZMQ_SNDMORE);

        msg.rebuild(reqUuid.size());
        memcpy(msg.data(), reqUuid.data(), reqUuid.size());
        this->dataPtr->requester->send(msg, ZMQ_SNDMORE);

        msg.rebuild(data.size());
        memcpy(msg.data(), data.data(), data.size());
        this->dataPtr->requester->send(msg, ZMQ_SNDMORE);

        msg.rebuild(_reqType.size());
        memcpy(msg.data(), _reqType.data(), _reqType.size());
        this->dataPtr->requester->send(msg, ZMQ_SNDMORE);

        msg.rebuild(_repType.size());
        memcpy(msg.data(), _repType.data(), _repType.size());
        this->dataPtr->requester->send(msg, 0);
      }
      catch(const zmq::error_t& /*ze*/)
      {
        // Debug output.
        // std::cerr << "Error connecting [" << ze.what() << "]\n";
      }

      // Remove the handler associated to this service request. We won't
      // receive a response because this is a oneway request.
      if (_repType == ignition::msgs::Empty().GetTypeName())
      {
        this->requests.RemoveHandler(_topic, nodeUuid, reqUuid);
      }
    }
  }
}

//////////////////////////////////////////////////
void NodeShared::OnNewConnection(const MessagePublisher &_pub)
{
  std::lock_guard<std::recursive_mutex> lock(this->mutex);

  std::string topic = _pub.Topic();
  std::string addr = _pub.Addr();
  std::string ctrl = _pub.Ctrl();
  std::string procUuid = _pub.PUuid();
  std::string type = _pub.MsgTypeName();

  if (this->verbose)
  {
    std::cout << "Connection callback" << std::endl;
    std::cout << _pub;
  }

  // Check if we are interested in this topic.
  if (this->localSubscriptions.HasHandlersForTopic(topic) &&
      this->pUuid.compare(procUuid) != 0)
  {
    try
    {
      // Handle security
      this->dataPtr->SecurityOnNewConnection();

      // I am not connected to the process.
      if (!this->connections.HasPublisher(addr))
        this->dataPtr->subscriber->connect(addr.c_str());

      // Add a new filter for the topic.
      this->dataPtr->subscriber->setsockopt(ZMQ_SUBSCRIBE,
          topic.data(), topic.size());

      // Register the new connection with the publisher.
      this->connections.AddPublisher(_pub);

      // Send a message to the publisher's control socket to notify it
      // about all my remoteSubscribers.
      zmq::socket_t socket(*this->dataPtr->context, ZMQ_DEALER);

      if (this->verbose)
      {
        std::cout << "\t* Connected to [" << addr << "] for data\n";
        std::cout << "\t* Connected to [" << ctrl << "] for control\n";
      }

      int lingerVal = 300;
      socket.setsockopt(ZMQ_LINGER, &lingerVal, sizeof(lingerVal));
      socket.connect(ctrl.c_str());

      std::this_thread::sleep_for(std::chrono::milliseconds(100));

      std::map<std::string, ISubscriptionHandler_M> handlers;
      if (this->localSubscriptions.Handlers(topic, handlers))
      {
        for (auto const &node : handlers)
        {
          for (auto const &handler : node.second)
          {
            if (handler.second->TypeName() != kGenericMessageType &&
                handler.second->TypeName() != _pub.MsgTypeName())
            {
              continue;
            }

            std::string nodeUuid = handler.second->NodeUuid();

            zmq::message_t msg;
            msg.rebuild(topic.size());
            memcpy(msg.data(), topic.data(), topic.size());
            socket.send(msg, ZMQ_SNDMORE);

            msg.rebuild(this->pUuid.size());
            memcpy(msg.data(), this->pUuid.data(), this->pUuid.size());
            socket.send(msg, ZMQ_SNDMORE);

            msg.rebuild(nodeUuid.size());
            memcpy(msg.data(), nodeUuid.data(), nodeUuid.size());
            socket.send(msg, ZMQ_SNDMORE);

            msg.rebuild(type.size());
            memcpy(msg.data(), type.data(), type.size());
            socket.send(msg, ZMQ_SNDMORE);

            std::string data = std::to_string(NewConnection);
            msg.rebuild(data.size());
            memcpy(msg.data(), data.data(), data.size());
            socket.send(msg, 0);
          }
        }
      }
    }
    // The remote node might not be available when we are connecting.
    catch(const zmq::error_t& /*ze*/)
    {
    }
  }
}

//////////////////////////////////////////////////
void NodeShared::OnNewDisconnection(const MessagePublisher &_pub)
{
  std::lock_guard<std::recursive_mutex> lock(this->mutex);

  std::string topic = _pub.Topic();
  std::string procUuid = _pub.PUuid();
  std::string nUuid = _pub.NUuid();

  if (this->verbose)
  {
    std::cout << "New disconnection detected " << std::endl;
    std::cout << "\tProcess UUID: " << procUuid << std::endl;
  }

  // A remote subscriber[s] has been disconnected.
  if (topic != "" && nUuid != "")
  {
    this->remoteSubscribers.DelPublisherByNode(topic, procUuid, nUuid);

    MessagePublisher connection;
    if (!this->connections.Publisher(topic, procUuid, nUuid, connection))
      return;

    // I am no longer connected.
    this->connections.DelPublisherByNode(topic, procUuid, nUuid);
  }
  else
  {
    this->remoteSubscribers.DelPublishersByProc(procUuid);

    MsgAddresses_M info;
    if (!this->connections.Publishers(topic, info))
      return;

    // Remove all the connections from the process disonnected.
    this->connections.DelPublishersByProc(procUuid);
  }
}

//////////////////////////////////////////////////
void NodeShared::OnNewSrvConnection(const ServicePublisher &_pub)
{
  std::string topic = _pub.Topic();
  std::string addr = _pub.Addr();
  std::string reqType = _pub.ReqTypeName();
  std::string repType = _pub.RepTypeName();

  std::lock_guard<std::recursive_mutex> lock(this->mutex);

  if (this->verbose)
  {
    std::cout << "Service call connection callback" << std::endl;
    std::cout << _pub;
  }

  // I am still not connected to this address.
  if (std::find(this->srvConnections.begin(), this->srvConnections.end(),
        addr) == this->srvConnections.end())
  {
    this->dataPtr->requester->connect(addr.c_str());
    this->srvConnections.push_back(addr);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (this->verbose)
    {
      std::cout << "\t* Connected to [" << addr
                << "] for service requests" << std::endl;
    }
  }

  // Check if there's a pending service request with this specific combination
  // of request and response types.
  IReqHandlerPtr handler;
  if (this->requests.FirstHandler(topic, reqType, repType, handler))
  {
    // Request all pending service calls for this topic and req/rep types.
    this->SendPendingRemoteReqs(topic, reqType, repType);
  }
}

//////////////////////////////////////////////////
void NodeShared::OnNewSrvDisconnection(const ServicePublisher &_pub)
{
  std::string addr = _pub.Addr();

  std::lock_guard<std::recursive_mutex> lock(this->mutex);

  // Remove the address from the list of connected addresses.
  this->srvConnections.erase(std::remove(std::begin(this->srvConnections),
    std::end(this->srvConnections), addr.c_str()),
    std::end(this->srvConnections));

  if (this->verbose)
  {
    std::cout << "Service call disconnection callback" << std::endl;
    std::cout << _pub;
  }
}

//////////////////////////////////////////////////
bool NodeShared::InitializeSockets()
{
  try
  {
    // Set the hostname's ip address.
    this->hostAddr = this->dataPtr->msgDiscovery->HostAddr();

    // Publisher socket listening in a random port.
    std::string anyTcpEp = "tcp://" + this->hostAddr + ":*";

    // Initialize security
    this->dataPtr->SecurityInit();

    char bindEndPoint[1024];
    int lingerVal = 0;
    this->dataPtr->publisher->setsockopt(ZMQ_LINGER,
        &lingerVal, sizeof(lingerVal));
    this->dataPtr->publisher->bind(anyTcpEp.c_str());
    size_t size = sizeof(bindEndPoint);
    this->dataPtr->publisher->getsockopt(ZMQ_LAST_ENDPOINT,
        &bindEndPoint, &size);
    this->myAddress = bindEndPoint;

    // Control socket listening in a random port.
    this->dataPtr->control->bind(anyTcpEp.c_str());
    this->dataPtr->control->getsockopt(ZMQ_LAST_ENDPOINT, &bindEndPoint, &size);
    this->myControlAddress = bindEndPoint;

    // ResponseReceiver socket listening in a random port.
    std::string id = this->responseReceiverId.ToString();
    this->dataPtr->responseReceiver->setsockopt(ZMQ_IDENTITY,
        id.c_str(), id.size());
    this->dataPtr->responseReceiver->bind(anyTcpEp.c_str());
    this->dataPtr->responseReceiver->getsockopt(ZMQ_LAST_ENDPOINT,
        &bindEndPoint, &size);
    this->myRequesterAddress = bindEndPoint;

    // Replier socket listening in a random port.
    id = this->replierId.ToString();
    this->dataPtr->replier->setsockopt(ZMQ_IDENTITY, id.c_str(), id.size());
    int RouteOn = 1;
    this->dataPtr->replier->setsockopt(ZMQ_LINGER,
        &lingerVal, sizeof(lingerVal));
    this->dataPtr->replier->setsockopt(ZMQ_ROUTER_MANDATORY,
        &RouteOn, sizeof(RouteOn));
    this->dataPtr->replier->bind(anyTcpEp.c_str());
    this->dataPtr->replier->getsockopt(ZMQ_LAST_ENDPOINT, &bindEndPoint, &size);
    this->myReplierAddress = bindEndPoint;

    this->dataPtr->requester->setsockopt(ZMQ_LINGER,
        &lingerVal, sizeof(lingerVal));
    this->dataPtr->requester->setsockopt(ZMQ_ROUTER_MANDATORY, &RouteOn,
      sizeof(RouteOn));
  }
  catch(const zmq::error_t& ze)
  {
    std::cerr << "InitializeSockets() Error: " << ze.what() << std::endl;
    std::cerr << "Ignition Transport has not been correctly initialized"
              << std::endl;
    return false;
  }

  return true;
}

/////////////////////////////////////////////////
bool NodeShared::TopicPublishers(const std::string &_topic,
                                 SrvAddresses_M &_publishers) const
{
  return this->dataPtr->srvDiscovery->Publishers(_topic, _publishers);
}

/////////////////////////////////////////////////
bool NodeShared::DiscoverService(const std::string &_topic) const
{
  return this->dataPtr->srvDiscovery->Discover(_topic);
}

/////////////////////////////////////////////////
bool NodeShared::AdvertisePublisher(const ServicePublisher &_publisher)
{
  return this->dataPtr->srvDiscovery->Advertise(_publisher);
}

//////////////////////////////////////////////////
void NodeSharedPrivate::SecurityOnNewConnection()
{
  std::string user, pass;

  // Set username and pass if they exist
  // \todo: This will cause the subscriber to connect only to secure
  // connections. Would be nice if the subscriber could still connect to
  // unsecure connections. This might require an unsecure and secure
  // subscriber.
  // See issue #74
  if (userPass(user, pass))
  {
    this->subscriber->setsockopt(ZMQ_PLAIN_USERNAME, user.c_str(), user.size());
    this->subscriber->setsockopt(ZMQ_PLAIN_PASSWORD, pass.c_str(), pass.size());
  }
}

//////////////////////////////////////////////////
void NodeSharedPrivate::SecurityInit()
{
  // Check if a username and password has been set. If so, then
  // setup a PLAIN authentication server.
  std::string user, pass;
  if (userPass(user, pass))
  {
    // Create the access control thread.
    this->accessControlThread = std::thread(
        &NodeSharedPrivate::AccessControlHandler, this);

    int asServer = 1;
    this->publisher->setsockopt(ZMQ_PLAIN_SERVER, &asServer, sizeof(asServer));

    this->publisher->setsockopt(ZMQ_ZAP_DOMAIN, kIgnAuthDomain.c_str(),
        kIgnAuthDomain.size());
  }
}

//////////////////////////////////////////////////
// Access control handler for plain security.
// This function is designed to be run in a thread.
void NodeSharedPrivate::AccessControlHandler()//zmq::socket_t *sock)
{
  zmq::socket_t *sock = new zmq::socket_t(*this->context, ZMQ_REP);

  try
  {
    // Bind to the zap address
    sock->bind("inproc://zeromq.zap.01");

    // Get the username and password
    std::string user, pass;
    if (!userPass(user, pass))
    {
      std::cerr << "Username and password not set. "
                << "Authentication is disabled\n";
      return;
    }

    std::string sequence;
    std::string domain;
    std::string address;
    std::string routingId;
    std::string mechanism;
    std::string givenUsername;
    std::string givenPassword;
    std::string version;

    bool exitLoop = false;
    zmq::pollitem_t items[] =
    {
      {static_cast<void*>(*sock), 0, ZMQ_POLLIN, 0},
    };

    // Process
    while (!exitLoop)
    {
      try
      {
        zmq::poll(&items[0], sizeof(items) / sizeof(items[0]),
            NodeSharedPrivate::Timeout);
      }
      catch(...)
      {
        continue;
      }

      if (items[0].revents & ZMQ_POLLIN)
      {
        // Get the version.
        version = receiveHelper(*sock);
        if (version.empty())
          return;

        // Get remaining data
        sequence = receiveHelper(*sock);
        domain = receiveHelper(*sock);
        address = receiveHelper(*sock);
        routingId = receiveHelper(*sock);
        mechanism = receiveHelper(*sock);
        givenUsername = receiveHelper(*sock);
        givenPassword = receiveHelper(*sock);

        // Debug statements
        // std::cout << "Version[" << version << "]\n";
        // std::cout << "Sequence[" << sequence << "]\n";
        // std::cout << "Domain[" << domain << "]\n";
        // std::cout << "Address[" << address << "]\n";
        // std::cout << "Routing Id[" << routingId << "]\n";
        // std::cout << "Mechanism[" << mechanism << "]\n";
        // std::cout << "Username[" << givenUsername << "] [" << user << "]\n";
        // std::cout << "Pass[" << givenPassword << "] [" << pass << "]\n";

        // Check the version
        if (version != "1.0")
        {
          sendAuthErrorHelper(*sock, "Invalid version");
          return;
        }

        // Check the mechanism
        if (mechanism != "PLAIN")
        {
          sendAuthErrorHelper(*sock, "Invalid mechanism");
          return;
        }

        // Check the domain
        if (domain != kIgnAuthDomain)
        {
          sendAuthErrorHelper(*sock, "Invalid domain");
          return;
        }

        sendHelper(*sock, version, ZMQ_SNDMORE);
        sendHelper(*sock, sequence, ZMQ_SNDMORE);

        // Check the username and password
        if (givenUsername == user && givenPassword == pass)
        {
          sendHelper(*sock, "200", ZMQ_SNDMORE);
          sendHelper(*sock, "OK", ZMQ_SNDMORE);
          sendHelper(*sock, "anonymous", ZMQ_SNDMORE);
          sendHelper(*sock, "", 0);
        }
        else
        {
          sendAuthErrorHelper(*sock, "Invalid username or password");
        }
      }

      // Is it time to exit?
      {
        std::lock_guard<std::mutex> lock(this->exitMutex);
        if (this->exit)
          exitLoop = true;
      }
    }
  }
  catch (...)
  {
    // This catch can be triggered when ctrl-c is pressed and the context is
    // deleted. Capture this case, and quit gracefully.
  }

  sock->close();
}
