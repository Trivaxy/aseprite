// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_ONLINE_PROTOCOL_H_INCLUDED
#define APP_ONLINE_PROTOCOL_H_INCLUDED
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace app::online {

enum class MsgType : uint8_t {
  Hello = 1,
  Welcome = 2,
  Error = 3,

  SnapshotBegin = 10,
  SnapshotData = 11,
  SnapshotEnd = 12,

  OpPropose = 20,
  Op = 21,
  OpRejected = 22,

  PermissionsSet = 30,
  Kick = 31,

  ChatSend = 40,
  ChatBroadcast = 41,

  CursorPosition = 50,
  CursorHide = 51,
};

enum class OpType : uint8_t {
  SetPixelsRect = 1,
  NewFrame = 2,
  RemoveFrame = 3,
  NewLayer = 4,
  RemoveLayer = 5,
  LayerLock = 6,
};

struct Reader {
  const uint8_t* p = nullptr;
  const uint8_t* end = nullptr;

  explicit Reader(const std::string& s)
    : p(reinterpret_cast<const uint8_t*>(s.data()))
    , end(reinterpret_cast<const uint8_t*>(s.data() + s.size()))
  {
  }

  bool ok(size_t n) const { return (p + n <= end); }

  bool readU8(uint8_t& out);
  bool readU32(uint32_t& out);
  bool readU64(uint64_t& out);
  bool readS32(int32_t& out);
  bool readString(std::string& out);
  bool readBytes(std::vector<uint8_t>& out);
};

struct Writer {
  std::vector<uint8_t> data;

  void writeU8(uint8_t v);
  void writeU32(uint32_t v);
  void writeU64(uint64_t v);
  void writeS32(int32_t v);
  void writeString(std::string_view s);
  void writeBytes(const std::vector<uint8_t>& bytes);
};

std::string toString(const std::vector<uint8_t>& bytes);

} // namespace app::online

#endif

