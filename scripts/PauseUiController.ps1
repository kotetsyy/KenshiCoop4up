# Actual visible input only. Called inside run_test's owned-session transaction.
# Resolve live owned render roots; foreground and keyboard focus must be observed before input.
function Get-CoopPauseObservation {
    param([string]$File,[string]$Role,[int]$Phase,[int]$AfterLine=0)
    if (-not (Test-Path -LiteralPath $File -PathType Leaf)) { return $null }
    $lines=@(Get-Content -LiteralPath $File)
    $latest=-1; $expect=$null; $sample=$null
    for ($i=0; $i -lt $lines.Count; $i++) {
        $line=$lines[$i]
        if ($line -match 'PAUSE_UI RESULT ') { throw 'Native pause scenario already terminated; no input authorized.' }
        if ($line -match "PAUSE_UI (?:EXPECT|SAMPLE|HOLD) role=$Role phase=(\d+)" -and [int]$Matches[1] -gt $latest) { $latest=[int]$Matches[1] }
        if ($line -match "PAUSE_UI HOLD role=$Role phase=$Phase ") { throw 'Native phase already completed; no input authorized.' }
        if ($line -match "PAUSE_UI EXPECT role=$Role phase=$Phase action=(\w+) button=(\d+) x=(-?\d+) y=(-?\d+) width=(\d+) height=(\d+) mouseX=(-?\d+) mouseY=(-?\d+)") {
            $expect=@{LineNumber=$i+1;Line=$line;Action=$Matches[1];Button=[int]$Matches[2];X=[int]$Matches[3];Y=[int]$Matches[4];Width=[int]$Matches[5];Height=[int]$Matches[6]}
        }
        if ($i+1 -gt $AfterLine -and $line -match "PAUSE_UI SAMPLE role=$Role phase=$Phase .*mouseX=(-?\d+) mouseY=(-?\d+)") {
            $sample=@{LineNumber=$i+1;Line=$line;X=[int]$Matches[1];Y=[int]$Matches[2]}
        }
    }
    if ($latest -gt $Phase) { throw 'Native pause phase advanced; no input authorized.' }
    if ($latest -ne $Phase -or -not $expect) { return $null }
    return @{Expect=$expect;Sample=$sample;LineCount=$lines.Count}
}

