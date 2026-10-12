// Shader lookup for the D3D11 FSR 3 upscaler. Replaces the fork's
// ffx_shader_blobs.cpp, which also links FSR 1/2, frame interpolation and
// optical flow: only the FSR 3 upscaler's permutations are built.
#include <ffx_shader_blobs.h>
#include <blob_accessors/ffx_fsr3upscaler_shaderblobs.h>

FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effectId, FfxPass passId, FfxBindStage, uint32_t permutationOptions, FfxShaderBlob* outBlob) {
    if(effectId!=FFX_EFFECT_FSR3UPSCALER) return FFX_ERROR_INVALID_ARGUMENT;
    return fsr3UpscalerGetPermutationBlobByIndex(static_cast<FfxFsr3UpscalerPass>(passId),permutationOptions,outBlob);
}
FfxErrorCode ffxIsWave64(FfxEffect effectId, uint32_t permutationOptions, bool& isWave64) {
    if(effectId!=FFX_EFFECT_FSR3UPSCALER) return FFX_ERROR_INVALID_ARGUMENT;
    return fsr3UpscalerIsWave64(permutationOptions,isWave64);
}
