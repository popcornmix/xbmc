/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace DVBRIDGE
{
// Shared by playback diagnostics and the information panel. No GPU synchronization.
class RenderUsage
{
public:
  struct Client
  {
    std::string device;
    uint64_t busy{};
    unsigned capacity{1};
  };
  using Clients = std::map<std::string, Client>;

  static void Parse(std::istream& input, Clients& clients)
  {
    std::string line, driver, device, id;
    char boundedLine[2049];
    unsigned bytes=0, count=0;
    Client render, gfx;
    bool renderEngine = false, gfxEngine = false;
    while (count++ < 128 && input.getline(boundedLine,sizeof(boundedLine)))
    {
      bytes+=input.gcount();
      if (bytes>65536) return;
      line=boundedLine;
      const auto colon = line.find(':');
      if (colon == std::string::npos)
        continue;
      const auto key = line.substr(0, colon);
      std::istringstream value(line.substr(colon + 1));
      if (key == "drm-driver") value >> driver;
      else if (key == "drm-pdev") value >> device;
      else if (key == "drm-client-id") value >> id;
      else if (key == "drm-engine-render")
      {
        std::string unit;
        renderEngine = bool(value >> render.busy >> unit) && unit == "ns";
      }
      else if (key == "drm-engine-gfx")
      {
        std::string unit;
        gfxEngine = bool(value >> gfx.busy >> unit) && unit == "ns";
      }
      else if (key == "drm-engine-capacity-render")
      {
        if (!(value >> render.capacity) || !render.capacity)
          return;
      }
      else if (key == "drm-engine-capacity-gfx")
      {
        if (!(value >> gfx.capacity) || !gfx.capacity)
          return;
      }
    }
    if (!input.eof()) return; // Long lines or an exhausted record budget are unavailable.
    const bool amd = driver == "amdgpu";
    if (id.empty() || device.empty() ||
        (amd ? !gfxEngine : (!renderEngine || (driver != "i915" && driver != "xe"))))
      return;
    // Graphics only: compute/decode counters are not independent render utilization.
    Client client = amd ? gfx : render;
    client.device = device;
    const auto key = driver + ":" + device + ":" + id;
    auto found = clients.find(key);
    if (found == clients.end() || found->second.busy < client.busy)
      clients[key] = client; // Shared/duplicated descriptors count only once.
  }

  std::optional<double> Update(const Clients& current, uint64_t now, uint64_t maxGap = 3000000000ULL)
  {
    if (current.empty())
    {
      Reset();
      return {};
    }
    const auto& device = current.begin()->second.device;
    const auto capacity = current.begin()->second.capacity;
    bool valid = m_time && now > m_time && now - m_time <= maxGap &&
                 current.size() == m_previous.size();
    long double busy = 0;
    Clients next = current;
    for (const auto& [id, client] : current)
    {
      const auto previous = m_previous.find(id);
      if (client.device != device || client.capacity != capacity ||
          previous == m_previous.end() || previous->second.capacity != capacity)
        valid = false;
      else
      {
        // The kernel permits temporarily decreasing counters; retain the high-water mark.
        next[id].busy = std::max(client.busy, previous->second.busy);
        busy += next[id].busy - previous->second.busy;
      }
    }
    const auto elapsed = now - m_time;
    m_previous = std::move(next);
    m_time = now;
    if (!valid)
      return {};
    return std::clamp(double(100.0L * busy / elapsed / capacity), 0.0, 100.0);
  }

  std::optional<double> Sample(uint64_t maxGap = 3000000000ULL)
  {
    Clients clients;
    std::error_code error;
    std::filesystem::directory_iterator it("/proc/self/fdinfo", error), end;
    unsigned count = 0;
    while (!error && it != end && count++ < 512)
    {
      std::ifstream input(it->path());
      Parse(input, clients);
      it.increment(error);
    }
    if (error || it != end)
    {
      Reset();
      return {};
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return Update(clients, std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(), maxGap);
  }

  void Reset()
  {
    m_previous.clear();
    m_time = 0;
  }

private:
  Clients m_previous;
  uint64_t m_time{};
};
}
