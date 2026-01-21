// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/online/online_protocol.h"

namespace app::online {

static void writeLE32(std::vector<uint8_t>& out, uint32_t v)
{
  out.push_back(uint8_t(v & 0xff));
  out.push_back(uint8_t((v >> 8) & 0xff));
  out.push_back(uint8_t((v >> 16) & 0xff));
  out.push_back(uint8_t((v >> 24) & 0xff));
}

static void writeLE64(std::vector<uint8_t>& out, uint64_t v)
{
  writeLE32(out, uint32_t(v & 0xffffffffu));
  writeLE32(out, uint32_t((v >> 32) & 0xffffffffu));
}

static bool readLE32(const uint8_t*& p, const uint8_t* end, uint32_t& out)
{
  if (p + 4 > end)
    return false;
  out = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  p += 4;
  return true;
}

static bool readLE64(const uint8_t*& p, const uint8_t* end, uint64_t& out)
{
  uint32_t lo = 0, hi = 0;
  if (!readLE32(p, end, lo) || !readLE32(p, end, hi))
    return false;
  out = uint64_t(lo) | (uint64_t(hi) << 32);
  return true;
}

bool Reader::readU8(uint8_t& out)
{
  if (!ok(1))
    return false;
  out = *p++;
  return true;
}

bool Reader::readU32(uint32_t& out)
{
  return readLE32(p, end, out);
}

bool Reader::readU64(uint64_t& out)
{
  return readLE64(p, end, out);
}

bool Reader::readS32(int32_t& out)
{
  uint32_t v = 0;
  if (!readU32(v))
    return false;
  out = int32_t(v);
  return true;
}

bool Reader::readString(std::string& out)
{
  uint32_t n = 0;
  if (!readU32(n))
    return false;
  if (!ok(n))
    return false;
  out.assign(reinterpret_cast<const char*>(p), reinterpret_cast<const char*>(p + n));
  p += n;
  return true;
}

bool Reader::readBytes(std::vector<uint8_t>& out)
{
  uint32_t n = 0;
  if (!readU32(n))
    return false;
  if (!ok(n))
    return false;
  out.assign(p, p + n);
  p += n;
  return true;
}

void Writer::writeU8(uint8_t v)
{
  data.push_back(v);
}

void Writer::writeU32(uint32_t v)
{
  writeLE32(data, v);
}

void Writer::writeU64(uint64_t v)
{
  writeLE64(data, v);
}

void Writer::writeS32(int32_t v)
{
  writeU32(uint32_t(v));
}

void Writer::writeString(std::string_view s)
{
  writeU32(uint32_t(s.size()));
  data.insert(data.end(), s.begin(), s.end());
}

void Writer::writeBytes(const std::vector<uint8_t>& bytes)
{
  writeU32(uint32_t(bytes.size()));
  data.insert(data.end(), bytes.begin(), bytes.end());
}

std::string toString(const std::vector<uint8_t>& bytes)
{
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

} // namespace app::online

