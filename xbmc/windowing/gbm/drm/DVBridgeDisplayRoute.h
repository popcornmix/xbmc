// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace DVBRIDGE
{
namespace DISPLAY_ROUTE
{
inline std::string Label(std::string value)
{
  const bool truncated = value.size() > 128;
  value.resize(std::min<size_t>(value.size(), 128));
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
    value.pop_back();
  for (char& c : value)
    if (c < 33 || c > 126)
      c = '_';
  return value.empty() ? "unknown" : value + (truncated ? "[truncated]" : "");
}
inline std::string Read(const std::filesystem::path& path)
{
  std::ifstream file(path);
  std::array<char, 129> text{};
  file.read(text.data(), text.size());
  return Label({text.data(), static_cast<size_t>(file.gcount())});
}
inline std::string LinkName(const std::filesystem::path& path)
{
  std::error_code error;
  const auto target = std::filesystem::read_symlink(path, error);
  return error ? "unknown" : Label(target.filename().string());
}
inline bool Component(const std::string& text)
{
  return !text.empty() && text.size() <= 128 &&
      std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
      });
}
}

// Called only at initialization/selected-link mode events, never a flip/frame.
// Fixed attributes + <=32 direct connector children; no global/recursive scan,
// no debugfs, AUX device transactions, shell commands or hardware writes.
inline std::string ReadDisplayRoute(const std::filesystem::path& sysRoot,
                                    const std::string& card, const std::string& connector,
                                    const std::string& type, uint32_t id)
{
  using namespace DISPLAY_ROUTE;
  std::string result = "connector=" + Label(connector) + " type=" + Label(type) +
      " id=" + std::to_string(id) + " card=" + Label(card);
  const bool valid = card.starts_with("card") && card.size() > 4 &&
      std::all_of(card.begin() + 4, card.end(), [](char c) { return c >= '0' && c <= '9'; }) &&
      Component(card) && Component(connector);
  const auto gpu = sysRoot / "class/drm" / card / "device";
  result += " gpu_device=" + (valid ? LinkName(gpu) : "unknown");
  result += " vendor=" + (valid ? Read(gpu / "vendor") : "unknown");
  result += " device=" + (valid ? Read(gpu / "device") : "unknown");
  result += " subvendor=" + (valid ? Read(gpu / "subsystem_vendor") : "unknown");
  result += " subdevice=" + (valid ? Read(gpu / "subsystem_device") : "unknown");
  result += " driver=" + (valid ? LinkName(gpu / "driver") : "unknown");
  const auto selected = sysRoot / "class/drm" / (card + "-" + connector);
  const bool matched = valid && Read(selected / "connector_id") == std::to_string(id);
  result += " sysfs_connector=" + std::string(matched ? "matched" : "unknown-or-mismatch");
  result += " status=" + (matched ? Read(selected / "status") : "unknown");
  result += " enabled=" + (matched ? Read(selected / "enabled") : "unknown");
  std::vector<std::string> aux;
  bool limited = false;
  if (matched)
  {
    std::error_code error;
    std::filesystem::directory_iterator entry(selected, error), end;
    unsigned int scanned = 0;
    for (; !error && entry != end && scanned < 32; entry.increment(error), ++scanned)
    {
      const auto name = entry->path().filename().string();
      if (name.starts_with("drm_dp_aux") && Component(name))
      {
        if (aux.size() < 4)
          aux.push_back(Label(name) + ":" + Read(entry->path() / "name"));
        else
          limited = true;
      }
    }
    limited |= error || entry != end;
  }
  std::sort(aux.begin(), aux.end());
  result += " aux=";
  if (aux.empty())
    result += "unknown";
  for (size_t i = 0; i < aux.size(); ++i)
    result += (i ? "," : "") + aux[i];
  if (limited)
    result += "[incomplete]";
  // A DRM HDMI label or AUX name does not prove physical/native HDMI or a
  // converter model. Boot kernel evidence is forwarded separately, unscoped.
  return result + " converter=unknown native_hdmi=unknown";
}
class DisplayRouteChanges
{
public:
  bool Report(uint64_t now, const std::string& snapshot)
  {
    if (m_reported && snapshot == m_last)
      return false;
    if (!m_reported || now < m_window || now - m_window >= 30000)
    {
      m_window = now;
      m_reports = 0;
    }
    if (m_reports >= 4)
      return false;
    ++m_reports;
    m_reported = true;
    m_last = snapshot;
    return true;
  }
private:
  std::string m_last;
  uint64_t m_window{};
  unsigned int m_reports{};
  bool m_reported{};
};
}
