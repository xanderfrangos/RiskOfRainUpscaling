#pragma once
#include "Core/CameraFrame.h"

// DLAA works in Unity's render-texture space, which is vertically flipped
// relative to the D3D swap chain. Frame Generation interpolates the top-down
// swap-chain color, so its depth/motion rows are flipped (FgDepthInput and
// FgMotionInput) and its camera constants use the matching clip space:
// P' = F*P, inverse(P') = inverse(P)*F, clipToPrevious' = F*C*F, F=diag(1,-1,1,1).
// Packets hold transposed (SL row-vector) matrices, so Unity's F*M negates
// packet column 1 and M*F negates packet row 1. Jitter follows the row flip.
inline CameraFrame FrameGenerationPacket(CameraFrame packet) {
    auto column=[](float* m){for(int r=0;r<4;++r)m[r*4+1]=-m[r*4+1];};
    auto row=[](float* m){for(int c=0;c<4;++c)m[4+c]=-m[4+c];};
    column(packet.projection); row(packet.inverseProjection);
    column(packet.clipToPrevious); row(packet.clipToPrevious);
    column(packet.previousToClip); row(packet.previousToClip);
    packet.jitterY=-packet.jitterY;
    return packet;
}
