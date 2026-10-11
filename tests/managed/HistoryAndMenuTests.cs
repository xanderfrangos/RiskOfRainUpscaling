using System;
using RiskOfRainUpscaling;

internal static class HistoryAndMenuTests
{
    internal static void Run()
    {
        void Expect(DlaaResetReason expected,int previous,int frame,float elapsed,float distance,float rotation,float fov)
        {
            var actual=DlaaHistoryPolicy.Reasons(previous,frame,elapsed,distance,rotation,fov);
            if(actual!=expected)throw new Exception($"History expected {expected}, got {actual}; dt={elapsed}, FOV step={fov}");
        }
        Expect(DlaaResetReason.FirstFrame,0,100,999,999,999,999);
        Expect(DlaaResetReason.None,100,101,.5f,5,45,.1f);
        Expect(DlaaResetReason.FrameGap|DlaaResetReason.TimeGap|DlaaResetReason.PositionJump|
            DlaaResetReason.RotationJump|DlaaResetReason.FovChange,100,102,.6f,6,46,120);
        Expect(DlaaResetReason.None,100,101,.016f,.1f,1,.11f);
        Expect(DlaaResetReason.None,100,101,1f/60f,0,0,5);
        Expect(DlaaResetReason.FovChange,100,101,1f/60f,0,0,5.01f);
        Expect(DlaaResetReason.None,100,101,1f/30f,0,0,6);
        Expect(DlaaResetReason.FovChange,100,101,1f/30f,0,0,6.01f);
        Expect(DlaaResetReason.FovChange,100,101,0,0,0,12);
        // Simulate sprint/zoom in both directions, with the same animation duration
        // at 15/30/60/120 FPS. History should survive each consecutive frame.
        foreach(int fps in new[]{15,30,60,120}) {
            float previous=70;
            int frames=(int)Math.Ceiling(fps*.4);
            for(int i=1;i<=frames*2;i++) {
                double t=i<=frames?(double)i/frames:(double)(frames*2-i)/frames;
                float current=70+30*(float)(.5-.5*Math.Cos(Math.PI*t));
                Expect(DlaaResetReason.None,100+i-1,100+i,1f/fps,.1f,1,Math.Abs(current-previous));
                previous=current;
            }
        }
        Expect(DlaaResetReason.PositionJump,100,101,1f/30f,6,0,4);
        Expect(DlaaResetReason.RotationJump,100,101,1f/30f,0,46,4);
        Expect(DlaaResetReason.FrameGap,100,102,1f/30f,0,0,4);
        Expect(DlaaResetReason.TimeGap,100,101,.51f,0,0,4);
        Console.WriteLine("PASS: animated FOV at 15/30/60/120 FPS retains history; abrupt cuts, gaps and teleports still reset.");

        int accepted=0;
        for(int bits=0;bits<512;bits++) {
            bool B(int i)=>(bits&(1<<i))!=0;
            bool result=CameraPolicy.IsMainMenu(B(0),B(1),B(2),B(3),B(4),B(5),B(6),B(7),B(8));
            if(result) {++accepted;if(bits!=463)throw new Exception("Excluded menu camera accepted: "+bits);}
        }
        if(accepted!=1)throw new Exception("Title/menu camera rejected");
        Console.WriteLine("PASS: 512 menu eligibility combinations exclude missing ownership/controller, gameplay runs, other scenes, foreign targets and partial viewports.");

        accepted=0;
        for(int bits=0;bits<256;bits++) {
            bool B(int i)=>(bits&(1<<i))!=0;
            bool result=CameraPolicy.IsCharacterSelect(B(0),B(1),B(2),B(3),B(4),B(5),B(6),B(7));
            if(result) {++accepted;if(bits!=231)throw new Exception("Excluded character select camera accepted: "+bits);}
        }
        if(accepted!=1)throw new Exception("Character select camera rejected");
        Console.WriteLine("PASS: 256 character select combinations exclude missing ownership/lobby controller, active runs, foreign targets and partial viewports.");

        accepted=0;
        for(int bits=0;bits<512;bits++) {
            bool B(int i)=>(bits&(1<<i))!=0;
            bool result=CameraPolicy.IsIntro(B(0),B(1),B(2),B(3),B(4),B(5),B(6),B(7),B(8));
            if(result) {++accepted;if(bits!=463)throw new Exception("Excluded intro camera accepted: "+bits);}
        }
        if(accepted!=1)throw new Exception("Intro cutscene camera rejected");
        Console.WriteLine("PASS: 512 intro eligibility combinations exclude missing ownership/intro controller, active runs, other scenes, foreign targets and partial viewports.");
    }
}
