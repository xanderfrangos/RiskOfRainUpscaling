#pragma once
#include "Core/CameraFrame.h"

// DLAA works in Unity's render-texture space, which is vertically flipped
// relative to the D3D swap chain. Frame Generation interpolates the top-down
// swap-chain color, so its depth/motion rows are flipped (FgDepthInput and
// FgMotionInput) and its camera constants use the matching clip space:
// P' = F*P, inverse(P') = inverse(P)*F, clipToPrevious' = F*C*F, F=diag(1,-1,1,1).
// Packets hold transposed (SL row-vector) matrices, so Unity's F*M negates
// packet column 1 and M*F negates packet row 1. Jitter follows the row flip.
// Where Unity's game image lies in the swap chain (top-down pixels). When the window
// is larger than the game resolution or has another aspect, Unity scales the image
// to fit, centred, with black bars (letterbox/pillarbox); otherwise it fills the chain.
struct DisplayRect { unsigned left = 0, top = 0, width = 0, height = 0; };
inline DisplayRect GameRect(unsigned gameWidth, unsigned gameHeight, unsigned displayWidth, unsigned displayHeight) {
    if (!gameWidth || !gameHeight || (gameWidth == displayWidth && gameHeight == displayHeight)) return {0, 0, displayWidth, displayHeight};
    DisplayRect r;
    const unsigned long long wide = 1ull * displayWidth * gameHeight, tall = 1ull * displayHeight * gameWidth;
    if (wide >= tall) { r.height = displayHeight; r.width = static_cast<unsigned>((1ull * gameWidth * displayHeight + gameHeight / 2) / gameHeight); }
    else { r.width = displayWidth; r.height = static_cast<unsigned>((1ull * gameHeight * displayWidth + gameWidth / 2) / gameWidth); }
    if (r.width > displayWidth) r.width = displayWidth;
    if (r.height > displayHeight) r.height = displayHeight;
    r.left = (displayWidth - r.width) / 2; r.top = (displayHeight - r.height) / 2;
    return r;
}
inline CameraFrame FrameGenerationPacket(CameraFrame packet) {
    auto column=[](float* m){for(int r=0;r<4;++r)m[r*4+1]=-m[r*4+1];};
    auto row=[](float* m){for(int c=0;c<4;++c)m[4+c]=-m[4+c];};
    column(packet.projection); row(packet.inverseProjection);
    column(packet.clipToPrevious); row(packet.clipToPrevious);
    column(packet.previousToClip); row(packet.previousToClip);
    packet.jitterY=-packet.jitterY;
    return packet;
}
