// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/online/online_session_manager.h"

#include "app/context.h"
#include "app/context_access.h"
#include "app/doc.h"
#include "app/doc_api.h"
#include "app/file/file.h"
#include "app/online/online_protocol.h"
#include "app/online/transport.h"
#include "app/site.h"
#include "app/tools/ink.h"
#include "app/tools/tool_loop.h"
#include "app/tx.h"
#include "app/util/expand_cel_canvas.h"
#include "base/fs.h"
#include "base/log.h"
#include "doc/cel.h"
#include "doc/image.h"
#include "doc/layer.h"
#include "doc/sprite.h"
#include "filters/tiled_mode.h"
#include "fmt/format.h"
#include "ui/alert.h"
#include "ui/system.h"
#include "app/ui/online_session_window.h"

#include <fstream>
#include <algorithm>
#include <cstring>
#include <ixwebsocket/IXNetSystem.h>

namespace app::online {

static OnlineSessionManager* g_instance = nullptr;

namespace {

static std::string snapshotFilename()
{
  return base::join_path(".build", "online_session_snapshot.aseprite");
}

static std::vector<uint8_t> readFileBytes(const std::string& filename)
{
  std::ifstream f(filename, std::ios::binary);
  if (!f)
    return {};

  f.seekg(0, std::ios::end);
  const auto size = static_cast<size_t>(f.tellg());
  f.seekg(0, std::ios::beg);

  std::vector<uint8_t> bytes(size);
  if (size)
    f.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  return bytes;
}

static bool writeFileBytes(const std::string& filename, const std::vector<uint8_t>& bytes)
{
  std::ofstream f(filename, std::ios::binary | std::ios::trunc);
  if (!f)
    return false;
  if (!bytes.empty())
    f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return bool(f);
}

static gfx::Rect clipRectToImage(const gfx::Rect& rc, const doc::Image* img)
{
  if (!img)
    return gfx::Rect();
  gfx::Rect bounds = img->bounds();
  gfx::Rect out = rc;
  out &= bounds;
  return out;
}

static bool buildLayerPath(const doc::Layer* layer, std::vector<uint32_t>& out)
{
  out.clear();
  if (!layer)
    return false;

  const doc::Sprite* sprite = layer->sprite();
  if (!sprite)
    return false;

  const doc::Layer* cur = layer;
  const doc::LayerGroup* parent = cur->parent();
  std::vector<uint32_t> reversed;

  while (parent) {
    const auto& children = parent->layers();
    auto it = std::find(children.begin(), children.end(), cur);
    if (it == children.end())
      return false;
    reversed.push_back(uint32_t(std::distance(children.begin(), it)));
    cur = parent;
    parent = cur->parent();
  }

  out.assign(reversed.rbegin(), reversed.rend());
  return true;
}

static doc::Layer* resolveLayerPath(doc::Sprite* sprite, const std::vector<uint32_t>& path)
{
  if (!sprite || path.empty())
    return nullptr;

  doc::Layer* cur = sprite->root();
  for (size_t i = 0; i < path.size(); ++i) {
    if (!cur || !cur->isGroup())
      return nullptr;
    auto* group = static_cast<doc::LayerGroup*>(cur);
    const auto& children = group->layers();
    const uint32_t idx = path[i];
    if (idx >= children.size())
      return nullptr;
    cur = children[idx];
  }
  return cur;
}

class ScopedOnlineSessionWrite {
public:
  explicit ScopedOnlineSessionWrite(app::Doc* doc) : m_doc(doc)
  {
    if (m_doc) {
      m_restore = m_doc->isOnlineSessionReadOnly();
      if (m_restore)
        m_doc->setOnlineSessionReadOnly(false);
    }
  }

