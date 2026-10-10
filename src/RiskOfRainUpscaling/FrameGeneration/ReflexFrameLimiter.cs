using System;
using System.Globalization;
using System.Reflection;
using HarmonyLib;
using UnityEngine;

namespace RiskOfRainUpscaling
{
    internal sealed class ReflexFrameLimiter : IDisposable
    {
        private const string HarmonyId="local.riskofrainupscaling.reflex.limiter";
        private static ReflexFrameLimiter instance;
        private readonly ReflexLimiterController controller;
        private readonly VSyncFrameLimiter vsyncLimiter;
        private readonly Func<bool> enabled;
        private readonly ModLog log;
        private readonly Harmony harmony=new Harmony(HarmonyId);
        private bool patched, disposed;
        internal string Status => patched?(vsyncLimiter.Status??controller.Status):"Unity limiter (hook unavailable)";
        internal ReflexFrameLimiter(Func<bool> enabled, ModLog log)
        {
            this.enabled=enabled;this.log=log;
            controller=new ReflexLimiterController(()=>Application.targetFrameRate,v=>Application.targetFrameRate=v,
                v=>NativeApi.ReflexSetFrameLimit(v),()=>NativeApi.ReflexFrameLimit(),log.Info,()=>NativeApi.FrameGenerationKind()!=2);
            vsyncLimiter=new VSyncFrameLimiter(()=>Application.targetFrameRate,v=>NativeApi.FramePacerWait(v),log.Info);
            if(instance!=null){log.Warning("REFLEX_LIMITER_HOOK_FAILED another limiter instance already owns fps_max");return;}
            try {
                var type=typeof(RoR2.SettingsConVars).GetNestedType("FpsMaxConVar",BindingFlags.NonPublic);
                var setter=AccessTools.DeclaredMethod(type,"SetString",new[]{typeof(string)});
                var getter=AccessTools.DeclaredMethod(type,"GetString");
                if(setter==null || getter==null)throw new MissingMethodException("fps_max setter/getter");
                instance=this;
                harmony.Patch(setter,prefix:new HarmonyMethod(typeof(ReflexFrameLimiter),nameof(SetPrefix)),
                    postfix:new HarmonyMethod(typeof(ReflexFrameLimiter),nameof(SetPostfix)));
                harmony.Patch(getter,prefix:new HarmonyMethod(typeof(ReflexFrameLimiter),nameof(GetPrefix)));
                patched=true;log.Info("REFLEX_LIMITER_HOOK fps_max setter/getter installed; caps mean rendered/base FPS; VSync unchanged.");
            }catch(Exception e){harmony.UnpatchSelf();if(instance==this)instance=null;log.Warning("REFLEX_LIMITER_HOOK_FAILED "+e.Message);}
        }
        private static bool SetPrefix(string __0, ref bool __state)
        {
            __state=int.TryParse(__0,NumberStyles.Integer,CultureInfo.InvariantCulture,out int value);
            if(instance==null || !instance.controller.Owned || !__state)return true;
            instance.controller.GameSetting(value);return false;
        }
        private static void SetPostfix(bool __state)
        {
            if(__state && instance!=null && !instance.controller.Owned)
                instance.controller.GameSetting(Application.targetFrameRate);
        }
        private static bool GetPrefix(ref string __result)
        {
            if(instance==null || !instance.controller.Owned)return true;
            __result=instance.controller.SavedValue;return false;
        }
        internal void Begin(){if(patched && !disposed){
            controller.Tick(enabled(),Application.isFocused,Time.timeScale>0,QualitySettings.vSyncCount);
            vsyncLimiter.Tick(!controller.Owned,Application.isFocused,QualitySettings.vSyncCount);
        }}
        internal void AfterSleep(){if(patched && !disposed)controller.CheckHealth();}
        public void Dispose()
        {
            if(disposed)return;disposed=true;
            if(!patched)return;
            vsyncLimiter.Dispose();controller.Dispose();harmony.UnpatchSelf();
            if(instance==this)instance=null;log.Info("REFLEX_LIMITER_RETIRED fps_max hooks removed.");
        }
    }
}
