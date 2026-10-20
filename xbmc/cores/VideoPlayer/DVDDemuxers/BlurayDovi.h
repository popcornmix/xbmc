/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <vector>
extern "C"
{
#include <libavcodec/packet.h>
#include <libavcodec/bsf.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

// Present separate HEVC layers as the joined stream consumed by CB1.
class CBlurayDovi
{
public:
  // Preserve each layer's HEVC configuration and time base.
  bool Configure(const AVCodecParameters* base, const AVCodecParameters* enhancement,
                 AVRational baseClock, AVRational enhancementClock)
  {
    const auto* filter = av_bsf_get_by_name("hevc_mp4toannexb");
    if (!filter || baseClock.num <= 0 || baseClock.den <= 0 ||
        enhancementClock.num <= 0 || enhancementClock.den <= 0)
      return false;
    std::array<Filter, 2> prepared;
    const AVCodecParameters* parameters[] = {base, enhancement};
    const AVRational clocks[] = {baseClock, enhancementClock};
    for (unsigned i = 0; i < prepared.size(); ++i)
    {
      AVBSFContext* context = nullptr;
      if (av_bsf_alloc(filter, &context) < 0)
        return false;
      prepared[i].reset(context);
      if (avcodec_parameters_copy(context->par_in, parameters[i]) < 0)
        return false;
      context->time_base_in = clocks[i];
      if (av_bsf_init(context) < 0)
        return false;
    }
    m_filters = std::move(prepared);
    return true;
  }
  const AVCodecParameters* BaseParameters() const { return m_filters[0]->par_out; }
  const AVCodecParameters* OriginalBaseParameters() const
  { return m_filters[0] ? m_filters[0]->par_in : nullptr; }

  void Reset(bool resync = false)
  {
    for (auto& filter : m_filters)
      if (filter) av_bsf_flush(filter.get());
    m_base.clear();
    m_enhancement.clear();
    m_rpu.clear();
    m_bytes = 0;
    m_error = "";
    m_resync = resync;
    m_preroll = 0;
    m_lastDts = {AV_NOPTS_VALUE, AV_NOPTS_VALUE};
    m_discontinuity = false;
    m_restartPeer = -1;
  }

  bool Push(const AVPacket* packet, bool enhancement)
  {
    m_error = "";
    if (!packet || packet->size <= 0 || static_cast<size_t>(packet->size) > MAX_BYTES)
      return Reject("packet");
    Packet normalized;
    if (m_filters[enhancement])
    {
      Packet input(av_packet_clone(packet));
      normalized.reset(av_packet_alloc());
      auto* filter = m_filters[enhancement].get();
      if (!input || !normalized || av_bsf_send_packet(filter, input.get()) < 0 ||
          av_bsf_receive_packet(filter, normalized.get()) < 0)
        return Reject("hevc-normalization");
      const auto from = filter->time_base_out, to = m_filters[0]->time_base_out;
      if (av_cmp_q(from, to))
      {
        const auto pts = normalized->pts, dts = normalized->dts, duration = normalized->duration;
        av_packet_rescale_ts(normalized.get(), from, to);
        if ((pts != AV_NOPTS_VALUE && av_rescale_q(normalized->pts, to, from) != pts) ||
            (dts != AV_NOPTS_VALUE && av_rescale_q(normalized->dts, to, from) != dts) ||
            av_rescale_q(normalized->duration, to, from) != duration)
          return Reject("layer-time-base");
      }
      packet = normalized.get();
    }
    // Wait for both transport clocks before accepting a new layer epoch.
    if (m_restartPeer == int(enhancement))
    {
      if (packet->dts == AV_NOPTS_VALUE) return Reject("restart-peer-dts");
      if (packet->dts >= m_lastDts[enhancement]) return SkipPreroll();
      m_restartPeer = -1;
      m_lastDts[enhancement] = packet->dts;
    }
    if (packet->dts != AV_NOPTS_VALUE)
    {
      if (m_lastDts[enhancement] != AV_NOPTS_VALUE && packet->dts < m_lastDts[enhancement])
      {
        if (m_restartPeer != -1) return Reject("overlapping-discontinuity");
        const auto peerDts = m_lastDts[!enhancement];
        if (peerDts == AV_NOPTS_VALUE) return Reject("missing-peer-dts");
        Reset(true);
        m_lastDts[!enhancement] = peerDts;
        m_restartPeer = !enhancement;
        m_discontinuity = true;
      }
      m_lastDts[enhancement] = packet->dts;
    }
    if (static_cast<size_t>(packet->size) > MAX_BYTES - m_bytes)
      return Reject("byte-limit");
    if (packet->pts == AV_NOPTS_VALUE)
    {
      if (m_resync && !enhancement) return SkipPreroll();
      return Reject("packet");
    }
    if (!enhancement)
    {
      ResyncPreroll();
      if (m_base.size() >= MAX_PACKETS)
        return Reject("base-queue");
      Packet owned(av_packet_clone(packet));
      if (!owned)
        return Reject("allocation");
      m_bytes += packet->size;
      m_base.push_back(std::move(owned));
      return true;
    }
    if (m_enhancement.size() >= MAX_PACKETS || m_rpu.size() >= MAX_PACKETS)
      return Reject("enhancement-queue");
    if (m_rpu.contains(packet->pts))
      return Reject("duplicate-rpu-pts");

    std::vector<uint8_t> layer, rpu;
    auto begin = Start(packet->data, packet->size, 0);
    if (begin[0] != 0)
      return Reject("annex-b");
    while (begin[0] < packet->size)
    {
      const auto next = Start(packet->data, packet->size, begin[1]);
      const int size = next[0] - begin[1];
      if (size < 2 || packet->data[begin[1]] & 0x80 || !(packet->data[begin[1]+1] & 7))
        return Reject("nal-header");
      const int type = (packet->data[begin[1]] >> 1) & 63;
      auto& destination = type == 62 ? rpu : layer;
      destination.insert(destination.end(), {0, 0, 0, 1});
      if (type != 62)
        destination.insert(destination.end(), {0x7e, 1});
      destination.insert(destination.end(), packet->data + begin[1], packet->data + next[0]);
      if (layer.size() + rpu.size() > MAX_BYTES - m_bytes)
        return Reject("byte-limit");
      begin = next;
    }
    if (layer.empty() || rpu.empty())
      return Reject("missing-layer-or-rpu");
    Packet owned(av_packet_alloc());
    if (!owned || av_new_packet(owned.get(), layer.size()) < 0)
      return Reject("allocation");
    std::memcpy(owned->data, layer.data(), layer.size());
    owned->pts = packet->pts;
    owned->dts = packet->dts;
    m_bytes += layer.size() + rpu.size();
    m_enhancement.push_back(std::move(owned));
    m_rpu.emplace(packet->pts, std::move(rpu));
    return true;
  }

  int Take(AVPacket* output)
  {
    ResyncPreroll();
    if (m_base.empty() || m_enhancement.empty())
      return AVERROR(EAGAIN);
    auto rpu = m_rpu.find(m_base.front()->pts);
    if (rpu == m_rpu.end())
      return AVERROR(EAGAIN);
    const auto& layer = m_enhancement.front();
    const int baseSize = m_base.front()->size;
    const size_t extra = layer->size + rpu->second.size();
    if (extra > MAX_BYTES - static_cast<size_t>(baseSize))
      return AVERROR_INVALIDDATA;
    Packet joined(av_packet_clone(m_base.front().get()));
    if (!joined || av_grow_packet(joined.get(), extra) < 0)
      return AVERROR(ENOMEM);
    std::memcpy(joined->data + baseSize, layer->data, layer->size);
    std::memcpy(joined->data + baseSize + layer->size, rpu->second.data(), rpu->second.size());
    m_bytes -= baseSize + extra;
    av_packet_move_ref(output, joined.get());
    m_base.pop_front();
    m_enhancement.pop_front();
    m_rpu.erase(rpu);
    m_resync = false;
    return 0;
  }

  bool Pending() const { return !m_base.empty() || !m_enhancement.empty() || !m_rpu.empty(); }
  const char* LastError() const { return m_error; }
  int64_t BasePTS() const { return m_base.empty() ? AV_NOPTS_VALUE : m_base.front()->pts; }
  int64_t RpuPTS() const { return m_rpu.empty() ? AV_NOPTS_VALUE : m_rpu.begin()->first; }
  size_t TakePrerollCount() { const auto count=m_preroll; m_preroll=0; return count; }
  bool TakeDiscontinuity() { const bool changed=m_discontinuity; m_discontinuity=false; return changed; }

private:
  bool Reject(const char* reason) { m_error = reason; return false; }
  bool SkipPreroll()
  {
    if (m_preroll >= MAX_PACKETS) return Reject("seek-preroll-limit");
    ++m_preroll;
    return true;
  }
  void ResyncPreroll()
  {
    if (!m_resync)
      return;
    if (!m_enhancement.empty() && m_enhancement.front()->dts != AV_NOPTS_VALUE)
    {
      if (m_base.size() < MAX_PACKETS && m_enhancement.size() < MAX_PACKETS)
        return;
      // A common decode anchor proves which BL units precede the first EL unit.
      const auto& first = m_enhancement.front();
      const auto anchor = std::find_if(m_base.begin(), m_base.end(), [&first](const Packet& base) {
        return base->pts == first->pts && base->dts == first->dts;
      });
      if (anchor == m_base.end() || !std::all_of(m_base.begin(), anchor, [&first](const Packet& base) {
            return base->dts != AV_NOPTS_VALUE && base->dts < first->dts;
          }))
        return;
      const auto count = static_cast<size_t>(std::distance(m_base.begin(), anchor));
      if (m_preroll > MAX_PACKETS || count > MAX_PACKETS - m_preroll)
        return;
      for (size_t i = 0; i < count; ++i)
      {
        m_bytes -= m_base.front()->size;
        m_base.pop_front();
        ++m_preroll;
      }
      return;
    }
    // Only retire leading seek packets after a full reorder window proves them orphaned.
    if (m_base.size() < MAX_PACKETS || m_rpu.size() < MAX_PACKETS - 1 ||
        std::none_of(m_base.begin(),m_base.end(),
            [this](const Packet& base) { return m_rpu.contains(base->pts); })) return;
    while (!m_base.empty() && m_base.front()->pts < m_rpu.begin()->first)
    {
      m_bytes -= m_base.front()->size;
      m_base.pop_front();
      ++m_preroll;
    }
  }
  static constexpr size_t MAX_PACKETS = 32;
  static constexpr size_t MAX_BYTES = 64u * 1024u * 1024u;
  struct Release { void operator()(AVPacket* p) const { av_packet_free(&p); } };
  using Packet = std::unique_ptr<AVPacket, Release>;
  struct ReleaseFilter { void operator()(AVBSFContext* p) const { av_bsf_free(&p); } };
  using Filter = std::unique_ptr<AVBSFContext, ReleaseFilter>;
  std::array<Filter, 2> m_filters;

  // Return the prefix and payload positions of the next Annex-B NAL.
  static std::array<int, 2> Start(const uint8_t* data, int size, int from)
  {
    for (int i = from; i < size - 2; ++i)
    {
      if (data[i] || data[i+1])
        continue;
      if (data[i+2] == 1)
        return {i, i+3};
      if (i < size - 3 && !data[i+2] && data[i+3] == 1)
        return {i, i+4};
    }
    return {size, size};
  }

  std::deque<Packet> m_base, m_enhancement;
  // Keep each layer's decode order; associate RPU only by presentation time.
  std::map<int64_t, std::vector<uint8_t>> m_rpu;
  size_t m_bytes{0};
  const char* m_error{""};
  bool m_resync{false};
  size_t m_preroll{0};
  std::array<int64_t, 2> m_lastDts{AV_NOPTS_VALUE, AV_NOPTS_VALUE};
  bool m_discontinuity{false};
  int m_restartPeer{-1};
};
