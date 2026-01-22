// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_ONLINE_SESSION_MANAGER_H_INCLUDED
#define APP_ONLINE_SESSION_MANAGER_H_INCLUDED
#pragma once

#include "app/online/online_protocol.h"
#include "doc/frame.h"
#include "gfx/point.h"
#include "gfx/rect.h"
#include "gfx/region.h"
#include "obs/signal.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace app {
class Context;
class Doc;
} // namespace app

namespace doc {
class Layer;
} // namespace doc

namespace app { namespace tools {
class ToolLoop;
}} // namespace app::tools

namespace app::online {

class ITransport;

using Permissions = uint32_t;
static constexpr Permissions kPermEditCanvas = 1u << 0;
static constexpr Permissions kPermEditLayers = 1u << 1;
static constexpr Permissions kPermLockLayers = 1u << 2;
static constexpr Permissions kPermEditTimeline = 1u << 3;

struct Peer {
  uint32_t id = 0;
  Permissions perms = 0;
  std::string name;
  gfx::Point cursorPos{0, 0};
  bool cursorVisible = false;
};

class OnlineSessionWindow;

class OnlineSessionManager {
public:
  static OnlineSessionManager* instance();

  bool isActive() const;
  bool isHost() const;
  bool isGuest() const;

  Doc* document() const;

  Permissions localPermissions() const;
  bool localCanEditCanvas() const;
  bool localCanEditLayers() const;
  bool localCanLockLayers() const;
  bool localCanEditTimeline() const;

  // Direct connection methods (existing)
  bool startHost(Context* ctx, Doc* doc, int port, const std::string& username, const std::string& password, const std::string& bindAddress = "127.0.0.1");
  bool join(Context* ctx, const std::string& address, int port, const std::string& username, const std::string& password);

  // PartyKit relay methods
  bool startHostPartyKit(Context* ctx, Doc* doc, const std::string& roomName, const std::string& username, const std::string& password);
  bool joinPartyKit(Context* ctx, const std::string& roomName, const std::string& username, const std::string& password);

  void leave();

  void attachWindow(OnlineSessionWindow* window);
  void detachWindow(OnlineSessionWindow* window);

  std::vector<Peer> peers() const;
  std::string chatLog() const;

  void onPaintStrokeCommitted(tools::ToolLoop* toolLoop, const gfx::Region& dirtyArea);

  bool requestNewFrame(Context* ctx, const std::string& content, doc::frame_t insertAt);
  bool requestRemoveFrame(Context* ctx, doc::frame_t frame);
  bool requestNewLayer(Context* ctx, doc::Layer* afterLayer, const std::string& name);
  bool requestRemoveLayer(Context* ctx, doc::Layer* layer);
  bool requestLayerLock(Context* ctx, doc::Layer* layer, bool locked);

  void hostSetGuestPermissions(uint32_t peerId, Permissions perms);
  void hostKick(uint32_t peerId, const std::string& reason);

  void sendChat(const std::string& text);

  // Cursor sharing
  void sendCursorPosition(int x, int y);
  void sendCursorHide();
  std::vector<Peer> peersWithVisibleCursors() const;

  // Signals
  obs::signal<void(uint32_t peerId, const gfx::Point& oldPos, const gfx::Point& newPos)> ClientCursorChange;
  obs::signal<void(uint32_t peerId, const gfx::Point& pos)> ClientCursorHide;

private:
  OnlineSessionManager();
  void stopNoLock();

  enum class Role { None, Host, Guest };
  enum class ConnectionType { None, Direct, PartyKit };

  struct PendingSnapshot {
    bool inProgress = false;
    uint32_t sizeBytes = 0;
    std::vector<uint8_t> bytes;
  };

  // Per-peer state tracked by the host
  struct HostPeerInfo {
    Permissions perms = 0;
    std::string username;
    bool authenticated = false;
  };

  void updateWindow();
  void appendChatLine(const std::string& line);

  // Unified message handling
  void handleMessage(uint32_t senderPeerId, const std::string& data);
  void handleHostMessage(uint32_t senderPeerId, MsgType type, Reader& r);
  void handleGuestMessage(MsgType type, Reader& r);

  // Host-side message handlers
  void handleHello(uint32_t senderPeerId, Reader& r);
  void handleOpPropose(uint32_t senderPeerId, Reader& r);
  void handleChatSend(uint32_t senderPeerId, Reader& r);
  void handleCursorPosition(uint32_t senderPeerId, Reader& r);
  void handleCursorHide(uint32_t senderPeerId, Reader& r);

  // Guest-side response handlers
  void onWelcome(uint32_t peerId, Permissions perms);
  void onSnapshotBegin(uint32_t sizeBytes);
  void onSnapshotData(const std::vector<uint8_t>& chunk);
  void onSnapshotEnd();
  void onOp(uint64_t rev, uint32_t authorPeerId, OpType opType, Reader& opReader);
  void onPermissionsSet(uint32_t peerId, Permissions perms);
  void onKick(const std::string& reason);
  void onChatBroadcast(uint32_t peerId, const std::string& text);
  void onCursorPosition(uint32_t peerId, int32_t x, int32_t y);
  void onCursorHide(uint32_t peerId);

  void applySetPixelsRect(doc::frame_t frame,
                          const std::vector<uint32_t>& layerPath,
                          const gfx::Rect& rc,
                          const std::vector<uint8_t>& bytes);
  void applyNewFrame(const std::string& content, doc::frame_t insertAt);
  void applyRemoveFrame(doc::frame_t frame);
  void applyNewLayer(const std::vector<uint32_t>& afterLayerPath, const std::string& name);
  void applyRemoveLayer(const std::vector<uint32_t>& layerPath);
  void applyLayerLock(const std::vector<uint32_t>& layerPath, bool locked);

  std::vector<uint8_t> buildSnapshotBytes(Doc* doc);

  // Transport layer helpers
  void setupTransportCallbacks();
  void broadcastMessage(const std::vector<uint8_t>& data);
  void sendToHost(const std::vector<uint8_t>& data);
  void sendToPeer(uint32_t peerId, const std::vector<uint8_t>& data);

private:
  mutable std::recursive_mutex m_mutex;

  struct PendingCursorSignal {
    enum class Type { Change, Hide };
    Type type = Type::Change;
    uint32_t peerId = 0;
    gfx::Point oldPos;
    gfx::Point newPos;
  };

  Role m_role = Role::None;
  ConnectionType m_connType = ConnectionType::None;
  Doc* m_doc = nullptr;
  Context* m_ctx = nullptr; // Context for loading snapshots
  std::string m_roomName; // For PartyKit sessions
  std::string m_localUsername;
  std::string m_password;

  uint32_t m_localPeerId = 0;
  Permissions m_localPerms = 0;

  uint64_t m_nextClientOpId = 1;
  uint64_t m_nextRev = 1;
  uint32_t m_nextPeerId = 2; // Host is 1, guests start at 2

  std::unique_ptr<ITransport> m_transport;
  std::vector<uint8_t> m_snapshotBytes; // Host's snapshot for sending to guests

  std::map<uint32_t, Peer> m_peers;
  std::map<uint32_t, HostPeerInfo> m_hostPeerInfo; // Only used by host
  PendingSnapshot m_pendingSnapshot;

  std::string m_chatLog;
  OnlineSessionWindow* m_window = nullptr; // owned by UI

  // Cursor state
  gfx::Point m_pendingCursorPos{0, 0};
  bool m_cursorDirty = false;
  bool m_cursorWasVisible = false;

  std::vector<PendingCursorSignal> m_pendingCursorSignals;
};

} // namespace app::online

#endif
