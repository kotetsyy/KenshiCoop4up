[CmdletBinding()]
param([switch]$DesktopFixtures)
# Disposable consumer-boundary fixtures; no game, desktop input or window movement.
$ErrorActionPreference='Stop'
$scripts=Split-Path $PSScriptRoot -Parent
. (Join-Path $scripts 'kit_preflight.ps1')
Import-Module (Join-Path $scripts 'CoopHarness.psm1') -Force
function Reject([scriptblock]$Action,[string]$Name) { $failed=$false; try { & $Action } catch { $failed=$true }; if(-not $failed){throw "Missing rejection: $Name"} }
function RejectMatching([scriptblock]$Action,[string]$Pattern) {
    try { & $Action; throw 'Missing expected boundary rejection.' } catch { if($_.Exception.Message -notmatch $Pattern) { throw } }
}
function Load-NativeBoundary([string]$File,[string]$Class,[string[]]$References=@()) {
    $tokens=$null; $errors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile($File,[ref]$tokens,[ref]$errors)
    if ($errors.Count) { throw $errors[0] }
    $strings=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.StringConstantExpressionAst]},$true) |
        Where-Object { $_.Value -match "public static class $Class\b" })
    if ($strings.Count -ne 1) { throw "Cannot load actual native boundary $Class." }
    if ($References.Count) { Add-Type -ReferencedAssemblies $References -TypeDefinition $strings[0].Value }
    else { Add-Type -TypeDefinition $strings[0].Value }
}
$root=Join-Path $env:TEMP ('HarnessSafety-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
$original=@{}
Get-ChildItem Env: | Where-Object {$_.Name -like 'KENSHICOOP_*'} | ForEach-Object {$original[$_.Name]=$_.Value}
function Snapshot([string]$Name) {
    $dir=Join-Path $root $Name
    New-Item -ItemType Directory -Path $dir | Out-Null
    return @{Root=$dir;Processes=@{};Environment=$original.Clone();Backups=@()}
}
try {
    $env:KENSHICOOP_CELL_AUTH='1'
    [void](Set-CoopDiagEnv $null)
    if($null -ne [Environment]::GetEnvironmentVariable('KENSHICOOP_CELL_AUTH')){throw 'Free play overrode native authority default'}
    [void](Set-CoopDiagEnv @{})
    if($env:KENSHICOOP_CELL_AUTH -ne '0'){throw 'Scripted baseline not pinned'}
    [void](Set-CoopDiagEnv @{DiagEnv=@{KENSHICOOP_CELL_AUTH='1'}})
    if($env:KENSHICOOP_CELL_AUTH -ne '1'){throw 'Explicit manifest lost priority'}
    $environmentState=@{Environment=@{KENSHICOOP_SPIKE='401';KENSHICOOP_UDP_PROFILE_PATH='private-profile'}}
    foreach ($none in @(@{Value=$null},@{Value=@()})) {
        $env:KENSHICOOP_MODE='stale'
        $env:KENSHICOOP_SPIKE='402'
        Clear-CoopRunEnvironment -State $environmentState -PreserveVariables $none.Value
        if ($null -ne [Environment]::GetEnvironmentVariable('KENSHICOOP_MODE') -or
            $null -ne [Environment]::GetEnvironmentVariable('KENSHICOOP_SPIKE')) {throw 'No-preserve run retained inherited scenario environment'}
    }
    $env:KENSHICOOP_SPIKE='402'
    $env:KENSHICOOP_UDP_PROFILE_PATH='stale-profile'
    Clear-CoopRunEnvironment -State $environmentState -PreserveVariables @('KENSHICOOP_SPIKE')
    if ($env:KENSHICOOP_SPIKE -ne '401' -or
        $null -ne [Environment]::GetEnvironmentVariable('KENSHICOOP_UDP_PROFILE_PATH')) {throw 'Explicit spike preservation retained stale credentials or lost captured scenario'}
    Reject {New-CoopRunState -GameDirs @($root) -SaveNames @('../fixture')} 'invalid save before any topology/snapshot'
    $hostInstall=Join-Path $root 'Host'
    Reject {Assert-CoopPeerInstallationDisjoint -GameDir $hostInstall -PeerExecutable (Join-Path $hostInstall 'kenshi_x64.exe')} 'peer root executable installation'
    Reject {Assert-CoopPeerInstallationDisjoint -GameDir $hostInstall.ToUpperInvariant() -PeerExecutable (Join-Path $hostInstall 'RE_Kenshi\kenshi_x64.exe')} 'peer RE executable installation'
    Assert-CoopPeerInstallationDisjoint -GameDir (Join-Path $root 'Join') -PeerExecutable (Join-Path $hostInstall 'RE_Kenshi\kenshi_x64.exe')
    if ((Get-CoopOwnedDirectorySlot -GameDirs @($hostInstall) -WorkDir $hostInstall.ToUpperInvariant() -PeerCount 0) -ne 0) {throw 'Case-equivalent host directory lost slot'}
    if ((Get-CoopOwnedDirectorySlot -GameDirs @($hostInstall) -WorkDir $hostInstall.ToUpperInvariant() -PeerCount 1) -ne 1) {throw 'Case-equivalent join directory lost reserved slot'}
    $work=@{Left=-1920;Top=0;Right=0;Bottom=1080;Width=1920;Height=1080}
    $placement=Get-CoopWindowPlacement -Work $work -Slot 1 -Columns 2 -Width 900 -Height 900 -Gap 20
    if ($placement.X -ne -950 -or $placement.Y -ne 90) {throw 'Negative-origin secondary slot geometry is wrong'}
    Reject {Get-CoopWindowPlacement -Work $work -Slot 1 -Columns 2 -Width 1280 -Height 900 -Gap 20} 'pair overflow'
    Reject {Get-CoopWindowPlacement -Work $work -Slot 0 -Columns 1 -Width 900 -Height 1200 -Gap 20} 'height overflow'
    Reject {Get-CoopWindowPlacement -Work $work -Slot -1 -Columns 2 -Width 900 -Height 900 -Gap 20} 'invalid role slot'
    $live=Join-Path $root 'live.cfg'; [IO.File]::WriteAllText($live,'original')
    $state=Snapshot 'missing-backup'; Add-CoopRunBackup $state $live
    [IO.File]::WriteAllText($live,'current-recoverable')
    Remove-Item -LiteralPath $state.Backups[0].Backup
    Reject {Restore-CoopRunState $state} 'missing backup before delete'
    if([IO.File]::ReadAllText($live) -ne 'current-recoverable' -or -not (Test-Path -LiteralPath $state.Root)){throw 'Missing backup destroyed destination/recovery root'}
    # Real Windows delete-sharing locks, not mocked filesystem errors.
    Add-Type @'
using System;
using System.IO;
using System.Threading;
public sealed class OwnedRollbackDelayedLock : IDisposable {
    readonly FileStream file;
    readonly Thread release;
    public OwnedRollbackDelayedLock(string path) {
        file=File.Open(path,FileMode.Open,FileAccess.Read,FileShare.Read);
        release=new Thread(delegate() { Thread.Sleep(1000); file.Dispose(); });
        release.IsBackground=true;
        release.Start();
    }
    public void Dispose() { release.Join(); file.Dispose(); }
}
'@
    $locked=Join-Path $root 'locked-runtime.dll'
    [IO.File]::WriteAllText($locked,'original-runtime')
    $state=Snapshot 'delayed-unmap'; Add-CoopRunBackup $state $locked
    [IO.File]::WriteAllText($locked,'test-runtime')
    $held=New-Object OwnedRollbackDelayedLock $locked
    try {Restore-CoopRunState $state} finally {$held.Dispose()}
    if([IO.File]::ReadAllText($locked) -cne 'original-runtime' -or (Test-Path -LiteralPath $state.Root)){throw 'Released native image lock prevented exact restoration'}
    $state=Snapshot 'persistent-unmap'; Add-CoopRunBackup $state $locked
    [IO.File]::WriteAllText($locked,'still-recoverable')
    $held=[IO.File]::Open($locked,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try {Reject {Restore-CoopRunState $state} 'persistent native image lock'} finally {$held.Dispose()}
    if([IO.File]::ReadAllText($locked) -cne 'still-recoverable' -or
        [IO.File]::ReadAllText($state.Backups[0].Backup) -cne 'original-runtime' -or
        -not (Test-Path -LiteralPath $state.Root)){throw 'Persistent lock lost original or current recovery data'}
    Restore-CoopRunState $state
    $external=Join-Path $root 'external'; New-Item -ItemType Directory -Path $external | Out-Null
    [IO.File]::WriteAllText((Join-Path $external 'existing.cfg'),'external')
    $link=Join-Path $root 'junction'; New-Item -ItemType Junction -Path $link -Target $external | Out-Null
    try {
        $state=Snapshot 'junction-backup'
        Reject {Add-CoopRunBackup $state (Join-Path $link 'existing.cfg')} 'existing leaf ancestor junction'
        Reject {Add-CoopRunBackup $state (Join-Path $link 'absent.cfg')} 'absent leaf ancestor junction'
        $state.Root=Join-Path $link 'backup'
        Reject {Add-CoopRunBackup $state $live} 'backup root ancestor junction'
        if([IO.File]::ReadAllText((Join-Path $external 'existing.cfg')) -ne 'external'){throw 'External target modified'}
    } finally {[IO.Directory]::Delete($link)}
    $parent=Join-Path $root 'redirect'; New-Item -ItemType Directory -Path $parent | Out-Null
    $dest=Join-Path $parent 'existing.cfg'; [IO.File]::WriteAllText($dest,'owned-before')
    $state=Snapshot 'redirect-backup'; Add-CoopRunBackup $state $dest
    Remove-Item -LiteralPath $parent -Recurse -Force
    New-Item -ItemType Junction -Path $parent -Target $external | Out-Null
    try {
        Reject {Restore-CoopRunState $state} 'post-snapshot redirection'
        if([IO.File]::ReadAllText((Join-Path $external 'existing.cfg')) -ne 'external' -or -not (Test-Path -LiteralPath $state.Root)){throw 'Restore followed redirected parent'}
    } finally {[IO.Directory]::Delete($parent)}
    $state=Snapshot 'uncertain-cleanup'; Add-CoopRunBackup $state $live
    $credentials=Join-Path $root 'credentials'; New-Item -ItemType Directory -Path $credentials | Out-Null
    [IO.File]::WriteAllText((Join-Path $credentials 'host.credential'),'private-fixture')
    $unassociated=New-Object Diagnostics.Process; $state.Processes[123]=$unassociated
    $state.Environment['KENSHICOOP_FIXTURE_ORIGINAL']='captured'; $env:KENSHICOOP_FIXTURE_ORIGINAL='replacement'; $env:KENSHICOOP_FIXTURE_NEW='new'
    try {Reject {Restore-CoopHarnessSession $state $credentials} 'uncertain closure'} finally {$unassociated.Dispose()}
    if(-not (Test-Path -LiteralPath $credentials) -or -not (Test-Path -LiteralPath $state.Root) -or $env:KENSHICOOP_FIXTURE_ORIGINAL -ne 'captured' -or $null -ne [Environment]::GetEnvironmentVariable('KENSHICOOP_FIXTURE_NEW')){throw 'Uncertain cleanup lost credentials/backups/environment'}
    # Setter must preserve every unrelated byte, BOM and newline, even bracket paths.
    $cfgDir=Join-Path $root 'config[owned]'; New-Item -ItemType Directory -Path $cfgDir | Out-Null
    $setter=Join-Path $scripts 'set_video_mode.ps1'; $cfg=Join-Path $cfgDir 'kenshi.cfg'
    foreach($encoding in @([Text.Encoding]::GetEncoding(28591),(New-Object Text.UTF8Encoding($true)),[Text.Encoding]::Unicode,[Text.Encoding]::BigEndianUnicode)) {
        foreach($newline in @("`n","`r`n")) {
            $before="Unrelated=caf$([char]233)${newline}Video Mode=1 x 2 @ 32-bit colour [0]${newline}Full Screen=Yes${newline}Tail=unchanged"
            $want="Unrelated=caf$([char]233)${newline}Video Mode=640 x 480 @ 32-bit colour [0]${newline}Full Screen=No${newline}Tail=unchanged"
            [IO.File]::WriteAllBytes($cfg,[byte[]]($encoding.GetPreamble()+$encoding.GetBytes($before)))
            & $setter -Width 640 -Height 480 -HostDir $cfgDir -JoinDir $cfgDir
            $expected=[Convert]::ToBase64String([byte[]]($encoding.GetPreamble()+$encoding.GetBytes($want)))
            if([Convert]::ToBase64String([IO.File]::ReadAllBytes($cfg)) -cne $expected){throw 'Display setter changed unrelated bytes'}
            $hash=(Get-FileHash -LiteralPath $cfg).Hash
            & $setter -Width 640 -Height 480 -HostDir $cfgDir -JoinDir $cfgDir
            if((Get-FileHash -LiteralPath $cfg).Hash -cne $hash){throw 'Setter not idempotent'}
        }
    }
    $settings=Join-Path $cfgDir 'settings.cfg'
    [IO.File]::WriteAllBytes($settings,[Text.Encoding]::GetEncoding(28591).GetBytes("Other=caf$([char]233)`nUser save location = 1`nNoTrailing=1"))
    Set-CoopSaveLocation -GameDir $cfgDir -UserLocation 0
    if ([Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($settings)) -cne "Other=caf$([char]233)`nUser save location = 0`nNoTrailing=1") {throw 'Save location setter changed unrelated bytes'}
    . (Join-Path $scripts 'PauseUiController.ps1')
    $log=Join-Path $root 'pause.log'
    $expect='PAUSE_UI EXPECT role=host phase=0 action=host_pause button=1 x=100 y=100 width=20 height=20 mouseX=5 mouseY=5'
    $sample='PAUSE_UI SAMPLE role=host phase=0 elapsedMs=1 mouseX=110 mouseY=110'
    [IO.File]::WriteAllLines($log,@($expect,$sample))
    $observation=Get-CoopPauseObservation $log host 0 2
    if ($observation.Sample) {throw 'Historical pre-movement sample authorized input'}
    [IO.File]::AppendAllText($log,$sample+[Environment]::NewLine)
    $observation=Get-CoopPauseObservation $log host 0 2
    if ($observation.Sample.LineNumber -ne 3 -or $observation.Sample.X -ne 110) {throw 'Fresh current-phase native sample not consumed'}
    [IO.File]::AppendAllText($log,'PAUSE_UI EXPECT role=host phase=1 action=join_resume'+[Environment]::NewLine)
    Reject {Get-CoopPauseObservation $log host 0} 'advanced native phase'
    [IO.File]::WriteAllLines($log,@($expect,$sample,'PAUSE_UI RESULT role=host pass=1 phases=4'))
    Reject {Get-CoopPauseObservation $log host 0} 'completed-before-input history'
    [IO.File]::WriteAllLines($log,@($expect,$sample,'PAUSE_UI HOLD role=host phase=0 duration=8000 pass=1'))
    Reject {Get-CoopPauseObservation $log host 0} 'historical HOLD authorization'
    # Pending transactions reject before executable discovery, mutation or launch.
    $pendingDir=Join-Path $root 'pending-game'; $pendingMod=Join-Path $pendingDir 'mods\KenshiCoop'
    New-Item -ItemType Directory -Path $pendingMod -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $pendingMod '.kenshicoop-deploy.pending'),'recovery evidence')
    Reject {Start-CoopOwnedGame -State @{} -ScriptDir $scripts -ExePath (Join-Path $pendingDir 'absent.exe') -WorkDir $pendingDir -TimeoutSec 1} 'pending shared launch'
    Reject {& (Join-Path $scripts 'start_kenshi.ps1') -ExePath (Join-Path $pendingDir 'absent.exe') -WorkDir $pendingDir -TimeoutSec 1} 'pending standalone launch'
    $capture=Join-Path $scripts 'screenshot.ps1'; $arrange=Join-Path $scripts 'arrange_windows.ps1'
    $noPng=Join-Path $root 'not-published.png'
    RejectMatching {& $capture -ProcessId $PID -ProcessStartTicks 0 -Out $noPng} 'start-time UTC ticks'
    RejectMatching {& $capture -ProcessId $PID -ProcessStartTicks 1 -Out $noPng} 'identity changed'
    if(Test-Path -LiteralPath $noPng){throw 'Rejected capture published evidence'}
    RejectMatching {& $arrange -HostPid $PID -HostStartTicks 0} 'start-time UTC ticks'
    RejectMatching {& $arrange -HostPid $PID -HostStartTicks 1} 'identity changed'
    Load-NativeBoundary (Join-Path $scripts 'start_kenshi.ps1') KenLauncher
    $self=Get-Process -Id $PID; $null=$self.Handle
    if([KenLauncher]::ClickButton([IntPtr]::Zero,'OK',$self,100)){throw 'Launcher accepted unowned HWND'}
    Load-NativeBoundary $arrange WinArrange
    RejectMatching {[WinArrange]::Move($self,[IntPtr]::Zero,0,0,100,100,$false)} 'ended or changed'
    $nativeRefs=if($PSVersionTable.PSVersion.Major -ge 6) {
        @('System.Drawing.Common','System.Drawing.Primitives','System.Diagnostics.Process','System.Threading.Thread','System.Runtime.InteropServices','System.ComponentModel.Primitives','System.Collections')
    } else { @('System','System.Core','System.Drawing') }
    if($PSVersionTable.PSVersion.Major -ge 6){$nativeRefs+=@('System.Private.Windows.GdiPlus','System.Private.Windows.Core') | Where-Object {Test-Path -LiteralPath (Join-Path $PSHOME "$_.dll")}}
    Load-NativeBoundary $capture WinCap $nativeRefs
    RejectMatching {[WinCap]::CaptureSequence([IntPtr]::Zero,$self,2,16)} 'no longer visible/owned'
    RejectMatching {[WinCap]::Capture([IntPtr]::Zero)} 'capture client|Empty capture'
    Load-NativeBoundary (Join-Path $scripts 'PauseUiController.ps1') CoopPauseInput $nativeRefs
    [CoopPauseInput]::Register($self)
    $noWork=New-Object CoopPauseInput+Rect
    RejectMatching {[CoopPauseInput]::Click([IntPtr]::Zero,$PID,$self.StartTime.ToUniversalTime().Ticks,$noWork,10,10,20,20)} 'Owned visible window'
    $self.Dispose()
    if ($DesktopFixtures) {
        # Opt-in owned STA thread only; never targets a user's game or sends global input.
        $desktopRefs=if($PSVersionTable.PSVersion.Major -ge 6) {
            @('System.Windows.Forms','System.Windows.Forms.Primitives','System.Drawing.Common','System.Drawing.Primitives','System.Threading.Thread','System.Threading','System.ComponentModel.Primitives','System.Runtime.InteropServices')
        } else { @('System','System.Core','System.Windows.Forms','System.Drawing') }
        if($PSVersionTable.PSVersion.Major -ge 6){$desktopRefs+=@('System.Private.Windows.GdiPlus','System.Private.Windows.Core') | Where-Object {Test-Path -LiteralPath (Join-Path $PSHOME "$_.dll")}}
        Add-Type -ReferencedAssemblies $desktopRefs -TypeDefinition @'
using System;
using System.Drawing;
using System.Threading;
using System.Windows.Forms;
using System.Runtime.InteropServices;
public static class HarnessOwnedDesktop {
    private static Thread UiThread;
    private static Form Form;
    private static ManualResetEvent Ready=new ManualResetEvent(false);
    public static IntPtr Window;
    public static volatile bool ClickObserved;
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] private static extern IntPtr CreateWindowEx(uint extended,string cls,string text,uint style,int x,int y,int width,int height,IntPtr parent,IntPtr id,IntPtr instance,IntPtr data);
    private sealed class FixtureForm:Form {
        protected override void WndProc(ref Message message) {
            if(message.Msg==0x111 && (message.WParam.ToInt64() & 0xffff)==1003) {
                ClickObserved=true; Thread.Sleep(2000);
            }
            base.WndProc(ref message);
        }
    }
    public static void Start(int x,int y) {
        UiThread=new Thread(delegate() {
            Form=new FixtureForm(); Form.Text="HarnessSafety owned disposable fixture";
            Form.StartPosition=FormStartPosition.Manual; Form.Location=new Point(x,y); Form.ClientSize=new Size(240,180);
            Form.BackColor=Color.CornflowerBlue;
            Form.Shown+=delegate {
                Window=Form.Handle;
                if(CreateWindowEx(0,"Button","OK",0x50000000,20,20,80,30,Window,(IntPtr)1003,IntPtr.Zero,IntPtr.Zero)==IntPtr.Zero)
                    throw new InvalidOperationException("Actual Win32 launcher button unavailable.");
                Ready.Set();
            };
            Application.Run(Form);
        });
        UiThread.SetApartmentState(ApartmentState.STA); UiThread.Start();
        if(!Ready.WaitOne(10000)) throw new InvalidOperationException("Disposable desktop not ready.");
    }
    public static void Stop() {
        if(Form!=null && !Form.IsDisposed) Form.BeginInvoke(new Action(delegate { Form.Close(); }));
        if(UiThread!=null && !UiThread.Join(10000)) throw new InvalidOperationException("Disposable desktop did not close.");
    }
}
'@
        $work=Get-CoopSecondaryWorkArea
        $self=Get-Process -Id $PID; $null=$self.Handle
        try {
            [HarnessOwnedDesktop]::Start($work.Left+30,$work.Top+30)
            $clock=[Diagnostics.Stopwatch]::StartNew()
            if([KenLauncher]::ClickButton([HarnessOwnedDesktop]::Window,'OK',$self,100)){throw 'Hung owned launcher click reported success'}
            if($clock.ElapsedMilliseconds -gt 1500){throw 'Launcher click exceeded bounded timeout'}
            Start-Sleep -Milliseconds 2200
            if(-not [HarnessOwnedDesktop]::ClickObserved){throw 'Fixture never exercised actual native button/hung message boundary'}
            $frames=[WinCap]::CaptureSequence([HarnessOwnedDesktop]::Window,$self,2,16)
            try {
                foreach($frame in $frames){
                    if($frame.Width -ne 240 -or $frame.Height -ne 180 -or -not [WinCap]::LooksNonBlank($frame)){throw 'Actual decorated client capture framing/content failed'}
                }
            } finally {foreach($frame in $frames){$frame.Dispose()}}
            $png=Join-Path $root 'owned-desktop.png'
            $captureTicks=$self.StartTime.ToUniversalTime().Ticks
            & ($self.Path) -NoProfile -ExecutionPolicy Bypass -File $capture -ProcessId $PID -ProcessStartTicks $captureTicks -Out $png
            if($LASTEXITCODE -ne 0){throw 'Actual screenshot helper returned failure for disposable visible window'}
            $decoded=[Drawing.Image]::FromFile($png)
            try {
                if($decoded.Width -ne 240 -or $decoded.Height -ne 180){throw 'Published PNG client framing failed'}
                foreach($corner in @(@(0,0),@(239,0),@(0,179),@(239,179))) {
                    if($decoded.GetPixel($corner[0],$corner[1]).ToArgb() -ne ([Drawing.Color]::CornflowerBlue).ToArgb()){throw 'Published PNG cropped or lost a client corner'}
                }
            } finally {$decoded.Dispose()}
            RejectMatching {[WinCap]::Capture([IntPtr]::Zero)} 'capture client|Empty capture'
            [HarnessOwnedDesktop]::Stop()
            RejectMatching {[WinCap]::CaptureSequence([HarnessOwnedDesktop]::Window,$self,2,16)} 'no longer visible/owned'
        } finally {try {[HarnessOwnedDesktop]::Stop()} finally {$self.Dispose()}}
    }
    # Real held shells distinguish timely completion from late zero exits.
    $shell=(Get-Process -Id $PID).Path
    $timely=Start-Process $shell -PassThru -ArgumentList @('-NoProfile','-Command','exit 0'); $null=$timely.Handle; $timely.WaitForExit()
    $timelyDeadline=$timely.ExitTime.ToUniversalTime().AddMilliseconds(200)
    Start-Sleep -Milliseconds 300
    Wait-CoopOwnedExit $timely $timelyDeadline; $timely.Dispose()
    $late=Start-Process $shell -PassThru -ArgumentList @('-NoProfile','-Command','exit 0'); $null=$late.Handle; $late.WaitForExit()
    try {Reject {Wait-CoopOwnedExit $late ($late.StartTime.ToUniversalTime().AddMilliseconds(-1))} 'late already-exited zero status'} finally {$late.Dispose()}
    $nonzero=Start-Process $shell -PassThru -ArgumentList @('-NoProfile','-Command','exit 9'); $null=$nonzero.Handle; $nonzero.WaitForExit()
    try {RejectMatching {Wait-CoopOwnedExit $nonzero ([DateTime]::MaxValue)} 'exited with code 9'} finally {$nonzero.Dispose()}
    $rounded=Start-Process $shell -PassThru -ArgumentList @('-NoProfile','-Command','Start-Sleep -Milliseconds 500; exit 0'); $null=$rounded.Handle
    try {RejectMatching {Wait-CoopOwnedExit $rounded ($rounded.StartTime.ToUniversalTime().AddMilliseconds(300))} 'run deadline'} finally {if(-not $rounded.HasExited){$rounded.Kill();$rounded.WaitForExit()};$rounded.Dispose()}
    Write-Output 'Harness safety boundaries passed; no gameplay/desktop certification.'
} finally {
    Get-ChildItem Env: | Where-Object {$_.Name -like 'KENSHICOOP_*'} | ForEach-Object {[Environment]::SetEnvironmentVariable($_.Name,[NullString]::Value,'Process')}
    foreach($name in $original.Keys){[Environment]::SetEnvironmentVariable($name,[string]$original[$name],'Process')}
    if(Test-Path -LiteralPath $root){Remove-Item -LiteralPath $root -Recurse -Force}
}
