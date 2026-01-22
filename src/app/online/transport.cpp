// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/online/transport.h"

#include "app/online/online_protocol.h"
#include "base/log.h"
#include "fmt/format.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <map>
#include <mutex>

namespace app::online {

//------------------------------------------------------------------------------
// DirectHostTransport - WebSocket server for direct LAN hosting
//------------------------------------------------------------------------------

class DirectHostTransport : public ITransport {
public:
  explicit DirectHostTransport(const DirectHostConfig& config)
    : m_config(config)
  {
  }

  ~DirectHostTransport() override
  {
    stop();
  }

  bool start() override
  {
    std::lock_guard lock(m_mutex);
    if (m_server)
      return false;

    m_server = std::make_unique<ix::WebSocketServer>(m_config.port, m_config.bindAddress);
    m_server->disablePerMessageDeflate();

    m_server->setOnConnectionCallback(
      [this](std::weak_ptr<ix::WebSocket> wsWeak, std::shared_ptr<ix::ConnectionState> state) {
        if (!state)
          return;

        uint32_t peerId = 0;
        {
          std::lock_guard lock(m_mutex);
          peerId = m_nextPeerId++;
          m_clients[peerId] = ClientInfo{ wsWeak };
        }

        if (auto ws = wsWeak.lock()) {
          ws->setOnMessageCallback([this, wsWeak, peerId](const ix::WebSocketMessagePtr& msg) {
            if (!msg)
              return;

            if (msg->type == ix::WebSocketMessageType::Open) {
              // Peer connected - notify via callback
              if (m_onPeerConnected)
                m_onPeerConnected(peerId);
              return;
            }

            if (msg->type == ix::WebSocketMessageType::Close) {
              {
                std::lock_guard lock(m_mutex);
                m_clients.erase(peerId);
              }
              if (m_onPeerDisconnected)
                m_onPeerDisconnected(peerId);
              return;
            }

            if (msg->type == ix::WebSocketMessageType::Error) {
              if (m_onError)
                m_onError(msg->errorInfo.reason);
              return;
            }

            if (msg->type == ix::WebSocketMessageType::Message && msg->binary) {
              if (m_onMessage)
                m_onMessage(peerId, msg->str);
            }
          });
        }
      });

    if (!m_server->listenAndStart()) {
      m_server.reset();
      return false;
    }

    m_connected = true;
    return true;
  }

  void stop() override
  {
    std::lock_guard lock(m_mutex);
    if (m_server) {
      m_server->stop();
      m_server.reset();
    }
    m_clients.clear();
    m_connected = false;
  }

  bool isConnected() const override
  {
    std::lock_guard lock(m_mutex);
    return m_connected;
  }

  void broadcast(const std::vector<uint8_t>& data) override
  {
    std::lock_guard lock(m_mutex);
    if (!m_server)
      return;

    const std::string payload = toString(data);
    for (auto& [peerId, info] : m_clients) {
      if (auto ws = info.ws.lock()) {
        ws->sendBinary(payload);
      }
    }
  }

  void sendTo(uint32_t peerId, const std::vector<uint8_t>& data) override
  {
    std::lock_guard lock(m_mutex);
    auto it = m_clients.find(peerId);
    if (it == m_clients.end())
      return;
    if (auto ws = it->second.ws.lock()) {
      ws->sendBinary(toString(data));
    }
  }

  void send(const std::vector<uint8_t>& /*data*/) override
  {
    // Not applicable for host transport
  }

  void setOnMessage(OnMessageCallback cb) override { m_onMessage = std::move(cb); }
  void setOnPeerConnected(OnPeerConnectedCallback cb) override { m_onPeerConnected = std::move(cb); }
  void setOnPeerDisconnected(OnPeerDisconnectedCallback cb) override { m_onPeerDisconnected = std::move(cb); }
  void setOnConnected(OnConnectedCallback cb) override { m_onConnected = std::move(cb); }
  void setOnDisconnected(OnDisconnectedCallback cb) override { m_onDisconnected = std::move(cb); }
  void setOnError(OnErrorCallback cb) override { m_onError = std::move(cb); }

private:
  struct ClientInfo {
    std::weak_ptr<ix::WebSocket> ws;
  };

  DirectHostConfig m_config;
  mutable std::recursive_mutex m_mutex;

