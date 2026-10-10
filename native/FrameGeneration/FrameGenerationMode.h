#pragma once
#include <algorithm>
#include <cstdint>

// Streamline counts generated frames, while the user selects total frames.
inline int SupportedFrameGenerationMultiplier(int requested, uint32_t maxGenerated) {
    if(!maxGenerated)return 0;
    return std::min(std::clamp(requested,2,6),static_cast<int>(std::min(maxGenerated,5u))+1);
}
