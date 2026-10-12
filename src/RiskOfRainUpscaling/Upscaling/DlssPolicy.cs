using System;

namespace RiskOfRainUpscaling
{
    // Native code maps these values to sl::DLSSMode; keep the order stable.
    internal enum DlssQuality { DLAA, Quality, Balanced, Performance, UltraPerformance }
    // Where DLAA/DLSS runs in the scene camera. Native code receives 1 for before post-processing.
    internal enum DlaaPlacement { BeforePostProcessing, EndOfEffects }
    // Super-resolution runtime. Native code: 0 Auto, 1 DLSS, 2 FSR3/4 DX12, 3 FSR 3 DX11
    // (RiskOfRainUpscaling_SetUpscalerBackend). FSR keeps its config name for existing configs.
    internal enum UpscalerBackend { Auto, DLSS, FSR, FSR3DX11 }
    // DLSS model for every quality mode. Default keeps NVIDIA's per-mode choice.
    internal enum DlssPreset { Default, K, M, L, J, E }

    internal static class DlssPolicy
    {
        // NVIDIA's fixed per-axis render ratios. Native code checks the result
        // against slDLSSGetOptimalSettings' supported range before evaluating.
        // FSR's ratios (1.5x, 1.7x, 2x, 3x) differ only at Balanced.
        internal static float Scale(DlssQuality quality, UpscalerBackend backend = UpscalerBackend.DLSS)
        {
            switch (quality)
            {
                case DlssQuality.Quality: return 2f/3f;
                case DlssQuality.Balanced: return IsFsr(backend) ? 1f/1.7f : 0.58f;
                case DlssQuality.Performance: return 0.5f;
                case DlssQuality.UltraPerformance: return 1f/3f;
                default: return 1f;
            }
        }
        internal static int Dimension(int output, DlssQuality quality, UpscalerBackend backend = UpscalerBackend.DLSS)
        {
            if (output < 1) throw new ArgumentOutOfRangeException(nameof(output));
            return Math.Max(1, (int)Math.Round(output * (double)Scale(quality, backend)));
        }
        internal static bool IsFsr(UpscalerBackend backend) => backend == UpscalerBackend.FSR || backend == UpscalerBackend.FSR3DX11;
        internal const int NvidiaVendorId = 0x10DE;
        // The sl::DLSSPreset value native code passes for every quality mode (RiskOfRainUpscaling_SetDlssPreset).
        internal static int NativePreset(DlssPreset preset)
        {
            switch (preset)
            {
                case DlssPreset.K: return 11;
                case DlssPreset.M: return 13;
                case DlssPreset.L: return 12;
                case DlssPreset.J: return 10;
                case DlssPreset.E: return 5;
                default: return 0;
            }
        }
        // The runtime that sizes the render target. Auto follows the runtime native
        // code last selected for Auto (1 DLSS, 2 FSR3/4 DX12, 3 FSR 3 DX11), otherwise
        // the GPU vendor. Both FSR paths share FSR's render sizes.
        internal static UpscalerBackend Resolve(UpscalerBackend requested, int autoKind, bool nvidia)
        {
            if (requested != UpscalerBackend.Auto) return requested;
            if (autoKind >= 1 && autoKind <= 3) return (UpscalerBackend)autoKind;
            return nvidia ? UpscalerBackend.DLSS : UpscalerBackend.FSR3DX11;
        }
        // Every quality can run before post-processing. DLAA writes the scene in
        // place; lower-resolution modes upscale into a display-sized texture and
        // rebuild the post stack at display size from it.
        internal static bool BeforePostProcessing(DlssQuality quality, DlaaPlacement placement)
        {
            return placement == DlaaPlacement.BeforePostProcessing;
        }
        internal static bool UpscalesBeforePostProcessing(DlssQuality quality, DlaaPlacement placement)
        {
            return quality != DlssQuality.DLAA && BeforePostProcessing(quality, placement);
        }
        internal static DlssQuality Next(DlssQuality quality)
        {
            return quality == DlssQuality.UltraPerformance ? DlssQuality.DLAA : quality + 1;
        }
        // DLSS guide 3.7: 8 base phases scaled by the output/render pixel-area ratio.
        internal static int JitterPhases(int render, int output)
        {
            if (render < 1 || output < render) throw new ArgumentOutOfRangeException(nameof(render));
            double ratio = (double)output / render;
            return Math.Max(8, (int)Math.Ceiling(8 * ratio * ratio - 1e-6));
        }
        // DLSS guide 3.5: log2(render/display) - 1 + epsilon. The caller supplies
        // the -1 + epsilon offset; 100% rendering keeps the game's native bias.
        internal static float MipBias(int render, int output, float offset)
        {
            if (render < 1 || output < render) throw new ArgumentOutOfRangeException(nameof(render));
            return render == output ? 0 : (float)(Math.Log((double)render / output, 2) + offset);
        }
    }
}
