// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_ONLINE_TRANSPORT_H_INCLUDED
#define APP_ONLINE_TRANSPORT_H_INCLUDED
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace app::online {

// Callback types for transport events
using OnMessageCallback = std::function<void(uint32_t peerId, const std::string& data)>;
using OnPeerConnectedCallback = std::function<void(uint32_t peerId)>;
using OnPeerDisconnectedCallback = std::function<void(uint32_t peerId)>;
using OnConnectedCallback = std::function<void()>;
using OnDisconnectedCallback = std::function<void(const std::string& reason)>;
using OnErrorCallback = std::function<void(const std::string& error)>;

// Abstract transport interface for online session networking.
// This provides a unified API regardless of whether we're using direct
// WebSocket server/client or a relay service like PartyKit.
class ITransport {
public:
  virtual ~ITransport() = default;

  // Start the transport (begin listening/connecting)
  virtual bool start() = 0;

  // Stop the transport
  virtual void stop() = 0;

  // Check if the transport is currently active/connected
  virtual bool isConnected() const = 0;

  // For hosts: broadcast a message to all connected peers
  virtual void broadcast(const std::vector<uint8_t>& data) = 0;

  // For hosts: send a message to a specific peer
  virtual void sendTo(uint32_t peerId, const std::vector<uint8_t>& data) = 0;

  // For guests: send a message (to the host)
  virtual void send(const std::vector<uint8_t>& data) = 0;

  // Set callbacks for transport events
  virtual void setOnMessage(OnMessageCallback cb) = 0;
  virtual void setOnPeerConnected(OnPeerConnectedCallback cb) = 0;
  virtual void setOnPeerDisconnected(OnPeerDisconnectedCallback cb) = 0;
  virtual void setOnConnected(OnConnectedCallback cb) = 0;
  virtual void setOnDisconnected(OnDisconnectedCallback cb) = 0;
  virtual void setOnError(OnErrorCallback cb) = 0;
};

// Configuration for direct (server/client) transport
struct DirectHostConfig {
  int port = 0;
  std::string bindAddress = "127.0.0.1";
};

struct DirectClientConfig {
  std::string address;
  int port = 0;
};

// Configuration for PartyKit relay transport
struct PartyKitConfig {
  std::string roomName;
  std::string relayHost = "aseprite-relay.trivaxy.partykit.dev";
  bool isHost = false;
};

// Factory functions for creating transports
std::unique_ptr<ITransport> createDirectHostTransport(const DirectHostConfig& config);
std::unique_ptr<ITransport> createDirectClientTransport(const DirectClientConfig& config);
std::unique_ptr<ITransport> createPartyKitTransport(const PartyKitConfig& config);

} // namespace app::online

#endif
