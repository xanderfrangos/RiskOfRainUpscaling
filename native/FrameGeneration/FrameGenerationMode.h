#pragma once
#include <algorithm>
#include <cstdint>

// Streamline counts generated frames, while the user selects total frames.
inline int SupportedFrameGenerationMultiplier(int requested, uint32_t maxGenerated) {
    if(!maxGenerated)return 0;
    return std::min(std::clamp(requested,2,6),static_cast<int>(std::min(maxGenerated,5u))+1);
}

// Dynamic picks its own ratio up to the system maximum; without support it
// falls back to the highest fixed multiplier, like an unsupported 2x-6x request.
struct FrameGenerationModeChoice { bool dynamic; int multiplier; };
inline FrameGenerationModeChoice SelectFrameGenerationMode(bool dynamicRequested, bool dynamicSupported, int requested, uint32_t maxGenerated) {
    if(!dynamicRequested)return {false,SupportedFrameGenerationMultiplier(requested,maxGenerated)};
    const int multiplier=SupportedFrameGenerationMultiplier(6,maxGenerated);
    return {dynamicSupported && multiplier!=0,multiplier};
}