function Invoke-CoopPauseUi {
    param($State, [int]$HostPid, [int]$JoinPid, [string]$HostLog, [string]$JoinLog,
          [string]$OutDir, [int]$TimeoutSec = 120, [hashtable]$Deadlines = @{})
    if (-not ('CoopPauseInput' -as [type])) {
        $references = if ($PSVersionTable.PSVersion.Major -ge 6) {
            @('System.Drawing.Common','System.Drawing.Primitives','System.Diagnostics.Process','System.Threading.Thread','System.Runtime.InteropServices','System.ComponentModel.Primitives','System.Collections')
        } else { @('System','System.Core','System.Drawing') }
        if ($PSVersionTable.PSVersion.Major -ge 6) {
            # Drawing's private dependencies moved in newer runtimes; older PS7 has neither.
            $references += @('System.Private.Windows.GdiPlus','System.Private.Windows.Core') |
                Where-Object { Test-Path -LiteralPath (Join-Path $PSHOME "$_.dll") -PathType Leaf }
        }
        Add-Type -ReferencedAssemblies $references @'
using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Threading;
public static class CoopPauseInput {
    private static readonly System.Collections.Generic.Dictionary<int,Process> Held=new System.Collections.Generic.Dictionary<int,Process>();
    private static Rect OldClip;
    private static bool Clipped;
    public static string DeliveredUtc;
    public static long DeadlineTicks=DateTime.MaxValue.Ticks;
    private static void RequireBudget() { if(DateTime.UtcNow.Ticks>=DeadlineTicks) throw new InvalidOperationException("Owned input deadline expired."); }
    public static void Register(Process process) { IntPtr handle=process.Handle; Held[process.Id]=process; }
    public static void Confine(IntPtr window) {
        Rect client=Client(window); Point origin=new Point();
        if(!ClientToScreen(window,ref origin) || !GetClipCursor(out OldClip)) throw new InvalidOperationException("Cannot establish original mouse confinement.");
        Rect owned=new Rect(); owned.Left=origin.X; owned.Top=origin.Y; owned.Right=origin.X+client.Right; owned.Bottom=origin.Y+client.Bottom;
        if(!ClipCursor(ref owned)) throw new InvalidOperationException("Cannot confine owned mouse input.");
        Clipped=true;
    }
    public static void ReleaseClip() { if(Clipped) { Clipped=false; if(!ClipCursor(ref OldClip)) throw new InvalidOperationException("Cannot restore original cursor clip."); } }
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] private struct GuiThreadInfo {
        public uint Size, Flags;
        public IntPtr Active, Focus, Capture, MenuOwner, MoveSize, Caret;
        public Rect CaretRect;
    }
    private delegate bool EnumProc(IntPtr window,IntPtr data);
    [StructLayout(LayoutKind.Sequential)] private struct Mouse { public int X,Y; public uint Data,Flags,Time; public UIntPtr Extra; }
    [StructLayout(LayoutKind.Sequential)] private struct Key { public ushort Virtual,Scan; public uint Flags,Time; public UIntPtr Extra; }
    [StructLayout(LayoutKind.Explicit)] private struct Union {
        [FieldOffset(0)] public Mouse Mouse; [FieldOffset(0)] public Key Key;
    }
    [StructLayout(LayoutKind.Sequential)] private struct Input { public uint Type; public Union Value; }
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr h,out Rect r);
    [DllImport("user32.dll")] private static extern bool GetClientRect(IntPtr h,out Rect r);
    [DllImport("user32.dll")] private static extern bool ClientToScreen(IntPtr h,ref Point p);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] private static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern IntPtr WindowFromPoint(Point p);
    [DllImport("user32.dll")] private static extern bool GetClipCursor(out Rect rect);
    [DllImport("user32.dll")] private static extern bool ClipCursor(ref Rect rect);
    [DllImport("user32.dll")] private static extern IntPtr GetAncestor(IntPtr h,uint flags);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumProc callback,IntPtr data);
    [DllImport("user32.dll",SetLastError=true)] private static extern bool AttachThreadInput(uint from,uint to,bool attach);
    [DllImport("user32.dll")] private static extern bool BringWindowToTop(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr SetActiveWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr SetFocus(IntPtr window);
    [DllImport("user32.dll")] private static extern bool GetGUIThreadInfo(uint thread,ref GuiThreadInfo info);
    [DllImport("kernel32.dll")] private static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out Point p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
    [DllImport("user32.dll")] private static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] private static extern IntPtr SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll",SetLastError=true)] private static extern uint SendInput(uint n,Input[] inputs,int size);
    public static void Dpi() {
        try { if(SetProcessDpiAwarenessContext((IntPtr)(-4))!=IntPtr.Zero) return; } catch {}
        SetProcessDPIAware();
    }
    public static void Check(IntPtr window,int pid,long start,Rect work) {
        RequireBudget();
        Process process;
        if(!Held.TryGetValue(pid,out process) || process.HasExited || process.StartTime.ToUniversalTime().Ticks!=start)
            throw new InvalidOperationException("Held owned process identity changed.");
        uint owner; GetWindowThreadProcessId(window,out owner);
        Rect bounds;
        if(owner!=pid || !IsWindowVisible(window) || IsIconic(window) || !GetWindowRect(window,out bounds) ||
           bounds.Left<work.Left || bounds.Top<work.Top || bounds.Right>work.Right || bounds.Bottom>work.Bottom)
            throw new InvalidOperationException("Owned visible window must be wholly inside the non-primary work area; "+FocusEvidence(window));
    }
    public static IntPtr Resolve(int pid,long start,Rect work) {
        // Match screenshot.ps1: do not retain a launcher/cached MainWindowHandle.
        IntPtr best=IntPtr.Zero; long area=0;
        EnumWindows(delegate(IntPtr h,IntPtr data) {
            uint owner; GetWindowThreadProcessId(h,out owner); Rect client;
            if(owner==pid && IsWindowVisible(h) && !IsIconic(h) &&
               GetAncestor(h,2)==h && GetClientRect(h,out client)) {
                long size=(long)client.Right*client.Bottom;
                if(size>area) { area=size; best=h; }
            }
            return true;
        },IntPtr.Zero);
        Check(best,pid,start,work);
        return best;
    }
    private static bool HasFocus(IntPtr window) {
        uint owner; uint thread=GetWindowThreadProcessId(window,out owner);
        GuiThreadInfo info=new GuiThreadInfo(); info.Size=(uint)Marshal.SizeOf(typeof(GuiThreadInfo));
        return thread!=0 && GetForegroundWindow()==window && GetGUIThreadInfo(thread,ref info) &&
               info.Focus!=IntPtr.Zero && GetAncestor(info.Focus,2)==window;
    }
    public static string FocusEvidence(IntPtr window) {
        uint owner,foregroundOwner;
        uint thread=GetWindowThreadProcessId(window,out owner);
        IntPtr foreground=GetForegroundWindow();
        uint foregroundThread=GetWindowThreadProcessId(foreground,out foregroundOwner);
        GuiThreadInfo info=new GuiThreadInfo(); info.Size=(uint)Marshal.SizeOf(typeof(GuiThreadInfo));
        bool observed=thread!=0 && GetGUIThreadInfo(thread,ref info);
        return String.Format("targetHWND={0} rootHWND={1} pid={2} tid={3} foregroundHWND={4} foregroundRootHWND={5} foregroundPid={6} foregroundTid={7} guiObserved={8} activeHWND={9} focusHWND={10}",
            window.ToInt64(),GetAncestor(window,2).ToInt64(),owner,thread,foreground.ToInt64(),
            GetAncestor(foreground,2).ToInt64(),foregroundOwner,foregroundThread,observed,
            info.Active.ToInt64(),info.Focus.ToInt64());
    }
    private static void RequireFocus(IntPtr window,string action) {
        if(!HasFocus(window)) throw new InvalidOperationException(action+"; "+FocusEvidence(window));
    }
    public static string Focus(IntPtr window,int pid,long start,Rect work) {
        Check(window,pid,start,work);
        bool direct=SetForegroundWindow(window); Thread.Sleep(200);
        bool targetAttached=false,foregroundAttached=false,raised=false,requested=false;
        int attachError=0;
        uint owner; uint targetThread=GetWindowThreadProcessId(window,out owner);
        uint currentThread=GetCurrentThreadId();
        IntPtr foreground=GetForegroundWindow();
        uint foregroundThread=GetWindowThreadProcessId(foreground,out owner);
        if(!HasFocus(window)) {
            // Temporary input-queue association, never clicks/Alt keys or unowned HWND operations.
            try {
                if(currentThread!=targetThread) {
                    targetAttached=AttachThreadInput(currentThread,targetThread,true);
                    if(!targetAttached) attachError=Marshal.GetLastWin32Error();
                }
                if((currentThread==targetThread || targetAttached) && foregroundThread!=0 &&
                   foregroundThread!=currentThread && foregroundThread!=targetThread) {
                    foregroundAttached=AttachThreadInput(currentThread,foregroundThread,true);
                    if(!foregroundAttached) attachError=Marshal.GetLastWin32Error();
                }
                Check(window,pid,start,work);
                raised=BringWindowToTop(window); requested=SetForegroundWindow(window);
                if(GetForegroundWindow()==window) { SetActiveWindow(window); SetFocus(window); }
            } finally {
                if(foregroundAttached) AttachThreadInput(currentThread,foregroundThread,false);
                if(targetAttached) AttachThreadInput(currentThread,targetThread,false);
            }
            for(int i=0;i<10 && !HasFocus(window);i++) Thread.Sleep(50);
        }
        Check(window,pid,start,work);
        string attempts=String.Format("direct={0} targetAttached={1} foregroundAttached={2} attachError={3} raised={4} requested={5}",
            direct,targetAttached,foregroundAttached,attachError,raised,requested);
        RequireFocus(window,"Owned window did not obtain foreground input ("+attempts+")");
        return attempts+"; "+FocusEvidence(window);
    }
    public static Rect Client(IntPtr window) {
        Rect r; if(!GetClientRect(window,out r) || r.Right<=0 || r.Bottom<=0)
            throw new InvalidOperationException("No render-client bounds.");
        return r;
    }
    public static void Capture(IntPtr window,int pid,long start,Rect work,string path) {
        Check(window,pid,start,work);
        RequireFocus(window,"Screenshot window lost foreground/focus");
        Rect r=Client(window); Point origin=new Point();
        if(!ClientToScreen(window,ref origin)) throw new InvalidOperationException("No screenshot origin.");
        using(Bitmap bitmap=new Bitmap(r.Right,r.Bottom,PixelFormat.Format32bppArgb)) {
            using(Graphics g=Graphics.FromImage(bitmap)) g.CopyFromScreen(origin.X,origin.Y,0,0,bitmap.Size,CopyPixelOperation.SourceCopy);
            bitmap.Save(path,ImageFormat.Png);
        }
    }
    private static void Send(Input input) {
        if(SendInput(1,new Input[]{input},Marshal.SizeOf(typeof(Input)))!=1)
            throw new InvalidOperationException("Actual OS SendInput was not delivered.");
    }
    public static Point Act(IntPtr window,int pid,long start,Rect work,bool pause,int x,int y,int width,int height,int mouseX,int mouseY) {
        Check(window,pid,start,work);
        RequireFocus(window,"Owned window lost foreground/focus before input");
        Input down=new Input(), up=new Input(); Point target=new Point();
        {
            if(pause) {
                Rect client=Client(window);
                if(x<0 || y<0 || width<=0 || height<=0 || x+width>client.Right || y+height>client.Bottom ||
                   mouseX<0 || mouseY<0 || mouseX>=client.Right || mouseY>=client.Bottom)
                    throw new InvalidOperationException("Native pause controls are outside the captured render-client.");
                int targetX=x+width/2,targetY=y+height/2;
                target.X=targetX; target.Y=targetY;
                Point origin=new Point();
                if(!ClientToScreen(window,ref target) || !ClientToScreen(window,ref origin))
                    throw new InvalidOperationException("Native pause control screen positioning failed.");
                if(GetAncestor(WindowFromPoint(target),2)!=window)
                    throw new InvalidOperationException("Native pause button is occluded by another window.");
                // Kenshi consumes relative raw input, not absolute desktop moves.
                // Small feedback steps converge using fresh native cursor samples.
                int dx=x+width/2-mouseX,dy=y+height/2-mouseY;
                Input move=new Input();
                move.Value.Mouse.X=dx/2; move.Value.Mouse.Y=dy/2;
                if(move.Value.Mouse.X==0 && dx!=0) move.Value.Mouse.X=Math.Sign(dx);
                if(move.Value.Mouse.Y==0 && dy!=0) move.Value.Mouse.Y=Math.Sign(dy);
                move.Value.Mouse.Flags=1;
                Send(move); Thread.Sleep(150);
                Point actual;
                if(!GetCursorPos(out actual) || GetAncestor(WindowFromPoint(actual),2)!=window)
                    throw new InvalidOperationException("Mouse input left the owned visible client.");
                return target; // The observer must confirm the native cursor before any click.
            } else {
                down.Type=up.Type=1; down.Value.Key.Scan=up.Value.Key.Scan=0x39;
                down.Value.Key.Flags=8; up.Value.Key.Flags=10;
            }
            Check(window,pid,start,work);
            RequireFocus(window,"Window changed before actual input");
            Send(down); DeliveredUtc=DateTime.UtcNow.ToString("o"); try { Thread.Sleep(100); } finally { Send(up); }
            return target;
        }
    }
    public static void Click(IntPtr window,int pid,long start,Rect work,int x,int y,int width,int height) {
        Check(window,pid,start,work);
        RequireFocus(window,"Owned window lost focus before verified cursor click");
        Point actual,origin=new Point(); Rect client=Client(window);
        Point target=new Point(); target.X=x+width/2; target.Y=y+height/2;
        if(x<0 || y<0 || width<=0 || height<=0 || x+width>client.Right || y+height>client.Bottom ||
           !ClientToScreen(window,ref target) || GetAncestor(WindowFromPoint(target),2)!=window ||
           !SetCursorPos(target.X,target.Y))
            throw new InvalidOperationException("Owned pause button cursor positioning failed; no click sent.");
        if(x<0 || y<0 || width<=0 || height<=0 || x+width>client.Right || y+height>client.Bottom ||
           !GetCursorPos(out actual) || !ClientToScreen(window,ref origin) ||
           actual.X-origin.X<x || actual.X-origin.X>=x+width || actual.Y-origin.Y<y || actual.Y-origin.Y>=y+height ||
           GetAncestor(WindowFromPoint(actual),2)!=window)
            throw new InvalidOperationException("Current cursor is outside or occluded at the owned pause button; no click sent.");
        Input down=new Input(), up=new Input();
        down.Value.Mouse.Flags=2; up.Value.Mouse.Flags=4;
        Send(down); DeliveredUtc=DateTime.UtcNow.ToString("o"); try { Thread.Sleep(100); } finally { Send(up); }
    }
    public static void Restore(IntPtr window,Point cursor) {
        SetCursorPos(cursor.X,cursor.Y);
        if(window!=IntPtr.Zero) SetForegroundWindow(window);
    }
}
'@
    }
    [CoopPauseInput]::Dpi()
    $liveWork = {
        $area = Get-CoopSecondaryWorkArea
        $rect = New-Object CoopPauseInput+Rect
        $rect.Left=$area.Left; $rect.Top=$area.Top; $rect.Right=$area.Right; $rect.Bottom=$area.Bottom
        return $rect
    }
    $work = & $liveWork
    $peers = @(@{ Role='host'; Id=$HostPid; Log=$HostLog }, @{ Role='join'; Id=$JoinPid; Log=$JoinLog })
    [CoopPauseInput]::DeadlineTicks=[DateTime]::MaxValue.Ticks
    foreach ($peer in $peers) {
        if (-not $State.Processes.ContainsKey($peer.Id)) { throw "Pause input PID $($peer.Id) is not owned by this runner." }
        $process=$State.Processes[$peer.Id]; $process.Refresh()
        if ($Deadlines.ContainsKey($peer.Id) -and $Deadlines[$peer.Id].Ticks -lt [CoopPauseInput]::DeadlineTicks) { [CoopPauseInput]::DeadlineTicks=$Deadlines[$peer.Id].Ticks }
        if ($process.HasExited) { throw 'Owned game exited before pause UI test.' }
        [CoopPauseInput]::Register($process)
        $peer.Start=$process.StartTime.ToUniversalTime().Ticks
        $peer.Window=[CoopPauseInput]::Resolve($peer.Id,$peer.Start,$work)
        [CoopPauseInput]::Check($peer.Window,$peer.Id,$peer.Start,$work)
    }
    $journal = [System.Collections.Generic.List[object]]::new()
    $journalPath=Join-Path $OutDir 'pause_ui_inputs.json'
    ConvertTo-Json -InputObject @() | Set-Content -LiteralPath $journalPath -Encoding UTF8
    $oldFocus=[CoopPauseInput]::GetForegroundWindow()
    $oldCursor=New-Object CoopPauseInput+Point
    [void][CoopPauseInput]::GetCursorPos([ref]$oldCursor)
    $checkBudget = {
        foreach ($ownedPeer in $peers) {
            if ($State.Processes[$ownedPeer.Id].HasExited) { throw 'Owned game exited during pause input.' }
            if ($Deadlines.ContainsKey($ownedPeer.Id) -and [DateTime]::UtcNow -ge $Deadlines[$ownedPeer.Id]) { throw 'Owned pause run deadline expired.' }
        }
    }
    $freshSample = {
        param($peer,$phase,$afterLine,$until)
        do {
            & $checkBudget
            if ([DateTime]::UtcNow -ge $until) { throw 'Fresh current-phase cursor sample timed out.' }
            $observation=Get-CoopPauseObservation $peer.Log $peer.Role $phase $afterLine
            if ($observation -and $observation.Sample) { return $observation }
            Start-Sleep -Milliseconds 50
        } while ($true)
    }
    try {
        $actions=@('host_pause','join_resume','join_pause','host_resume')
        foreach ($phase in 0..3) {
            $expected=@{}; $deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSec)
            while ($expected.Count -ne 2) {
                & $checkBudget
                if ([DateTime]::UtcNow -ge $deadline) { throw "Timed out waiting for both real UI observers at phase $phase." }
                $expected=@{}
                foreach ($observer in $peers) {
                    $observation=Get-CoopPauseObservation $observer.Log $observer.Role $phase
                    if ($observation -and $observation.Expect.Action -eq $actions[$phase]) { $expected[$observer.Role]=$observation.Expect }
                }
                if ($expected.Count -ne 2) { Start-Sleep -Milliseconds 50 }
            }
            $peer=if ($phase -eq 0 -or $phase -eq 3) { $peers[0] } else { $peers[1] }
            $pause=$phase -eq 0 -or $phase -eq 2
            $button=$expected[$peer.Role]
            if ($pause -and $button.Button -ne 1) { throw 'Native pause button unavailable.' }
            & $checkBudget
            $peer.Window=[CoopPauseInput]::Resolve($peer.Id,$peer.Start,(& $liveWork))
            $focusEvidence=[CoopPauseInput]::Focus($peer.Window,$peer.Id,$peer.Start,(& $liveWork))
            $focused=(Get-CoopPauseObservation $peer.Log $peer.Role $phase).LineCount
            $focusObservation=& $freshSample $peer $phase $focused ([DateTime]::UtcNow.AddSeconds(5))
            $origin=$focusObservation.Sample
            $before="pause_phase${phase}_$($peer.Role)_before.png"
            $after="pause_phase${phase}_$($peer.Role)_after.png"
            & $checkBudget
            [CoopPauseInput]::Capture($peer.Window,$peer.Id,$peer.Start,(& $liveWork),(Join-Path $OutDir $before))
            $client=[CoopPauseInput]::Client($peer.Window)
            $inputAfterLines=@{}; $confirm=$origin
            try {
                if ($pause) {
                    [CoopPauseInput]::Confine($peer.Window)
                    $cursorDeadline=[DateTime]::UtcNow.AddSeconds(5)
                    $moveAfter=(Get-CoopPauseObservation $peer.Log $peer.Role $phase).LineCount
                    $origin=(& $freshSample $peer $phase $moveAfter $cursorDeadline).Sample
                    $moveAfter=$origin.LineNumber
                    & $checkBudget
                    $point=[CoopPauseInput]::Act($peer.Window,$peer.Id,$peer.Start,(& $liveWork),$true,
                        $button.X,$button.Y,$button.Width,$button.Height,$origin.X,$origin.Y)
                    do {
                        $arrival=& $freshSample $peer $phase $moveAfter $cursorDeadline
                        $confirm=$arrival.Sample
                        $arrived=[Math]::Abs($confirm.X-($button.X+[int][Math]::Floor($button.Width/2))) -le 1 -and
                            [Math]::Abs($confirm.Y-($button.Y+[int][Math]::Floor($button.Height/2))) -le 1
                        if (-not $arrived) {
                            $moveAfter=$confirm.LineNumber
                            & $checkBudget
                            $point=[CoopPauseInput]::Act($peer.Window,$peer.Id,$peer.Start,(& $liveWork),$true,
                                $button.X,$button.Y,$button.Width,$button.Height,$confirm.X,$confirm.Y)
                        }
                    } while (-not $arrived)
                } else {
                    # Even a key action needs a distinct current-phase observation.
                    $confirm=(& $freshSample $peer $phase $origin.LineNumber ([DateTime]::UtcNow.AddSeconds(5))).Sample
                }
                foreach ($observer in $peers) {
                    $current=Get-CoopPauseObservation $observer.Log $observer.Role $phase
                    if (-not $current) { throw 'Native phase lost before input.' }
                    $inputAfterLines[$observer.Role]=$current.LineCount
                }
                & $checkBudget
                if ($pause) {
                    [CoopPauseInput]::Click($peer.Window,$peer.Id,$peer.Start,(& $liveWork),$button.X,$button.Y,$button.Width,$button.Height)
                } else {
                    $point=[CoopPauseInput]::Act($peer.Window,$peer.Id,$peer.Start,(& $liveWork),$false,
                        $button.X,$button.Y,$button.Width,$button.Height,$confirm.X,$confirm.Y)
                }
                $sentAt=[CoopPauseInput]::DeliveredUtc
            } finally { [CoopPauseInput]::ReleaseClip() }
            Start-Sleep -Milliseconds 300
            & $checkBudget
            $work = & $liveWork
            [CoopPauseInput]::Capture($peer.Window,$peer.Id,$peer.Start,$work,(Join-Path $OutDir $after))
            $journal.Add([ordered]@{
                phase=$phase; action=$actions[$phase]; role=$peer.Role; pid=$peer.Id
                processStartUtcTicks=$peer.Start; windowId=$peer.Window.ToInt64().ToString(); owned=$true
                monitor='non-primary'; workArea=@($work.Left,$work.Top,$work.Right,$work.Bottom)
                focusEvidence=$focusEvidence
                method=$(if ($pause) { 'pause_button' } else { 'space' }); transport='SendInput'; delivered=$true
                client=@($client.Right,$client.Bottom); nativeButton=@($button.X,$button.Y,$button.Width,$button.Height)
                nativeCursor=@($confirm.X,$confirm.Y)
                expectLine=$button.LineNumber; focusSampleLine=$origin.LineNumber; confirmSampleLine=$confirm.LineNumber
                inputAfterLine=$inputAfterLines[$peer.Role]; inputAfterLines=$inputAfterLines; deliveredUtc=$sentAt
                screenPoint=@($point.X,$point.Y); utc=$sentAt; screenshot=$before; afterScreenshot=$after
                screenshotSha256=(Get-FileHash -LiteralPath (Join-Path $OutDir $before) -Algorithm SHA256).Hash
                afterScreenshotSha256=(Get-FileHash -LiteralPath (Join-Path $OutDir $after) -Algorithm SHA256).Hash
            })
            ConvertTo-Json -InputObject @($journal.ToArray()) -Depth 6 | Set-Content -LiteralPath $journalPath -Encoding UTF8
            Copy-Item -LiteralPath (Join-Path $OutDir $after) -Destination (Join-Path $OutDir "$($peer.Role).png") -Force
            # Do not send the next action until BOTH native clocks have completed
            # their eight-second stable hold. A marker on only one peer is unsafe.
            foreach ($observer in $peers) {
                $holdDeadline=[DateTime]::UtcNow.AddSeconds(45)
                do {
                    & $checkBudget
                    $fresh=@(Select-String -LiteralPath $observer.Log -Pattern "PAUSE_UI HOLD role=$($observer.Role) phase=$phase .*pass=1" |
                        Where-Object { $_.LineNumber -gt $inputAfterLines[$observer.Role] })
                    if ($fresh.Count) { break }
                    if (Select-String -LiteralPath $observer.Log -Pattern 'PAUSE_UI RESULT .*pass=0' -Quiet) { throw 'Native pause hold failed.' }
                    if ([DateTime]::UtcNow -ge $holdDeadline) { throw "Native $($observer.Role) did not complete real UI phase $phase." }
                    Start-Sleep -Milliseconds 50
                } while ($true)
            }
        }
    } finally { try { [CoopPauseInput]::ReleaseClip() } finally { [CoopPauseInput]::Restore($oldFocus,$oldCursor) } }
}