  std::unique_ptr<ix::WebSocketServer> m_server;
  std::map<uint32_t, ClientInfo> m_clients;
  uint32_t m_nextPeerId = 2; // Host is peerId 1
  bool m_connected = false;

  OnMessageCallback m_onMessage;
  OnPeerConnectedCallback m_onPeerConnected;
  OnPeerDisconnectedCallback m_onPeerDisconnected;
  OnConnectedCallback m_onConnected;
  OnDisconnectedCallback m_onDisconnected;
  OnErrorCallback m_onError;
};

//------------------------------------------------------------------------------
// DirectClientTransport - WebSocket client for direct connection to host
//------------------------------------------------------------------------------

class DirectClientTransport : public ITransport {
public:
  explicit DirectClientTransport(const DirectClientConfig& config)
    : m_config(config)
  {
  }

  ~DirectClientTransport() override
  {
    stop();
  }

  bool start() override
  {
    std::lock_guard lock(m_mutex);
    if (m_ws)
      return false;

    m_ws = std::make_unique<ix::WebSocket>();
    m_ws->disablePerMessageDeflate();
    m_ws->disableAutomaticReconnection();
    m_ws->setUrl(fmt::format("ws://{}:{}/", m_config.address, m_config.port));

    m_ws->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
      if (!msg)
        return;

      if (msg->type == ix::WebSocketMessageType::Open) {
        {
          std::lock_guard lock(m_mutex);
          m_connected = true;
        }
        if (m_onConnected)
          m_onConnected();
        return;
      }

      if (msg->type == ix::WebSocketMessageType::Close) {
        {
          std::lock_guard lock(m_mutex);
          m_connected = false;
        }
        if (m_onDisconnected)
          m_onDisconnected("Connection closed");
        return;
      }

      if (msg->type == ix::WebSocketMessageType::Error) {
        if (m_onError)
          m_onError(msg->errorInfo.reason);
        return;
      }

      if (msg->type == ix::WebSocketMessageType::Message && msg->binary) {
        // peerId 0 means "from host" for guest perspective
        if (m_onMessage)
          m_onMessage(0, msg->str);
      }
    });

    m_ws->start();
    return true;
  }

  void stop() override
  {
    std::lock_guard lock(m_mutex);
    if (m_ws) {
      m_ws->stop();
      m_ws.reset();
    }
    m_connected = false;
  }

  bool isConnected() const override
  {
    std::lock_guard lock(m_mutex);
    return m_connected;
  }

  void broadcast(const std::vector<uint8_t>& /*data*/) override
  {
    // Not applicable for client transport
  }

  void sendTo(uint32_t /*peerId*/, const std::vector<uint8_t>& /*data*/) override
  {
    // Not applicable for client transport
  }

  void send(const std::vector<uint8_t>& data) override
  {
    std::lock_guard lock(m_mutex);
    if (m_ws)
      m_ws->sendBinary(toString(data));
  }

  void setOnMessage(OnMessageCallback cb) override { m_onMessage = std::move(cb); }
  void setOnPeerConnected(OnPeerConnectedCallback cb) override { m_onPeerConnected = std::move(cb); }
  void setOnPeerDisconnected(OnPeerDisconnectedCallback cb) override { m_onPeerDisconnected = std::move(cb); }
  void setOnConnected(OnConnectedCallback cb) override { m_onConnected = std::move(cb); }
  void setOnDisconnected(OnDisconnectedCallback cb) override { m_onDisconnected = std::move(cb); }
  void setOnError(OnErrorCallback cb) override { m_onError = std::move(cb); }

private:
  DirectClientConfig m_config;
  mutable std::recursive_mutex m_mutex;

  std::unique_ptr<ix::WebSocket> m_ws;
  bool m_connected = false;

  OnMessageCallback m_onMessage;
  OnPeerConnectedCallback m_onPeerConnected;
  OnPeerDisconnectedCallback m_onPeerDisconnected;
  OnConnectedCallback m_onConnected;
  OnDisconnectedCallback m_onDisconnected;
  OnErrorCallback m_onError;
};

//------------------------------------------------------------------------------
// PartyKitTransport - Relay-based transport via PartyKit
// Works for both host and guest mode through the relay
//------------------------------------------------------------------------------

class PartyKitTransport : public ITransport {
public:
  explicit PartyKitTransport(const PartyKitConfig& config)
    : m_config(config)
  {
  }

  ~PartyKitTransport() override
  {
    stop();
  }

