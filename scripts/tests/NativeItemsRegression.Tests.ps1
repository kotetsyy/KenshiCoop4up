param([string]$Compiler = 'cl.exe')
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$temp = Join-Path ([IO.Path]::GetTempPath()) ('NativeItemsRegression-' + [guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $temp)
try {
    $source = Join-Path $temp 'witness.cpp'
    @'
#include <cassert>
#include "src/plugin/game/EngineScenario.h"
using namespace coop::engine;
int main() {
    ScenarioItemSample s = {};
    s.bag=4; s.source=2; s.ground=7; s.location=1; s.itemHand[3]=9; s.itemHand[4]=12;
    ScenarioItemStability stable;
    for(unsigned long t=0;t<=1500;t+=500) { stable.observe(true,s,t); assert(!stable.stable()); }
    stable.observe(true,s,2000); assert(stable.stable()); assert(stable.sampleCount==5);
    stable.observe(false,s,2500); assert(!stable.stable()); assert(stable.sampleCount==0);
    for(unsigned long t=3000;t<=5000;t+=500) stable.observe(true,s,t);
    assert(stable.stable());
    ++s.itemHand[4]; stable.observe(true,s,5500); assert(!stable.stable());
    for(unsigned long t=6000;t<=7500;t+=500) stable.observe(true,s,t);
    assert(stable.stable());
    stable.observe(true,s,9500); assert(!stable.stable()); // missing cadence cannot bridge a streak
    --s.bag; stable.observe(true,s,10000); assert(!stable.stable());
    ++s.ground; stable.observe(true,s,10500); assert(!stable.stable());
    s.location=2; stable.observe(true,s,11000); assert(!stable.stable());
    ScenarioItemSample other=s; ++other.source; assert(!sameScenarioItemSample(s,other));
    other=s; ++other.itemHand[0]; assert(!sameScenarioItemSample(s,other));
    other=s; ++other.metadataHash; assert(!sameScenarioItemSample(s,other));
    int world=0,otherWorld=0,minted=0,replacement=0;
    unsigned int hand[5]={4,2,8,9,12},unregistered[5]={};
    assert(scenarioProxyIdentityMatches(&world,&world,3,3,hand,&minted,&minted));
    assert(!scenarioProxyIdentityMatches(&world,&world,3,3,unregistered,&minted,&minted));
    assert(!scenarioProxyIdentityMatches(&world,&world,3,3,hand,&minted,0));
    assert(!scenarioProxyIdentityMatches(&world,&world,3,3,hand,&minted,&replacement));
    assert(!scenarioProxyIdentityMatches(&world,&otherWorld,3,3,hand,&minted,&minted));
    assert(!scenarioProxyIdentityMatches(&world,&world,3,4,hand,&minted,&minted));
}
'@ | Set-Content -LiteralPath $source -Encoding Ascii
    $exe = Join-Path $temp 'witness.exe'
    & $Compiler /nologo /EHsc /I $root $source "/Fe$exe" "/Fo$temp\witness.obj"
    if ($LASTEXITCODE -ne 0) { throw 'NativeItems witness regression compile failed' }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw 'NativeItems witness behavioral assertions failed' }
    Write-Output 'NativeItems stability behavioral assertions passed'
} finally { Remove-Item -LiteralPath $temp -Recurse -Force }
