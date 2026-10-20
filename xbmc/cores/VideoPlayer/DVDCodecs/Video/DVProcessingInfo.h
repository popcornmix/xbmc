/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "cores/VideoProcessingInfo.h"

namespace DVBRIDGE
{
// Convert already accepted adapter facts, without retaining RPU or renderer state.
inline VideoProcessingInfo NativeProcessingInfo(const StreamSnapshot& source)
{
  VideoProcessingInfo info;
  info.informationGeneration=source.informationGeneration;
  const auto& output=source.output;
  auto& accepted=info.output;
  accepted.flags=output.flags; accepted.bits=output.bits; accepted.target=output.target;
  accepted.level6=output.level6; accepted.requested=output.requested;
  accepted.effective=output.effective; accepted.method=output.method;
  accepted.panel=output.panel;
  accepted.gamut=output.gamut; accepted.manualPeak=output.manualPeak;
  accepted.revision=output.revision; accepted.stream=output.stream; accepted.picture=output.picture;
  accepted.format=output.format; accepted.width=output.width; accepted.height=output.height;
  accepted.refresh=output.refresh; accepted.fallback=output.fallback;
  accepted.authoredLegacy=output.authoredLegacy;
  accepted.reason=static_cast<VideoProcessingReason>(output.reason);
  accepted.minimumLinkBits=output.minimumLinkBits;
  accepted.creativeKnown=output.creativeKnown; accepted.creativeApplied=output.creativeApplied;
  accepted.creativeEdited=output.creativeEdited; accepted.creativeStatus=output.creativeStatus;
  accepted.creativeReason=output.creativeReason; accepted.fastChroma=output.fastChroma;
  accepted.fastRenderingMode=output.fastRenderingMode;
  accepted.felUpscaling=output.felUpscaling;
#ifdef CB1_HAS_HDR10_AI
  accepted.hdr10AI=output.hdr10AI; accepted.aiL1L3=output.aiL1L3;
  accepted.emittedL1Known=output.emittedL1Known; accepted.emittedL1=output.emittedL1;
  if (output.flags) { accepted.engine=cb1_engine_version(); accepted.circuit=output.Circuit(); }
#endif
  if (output.flags & CDVPlaybackInfo::HDMI) info.outputType=VideoOutputType::DolbyVision;
  else if (output.flags & CDVPlaybackInfo::HDR10) info.outputType=VideoOutputType::HDR10;
  info.profile=source.profile; info.level=source.level; info.major=source.major; info.minor=source.minor;
  info.bl=source.bl; info.el=source.el; info.rpu=source.rpu; info.fel=source.fel;
  info.sourceBits=source.sourceBits; info.sourceFormat=static_cast<int>(source.sourceFormat);
  info.sourceMastering=source.sourceMastering; info.sourceLight=source.sourceLight;
  info.sourceMax=source.sourceMax; info.sourceMin=source.sourceMin;
  info.sourceCLL=source.sourceCLL; info.sourceFALL=source.sourceFALL;
  info.dropped=source.dropped; info.skipped=source.skipped; info.counters=source.counters;
  // Generated AI metadata describes output, never the encoded HDR10 source.
  info.metadata=source.metadata && source.profile && !accepted.hdr10AI;
  info.transmittedBlocks=source.TransmittedBlocks();
  if (info.metadata)
  {
    info.cm4=source.cm4; info.l1=source.l1; info.l5=source.l5; info.l6=source.l6;
    info.pq=source.pq; info.area=source.area; info.maxMaster=source.maxMaster;
    info.minMaster=source.minMaster; info.maxCLL=source.maxCLL; info.maxFALL=source.maxFALL;
    info.framebufferBits=source.framebufferBits; info.blocks=source.blocks;
    info.appliedBlocks=source.AppliedBlocks();
  }
  const auto& pending=source.transaction;
  info.transaction.identity={pending.identity.stream,pending.identity.picture,pending.identity.revision};
  info.transaction.requested=pending.requested; info.transaction.effective=pending.effective;
  info.transaction.atomicError=pending.atomicError;
  info.transaction.reason=static_cast<VideoProcessingReason>(pending.reason);
  info.transaction.retryCount=pending.retryCount; info.transaction.restored=pending.restored;
  info.transaction.stage=pending.stage;
  info.transaction.hdr10AI=pending.capabilities.isHDR10 && !pending.capabilities.isDV;
  info.referenceWarning=source.referenceWarning;
  return info;
}
}