  bool start() override
  {
    std::lock_guard lock(m_mutex);
    if (m_ws)
      return false;

    m_ws = std::make_unique<ix::WebSocket>();
    m_ws->disablePerMessageDeflate();
    m_ws->disableAutomaticReconnection();

    std::string url = fmt::format("wss://{}/party/main/{}", m_config.relayHost, m_config.roomName);
    if (m_config.isHost)
      url += "?host=true";

    LOG(INFO, "PARTYKIT: Connecting to %s (isHost=%d)\n", url.c_str(), m_config.isHost ? 1 : 0);
    m_ws->setUrl(url);

    m_ws->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
      if (!msg)
        return;

      if (msg->type == ix::WebSocketMessageType::Open) {
        LOG(INFO, "PARTYKIT: WebSocket opened\n");
        // Wait for relay confirmation before considering connected
        return;
      }

      if (msg->type == ix::WebSocketMessageType::Close) {
        LOG(INFO, "PARTYKIT: WebSocket closed\n");
        {
          std::lock_guard lock(m_mutex);
          m_connected = false;
          m_relayConfirmed = false;
        }
        if (m_onDisconnected)
          m_onDisconnected("Connection closed");
        return;
      }

      if (msg->type == ix::WebSocketMessageType::Error) {
        LOG(WARNING, "PARTYKIT: WebSocket error: %s\n", msg->errorInfo.reason.c_str());
        if (m_onError)
          m_onError(msg->errorInfo.reason);
        return;
      }

      if (msg->type != ix::WebSocketMessageType::Message)
        return;

      // Handle relay JSON responses (before binary protocol)
      if (!m_relayConfirmed && !msg->binary) {
        handleRelayResponse(msg->str);
        return;
      }

      // Binary protocol messages
      if (msg->binary && m_onMessage) {
        if (m_config.isHost) {
          uint32_t peerId = 0;
          std::string payload;
          if (!unwrapGuestToHostEnvelope(msg->str, peerId, payload))
            return;

          m_onMessage(peerId, payload);
        }
        else {
          // Guest receives raw protocol bytes from host (no transport-level peerId).
          m_onMessage(0, msg->str);
        }
      }
    });

    m_ws->start();
    return true;
  }

  void stop() override
  {
    std::lock_guard lock(m_mutex);
    if (m_ws) {
      m_ws->stop();
      m_ws.reset();
    }
    m_connected = false;
    m_relayConfirmed = false;
    m_nextPeerId = 2;
    m_peerIdToConnId.clear();
    m_connIdToPeerId.clear();
  }

  bool isConnected() const override
  {
    std::lock_guard lock(m_mutex);
    return m_connected && m_relayConfirmed;
  }

  void broadcast(const std::vector<uint8_t>& data) override
  {
    // In PartyKit host mode, sending to the relay broadcasts to all guests
    send(data);
  }

  void sendTo(uint32_t peerId, const std::vector<uint8_t>& data) override
  {
    if (!m_config.isHost)
      return;

    std::string connId;
    {
      std::lock_guard lock(m_mutex);
      auto it = m_peerIdToConnId.find(peerId);
      if (it == m_peerIdToConnId.end())
        return;
      connId = it->second;
    }

    Writer w;
    w.writeU8(kEnvelopeMagic);
    w.writeU8(kEnvelopeHostToGuest);
    w.writeString(connId);
    w.data.insert(w.data.end(), data.begin(), data.end());
    send(w.data);
  }

  void send(const std::vector<uint8_t>& data) override
  {
    std::lock_guard lock(m_mutex);
    if (m_ws)
      m_ws->sendBinary(toString(data));
  }

  void setOnMessage(OnMessageCallback cb) override { m_onMessage = std::move(cb); }
  void setOnPeerConnected(OnPeerConnectedCallback cb) override { m_onPeerConnected = std::move(cb); }
  void setOnPeerDisconnected(OnPeerDisconnectedCallback cb) override { m_onPeerDisconnected = std::move(cb); }
  void setOnConnected(OnConnectedCallback cb) override { m_onConnected = std::move(cb); }
  void setOnDisconnected(OnDisconnectedCallback cb) override { m_onDisconnected = std::move(cb); }
  void setOnError(OnErrorCallback cb) override { m_onError = std::move(cb); }

