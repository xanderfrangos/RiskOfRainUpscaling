using System;

namespace RiskOfRainUpscaling
{
    [Flags]
    internal enum DlaaResetReason { None=0, FirstFrame=1, FrameGap=2, TimeGap=4, PositionJump=8, RotationJump=16, FovChange=32 }

    internal static class DlaaHistoryPolicy
    {
        // Sprint/zoom animations keep valid previous-camera matrices. Treat only a
        // large, fast FOV step as a cut; allow larger animation steps at lower FPS.
        internal static float FovCutThreshold(float elapsed) => Math.Max(5f,180f*Math.Max(0f,elapsed));
        internal static DlaaResetReason Reasons(int previousFrame,int frame,float elapsed,float distance,float angle,float fovDelta)
        {
            if(previousFrame==0)return DlaaResetReason.FirstFrame;
            var reason=DlaaResetReason.None;
            if(frame!=previousFrame+1)reason|=DlaaResetReason.FrameGap;
            if(elapsed>0.5f)reason|=DlaaResetReason.TimeGap;
            if(distance>5f)reason|=DlaaResetReason.PositionJump;
            if(angle>45f)reason|=DlaaResetReason.RotationJump;
            if(fovDelta>FovCutThreshold(elapsed))reason|=DlaaResetReason.FovChange;
            return reason;
        }
    }
}