  ~ScopedOnlineSessionWrite()
  {
    if (m_doc && m_restore)
      m_doc->setOnlineSessionReadOnly(true);
  }

private:
  app::Doc* m_doc = nullptr;
  bool m_restore = false;
};

} // namespace

//------------------------------------------------------------------------------
// OnlineSessionManager - Singleton
//------------------------------------------------------------------------------

OnlineSessionManager* OnlineSessionManager::instance()
{
  if (!g_instance)
    g_instance = new OnlineSessionManager;
  return g_instance;
}

OnlineSessionManager::OnlineSessionManager()
{
  ix::initNetSystem();
}

bool OnlineSessionManager::isActive() const
{
  std::lock_guard lock(m_mutex);
  return m_role != Role::None;
}

bool OnlineSessionManager::isHost() const
{
  std::lock_guard lock(m_mutex);
  return m_role == Role::Host;
}

bool OnlineSessionManager::isGuest() const
{
  std::lock_guard lock(m_mutex);
  return m_role == Role::Guest;
}

Doc* OnlineSessionManager::document() const
{
  std::lock_guard lock(m_mutex);
  return m_doc;
}

Permissions OnlineSessionManager::localPermissions() const
{
  std::lock_guard lock(m_mutex);
  return m_localPerms;
}

bool OnlineSessionManager::localCanEditCanvas() const
{
  std::lock_guard lock(m_mutex);
  return (m_localPerms & kPermEditCanvas) != 0;
}

bool OnlineSessionManager::localCanEditLayers() const
{
  std::lock_guard lock(m_mutex);
  return (m_localPerms & kPermEditLayers) != 0;
}

bool OnlineSessionManager::localCanLockLayers() const
{
  std::lock_guard lock(m_mutex);
  return (m_localPerms & kPermLockLayers) != 0;
}

bool OnlineSessionManager::localCanEditTimeline() const
{
  std::lock_guard lock(m_mutex);
  return (m_localPerms & kPermEditTimeline) != 0;
}

void OnlineSessionManager::attachWindow(OnlineSessionWindow* window)
{
  std::lock_guard lock(m_mutex);
  m_window = window;
  updateWindow();
}

void OnlineSessionManager::detachWindow(OnlineSessionWindow* window)
{
  std::lock_guard lock(m_mutex);
  if (m_window == window)
    m_window = nullptr;
}

std::vector<Peer> OnlineSessionManager::peers() const
{
  std::lock_guard lock(m_mutex);
  std::vector<Peer> out;
  out.reserve(m_peers.size());
  for (const auto& [id, peer] : m_peers)
    out.push_back(peer);
  return out;
}

std::string OnlineSessionManager::chatLog() const
{
  std::lock_guard lock(m_mutex);
  return m_chatLog;
}

void OnlineSessionManager::appendChatLine(const std::string& line)
{
  if (!m_chatLog.empty())
    m_chatLog.push_back('\n');
  m_chatLog += line;
}

void OnlineSessionManager::updateWindow()
{
  if (m_window)
    m_window->refresh();
}

void OnlineSessionManager::stopNoLock()
{
  if (m_doc)
    m_doc->setOnlineSessionReadOnly(false);

  if (m_transport)
    m_transport->stop();

  m_transport.reset();

  m_role = Role::None;
  m_connType = ConnectionType::None;
  m_doc = nullptr;
  m_ctx = nullptr;
  m_roomName.clear();
  m_localUsername.clear();
  m_password.clear();
  m_localPeerId = 0;
  m_localPerms = 0;
  m_peers.clear();
  m_hostPeerInfo.clear();
  m_snapshotBytes.clear();
  m_pendingSnapshot = PendingSnapshot();
  m_chatLog.clear();
  m_nextClientOpId = 1;
  m_nextRev = 1;
  m_nextPeerId = 2;
}

//------------------------------------------------------------------------------
// Transport callback setup - unified for all transport types
//------------------------------------------------------------------------------

void OnlineSessionManager::setupTransportCallbacks()
{
  if (!m_transport)
    return;

  m_transport->setOnMessage([this](uint32_t peerId, const std::string& data) {
    ui::execute_from_ui_thread([this, peerId, data] {
      handleMessage(peerId, data);
    });
  });

  m_transport->setOnPeerConnected([this](uint32_t peerId) {
    ui::execute_from_ui_thread([this, peerId] {
      std::lock_guard lock(m_mutex);
      // Peer connected but not yet authenticated (Direct mode only)
      // PartyKit handles this differently via Hello message
      if (m_role == Role::Host && m_connType == ConnectionType::Direct) {
        m_hostPeerInfo[peerId] = HostPeerInfo{};
      }
    });
  });

  m_transport->setOnPeerDisconnected([this](uint32_t peerId) {
    ui::execute_from_ui_thread([this, peerId] {
      std::lock_guard lock(m_mutex);
      if (m_role == Role::Host) {
        auto it = m_hostPeerInfo.find(peerId);
        if (it != m_hostPeerInfo.end()) {
          bool wasAuth = it->second.authenticated;
          std::string name = it->second.username;
          m_hostPeerInfo.erase(it);
          m_peers.erase(peerId);
          if (wasAuth) {
            appendChatLine(fmt::format("{} disconnected.", name.empty() ? fmt::format("Guest {}", peerId) : name));
            updateWindow();
          }
        }
      }
    });
  });

  m_transport->setOnConnected([this]() {
    ui::execute_from_ui_thread([this] {
      std::lock_guard lock(m_mutex);
      if (m_role == Role::Host) {
        // Host is now ready to receive connections
        if (m_connType == ConnectionType::PartyKit) {
          appendChatLine(fmt::format("Hosting room '{}'", m_roomName));
        }
        else {
          appendChatLine("Server started.");
        }
      }
      else if (m_role == Role::Guest) {
        // Guest connected, send Hello
        appendChatLine("Connected. Sending credentials...");
        Writer w;
        w.writeU8(uint8_t(MsgType::Hello));
        w.writeString(m_localUsername);
        w.writeString(m_password);
        sendToHost(w.data);
      }
      updateWindow();
    });
  });

  m_transport->setOnDisconnected([this](const std::string& reason) {
    ui::execute_from_ui_thread([this, reason] {
      std::lock_guard lock(m_mutex);
      appendChatLine(fmt::format("Disconnected: {}", reason));
      updateWindow();
    });
  });

  m_transport->setOnError([this](const std::string& error) {
    ui::execute_from_ui_thread([this, error] {
      std::lock_guard lock(m_mutex);
      appendChatLine(fmt::format("Error: {}", error));
      updateWindow();
    });
  });
}

//------------------------------------------------------------------------------
// Unified message handling
//------------------------------------------------------------------------------

void OnlineSessionManager::handleMessage(uint32_t senderPeerId, const std::string& data)
{
  std::lock_guard lock(m_mutex);

  Reader r(data);
  uint8_t type8 = 0;
  if (!r.readU8(type8))
    return;

  const auto type = MsgType(type8);

  if (m_role == Role::Host) {
    handleHostMessage(senderPeerId, type, r);
  }
  else if (m_role == Role::Guest) {
    handleGuestMessage(type, r);
  }
}

void OnlineSessionManager::handleHostMessage(uint32_t senderPeerId, MsgType type, Reader& r)
{
  // In Direct mode, senderPeerId is the actual peer ID from the transport layer.
  // In PartyKit mode, newer relays/transports can provide a stable senderPeerId too.
  // Keep fallback logic for senderPeerId==0 for older PartyKit relays.

  switch (type) {
    case MsgType::Hello:
      handleHello(senderPeerId, r);
      break;

    case MsgType::OpPropose:
      handleOpPropose(senderPeerId, r);
      break;

    case MsgType::ChatSend:
      handleChatSend(senderPeerId, r);
      break;

    case MsgType::CursorPosition:
      handleCursorPosition(senderPeerId, r);
      break;

    case MsgType::CursorHide:
      handleCursorHide(senderPeerId, r);
      break;

    default:
      break;
  }
}

void OnlineSessionManager::handleGuestMessage(MsgType type, Reader& r)
{
  switch (type) {
    case MsgType::Error: {
      std::string text;
      if (r.readString(text)) {
        ui::Alert::show(fmt::format("Connection Error<<{}", text));
      }
      break;
    }

    case MsgType::Welcome: {
      uint32_t peerId = 0;
      uint32_t perms = 0;
      if (r.readU32(peerId) && r.readU32(perms)) {
        onWelcome(peerId, perms);
      }
      break;
    }

    case MsgType::SnapshotBegin: {
      uint32_t sizeBytes = 0;
      if (r.readU32(sizeBytes)) {
        onSnapshotBegin(sizeBytes);
      }
      break;
    }

    case MsgType::SnapshotData: {
      std::vector<uint8_t> chunk;
      if (r.readBytes(chunk)) {
        onSnapshotData(chunk);
      }
      break;
    }

    case MsgType::SnapshotEnd: {
      onSnapshotEnd();
      break;
    }

    case MsgType::Op: {
      uint64_t rev = 0;
      uint32_t authorPeerId = 0;
      uint8_t opType8 = 0;
      if (r.readU64(rev) && r.readU32(authorPeerId) && r.readU8(opType8)) {
        const auto opType = OpType(opType8);
        onOp(rev, authorPeerId, opType, r);
      }
      break;
    }

    case MsgType::PermissionsSet: {
      uint32_t peerId = 0;
      uint32_t perms = 0;
      if (r.readU32(peerId) && r.readU32(perms)) {
        onPermissionsSet(peerId, perms);
      }
      break;
    }

    case MsgType::Kick: {
      std::string reason;
      r.readString(reason);
      onKick(reason);
      break;
    }

    case MsgType::ChatBroadcast: {
      uint32_t peerId = 0;
      std::string text;
      if (r.readU32(peerId) && r.readString(text)) {
        onChatBroadcast(peerId, text);
      }
      break;
    }

    case MsgType::CursorPosition: {
      uint32_t peerId = 0;
      int32_t x = 0, y = 0;
      if (r.readU32(peerId) && r.readS32(x) && r.readS32(y)) {
        onCursorPosition(peerId, x, y);
      }
      break;
    }

    case MsgType::CursorHide: {
      uint32_t peerId = 0;
      if (r.readU32(peerId)) {
        onCursorHide(peerId);
      }
      break;
    }

    default:
      break;
  }
}

//------------------------------------------------------------------------------
// Host-side message handlers
//------------------------------------------------------------------------------

void OnlineSessionManager::handleHello(uint32_t senderPeerId, Reader& r)
{
  std::string user, pass;
  if (!r.readString(user) || !r.readString(pass))
    return;

  LOG(INFO, "ONLINE: Received Hello from user='%s'\n", user.c_str());

  // Validate Password
  if (!m_password.empty() && m_password != pass) {
    Writer w;
    w.writeU8(uint8_t(MsgType::Error));
    w.writeString("Invalid password");
    if (senderPeerId != 0) {
      sendToPeer(senderPeerId, w.data);
    }
    else {
      broadcastMessage(w.data);
    }
    return;
  }

  // Validate Username
  for (const auto& [id, p] : m_peers) {
    if (p.name == user) {
      Writer w;
      w.writeU8(uint8_t(MsgType::Error));
      w.writeString("Username taken");
      if (senderPeerId != 0) {
        sendToPeer(senderPeerId, w.data);
      }
      else {
        broadcastMessage(w.data);
      }
      return;
    }
  }

  uint32_t peerId = senderPeerId;
  if (peerId == 0) {
    peerId = m_nextPeerId++;
  }

  // Register the peer
  m_hostPeerInfo[peerId] = HostPeerInfo{ 0, user, true };
  m_peers[peerId] = Peer{ peerId, 0, user };

  // Send Welcome
  Writer w;
  w.writeU8(uint8_t(MsgType::Welcome));
  w.writeU32(peerId);
  w.writeU32(0); // Default permissions (viewer)
  sendToPeer(peerId, w.data);

  // Send snapshot
  {
    Writer wb;
    wb.writeU8(uint8_t(MsgType::SnapshotBegin));
    wb.writeU32(uint32_t(m_snapshotBytes.size()));
    sendToPeer(peerId, wb.data);
  }

  static constexpr size_t kChunkSize = 128 * 1024;
  size_t offset = 0;
  while (offset < m_snapshotBytes.size()) {
    const size_t remaining = m_snapshotBytes.size() - offset;
    const size_t n = (remaining < kChunkSize ? remaining : kChunkSize);
    std::vector<uint8_t> chunk(m_snapshotBytes.begin() + offset, m_snapshotBytes.begin() + offset + n);
    Writer wd;
    wd.writeU8(uint8_t(MsgType::SnapshotData));
    wd.writeBytes(chunk);
    sendToPeer(peerId, wd.data);
    offset += n;
  }

  {
    Writer we;
    we.writeU8(uint8_t(MsgType::SnapshotEnd));
    sendToPeer(peerId, we.data);
  }

  appendChatLine(fmt::format("{} connected.", user));
  updateWindow();
}

void OnlineSessionManager::handleOpPropose(uint32_t senderPeerId, Reader& r)
{
  uint32_t claimedPeerId = 0;
  uint64_t clientOpId = 0;
  uint8_t opType8 = 0;
  if (!r.readU32(claimedPeerId) || !r.readU64(clientOpId) || !r.readU8(opType8))
    return;

  // For PartyKit, use claimed peerId; for Direct, verify it matches
  uint32_t peerId = (senderPeerId == 0) ? claimedPeerId : senderPeerId;
  if (senderPeerId != 0 && claimedPeerId != senderPeerId)
    return;

  const auto opType = OpType(opType8);
  const std::vector<uint8_t> payload(r.p, r.end);

  if (!m_doc)
    return;

  // Permission check (host authoritative)
  Permissions required = 0;
  switch (opType) {
    case OpType::SetPixelsRect: required = kPermEditCanvas; break;
    case OpType::NewFrame:
    case OpType::RemoveFrame: required = kPermEditTimeline; break;
    case OpType::NewLayer:
    case OpType::RemoveLayer: required = kPermEditLayers; break;
    case OpType::LayerLock: required = kPermLockLayers; break;
  }

  auto it = m_hostPeerInfo.find(peerId);
  if (it == m_hostPeerInfo.end())
    return;
  if ((it->second.perms & required) != required) {
    Writer rej;
    rej.writeU8(uint8_t(MsgType::OpRejected));
    rej.writeU64(clientOpId);
    rej.writeString("Permission denied");
    sendToPeer(peerId, rej.data);
    return;
  }

  // Decode + apply the operation
  const std::string payloadStr = toString(payload);
  Reader opReader(payloadStr);
  bool ok = true;

  switch (opType) {
    case OpType::SetPixelsRect: {
      uint32_t frame = 0;
      uint32_t pathLen = 0;
      std::vector<uint32_t> layerPath;
      int32_t x = 0, y = 0, w = 0, h = 0;
      std::vector<uint8_t> bytes;
      if (!opReader.readU32(frame) || !opReader.readU32(pathLen)) {
        ok = false;
        break;
      }
      if (pathLen > 64) {
        ok = false;
        break;
      }
      layerPath.reserve(pathLen);
      for (uint32_t i = 0; i < pathLen; ++i) {
        uint32_t idx = 0;
        if (!opReader.readU32(idx)) {
          ok = false;
          break;
        }
        layerPath.push_back(idx);
      }
      if (!ok)
        break;
      if (!opReader.readS32(x) || !opReader.readS32(y) || !opReader.readS32(w) ||
          !opReader.readS32(h) || !opReader.readBytes(bytes)) {
        ok = false;
        break;
      }

      auto* sprite = m_doc->sprite();
      doc::Layer* layer = resolveLayerPath(sprite, layerPath);
      if (!layer || !layer->isImage() || !layer->isEditable()) {
        ok = false;
        break;
      }

      applySetPixelsRect(doc::frame_t(frame), layerPath, gfx::Rect(x, y, w, h), bytes);
      break;
    }
    case OpType::NewFrame: {
      std::string content;
      uint32_t insertAt = 0;
      if (!opReader.readString(content) || !opReader.readU32(insertAt)) {
        ok = false;
        break;
      }
      applyNewFrame(content, doc::frame_t(insertAt));
      break;
    }
    case OpType::RemoveFrame: {
      uint32_t frame = 0;
      if (!opReader.readU32(frame)) {
        ok = false;
        break;
      }
      applyRemoveFrame(doc::frame_t(frame));
      break;
    }
    case OpType::NewLayer: {
      uint32_t pathLen = 0;
      std::vector<uint32_t> afterPath;
      std::string name;
      if (!opReader.readU32(pathLen)) {
        ok = false;
        break;
      }
      if (pathLen > 64) {
        ok = false;
        break;
      }
      afterPath.reserve(pathLen);
      for (uint32_t i = 0; i < pathLen; ++i) {
        uint32_t idx = 0;
        if (!opReader.readU32(idx)) {
          ok = false;
          break;
        }
        afterPath.push_back(idx);
      }
      if (!ok)
        break;
      if (!opReader.readString(name)) {
        ok = false;
        break;
      }
      applyNewLayer(afterPath, name);
      break;
    }
    case OpType::RemoveLayer: {
      uint32_t pathLen = 0;
      std::vector<uint32_t> layerPath;
      if (!opReader.readU32(pathLen)) {
        ok = false;
        break;
      }
      if (pathLen > 64) {
        ok = false;
        break;
      }
      layerPath.reserve(pathLen);
      for (uint32_t i = 0; i < pathLen; ++i) {
        uint32_t idx = 0;
        if (!opReader.readU32(idx)) {
          ok = false;
          break;
        }
        layerPath.push_back(idx);
      }
      if (!ok)
        break;
      applyRemoveLayer(layerPath);
      break;
    }
    case OpType::LayerLock: {
      uint32_t pathLen = 0;
      std::vector<uint32_t> layerPath;
      uint8_t locked = 0;
      if (!opReader.readU32(pathLen)) {
        ok = false;
        break;
      }
      if (pathLen > 64) {
        ok = false;
        break;
      }
      layerPath.reserve(pathLen);
      for (uint32_t i = 0; i < pathLen; ++i) {
        uint32_t idx = 0;
        if (!opReader.readU32(idx)) {
          ok = false;
          break;
        }
        layerPath.push_back(idx);
      }
      if (!ok)
        break;
      if (!opReader.readU8(locked)) {
        ok = false;
        break;
      }
      applyLayerLock(layerPath, locked != 0);
      break;
    }
  }

  if (!ok) {
    Writer rej;
    rej.writeU8(uint8_t(MsgType::OpRejected));
    rej.writeU64(clientOpId);
    rej.writeString("Invalid operation");
    sendToPeer(peerId, rej.data);
    return;
  }

  // Broadcast accepted op
  Writer w;
  w.writeU8(uint8_t(MsgType::Op));
  w.writeU64(m_nextRev++);
  w.writeU32(peerId);
  w.writeU8(uint8_t(opType));
  w.data.insert(w.data.end(), payload.begin(), payload.end());
  broadcastMessage(w.data);
}

void OnlineSessionManager::handleChatSend(uint32_t senderPeerId, Reader& r)
{
  uint32_t claimedPeerId = 0;
  std::string text;
  if (!r.readU32(claimedPeerId) || !r.readString(text))
    return;

  uint32_t peerId = (senderPeerId == 0) ? claimedPeerId : senderPeerId;
  if (senderPeerId != 0 && claimedPeerId != senderPeerId)
    return;

  // Broadcast chat
  Writer w;
  w.writeU8(uint8_t(MsgType::ChatBroadcast));
  w.writeU32(peerId);
  w.writeString(text);
  broadcastMessage(w.data);

  appendChatLine(fmt::format("[{}] {}", peerId, text));
  updateWindow();
}

void OnlineSessionManager::handleCursorPosition(uint32_t senderPeerId, Reader& r)
{
  uint32_t claimedPeerId = 0;
  int32_t x = 0, y = 0;
  if (!r.readU32(claimedPeerId) || !r.readS32(x) || !r.readS32(y))
    return;

  uint32_t peerId = (senderPeerId == 0) ? claimedPeerId : senderPeerId;
  if (senderPeerId != 0 && claimedPeerId != senderPeerId)
    return;

  // Validate peer has canvas edit permission
  auto it = m_hostPeerInfo.find(peerId);
  if (it == m_hostPeerInfo.end())
    return;
  if ((it->second.perms & kPermEditCanvas) == 0)
    return;

  // Update stored cursor position
  auto peerIt = m_peers.find(peerId);
  if (peerIt != m_peers.end()) {
    peerIt->second.cursorPos = gfx::Point(x, y);
    peerIt->second.cursorVisible = true;
  }

  // Broadcast to all clients
  Writer w;
  w.writeU8(uint8_t(MsgType::CursorPosition));
  w.writeU32(peerId);
  w.writeS32(x);
  w.writeS32(y);
  broadcastMessage(w.data);
}

void OnlineSessionManager::handleCursorHide(uint32_t senderPeerId, Reader& r)
{
  uint32_t claimedPeerId = 0;
  if (!r.readU32(claimedPeerId))
    return;

  uint32_t peerId = (senderPeerId == 0) ? claimedPeerId : senderPeerId;
  if (senderPeerId != 0 && claimedPeerId != senderPeerId)
    return;

  // Update stored cursor visibility
  auto peerIt = m_peers.find(peerId);
  if (peerIt != m_peers.end()) {
    peerIt->second.cursorVisible = false;
  }

  // Broadcast to all clients
  Writer w;
  w.writeU8(uint8_t(MsgType::CursorHide));
  w.writeU32(peerId);
  broadcastMessage(w.data);
}

//------------------------------------------------------------------------------
// Guest-side response handlers
//------------------------------------------------------------------------------

void OnlineSessionManager::onWelcome(uint32_t peerId, Permissions perms)
{
  if (m_localPeerId != 0 && m_localPeerId != peerId)
    return;

  m_localPeerId = peerId;
  m_localPerms = perms;
  m_peers[1] = Peer{ 1, kPermEditCanvas | kPermEditLayers | kPermLockLayers | kPermEditTimeline, "Host" };
  std::string name = "You";
  if (!m_localUsername.empty())
    name = fmt::format("{} ({})", m_localUsername, peerId);
  else
    name = fmt::format("You ({})", peerId);

  m_peers[peerId] = Peer{ peerId, perms, name };
  appendChatLine(fmt::format("Connected. Permissions: {}", perms));
  updateWindow();
}

void OnlineSessionManager::onSnapshotBegin(uint32_t sizeBytes)
{
  m_pendingSnapshot.inProgress = true;
  m_pendingSnapshot.sizeBytes = sizeBytes;
  m_pendingSnapshot.bytes.clear();
  m_pendingSnapshot.bytes.reserve(sizeBytes);
  appendChatLine(fmt::format("Receiving snapshot ({} bytes)...", sizeBytes));
  updateWindow();
}

void OnlineSessionManager::onSnapshotData(const std::vector<uint8_t>& chunk)
{
  if (!m_pendingSnapshot.inProgress)
    return;
  m_pendingSnapshot.bytes.insert(m_pendingSnapshot.bytes.end(), chunk.begin(), chunk.end());
}

void OnlineSessionManager::onSnapshotEnd()
{
  m_pendingSnapshot.inProgress = false;
  std::vector<uint8_t> bytes;
  bytes.swap(m_pendingSnapshot.bytes);
  appendChatLine("Snapshot received.");

  if (!m_ctx) {
    ui::Alert::show("Cannot load snapshot (no context).");
    return;
  }
  if (bytes.empty()) {
    ui::Alert::show("Cannot load snapshot (empty).");
    return;
  }

  const std::string fn = snapshotFilename();
  base::make_all_directories(base::get_file_path(fn));
  if (!writeFileBytes(fn, bytes)) {
    ui::Alert::show("Cannot write snapshot file.");
    return;
  }

  std::unique_ptr<Doc> doc(load_document(m_ctx, fn));
  if (!doc) {
    ui::Alert::show("Cannot load snapshot.");
    return;
  }

  Doc* raw = doc.release();
  raw->setContext(m_ctx);
  m_ctx->documents().add(raw);
  m_ctx->setActiveDocument(raw);

  m_doc = raw;
  raw->setOnlineSessionReadOnly(m_localPerms == 0);

  updateWindow();
}

void OnlineSessionManager::onOp(uint64_t rev, uint32_t authorPeerId, OpType opType, Reader& opReader)
{
  if (!m_doc)
    return;

  if (base::get_log_level() >= VERBOSE) {
    LOG(VERBOSE,
        "ONLINE: recv Op: rev=%u author=%u type=%u\n",
        unsigned(rev),
        unsigned(authorPeerId),
        unsigned(opType));
  }

  switch (opType) {
    case OpType::SetPixelsRect: {
      uint32_t frame = 0;
      uint32_t pathLen = 0;
      std::vector<uint32_t> layerPath;
      int32_t x = 0, y = 0, w = 0, h = 0;
      std::vector<uint8_t> bytes;
      if (!opReader.readU32(frame) || !opReader.readU32(pathLen))
        return;
      if (pathLen > 64)
        return;
      layerPath.reserve(pathLen);
      for (uint32_t i = 0; i < pathLen; ++i) {
        uint32_t idx = 0;
        if (!opReader.readU32(idx))
          return;
        layerPath.push_back(idx);
      }
      if (!opReader.readS32(x) || !opReader.readS32(y) || !opReader.readS32(w) || !opReader.readS32(h))
        return;
      uint32_t nBytes = 0;
      if (!opReader.readU32(nBytes))
        return;
      if (!opReader.ok(nBytes))
        return;
      bytes.assign(opReader.p, opReader.p + nBytes);
      opReader.p += nBytes;
      applySetPixelsRect(doc::frame_t(frame), layerPath, gfx::Rect(x, y, w, h), bytes);
      break;
    }
    case OpType::NewFrame: {
      std::string content;
      uint32_t insertAt = 0;
      if (!opReader.readString(content) || !opReader.readU32(insertAt))
        return;
      applyNewFrame(content, doc::frame_t(insertAt));
      break;
    }
    case OpType::RemoveFrame: {
      uint32_t frame = 0;
      if (!opReader.readU32(frame))
        return;
      applyRemoveFrame(doc::frame_t(frame));
      break;
    }
    case OpType::NewLayer: {
      uint32_t pathLen = 0;
      std::vector<uint32_t> afterPath;
      std::string name;
      if (!opReader.readU32(pathLen))
        return;
      if (pathLen > 64)
        return;
      afterPath.reserve(pathLen);
      for (uint32_t i = 0; i < pathLen; ++i) {
        uint32_t idx = 0;
        if (!opReader.readU32(idx))
          return;
        afterPath.push_back(idx);
      }
      if (!opReader.readString(name))
        return;
      applyNewLayer(afterPath, name);
      break;
    }
    case OpType::RemoveLayer: {
      uint32_t pathLen = 0;
      std::vector<uint32_t> layerPath;
      if (!opReader.readU32(pathLen))
        return;
      if (pathLen > 64)
        return;
      layerPath.reserve(pathLen);
      for (uint32_t i = 0; i < pathLen; ++i) {
        uint32_t idx = 0;
        if (!opReader.readU32(idx))
          return;
        layerPath.push_back(idx);
      }
      applyRemoveLayer(layerPath);
      break;
    }
    case OpType::LayerLock: {
      uint32_t pathLen = 0;
      std::vector<uint32_t> layerPath;
      uint8_t locked = 0;
      if (!opReader.readU32(pathLen))
        return;
      if (pathLen > 64)
        return;
      layerPath.reserve(pathLen);
      for (uint32_t i = 0; i < pathLen; ++i) {
        uint32_t idx = 0;
        if (!opReader.readU32(idx))
          return;
        layerPath.push_back(idx);
      }
      if (!opReader.readU8(locked))
        return;
      applyLayerLock(layerPath, locked != 0);
      break;
    }
  }
}

void OnlineSessionManager::onPermissionsSet(uint32_t peerId, Permissions perms)
{
  auto& p = m_peers[peerId];
  p.id = peerId;
  p.perms = perms;
  if (peerId == m_localPeerId) {
    m_localPerms = perms;
    if (m_doc)
      m_doc->setOnlineSessionReadOnly(m_localPerms == 0);
    appendChatLine(fmt::format("Permissions updated: {}", perms));
  }
  updateWindow();
}

void OnlineSessionManager::onKick(const std::string& reason)
{
  ui::Alert::show(fmt::format("You were kicked.\n{}", reason).c_str());
  leave();
}

void OnlineSessionManager::onChatBroadcast(uint32_t peerId, const std::string& text)
{
  appendChatLine(fmt::format("[{}] {}", peerId, text));
  updateWindow();
}

void OnlineSessionManager::onCursorPosition(uint32_t peerId, int32_t x, int32_t y)
{
  auto it = m_peers.find(peerId);
  if (it != m_peers.end()) {
    it->second.cursorPos = gfx::Point(x, y);
    it->second.cursorVisible = true;
  }
}

void OnlineSessionManager::onCursorHide(uint32_t peerId)
{
  auto it = m_peers.find(peerId);
  if (it != m_peers.end()) {
    it->second.cursorVisible = false;
  }
}

//------------------------------------------------------------------------------
// Transport helpers
//------------------------------------------------------------------------------

void OnlineSessionManager::broadcastMessage(const std::vector<uint8_t>& data)
{
  if (m_transport)
    m_transport->broadcast(data);
}

void OnlineSessionManager::sendToHost(const std::vector<uint8_t>& data)
{
  if (m_transport)
    m_transport->send(data);
}

void OnlineSessionManager::sendToPeer(uint32_t peerId, const std::vector<uint8_t>& data)
{
  if (m_transport)
    m_transport->sendTo(peerId, data);
}

//------------------------------------------------------------------------------
// Public API: Start/Join/Leave
//------------------------------------------------------------------------------

bool OnlineSessionManager::startHost(Context* ctx, Doc* doc, int port, const std::string& username, const std::string& password, const std::string& bindAddress)
{
  if (!ctx || !doc) {
    ui::Alert::show("No active document to host.");
    return false;
  }

  std::lock_guard lock(m_mutex);
  stopNoLock();

  m_snapshotBytes = buildSnapshotBytes(doc);
  if (m_snapshotBytes.empty()) {
    ui::Alert::show("Failed to build snapshot.");
    stopNoLock();
    return false;
  }

  m_role = Role::Host;
  m_connType = ConnectionType::Direct;
  m_doc = doc;
  m_ctx = ctx;
  m_localUsername = username.empty() ? "Host" : username;
  m_password = password;
  m_localPeerId = 1;
  m_localPerms = (kPermEditCanvas | kPermEditLayers | kPermLockLayers | kPermEditTimeline);
  m_peers.clear();
  m_peers[m_localPeerId] = Peer{ m_localPeerId, m_localPerms, m_localUsername };

  DirectHostConfig config;
  config.port = port;
  config.bindAddress = bindAddress;
  m_transport = createDirectHostTransport(config);

  setupTransportCallbacks();

  if (!m_transport->start()) {
    ui::Alert::show("Failed to start server.");
    stopNoLock();
    return false;
  }

  appendChatLine(fmt::format("Hosting on {}:{}", bindAddress, port));
  updateWindow();
  return true;
}

bool OnlineSessionManager::join(Context* ctx, const std::string& address, int port, const std::string& username, const std::string& password)
{
  if (!ctx) {
    ui::Alert::show("No context.");
    return false;
  }

  std::lock_guard lock(m_mutex);
  stopNoLock();

  m_role = Role::Guest;
  m_connType = ConnectionType::Direct;
  m_ctx = ctx;
  m_doc = nullptr;
  m_localUsername = username;
  m_password = password;
  m_localPeerId = 0;
  m_localPerms = 0;
  m_peers.clear();

  DirectClientConfig config;
  config.address = address;
  config.port = port;
  m_transport = createDirectClientTransport(config);

  setupTransportCallbacks();
  m_transport->start();

  appendChatLine(fmt::format("Connecting to {}:{}", address, port));
  updateWindow();
  return true;
}

bool OnlineSessionManager::startHostPartyKit(Context* ctx, Doc* doc, const std::string& roomName, const std::string& username, const std::string& password)
{
  if (!ctx || !doc) {
    ui::Alert::show("No active document to host.");
    return false;
  }

  if (roomName.empty()) {
    ui::Alert::show("Room name cannot be empty.");
    return false;
  }

  std::lock_guard lock(m_mutex);
  stopNoLock();

  m_snapshotBytes = buildSnapshotBytes(doc);
  if (m_snapshotBytes.empty()) {
    ui::Alert::show("Failed to build snapshot.");
    stopNoLock();
    return false;
  }

  m_role = Role::Host;
  m_connType = ConnectionType::PartyKit;
  m_doc = doc;
  m_ctx = ctx;
  m_roomName = roomName;
  m_localUsername = username.empty() ? "Host" : username;
  m_password = password;
  m_localPeerId = 1;
  m_localPerms = (kPermEditCanvas | kPermEditLayers | kPermLockLayers | kPermEditTimeline);
  m_peers.clear();
  m_peers[m_localPeerId] = Peer{ m_localPeerId, m_localPerms, m_localUsername };

  PartyKitConfig config;
  config.roomName = roomName;
  config.isHost = true;
  m_transport = createPartyKitTransport(config);

  setupTransportCallbacks();
  m_transport->start();

  appendChatLine(fmt::format("Connecting to room '{}'...", roomName));
  updateWindow();
  return true;
}

bool OnlineSessionManager::joinPartyKit(Context* ctx, const std::string& roomName, const std::string& username, const std::string& password)
{
  if (!ctx) {
    ui::Alert::show("No context.");
    return false;
  }

  if (roomName.empty()) {
    ui::Alert::show("Room name cannot be empty.");
    return false;
  }

  std::lock_guard lock(m_mutex);
  stopNoLock();

  m_role = Role::Guest;
  m_connType = ConnectionType::PartyKit;
  m_ctx = ctx;
  m_roomName = roomName;
  m_doc = nullptr;
  m_localUsername = username;
  m_password = password;
  m_localPeerId = 0;
  m_localPerms = 0;
  m_peers.clear();

  PartyKitConfig config;
  config.roomName = roomName;
  config.isHost = false;
  m_transport = createPartyKitTransport(config);

  setupTransportCallbacks();
  m_transport->start();

  appendChatLine(fmt::format("Connecting to room '{}'...", roomName));
  updateWindow();
  return true;
}

void OnlineSessionManager::leave()
{
  std::lock_guard lock(m_mutex);
  stopNoLock();
  updateWindow();
}

//------------------------------------------------------------------------------
// Paint stroke syncing
//------------------------------------------------------------------------------

void OnlineSessionManager::onPaintStrokeCommitted(tools::ToolLoop* toolLoop,
                                                  const gfx::Region& dirtyArea)
{
  if (!toolLoop || !toolLoop->getInk() || !toolLoop->getInk()->isPaint())
    return;

  if (dirtyArea.isEmpty())
    return;

  std::lock_guard lock(m_mutex);
  if (m_role == Role::None || !m_doc || m_doc != toolLoop->getDocument())
    return;

  if (m_role == Role::Guest && (m_localPerms & kPermEditCanvas) == 0)
    return;

  auto* layer = toolLoop->getLayer();
  if (!layer)
    return;

  std::vector<uint32_t> layerPath;
  if (!buildLayerPath(layer, layerPath))
    return;

  gfx::Rect spriteRc = dirtyArea.bounds();
  static constexpr int kStrokeMargin = 4;
  spriteRc = spriteRc.enlarge(kStrokeMargin);
  auto* spr = toolLoop->sprite();
  if (spr)
    spriteRc &= spr->bounds();

  if (spriteRc.isEmpty())
    return;

  const doc::frame_t frame = toolLoop->getFrame();
  doc::Cel* cel = layer->cel(frame);
  doc::Image* img = (cel ? cel->image() : nullptr);

  const int bpp = (img ? img->bytesPerPixel() : (spr ? spr->spec().bytesPerPixel() : 0));
  if (bpp <= 0)
    return;
  const doc::color_t maskColor =
    (img ? img->maskColor() : (spr ? spr->transparentColor() : doc::color_t(0)));
  const size_t strideBytes = size_t(spriteRc.w) * size_t(bpp);
  std::vector<uint8_t> bytes(size_t(spriteRc.w) * size_t(spriteRc.h) * size_t(bpp));

  if (maskColor == 0) {
    std::fill(bytes.begin(), bytes.end(), 0);
  }
  else {
    uint8_t maskBytes[4] = { 0, 0, 0, 0 };
    std::memcpy(maskBytes, &maskColor, (std::min)(4, bpp));
    for (size_t i = 0; i < bytes.size(); i += size_t(bpp))
      std::memcpy(bytes.data() + i, maskBytes, size_t(bpp));
  }

  if (img) {
    const gfx::Point celPos = cel->position();
    const gfx::Rect celSpriteRc(celPos.x, celPos.y, img->width(), img->height());
    const gfx::Rect overlapSpriteRc = spriteRc.createIntersection(celSpriteRc);
    if (!overlapSpriteRc.isEmpty()) {
      for (int y = overlapSpriteRc.y; y < overlapSpriteRc.y2(); ++y) {
        const int imgY = y - celPos.y;
        const int imgX0 = overlapSpriteRc.x - celPos.x;
        const uint8_t* src = img->getPixelAddress(imgX0, imgY);

        const int dstX0 = overlapSpriteRc.x - spriteRc.x;
        const size_t dstOffset =
          (size_t(y - spriteRc.y) * size_t(spriteRc.w) + size_t(dstX0)) * size_t(bpp);
        const size_t rowBytes = size_t(overlapSpriteRc.w) * size_t(bpp);
        std::memcpy(bytes.data() + dstOffset, src, rowBytes);
      }
    }
  }

  Writer payload;
  payload.writeU32(uint32_t(frame));
  payload.writeU32(uint32_t(layerPath.size()));
  for (uint32_t idx : layerPath)
    payload.writeU32(idx);
  payload.writeS32(spriteRc.x);
  payload.writeS32(spriteRc.y);
  payload.writeS32(spriteRc.w);
  payload.writeS32(spriteRc.h);
  payload.writeBytes(bytes);

  if (m_role == Role::Host) {
    const uint64_t rev = m_nextRev++;
    Writer w;
    w.writeU8(uint8_t(MsgType::Op));
    w.writeU64(rev);
    w.writeU32(1);
    w.writeU8(uint8_t(OpType::SetPixelsRect));
    w.data.insert(w.data.end(), payload.data.begin(), payload.data.end());
    broadcastMessage(w.data);
  }
  else if (m_role == Role::Guest) {
    Writer w;
    w.writeU8(uint8_t(MsgType::OpPropose));
    w.writeU32(m_localPeerId);
    w.writeU64(m_nextClientOpId++);
    w.writeU8(uint8_t(OpType::SetPixelsRect));
    w.data.insert(w.data.end(), payload.data.begin(), payload.data.end());
    sendToHost(w.data);
  }
}

//------------------------------------------------------------------------------
// Frame/Layer requests
//------------------------------------------------------------------------------

bool OnlineSessionManager::requestNewFrame(Context* ctx,
                                          const std::string& content,
                                          doc::frame_t insertAt)
{
  std::lock_guard lock(m_mutex);
  if (m_role == Role::None || !m_doc || !ctx || ctx->activeDocument() != m_doc)
    return false;

  if (m_role == Role::Guest) {
    if ((m_localPerms & kPermEditTimeline) == 0)
      return true;

    Writer payload;
    payload.writeString(content);
    payload.writeU32(uint32_t(insertAt));

    Writer w;
    w.writeU8(uint8_t(MsgType::OpPropose));
    w.writeU32(m_localPeerId);
    w.writeU64(m_nextClientOpId++);
    w.writeU8(uint8_t(OpType::NewFrame));
    w.data.insert(w.data.end(), payload.data.begin(), payload.data.end());
    sendToHost(w.data);
    return true;
  }

  applyNewFrame(content, insertAt);
  {
    Writer payload;
    payload.writeString(content);
    payload.writeU32(uint32_t(insertAt));
    Writer w;
    w.writeU8(uint8_t(MsgType::Op));
    w.writeU64(m_nextRev++);
    w.writeU32(1);
    w.writeU8(uint8_t(OpType::NewFrame));
    w.data.insert(w.data.end(), payload.data.begin(), payload.data.end());
    broadcastMessage(w.data);
  }
  return true;
}

bool OnlineSessionManager::requestRemoveFrame(Context* ctx, doc::frame_t frame)
{
  std::lock_guard lock(m_mutex);
  if (m_role == Role::None || !m_doc || !ctx || ctx->activeDocument() != m_doc)
    return false;

  if (m_role == Role::Guest) {
    if ((m_localPerms & kPermEditTimeline) == 0)
      return true;

    Writer payload;
    payload.writeU32(uint32_t(frame));
    Writer w;
    w.writeU8(uint8_t(MsgType::OpPropose));
    w.writeU32(m_localPeerId);
    w.writeU64(m_nextClientOpId++);
    w.writeU8(uint8_t(OpType::RemoveFrame));
    w.data.insert(w.data.end(), payload.data.begin(), payload.data.end());
    sendToHost(w.data);
    return true;
  }

  applyRemoveFrame(frame);
  {
    Writer payload;
    payload.writeU32(uint32_t(frame));
    Writer w;
    w.writeU8(uint8_t(MsgType::Op));
    w.writeU64(m_nextRev++);
    w.writeU32(1);
    w.writeU8(uint8_t(OpType::RemoveFrame));
    w.data.insert(w.data.end(), payload.data.begin(), payload.data.end());
    broadcastMessage(w.data);
  }
  return true;
}

bool OnlineSessionManager::requestNewLayer(Context* ctx, doc::Layer* afterLayer, const std::string& name)
{
  std::lock_guard lock(m_mutex);
  if (m_role == Role::None || !m_doc || !ctx || ctx->activeDocument() != m_doc)
    return false;

  std::vector<uint32_t> afterPath;
  if (afterLayer && !buildLayerPath(afterLayer, afterPath))
    afterPath.clear();

  if (m_role == Role::Guest) {
    if ((m_localPerms & kPermEditLayers) == 0)
      return true;

    Writer payload;
    payload.writeU32(uint32_t(afterPath.size()));
    for (uint32_t idx : afterPath)
      payload.writeU32(idx);
    payload.writeString(name);
    Writer w;
    w.writeU8(uint8_t(MsgType::OpPropose));
    w.writeU32(m_localPeerId);
    w.writeU64(m_nextClientOpId++);
    w.writeU8(uint8_t(OpType::NewLayer));
    w.data.insert(w.data.end(), payload.data.begin(), payload.data.end());
    sendToHost(w.data);
    return true;
  }

  applyNewLayer(afterPath, name);
  {
    Writer payload;
    payload.writeU32(uint32_t(afterPath.size()));
    for (uint32_t idx : afterPath)
      payload.writeU32(idx);
    payload.writeString(name);
    Writer w;
    w.writeU8(uint8_t(MsgType::Op));
    w.writeU64(m_nextRev++);
    w.writeU32(1);
    w.writeU8(uint8_t(OpType::NewLayer));
    w.data.insert(w.data.end(), payload.data.begin(), payload.data.end());
    broadcastMessage(w.data);
  }
  return true;
}

bool OnlineSessionManager::requestRemoveLayer(Context* ctx, doc::Layer* layer)
{
  std::lock_guard lock(m_mutex);
  if (m_role == Role::None || !m_doc || !ctx || ctx->activeDocument() != m_doc)
    return false;

  std::vector<uint32_t> layerPath;
  if (!layer || !buildLayerPath(layer, layerPath))
    return false;

  if (m_role == Role::Guest) {
    if ((m_localPerms & kPermEditLayers) == 0)
      return true;

    Writer payload;
    payload.writeU32(uint32_t(layerPath.size()));
    for (uint32_t idx : layerPath)
      payload.writeU32(idx);
    Writer w;
    w.writeU8(uint8_t(MsgType::OpPropose));
    w.writeU32(m_localPeerId);
    w.writeU64(m_nextClientOpId++);
    w.writeU8(uint8_t(OpType::RemoveLayer));
    w.data.insert(w.data.end(), payload.data.begin(), payload.data.end());
    sendToHost(w.data);
    return true;
  }

  applyRemoveLayer(layerPath);
  {
    Writer payload;
    payload.writeU32(uint32_t(layerPath.size()));
    for (uint32_t idx : layerPath)
      payload.writeU32(idx);
    Writer w;
    w.writeU8(uint8_t(MsgType::Op));
    w.writeU64(m_nextRev++);
    w.writeU32(1);
    w.writeU8(uint8_t(OpType::RemoveLayer));
    w.data.insert(w.data.end(), payload.data.begin(), payload.data.end());
    broadcastMessage(w.data);
  }
  return true;
}

bool OnlineSessionManager::requestLayerLock(Context* ctx, doc::Layer* layer, bool locked)
{
  std::lock_guard lock(m_mutex);
  if (m_role == Role::None || !m_doc || !ctx || ctx->activeDocument() != m_doc)
    return false;

  std::vector<uint32_t> layerPath;
  if (!layer || !buildLayerPath(layer, layerPath))
    return false;

  if (m_role == Role::Guest) {
    if ((m_localPerms & kPermLockLayers) == 0)
      return true;

    Writer payload;
    payload.writeU32(uint32_t(layerPath.size()));
    for (uint32_t idx : layerPath)
      payload.writeU32(idx);
    payload.writeU8(locked ? 1 : 0);
    Writer w;
    w.writeU8(uint8_t(MsgType::OpPropose));
    w.writeU32(m_localPeerId);
    w.writeU64(m_nextClientOpId++);
    w.writeU8(uint8_t(OpType::LayerLock));
    w.data.insert(w.data.end(), payload.data.begin(), payload.data.end());
    sendToHost(w.data);
    return true;
  }

  applyLayerLock(layerPath, locked);
  {
    Writer payload;
    payload.writeU32(uint32_t(layerPath.size()));
    for (uint32_t idx : layerPath)
      payload.writeU32(idx);
    payload.writeU8(locked ? 1 : 0);
    Writer w;
    w.writeU8(uint8_t(MsgType::Op));
    w.writeU64(m_nextRev++);
    w.writeU32(1);
    w.writeU8(uint8_t(OpType::LayerLock));
    w.data.insert(w.data.end(), payload.data.begin(), payload.data.end());
    broadcastMessage(w.data);
  }
  return true;
}

//------------------------------------------------------------------------------
// Host moderation
//------------------------------------------------------------------------------

void OnlineSessionManager::hostSetGuestPermissions(uint32_t peerId, Permissions perms)
{
  std::lock_guard lock(m_mutex);
  if (m_role != Role::Host || peerId == 1)
    return;

  auto it = m_hostPeerInfo.find(peerId);
  if (it == m_hostPeerInfo.end())
    return;

  it->second.perms = perms;
  m_peers[peerId].perms = perms;

  Writer w;
  w.writeU8(uint8_t(MsgType::PermissionsSet));
  w.writeU32(peerId);
  w.writeU32(perms);
  broadcastMessage(w.data);
}

void OnlineSessionManager::hostKick(uint32_t peerId, const std::string& reason)
{
  std::lock_guard lock(m_mutex);
  if (m_role != Role::Host || peerId == 1)
    return;

  Writer w;
  w.writeU8(uint8_t(MsgType::Kick));
  w.writeString(reason);
  sendToPeer(peerId, w.data);

  m_hostPeerInfo.erase(peerId);
  m_peers.erase(peerId);
}

//------------------------------------------------------------------------------
// Chat
//------------------------------------------------------------------------------

void OnlineSessionManager::sendChat(const std::string& text)
{
  if (text.empty())
    return;

  std::lock_guard lock(m_mutex);
  if (m_role == Role::Host) {
    Writer w;
    w.writeU8(uint8_t(MsgType::ChatBroadcast));
    w.writeU32(1);
    w.writeString(text);
    broadcastMessage(w.data);
    appendChatLine(fmt::format("[1] {}", text));
    updateWindow();
  }
  else if (m_role == Role::Guest) {
    Writer w;
    w.writeU8(uint8_t(MsgType::ChatSend));
    w.writeU32(m_localPeerId);
    w.writeString(text);
    sendToHost(w.data);
  }
}

//------------------------------------------------------------------------------
// Cursor sharing
//------------------------------------------------------------------------------

void OnlineSessionManager::sendCursorPosition(int x, int y)
{
  std::lock_guard lock(m_mutex);
  if (m_role == Role::None)
    return;

  if ((m_localPerms & kPermEditCanvas) == 0)
    return;

  m_pendingCursorPos = gfx::Point(x, y);
  m_cursorDirty = true;
  m_cursorWasVisible = true;

  Writer w;
  w.writeU8(uint8_t(MsgType::CursorPosition));
  w.writeU32(m_localPeerId);
  w.writeS32(x);
  w.writeS32(y);

  if (m_role == Role::Host) {
    broadcastMessage(w.data);
  }
  else if (m_role == Role::Guest) {
    sendToHost(w.data);
  }
}

void OnlineSessionManager::sendCursorHide()
{
  std::lock_guard lock(m_mutex);
  if (m_role == Role::None)
    return;

  if (!m_cursorWasVisible)
    return;

  m_cursorWasVisible = false;
  m_cursorDirty = false;

  Writer w;
  w.writeU8(uint8_t(MsgType::CursorHide));
  w.writeU32(m_localPeerId);

  if (m_role == Role::Host) {
    broadcastMessage(w.data);
  }
  else if (m_role == Role::Guest) {
    sendToHost(w.data);
  }
}

std::vector<Peer> OnlineSessionManager::peersWithVisibleCursors() const
{
  std::lock_guard lock(m_mutex);
  std::vector<Peer> result;
  for (const auto& [id, peer] : m_peers) {
    if (id == m_localPeerId)
      continue;
    if (peer.cursorVisible)
      result.push_back(peer);
  }
  return result;
}

//------------------------------------------------------------------------------
// Apply operations (shared between host and guest)
//------------------------------------------------------------------------------

void OnlineSessionManager::applySetPixelsRect(doc::frame_t frame,
                                              const std::vector<uint32_t>& layerPath,
                                              const gfx::Rect& rcIn,
                                              const std::vector<uint8_t>& bytes)
{
  if (base::get_log_level() >= VERBOSE) {
    LOG(VERBOSE,
        "ONLINE: applySetPixelsRect: frame=%d rc=(%d,%d %dx%d) bytes=%u pathLen=%u\n",
        int(frame),
        rcIn.x,
        rcIn.y,
        rcIn.w,
        rcIn.h,
        unsigned(bytes.size()),
        unsigned(layerPath.size()));
  }

  Doc* doc = m_doc;
  if (!doc)
    return;

  ScopedOnlineSessionWrite writeScope(doc);

  auto* sprite = doc->sprite();
  if (!sprite)
    return;

  doc::Layer* layer = resolveLayerPath(sprite, layerPath);
  if (!layer || !layer->isImage() || layer->sprite() != sprite)
    return;

  Tx tx(doc, "Online: Set Pixels");

  Site site;
  site.document(doc);
  site.sprite(sprite);
  site.layer(layer);
  site.frame(frame);
  site.focus(Site::InEditor);

  ExpandCelCanvas canvas(site, layer, filters::TiledMode::NONE, tx, ExpandCelCanvas::NeedsSource);
  doc::Image* dst = canvas.getDestCanvas();
  if (!dst)
    return;

  gfx::Rect spriteRc = rcIn;
  const gfx::Point celOrigin = canvas.getCelOrigin();
  gfx::Rect dstRc = spriteRc;
  dstRc.offset(-celOrigin);

  gfx::Rect clippedDstRc = clipRectToImage(dstRc, dst);
  if (clippedDstRc.isEmpty())
    return;

  canvas.validateDestCanvas(gfx::Region(spriteRc));

  const int bpp = dst->bytesPerPixel();
  const size_t expected = size_t(spriteRc.w) * size_t(spriteRc.h) * size_t(bpp);
  if (bytes.size() < expected)
    return;

  const int xSkip = clippedDstRc.x - dstRc.x;
  const int ySkip = clippedDstRc.y - dstRc.y;
  const size_t rowBytes = size_t(clippedDstRc.w) * size_t(bpp);

  for (int y = 0; y < clippedDstRc.h; ++y) {
    uint8_t* out = dst->getPixelAddress(clippedDstRc.x, clippedDstRc.y + y);
    const size_t srcOffset = (size_t(ySkip + y) * size_t(spriteRc.w) + size_t(xSkip)) * size_t(bpp);
    std::memcpy(out, bytes.data() + srcOffset, rowBytes);
  }

  canvas.commit();
  tx.commit();
}

void OnlineSessionManager::applyNewFrame(const std::string& content, doc::frame_t insertAt)
{
  Doc* doc = m_doc;
  if (!doc)
    return;

  ScopedOnlineSessionWrite writeScope(doc);

  auto* sprite = doc->sprite();
  if (!sprite)
    return;

  Tx tx(doc, "Online: New Frame");
  DocApi api = doc->getApi(tx);
  if (content == "empty")
    api.addEmptyFrame(sprite, insertAt);
  else
    api.addFrame(sprite, insertAt);
  tx.commit();
}

void OnlineSessionManager::applyRemoveFrame(doc::frame_t frame)
{
  Doc* doc = m_doc;
  if (!doc)
    return;

  ScopedOnlineSessionWrite writeScope(doc);

  auto* sprite = doc->sprite();
  if (!sprite || sprite->totalFrames() <= 1)
    return;

  Tx tx(doc, "Online: Remove Frame");
  DocApi api = doc->getApi(tx);
  api.removeFrame(sprite, frame);
  tx.commit();
}

void OnlineSessionManager::applyNewLayer(const std::vector<uint32_t>& afterLayerPath,
                                         const std::string& name)
{
  Doc* doc = m_doc;
  if (!doc)
    return;

  ScopedOnlineSessionWrite writeScope(doc);

  auto* sprite = doc->sprite();
  if (!sprite)
    return;

  doc::Layer* afterLayer = resolveLayerPath(sprite, afterLayerPath);
  if (!afterLayer || afterLayer->sprite() != sprite)
    afterLayer = sprite->root()->lastLayer();

  doc::LayerGroup* parent = (afterLayer ? afterLayer->parent() : sprite->root());
  if (!parent)
    parent = sprite->root();

  Tx tx(doc, "Online: New Layer");
  DocApi api = doc->getApi(tx);
  api.newLayerAfter(parent, name.empty() ? "Layer" : name, afterLayer);
  tx.commit();
}

void OnlineSessionManager::applyRemoveLayer(const std::vector<uint32_t>& layerPath)
{
  Doc* doc = m_doc;
  if (!doc)
    return;

  ScopedOnlineSessionWrite writeScope(doc);

  doc::Layer* layer = resolveLayerPath(doc->sprite(), layerPath);
  if (!layer)
    return;

  Tx tx(doc, "Online: Remove Layer");
  DocApi api = doc->getApi(tx);
  api.removeLayer(layer);
  tx.commit();
}

void OnlineSessionManager::applyLayerLock(const std::vector<uint32_t>& layerPath, bool locked)
{
  Doc* doc = m_doc;
  if (!doc)
    return;

  ScopedOnlineSessionWrite writeScope(doc);

  doc::Layer* layer = resolveLayerPath(doc->sprite(), layerPath);
  if (!layer)
    return;

  doc->setLayerEditableWithNotifications(layer, !locked);
}

std::vector<uint8_t> OnlineSessionManager::buildSnapshotBytes(Doc* doc)
{
  if (!doc)
    return {};

  const std::string fn = snapshotFilename();
  base::make_all_directories(base::get_file_path(fn));

  FileOpROI roi(doc, doc->sprite()->bounds(), "", "", FramesSequence(), false);
  std::unique_ptr<FileOp> fop(FileOp::createSaveDocumentOperation(nullptr, roi, fn, "", false));
  if (!fop)
    return {};

  fop->operate();
  fop->done();
  if (fop->hasError())
    return {};

  return readFileBytes(fn);
}

} // namespace app::online
