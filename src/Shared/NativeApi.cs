using System;
using System.IO;
using System.Runtime.InteropServices;

namespace RiskOfRainUpscaling
{
    // Keep this file independent of Unity and RoR2: it also runs in the preloader.
    // Every field binds a RiskOfRainUpscaling_<Name> C export of RiskOfRainUpscaling.Native.dll.
    internal static class NativeApi
    {
        [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
        [DllImport("kernel32", CharSet = CharSet.Ansi, ExactSpelling = true)]
        private static extern IntPtr GetProcAddress(IntPtr module, string name);

        // Delegates by signature.
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        internal delegate int InitializeDelegate(string logs, string runtimes, string startupStage);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate IntPtr EventGetter();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int IntQuery();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void IntAction(int value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int IntSetter(int value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int FloatSetter(float value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void MarkerAction(int marker, int frame);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        internal delegate int StringQuery(string value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        internal delegate void StringAction(string value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int ContextQuery(IntPtr context);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate void ContextAction(IntPtr context);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate IntPtr SceneInputsCreateDelegate(ref SceneInputsDescription description);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate IntPtr DiagnosticsCreateDelegate(ref DiagnosticsDescription description);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int CaptureArmDelegate(IntPtr context, ref CaptureDescription description);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate IntPtr UpscalerCreateDelegate(ref UpscalerDescription description);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int UpscalerSubmitDelegate(IntPtr context, ref CameraFrame frame);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int OutlineMasksSetter(IntPtr context, IntPtr source, IntPtr target);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int TemporalOutlinesSetter(IntPtr context, IntPtr mask, IntPtr sobel, IntPtr composite);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        internal delegate int FullResolutionOutlinesSetter(IntPtr context, IntPtr mask, IntPtr objectDepth, IntPtr occlusion, IntPtr sobel, int reversedDepth);

        [StructLayout(LayoutKind.Sequential)]
        internal struct Matrix16
        {
            internal float A0,A1,A2,A3,B0,B1,B2,B3,C0,C1,C2,C3,D0,D1,D2,D3;
        }
        [StructLayout(LayoutKind.Sequential)]
        internal struct SceneInputsDescription
        {
            internal uint Size, Version;
            internal int Width, Height;
            internal IntPtr Depth;
        }
        [StructLayout(LayoutKind.Sequential)]
        internal struct UpscalerDescription
        {
            internal uint Size,Version;
            internal int Width,Height;
            internal IntPtr SceneInputs,Color,Depth,Motion,Output;
            // Version 2: render size above; display-sized output; DlssQuality value.
            // Placement: 0 end of effects, 1 before post-processing (DLAA in place; DLSS into the display-sized output).
            internal int OutputWidth,OutputHeight,Quality,Placement;
        }
        [StructLayout(LayoutKind.Sequential)]
        internal struct CameraFrame
        {
            internal uint Size,Version;
            internal int Frame,Reset;
            internal Matrix16 Projection,InverseProjection,ClipToPrevious,PreviousToClip;
            internal float JitterX,JitterY,MotionScaleX,MotionScaleY,Near,Far,Fov,Aspect;
            internal float PosX,PosY,PosZ,UpX,UpY,UpZ,RightX,RightY,RightZ,ForwardX,ForwardY,ForwardZ;
            internal int ReversedDepth;
        }
        [StructLayout(LayoutKind.Sequential)]
        internal struct DiagnosticsDescription
        {
            internal uint Size, Version;
            internal int InputWidth, InputHeight, OutputWidth, OutputHeight;
            internal float Near, Far, DepthRange, MotionGain;
            internal int ReversedDepth, Reserved;
            internal IntPtr Depth, Motion, DepthPreview, MotionPreview, FinalColor, SceneColor, SceneBeforeHud;
        }
        [StructLayout(LayoutKind.Sequential)]
        internal struct CaptureDescription
        {
            internal uint Size, Version;
            internal int UnityFrame, Reserved;
            internal IntPtr SceneColor, SceneBeforeHud, Depth, Motion, DepthPreview, MotionPreview, FinalColor;
        }

        // Module, logging and presentation.
        internal static InitializeDelegate Initialize;
        internal static IntAction SetLoggingLevel;
        internal static IntQuery GetObservedDeviceCount, GetPresentationState;
        internal static string PresentationLabel
        {
            get
            {
                int state=GetPresentationState!=null?GetPresentationState():-3;
                return state==2?"D3D12 bridge":state==1?"Bridge pending":state==-2?"Bridge failed":state==-1?"Native (bridge unavailable)":state==0?"Native D3D11":"Unavailable";
            }
        }
        // Scene inputs.
        internal static SceneInputsCreateDelegate SceneInputsCreate;
        internal static EventGetter SceneInputsGetEvent, SceneInputsGetFrameEvent;
        internal static ContextQuery SceneInputsState;
        internal static ContextAction SceneInputsDestroy;
        // Upscaling.
        internal static UpscalerCreateDelegate UpscalerCreate;
        internal static UpscalerSubmitDelegate UpscalerSubmitFrame;
        internal static EventGetter UpscalerGetEvaluateEvent, UpscalerGetSharpenEvent, UpscalerGetFallbackEvent, UpscalerGetOutlineUpscaleEvent, UpscalerGetOutlineCompositeEvent,
            UpscalerGetOutlineOcclusionEvent, UpscalerGetOutlineSobelEvent;
        internal static OutlineMasksSetter UpscalerSetOutlineMasks;
        internal static TemporalOutlinesSetter UpscalerSetTemporalOutlines;
        internal static FullResolutionOutlinesSetter UpscalerSetFullResolutionOutlines;
        internal static ContextQuery UpscalerState, UpscalerTemporalOutlinesActive, UpscalerFullResolutionOutlinesActive, UpscalerKind;
        internal static ContextAction UpscalerDestroy;
        internal static IntSetter SetUpscalerBackend, SetDlssPreset;
        internal static FloatSetter SetSharpness;
        internal static string UpscalerName(IntPtr context) => context == IntPtr.Zero || UpscalerKind == null ? "" : UpscalerKind(context) switch { 1 => "DLSS", 2 => "FSR", _ => "pending" };
        // Frame Generation and pacing.
        internal static IntAction FrameGenerationSetEnabled;
        internal static IntSetter FrameGenerationSetMultiplier, FrameGenerationSetDynamic, FrameGenerationSetDynamicTarget, ReflexSetFrameLimit, FramePacerWait;
        internal static IntQuery FrameGenerationState, FrameGenerationPresentedFrames, FrameGenerationMultiplier, FrameGenerationDynamic, FrameGenerationKind, ReflexFrameLimit, ReflexGuardWaits;
        internal static EventGetter FrameGenerationGetBeforeHudEvent, FrameGenerationGetAfterHudEvent;
        internal static MarkerAction FrameGenerationMarker;
        internal static string FrameGenerationLabel => FrameGenerationState == null ? "unavailable" : FrameGenerationState() switch {
            3 => BackendName + (FrameGenerationDynamic() == 1 ? " Dynamic (up to " + FrameGenerationMultiplier() + "x)" : " " + FrameGenerationMultiplier() + "x") + " enabled",
            2 => "waiting for gameplay", 1 => "startup pending", -2 => "failed; see native.log", _ => "Off"
        };
        private static string BackendName => FrameGenerationKind?.Invoke() switch { 1 => "DLSS", 2 => "FSR", _ => "" };
        // Diagnostics (overlay previews and capture).
        internal static DiagnosticsCreateDelegate DiagnosticsCreate;
        internal static EventGetter DiagnosticsGetEvent, DiagnosticsGetFinalCopyEvent;
        internal static ContextQuery DiagnosticsState, CaptureState;
        internal static ContextAction DiagnosticsDestroy, CaptureCancel;
        internal static CaptureArmDelegate CaptureArm;
        // Cost profiler (profile.log).
        internal static IntAction ProfilerSetEnabled;
        internal static StringQuery ProfilerRegister;
        internal static StringAction ProfilerSetContext;
        internal static EventGetter ProfilerGetEvent;
        private static IntPtr module;

        internal static void Load(string path)
        {
            if (module != IntPtr.Zero) return;
            module = LoadLibraryEx(Path.GetFullPath(path), IntPtr.Zero, 0x100 | 0x800);
            if (module == IntPtr.Zero) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "Native library load failed: " + path);
            Bind(out Initialize, nameof(Initialize));
            Bind(out SetLoggingLevel, nameof(SetLoggingLevel));
            Bind(out GetObservedDeviceCount, nameof(GetObservedDeviceCount));
            Bind(out GetPresentationState, nameof(GetPresentationState));
            Bind(out SceneInputsCreate, nameof(SceneInputsCreate));
            Bind(out SceneInputsGetEvent, nameof(SceneInputsGetEvent));
            Bind(out SceneInputsGetFrameEvent, nameof(SceneInputsGetFrameEvent));
            Bind(out SceneInputsState, nameof(SceneInputsState));
            Bind(out SceneInputsDestroy, nameof(SceneInputsDestroy));
            Bind(out UpscalerCreate, nameof(UpscalerCreate));
            Bind(out UpscalerSubmitFrame, nameof(UpscalerSubmitFrame));
            Bind(out UpscalerGetEvaluateEvent, nameof(UpscalerGetEvaluateEvent));
            Bind(out UpscalerGetSharpenEvent, nameof(UpscalerGetSharpenEvent));
            Bind(out UpscalerGetFallbackEvent, nameof(UpscalerGetFallbackEvent));
            Bind(out UpscalerGetOutlineUpscaleEvent, nameof(UpscalerGetOutlineUpscaleEvent));
            Bind(out UpscalerGetOutlineCompositeEvent, nameof(UpscalerGetOutlineCompositeEvent));
            Bind(out UpscalerSetOutlineMasks, nameof(UpscalerSetOutlineMasks));
            Bind(out UpscalerSetTemporalOutlines, nameof(UpscalerSetTemporalOutlines));
            Bind(out UpscalerSetFullResolutionOutlines, nameof(UpscalerSetFullResolutionOutlines));
            Bind(out UpscalerGetOutlineOcclusionEvent, nameof(UpscalerGetOutlineOcclusionEvent));
            Bind(out UpscalerGetOutlineSobelEvent, nameof(UpscalerGetOutlineSobelEvent));
            Bind(out UpscalerState, nameof(UpscalerState));
            Bind(out UpscalerTemporalOutlinesActive, nameof(UpscalerTemporalOutlinesActive));
            Bind(out UpscalerFullResolutionOutlinesActive, nameof(UpscalerFullResolutionOutlinesActive));
            Bind(out UpscalerKind, nameof(UpscalerKind));
            Bind(out UpscalerDestroy, nameof(UpscalerDestroy));
            Bind(out SetUpscalerBackend, nameof(SetUpscalerBackend));
            Bind(out SetDlssPreset, nameof(SetDlssPreset));
            Bind(out SetSharpness, nameof(SetSharpness));
            Bind(out FrameGenerationSetEnabled, nameof(FrameGenerationSetEnabled));
            Bind(out FrameGenerationSetMultiplier, nameof(FrameGenerationSetMultiplier));
            Bind(out FrameGenerationSetDynamic, nameof(FrameGenerationSetDynamic));
            Bind(out FrameGenerationSetDynamicTarget, nameof(FrameGenerationSetDynamicTarget));
            Bind(out FrameGenerationDynamic, nameof(FrameGenerationDynamic));
            Bind(out FrameGenerationState, nameof(FrameGenerationState));
            Bind(out FrameGenerationPresentedFrames, nameof(FrameGenerationPresentedFrames));
            Bind(out FrameGenerationMultiplier, nameof(FrameGenerationMultiplier));
            Bind(out FrameGenerationKind, nameof(FrameGenerationKind));
            Bind(out FrameGenerationGetBeforeHudEvent, nameof(FrameGenerationGetBeforeHudEvent));
            Bind(out FrameGenerationGetAfterHudEvent, nameof(FrameGenerationGetAfterHudEvent));
            Bind(out FrameGenerationMarker, nameof(FrameGenerationMarker));
            Bind(out ReflexSetFrameLimit, nameof(ReflexSetFrameLimit));
            Bind(out ReflexFrameLimit, nameof(ReflexFrameLimit));
            Bind(out ReflexGuardWaits, nameof(ReflexGuardWaits));
            Bind(out FramePacerWait, nameof(FramePacerWait));
            Bind(out DiagnosticsCreate, nameof(DiagnosticsCreate));
            Bind(out DiagnosticsGetEvent, nameof(DiagnosticsGetEvent));
            Bind(out DiagnosticsGetFinalCopyEvent, nameof(DiagnosticsGetFinalCopyEvent));
            Bind(out DiagnosticsState, nameof(DiagnosticsState));
            Bind(out DiagnosticsDestroy, nameof(DiagnosticsDestroy));
            Bind(out CaptureArm, nameof(CaptureArm));
            Bind(out CaptureState, nameof(CaptureState));
            Bind(out CaptureCancel, nameof(CaptureCancel));
            Bind(out ProfilerSetEnabled, nameof(ProfilerSetEnabled));
            Bind(out ProfilerRegister, nameof(ProfilerRegister));
            Bind(out ProfilerSetContext, nameof(ProfilerSetContext));
            Bind(out ProfilerGetEvent, nameof(ProfilerGetEvent));
            // Never FreeLibrary: startup hooks and queued render events need this module.
        }

        private static void Bind<T>(out T field, string name) where T : Delegate
        {
            var ptr = GetProcAddress(module, "RiskOfRainUpscaling_" + name);
            if (ptr == IntPtr.Zero) throw new EntryPointNotFoundException("RiskOfRainUpscaling_" + name);
            field = Marshal.GetDelegateForFunctionPointer<T>(ptr);
        }
    }
}
