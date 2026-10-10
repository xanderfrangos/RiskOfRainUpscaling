#pragma once
// Internal to the upscaler context: shared by UpscalerContext.cpp, Sharpening.cpp and Outlines.cpp.
#include "Upscaling/UpscalerContext.h"
#include "Upscaling/Upscaler.h"
#include "Core/D3D11StateScope.h"
#include "Upscaling/RcasSettings.h"
#include "Diagnostics/Profiler.h"
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>

inline void RequireUpscaler(HRESULT hr, const char* label) {
    if (FAILED(hr)) { Log(ModLogLevel::Error, "UPSCALER_FAILED %s HRESULT=0x%08lx", label, hr); throw std::runtime_error(label); }
}
struct UpscalerContext {
    UpscalerDescription desc{};
    std::atomic<int> state{0}; // 0 pending, 1 ready, 2 evaluated, -2 failed, -1 retired
    std::mutex mutex;
    std::map<int, CameraFrame> frames;
    int lastFrame=0, evaluations=0, resets=0, resetRun=0, longestResetRun=0;
    DWORD renderThread=0;
    bool ownsFeature=false, perceptual=true, sharpenFailed=false;
    int sharpened=0, fallbacks=0, outlines=0;
    bool fallbackFailed=false, outlineFailed=false;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11DeviceContext1> immediate1;
    ComPtr<ID3DDeviceContextState> privateState;
    ComPtr<ID3D11Texture2D> source, output, color, result, merged, depth, motion;
    ComPtr<ID3D11Texture2D> encoded, decoded, fgDepth, fgMotion;
    ComPtr<ID3D11ShaderResourceView> rawDepth, rawMotion, resultView, colorView;
    ComPtr<ID3D11Texture2D> rawDepthTexture, rawMotionTexture;
    bool depthCopy=false; // Raw depth is RFloat: copied, not converted.
    ComPtr<ID3D11ShaderResourceView> depthView, motionView, decodedView;
    ComPtr<ID3D11RenderTargetView> depthTarget, motionTarget, mergedTarget;
    ComPtr<ID3D11RenderTargetView> encodedTarget, decodedTarget, fgDepthTarget, fgMotionTarget;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> depthShader, motionShader, alphaShader, rcasShader, upscaleShader;
    ComPtr<ID3D11PixelShader> fgDepthShader, fgMotionShader, encodeShader, decodeShader, sanitizeShader, upscaleHdrShader;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Buffer> rcasConstants;
    // Upscaling only: the render-sized interactable outline mask and its sharpened
    // display-sized copy, both Unity textures registered after creation.
    ComPtr<ID3D11PixelShader> outlineShader;
    ComPtr<ID3D11Buffer> outlineConstants;
    ComPtr<ID3D11ShaderResourceView> outlineSource;
    ComPtr<ID3D11RenderTargetView> outlineTarget;
    // Before post-processing: temporal outlines. Unity's jittered render-sized
    // highlight mask and Sobel edges give an outline weight that rides through
    // DLSS in the input alpha; the resolved display-sized outline (rgb colour,
    // alpha weight) is composited into Unity's display-sized texture later.
    bool temporalOutlines=false, compositeFailed=false;
    int outlineFrame=0, composites=0;
    ComPtr<ID3D11PixelShader> weightInputShader, resolveShader, resolveFallbackShader, compositeShader;
    ComPtr<ID3D11ShaderResourceView> temporalMask, temporalSobel, resolvedView;
    ComPtr<ID3D11Texture2D> resolved, composite;
    ComPtr<ID3D11RenderTargetView> resolvedTarget, compositeTarget;
    // Before-post-processing upscaling: full-resolution outlines. Unity draws the
    // display-sized highlight mask and the highlighted objects' depth; we build
    // the occlusion depth the game's fill pass tests against, a display-sized
    // Sobel depth, and the anti-aliased Sobel edges the game's composite reads.
    bool fullResolutionOutlines=false, fullResolutionFailed=false, reversedOutlineDepth=true;
    int outlineDepths=0;
    float outlineNear=.1f, outlineFar=1000.f; // the last evaluated camera's planes
    ComPtr<ID3D11PixelShader> occlusionDepthShader, sobelDepthShader, sobelShader;
    ComPtr<ID3D11Buffer> fullResolutionConstants, sobelConstants;
    ComPtr<ID3D11ShaderResourceView> displayMask, objectDepth, sobelDepth;
    ComPtr<ID3D11RenderTargetView> occlusionTarget, sobelDepthTarget, sobelTarget;
    float sharpness=DefaultRcasSharpness;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depthState;
    std::unique_ptr<UpscalerFeature> feature;
    UpscalerBackend upscaler=UpscalerBackend::Auto; // Captured at creation; a change creates a new context.
    int preset=0; // DLSS preset, captured at creation like the backend.
    std::atomic<int> kind{0};
    bool Upscaling() const { return desc.outputWidth!=desc.width || desc.outputHeight!=desc.height; }
    D3D11StateScope Scope() { return {immediate1.Get(), privateState.Get()}; }
    bool BeforePost() const { return desc.placement==1; }
};
void OwnedTexture(UpscalerContext& c, DXGI_FORMAT format, UINT bind, ComPtr<ID3D11Texture2D>& texture, bool output=false);
void Convert(UpscalerContext& c, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target, ID3D11PixelShader* shader,
    ID3D11ShaderResourceView* second=nullptr, bool output=false, ID3D11ShaderResourceView* third=nullptr);
// Sharpening.cpp: RCAS shader and constants at creation; applies a changed strength on the render thread.
void CreateSharpening(UpscalerContext& c);
void UpdateSharpness(UpscalerContext& c);
// Outlines.cpp: outline mask upscale and full-resolution outline depths (upscaling), temporal outline shaders (before post-processing).
void CreateOutlines(UpscalerContext& c);
