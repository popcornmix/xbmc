/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

extern "C"
{
#include <libavutil/buffer.h>
}

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <atomic>
#include <algorithm>
#include <limits>

namespace DVBRIDGE
{
struct SourceBinding
{
  uint64_t stream{}, picture{}, revision{};
  bool Valid() const { return stream && picture && revision; }
  bool operator==(const SourceBinding&) const = default;
};
struct SourceInfo
{
  unsigned profile{}, level{}, major{}, minor{}, bits{};
  int format{-1};
  bool bl{}, el{}, rpu{}, mastering{}, light{};
  double maxLuma{}, minLuma{};
  unsigned maxCLL{}, maxFALL{};
  unsigned compatibility{};
  uint64_t informationGeneration{};
};
// Native source epochs and preference changes share a monotonic revision domain.
inline std::atomic<uint64_t> nativeRevision{1};
inline uint64_t NextRevisionAfter(uint64_t minimum)
{
  auto previous = nativeRevision.load();
  for (;;)
  {
    const auto last = std::max(previous,minimum);
    if (last == std::numeric_limits<uint64_t>::max()) return 0;
    if (nativeRevision.compare_exchange_weak(previous,last+1)) return last+1;
  }
}
inline uint64_t NextRevision() { return NextRevisionAfter(0); }
class SourceOwner
{
public:
  void BeginStream() { m_stream = NextRevision(); m_revision = m_stream; m_picture = 0; }
  void Seek() { m_revision = NextRevision(); }
  SourceBinding NextPicture() { return {m_stream,++m_picture,m_revision}; }
private:
  uint64_t m_stream{}, m_picture{}, m_revision{};
};
}

class CDVMetadataBuffer
{
public:
  bool Assign(const uint8_t* data, size_t size, DVBRIDGE::SourceBinding binding = {})
  {
    if (!data || size == 0 || size > MAX_PAYLOAD_SIZE)
    {
      Reset();
      return false;
    }

    AVBufferRef* buffer = av_buffer_alloc(size);
    if (!buffer)
    {
      Reset();
      return false;
    }
    std::memcpy(buffer->data, data, size);
    m_buffer = std::shared_ptr<AVBufferRef>(buffer, [](AVBufferRef* ref) { av_buffer_unref(&ref); });
    m_binding = binding;
    return true;
  }

  void Reset() { m_buffer.reset(); m_binding = {}; }
  const uint8_t* Data() const { return m_buffer ? m_buffer->data : nullptr; }
  size_t Size() const { return m_buffer ? m_buffer->size : 0; }
  DVBRIDGE::SourceBinding Binding() const { return m_binding; }

private:
  // Bound unexpected side data before allocating; no truncation is allowed.
  static constexpr size_t MAX_PAYLOAD_SIZE = 1024 * 1024;
  std::shared_ptr<AVBufferRef> m_buffer;
  DVBRIDGE::SourceBinding m_binding;
};
