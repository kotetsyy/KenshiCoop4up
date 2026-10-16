[CmdletBinding(SupportsShouldProcess=$true)]
param()
$ErrorActionPreference='Stop'
# Aggregate native statuses ourselves, independent of the caller's PS7 preference.
$PSNativeCommandUseErrorActionPreference=$false
$started=$false;$failed=$false
try {
    foreach($id in @('Microsoft.VCRedist.2010.x64','Microsoft.VCRedist.2010.x86')) {
        if(-not $PSCmdlet.ShouldProcess($id,'Uninstall Visual C++ 2010 redistributable')){continue}
        $winget=(Get-Command winget.exe -ErrorAction Stop).Source
        if(-not $started){Start-Transcript -Path "$PSScriptRoot\uninstall_redists.log" -Force|Out-Null;$started=$true}
        & $winget uninstall --exact --id $id --silent --disable-interactivity
        $code=$LASTEXITCODE
        Write-Output "$id exit code: $code"
        if($code -notin @(0,1641,3010)){$failed=$true}
    }
} finally {if($started){Stop-Transcript|Out-Null}}
if($failed){exit 1}
exit 0
