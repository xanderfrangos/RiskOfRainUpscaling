using System;
using RiskOfRainUpscaling;
PackagePathsTests.Run();
ReflexLimiterTests.Run();
HistoryAndMenuTests.Run();
ModSettingsTests.Run();
GameFrameRateTests.Run();
FrameGenerationSettingTests.Run();

// Exhaustive combinations ensure no excluded camera can schedule native work.
int accepted = 0;
for (int bits = 0; bits < 256; bits++)
{
    bool B(int i) => (bits & (1 << i)) != 0;
    bool result = CameraPolicy.IsGameplay(B(0), B(1), B(2), B(3), B(4), B(5), B(6), B(7));
    if (result) { accepted++; if (bits != 231) throw new Exception("An excluded camera was accepted: " + bits); }
}
if (accepted != 1) throw new Exception("Gameplay camera was rejected");
Console.WriteLine("PASS: 256 camera eligibility combinations; only full-viewport local owned gameplay accepted.");
if(System.Runtime.InteropServices.Marshal.SizeOf<NativeApi.SceneInputsDescription>()!=24) throw new Exception("Scene inputs ABI mismatch");
for(int flags=0;flags<32;++flags)
{
    bool B(int bit)=>(flags&(1<<bit))!=0;
    bool routed=WorldRoutingPolicy.CanRoute(B(0),B(1),B(2),B(3),B(4),0,2,-1,true);
    if(routed!=(flags==3))throw new Exception("Unsafe world routing accepted: "+flags);
}
if(WorldRoutingPolicy.CanRoute(true,true,false,false,false,2,0,-1,true)||WorldRoutingPolicy.CanRoute(true,true,false,false,false,0,2,1,true))throw new Exception("Incompatible camera order accepted");
Console.WriteLine("PASS: scene-input ABI, world-routing foreign-target/ownership guards, and camera ordering.");

if(System.Runtime.InteropServices.Marshal.SizeOf<NativeApi.DiagnosticsDescription>()!=104) throw new Exception("Diagnostics ABI mismatch");
if(System.Runtime.InteropServices.Marshal.SizeOf<NativeApi.CaptureDescription>()!=72) throw new Exception("Capture ABI mismatch");
if(System.Runtime.InteropServices.Marshal.SizeOf<NativeApi.UpscalerDescription>()!=72 || System.Runtime.InteropServices.Marshal.SizeOf<NativeApi.CameraFrame>()!=356 ||
   System.Runtime.InteropServices.Marshal.SizeOf<NativeApi.Matrix16>()!=64) throw new Exception("DLAA frame/description ABI mismatch");
void ExpectSize(DlssQuality quality,int width,int height)
{
    if(DlssPolicy.Dimension(2560,quality)!=width || DlssPolicy.Dimension(1440,quality)!=height)
        throw new Exception("DLSS render size for "+quality+" is "+DlssPolicy.Dimension(2560,quality)+"x"+DlssPolicy.Dimension(1440,quality));
}
ExpectSize(DlssQuality.DLAA,2560,1440);ExpectSize(DlssQuality.Quality,1707,960);ExpectSize(DlssQuality.Balanced,1485,835);
ExpectSize(DlssQuality.Performance,1280,720);ExpectSize(DlssQuality.UltraPerformance,853,480);
// FSR's ratios match DLSS except Balanced (1/1.7); both FSR paths size alike.
foreach(DlssQuality q in Enum.GetValues(typeof(DlssQuality))) {
    if(q!=DlssQuality.Balanced && DlssPolicy.Dimension(2560,q,UpscalerBackend.FSR)!=DlssPolicy.Dimension(2560,q))throw new Exception("FSR render size for "+q);
    if(DlssPolicy.Dimension(2560,q,UpscalerBackend.FSR3DX11)!=DlssPolicy.Dimension(2560,q,UpscalerBackend.FSR))throw new Exception("FSR 3 DX11 render size for "+q);
}
if(DlssPolicy.Dimension(2560,DlssQuality.Balanced,UpscalerBackend.FSR)!=1506 || DlssPolicy.Dimension(1440,DlssQuality.Balanced,UpscalerBackend.FSR)!=847)
    throw new Exception("FSR Balanced render size incorrect");
