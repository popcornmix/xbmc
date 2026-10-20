/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#ifdef HAVE_DVBRIDGE
#include "DVOutputSettings.h"
#include "DVPlaybackInfo.h"
#include "DVMetadataBuffer.h"
#include <atomic>
#include <mutex>
#include <string>

namespace DVBRIDGE
{
// File lifecycle arms once. Only accepted presentation consumes the message.
class PlaybackNotification
{
public:
  static bool Pending() { return m_pending.load(std::memory_order_relaxed); }
  static uint64_t Generation() { return Pending() ? m_generation.load(std::memory_order_relaxed) : 0; }
  static void ArmMasterRecovery() { m_masterRecovery.store(true); }
  static void StartVideo()
  {
    const auto component=CServiceBroker::GetSettingsComponent();
    const auto settings=component ? component->GetSettings() : nullptr;
    const bool enabled=settings && settings->GetBool("dvbridge.startupmode");
    std::lock_guard lock(m_mutex);
    m_generation.fetch_add(1,std::memory_order_relaxed);
    m_masterRecovery.store(false);
    m_nativeFloor=nativeRevision.load(std::memory_order_relaxed);
    m_ordinaryFrame=false;
    m_nativeFrame={};
    m_pending.store(enabled,std::memory_order_relaxed);
  }
  static void StopVideo()
  {
    std::lock_guard lock(m_mutex);
    m_ordinaryFrame=false;
    m_nativeFrame={};
    m_pending.store(false,std::memory_order_relaxed);
    m_masterRecovery.store(false);
  }
  static void RenderedOrdinary(uint64_t generation)
  {
    if (!generation || !Pending()) return;
    const auto preferences=FrameOutputSettings();
    std::lock_guard lock(m_mutex);
    if (!Pending() || generation!=m_generation.load(std::memory_order_relaxed)) return;
    m_ordinaryMode=preferences.output==OutputMode::Disabled ? 0 : 1;
    m_nativeFrame={};
    m_ordinaryFrame=true;
  }
  static void RenderedNative(uint64_t generation,const SourceBinding& binding)
  {
    if (!Pending()) return;
    std::lock_guard lock(m_mutex);
    m_nativeFrame={};
    m_ordinaryFrame=false;
    if (generation && generation==m_generation.load(std::memory_order_relaxed) && binding.Valid())
      m_nativeFrame=binding;
  }
  static void OrdinaryOutputAccepted(bool accepted)
  {
    if (!Pending()) return;
    int mode{};
    {
      std::lock_guard lock(m_mutex);
      const bool notify=accepted && m_ordinaryFrame && m_pending.exchange(false);
      m_ordinaryFrame=false;
      if (!notify) return;
      mode=m_ordinaryMode;
    }
    Emit(mode==0 ? "Disabled / Kodi playback" : "Standard / Kodi playback");
  }
  template<class Output> static void NativeOutputAccepted(const Output& output)
  {
    if ((output.flags & CDVPlaybackInfo::HDMI) && output.requested==5 && output.effective==1 &&
        m_masterRecovery.exchange(false))
      EmitMasterRecovery();
    if (!Pending()) return;
    if (!(output.flags & (CDVPlaybackInfo::HDMI | CDVPlaybackInfo::HDR10))) return;
    {
      std::lock_guard lock(m_mutex);
      if (!output.picture || output.stream<=m_nativeFloor) return;
      if (output.stream!=m_nativeFrame.stream || output.picture!=m_nativeFrame.picture) return;
      if (!m_pending.exchange(false)) return;
      m_ordinaryFrame=false;
      m_nativeFrame={};
    }
#ifdef CB1_HAS_HDR10_AI
    if constexpr (requires { output.hdr10AI; })
      if (output.hdr10AI)
      {
        EmitAI(output.effective);
        return;
      }
#endif
    Emit(CDVPlaybackInfo::Mode(output));
  }
  static void Emit(const std::string& text);
  static void EmitAI(int mode);
  static void HDR10Recovery();
  static void EmitMasterRecovery();
private:
  inline static std::atomic<bool> m_masterRecovery{false};
  inline static std::atomic<bool> m_pending{false};
  inline static std::atomic<uint64_t> m_generation{0};
  inline static std::mutex m_mutex;
  inline static bool m_ordinaryFrame{};
  inline static int m_ordinaryMode{};
  inline static uint64_t m_nativeFloor{};
  inline static SourceBinding m_nativeFrame{};
};
}
#endif
