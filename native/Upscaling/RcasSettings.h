#pragma once
#include <cmath>
#include <algorithm>

inline constexpr float DefaultRcasSharpness=0.7f;
struct RcasConstants {
    float sharpness;
    int width,height;
    float padding=0;
};
static_assert(sizeof(RcasConstants)==16,"RCAS shader constants");
