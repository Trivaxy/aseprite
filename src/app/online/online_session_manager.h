// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_ONLINE_SESSION_MANAGER_H_INCLUDED
#define APP_ONLINE_SESSION_MANAGER_H_INCLUDED
#pragma once

#include "doc/frame.h"
#include "gfx/rect.h"
#include "gfx/region.h"

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

using Permissions = uint32_t;
static constexpr Permissions kPermEditCanvas = 1u << 0;
static constexpr Permissions kPermEditLayers = 1u << 1;
static constexpr Permissions kPermLockLayers = 1u << 2;
static constexpr Permissions kPermEditTimeline = 1u << 3;

struct Peer {
  uint32_t id = 0;
  Permissions perms = 0;
  std::string name;
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

  bool startHost(Context* ctx, Doc* doc, int port, const std::string& username, const std::string& password, const std::string& bindAddress = "127.0.0.1");
  bool join(Context* ctx, const std::string& address, int port, const std::string& username, const std::string& password);
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

private:
  OnlineSessionManager();
  void stopNoLock();

  enum class Role { None, Host, Guest };

  struct PendingSnapshot {
    bool inProgress = false;
    uint32_t sizeBytes = 0;
    std::vector<uint8_t> bytes;
  };

  struct NetHost;
  struct NetClient;

  void updateWindow();
  void appendChatLine(const std::string& line);

  void onWelcome(uint32_t peerId, Permissions perms);
  void onSnapshotBegin(uint32_t sizeBytes);
  void onSnapshotData(const std::vector<uint8_t>& chunk);
  void onSnapshotEnd(Context* ctx);
  void onPermissionsSet(uint32_t peerId, Permissions perms);
  void onKick(const std::string& reason);
  void onChatBroadcast(uint32_t peerId, const std::string& text);

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

private:
  mutable std::recursive_mutex m_mutex;

  Role m_role = Role::None;
  Doc* m_doc = nullptr;

  uint32_t m_localPeerId = 0;
  Permissions m_localPerms = 0;

  uint64_t m_nextClientOpId = 1;
  uint64_t m_nextRev = 1;

  std::unique_ptr<NetHost> m_host;
  std::unique_ptr<NetClient> m_client;

  std::map<uint32_t, Peer> m_peers;
  PendingSnapshot m_pendingSnapshot;

  std::string m_chatLog;
  OnlineSessionWindow* m_window = nullptr; // owned by UI
};

} // namespace app::online

#endif