if(DlssPolicy.Resolve(UpscalerBackend.Auto,0,true)!=UpscalerBackend.DLSS || DlssPolicy.Resolve(UpscalerBackend.Auto,0,false)!=UpscalerBackend.FSR3DX11 ||
   DlssPolicy.Resolve(UpscalerBackend.Auto,3,true)!=UpscalerBackend.FSR3DX11 || DlssPolicy.Resolve(UpscalerBackend.FSR3DX11,2,false)!=UpscalerBackend.FSR3DX11 ||
   DlssPolicy.Resolve(UpscalerBackend.Auto,2,true)!=UpscalerBackend.FSR || DlssPolicy.Resolve(UpscalerBackend.Auto,1,false)!=UpscalerBackend.DLSS ||
   DlssPolicy.Resolve(UpscalerBackend.FSR,1,true)!=UpscalerBackend.FSR || DlssPolicy.Resolve(UpscalerBackend.DLSS,2,false)!=UpscalerBackend.DLSS)
    throw new Exception("Upscaler resolution for render sizing incorrect");
if(DlssPolicy.JitterPhases(2560,2560)!=8 || DlssPolicy.JitterPhases(1707,2560)!=18 || DlssPolicy.JitterPhases(1280,2560)!=32 || DlssPolicy.JitterPhases(853,2560)!=73)
    throw new Exception("DLSS jitter phase count incorrect");
if(DlssPolicy.MipBias(2560,2560,-1)!=0 || Math.Abs(DlssPolicy.MipBias(1280,2560,-1)+2)>1e-6 || Math.Abs(DlssPolicy.MipBias(1707,2560,0)-Math.Log(1707.0/2560,2))>1e-6)
    throw new Exception("DLSS mip bias incorrect");
if(DlssPolicy.NativePreset(DlssPreset.Default)!=0 || DlssPolicy.NativePreset(DlssPreset.K)!=11 || DlssPolicy.NativePreset(DlssPreset.M)!=13 ||
   DlssPolicy.NativePreset(DlssPreset.L)!=12 || DlssPolicy.NativePreset(DlssPreset.J)!=10 || DlssPolicy.NativePreset(DlssPreset.E)!=5)
    throw new Exception("DLSS preset mapping incorrect");
var cycled=DlssQuality.DLAA;for(int i=0;i<5;++i)cycled=DlssPolicy.Next(cycled);
if(cycled!=DlssQuality.DLAA || DlssPolicy.Next(DlssQuality.DLAA)!=DlssQuality.Quality) throw new Exception("F7 quality cycle incorrect");
foreach(DlssQuality q in Enum.GetValues(typeof(DlssQuality)))foreach(DlaaPlacement p in Enum.GetValues(typeof(DlaaPlacement)))
    if(DlssPolicy.BeforePostProcessing(q,p)!=(p==DlaaPlacement.BeforePostProcessing) || DlssPolicy.UpscalesBeforePostProcessing(q,p)!=(q!=DlssQuality.DLAA && p==DlaaPlacement.BeforePostProcessing))
        throw new Exception("Pre-post placement policy incorrect: "+q+"/"+p);
foreach(var bad in new[]{(0,2560),(2561,2560)})
{
    bool threw=false;try{DlssPolicy.JitterPhases(bad.Item1,bad.Item2);}catch(ArgumentOutOfRangeException){threw=true;}
    if(!threw)throw new Exception("Invalid DLSS render size accepted");
}
Console.WriteLine("PASS: DLSS render sizes (DLAA/Quality/Balanced/Performance/Ultra Performance at 1440p), FSR ratios and Auto sizing resolution, jitter phases, mip bias, F7 cycle, pre-post placement for DLAA and upscaling, and 72-byte DLSS descriptor ABI.");

// Managed and native ship together: every binding must resolve in the built library.
string nativeLibrary = null;
for (var directory = new System.IO.DirectoryInfo(AppContext.BaseDirectory); directory != null && nativeLibrary == null; directory = directory.Parent)
{
    var candidate = System.IO.Path.Combine(directory.FullName, "build", "native-nmake", "RiskOfRainUpscaling.Native.dll");
    if (System.IO.File.Exists(candidate)) nativeLibrary = candidate;
}
if (nativeLibrary == null) Console.WriteLine("SKIP: native library not built; export binding check skipped.");
else
{
    NativeApi.Load(nativeLibrary);
    foreach(DlssPreset preset in Enum.GetValues(typeof(DlssPreset)))
        if(NativeApi.SetDlssPreset(DlssPolicy.NativePreset(preset))!=0) throw new Exception("Native rejected DLSS preset "+preset);
    if(NativeApi.SetDlssPreset(1)!=-1 || NativeApi.SetDlssPreset(14)!=-1 || NativeApi.SetDlssPreset(0)!=0) throw new Exception("Native accepted a removed/unsupported DLSS preset");
    Console.WriteLine("PASS: every managed binding resolves in " + nativeLibrary + "; every DLSS preset is accepted natively.");
}