private:
  static constexpr uint8_t kEnvelopeMagic = 0xff;
  static constexpr uint8_t kEnvelopeGuestToHost = 1;
  static constexpr uint8_t kEnvelopeHostToGuest = 2;

  bool unwrapGuestToHostEnvelope(const std::string& data, uint32_t& peerId, std::string& payload)
  {
    Reader r(data);
    uint8_t magic = 0;
    uint8_t kind = 0;
    std::string connId;
    if (!r.readU8(magic) || magic != kEnvelopeMagic || !r.readU8(kind) ||
        kind != kEnvelopeGuestToHost || !r.readString(connId))
      return false;

    payload.assign(reinterpret_cast<const char*>(r.p), reinterpret_cast<const char*>(r.end));

    {
      std::lock_guard lock(m_mutex);
      auto it = m_connIdToPeerId.find(connId);
      if (it != m_connIdToPeerId.end()) {
        peerId = it->second;
      }
      else {
        peerId = m_nextPeerId++;
        m_connIdToPeerId[connId] = peerId;
        m_peerIdToConnId[peerId] = connId;
      }
    }
    return true;
  }

  void handleRelayResponse(const std::string& text)
  {
    LOG(INFO, "PARTYKIT: Relay response: %s\n", text.c_str());

    // Simple JSON parsing for relay responses
    if (text.find("\"type\":\"host_ok\"") != std::string::npos ||
        text.find("\"type\": \"host_ok\"") != std::string::npos) {
      LOG(INFO, "PARTYKIT: Host confirmed\n");
      {
        std::lock_guard lock(m_mutex);
        m_relayConfirmed = true;
        m_connected = true;
      }
      if (m_onConnected)
        m_onConnected();
      return;
    }

    if (text.find("\"type\":\"guest_ok\"") != std::string::npos ||
        text.find("\"type\": \"guest_ok\"") != std::string::npos) {
      LOG(INFO, "PARTYKIT: Guest confirmed\n");
      {
        std::lock_guard lock(m_mutex);
        m_relayConfirmed = true;
        m_connected = true;
      }
      if (m_onConnected)
        m_onConnected();
      return;
    }

    if (text.find("\"type\":\"error\"") != std::string::npos ||
        text.find("\"type\": \"error\"") != std::string::npos) {
      // Extract error message
      std::string errMsg = "Unknown error";
      auto msgPos = text.find("\"message\"");
      if (msgPos != std::string::npos) {
        auto colonPos = text.find(':', msgPos);
        auto quoteStart = text.find('"', colonPos);
        auto quoteEnd = text.find('"', quoteStart + 1);
        if (quoteStart != std::string::npos && quoteEnd != std::string::npos) {
          errMsg = text.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
        }
      }
      LOG(WARNING, "PARTYKIT: Error from relay: %s\n", errMsg.c_str());
      if (m_onError)
        m_onError(errMsg);
      return;
    }

    if (text.find("\"type\":\"host_disconnected\"") != std::string::npos ||
        text.find("\"type\": \"host_disconnected\"") != std::string::npos) {
      LOG(INFO, "PARTYKIT: Host disconnected\n");
      if (m_onDisconnected)
        m_onDisconnected("Host disconnected");
      return;
    }
  }

  PartyKitConfig m_config;
  mutable std::recursive_mutex m_mutex;

  std::unique_ptr<ix::WebSocket> m_ws;
  bool m_connected = false;
  bool m_relayConfirmed = false;

  uint32_t m_nextPeerId = 2; // Host is 1, guests start at 2
  std::map<uint32_t, std::string> m_peerIdToConnId;
  std::map<std::string, uint32_t> m_connIdToPeerId;

  OnMessageCallback m_onMessage;
  OnPeerConnectedCallback m_onPeerConnected;
  OnPeerDisconnectedCallback m_onPeerDisconnected;
  OnConnectedCallback m_onConnected;
  OnDisconnectedCallback m_onDisconnected;
  OnErrorCallback m_onError;
};

//------------------------------------------------------------------------------
// Factory functions
//------------------------------------------------------------------------------

std::unique_ptr<ITransport> createDirectHostTransport(const DirectHostConfig& config)
{
  return std::make_unique<DirectHostTransport>(config);
}

std::unique_ptr<ITransport> createDirectClientTransport(const DirectClientConfig& config)
{
  return std::make_unique<DirectClientTransport>(config);
}

std::unique_ptr<ITransport> createPartyKitTransport(const PartyKitConfig& config)
{
  return std::make_unique<PartyKitTransport>(config);
}

} // namespace app::online
