[CmdletBinding(SupportsShouldProcess=$true)]
param([string]$VsRoot="${env:ProgramFiles(x86)}\Microsoft Visual Studio 10.0")
$ErrorActionPreference='Stop'
$VsRoot=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($VsRoot).TrimEnd('\')+'\'
if(-not(Test-Path -LiteralPath $VsRoot -PathType Container)){throw "Visual Studio 2010 installation not found: $VsRoot"}
# SDK/compiler-only installs are supported; the compiler must be VC10, not
# merely any directory (or a newer Visual Studio tree).
$compiler=Join-Path $VsRoot 'VC\bin\cl.exe'
$vcvars=Join-Path $VsRoot 'VC\vcvarsall.bat'
$headers=Join-Path $VsRoot 'VC\include'
$libraries=Join-Path $VsRoot 'VC\lib'
foreach($artifact in @($compiler,$vcvars)) {
    if(-not(Test-Path -LiteralPath $artifact -PathType Leaf)){throw "Missing VC10 toolchain artifact: $artifact"}
}
foreach($artifact in @($headers,$libraries)) {
    if(-not(Test-Path -LiteralPath $artifact -PathType Container)){throw "Missing VC10 toolchain directory: $artifact"}
}
$version=[Diagnostics.FileVersionInfo]::GetVersionInfo($compiler)
if($version.FileMajorPart -ne 16){throw "Expected Visual C++ 2010 compiler version 16, found $($version.FileVersion): $compiler"}
$path='SOFTWARE\Microsoft\VisualStudio\SxS\VS7'
if(-not $PSCmdlet.ShouldProcess("HKLM (32-bit view)\$path\10.0","Set Visual Studio path to $VsRoot")){return}
$principal=New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Registry repair requires an elevated PowerShell session.'}
$base=$null;$key=$null;$started=$false
try {
    Start-Transcript -Path "$PSScriptRoot\fix_vs7_registry.log" -Force|Out-Null;$started=$true
    # Explicit view avoids double Wow6432Node redirection in a 32-bit shell.
    $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry32)
    $key=$base.CreateSubKey($path)
    $key.SetValue('10.0',$VsRoot,[Microsoft.Win32.RegistryValueKind]::String)
    if($key.GetValue('10.0') -cne $VsRoot){throw 'Visual Studio registry value verification failed.'}
    Write-Output "Visual Studio 2010 registry path verified: $VsRoot"
} finally {
    if($key){$key.Dispose()}
    if($base){$base.Dispose()}
    if($started){Stop-Transcript|Out-Null}
}
