#pragma once

// One Unity camera frame: matrices, jitter and motion scale shared by the
// upscaler (per evaluation) and Frame Generation (flipped to swap-chain space).
struct CameraFrame {
    unsigned size, version;
    int frame, reset;
    float projection[16], inverseProjection[16], clipToPrevious[16], previousToClip[16];
    float jitterX, jitterY, motionScaleX, motionScaleY;
    float nearPlane, farPlane, fov, aspect;
    float position[3], up[3], right[3], forward[3];
    int reversedDepth;
};
