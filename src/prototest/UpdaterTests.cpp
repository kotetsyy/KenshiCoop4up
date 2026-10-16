// Disposable-install tests of the production parser and transaction implementation.
#include "../plugin/net/UpdatePackage.h"
#include "../plugin/net/RuntimeImages.h"
#include "LegacyUpdaterParser.h"
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <fstream>
#include <iterator>
#include <stdlib.h>
#include <string.h>
using namespace coop::updater;
static std::string readFile(const std::string& p) { std::ifstream f(p.c_str(), std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
static void put(const std::string& p, const std::string& s) { std::ofstream f(p.c_str(),std::ios::binary); f << s; f.close(); assert(f.good()); }
static std::string manifest(const std::string* hashes) {
    std::string s="schema=2\ntransportAbi=2\nversion=0.2.0\nproto=72\n";
    const char* keys[]={"core","ui","steam","udp"};
    for(int i=0;i<4;++i) s+=std::string(keys[i])+"Sha256="+hashes[i]+"\n"+keys[i]+"Url=https://github.com/kotetsyy/KenshiCoop4up/releases/download/v0.2.0/"+kComponentNames[i]+"\n";
    return s;
}
static void child(const char* exe, const std::string& arguments, DWORD expected) {
    std::string command=std::string("\"")+exe+"\" "+arguments;
    STARTUPINFOA si; PROCESS_INFORMATION pi; ZeroMemory(&si,sizeof(si)); ZeroMemory(&pi,sizeof(pi)); si.cb=sizeof(si);
    assert(CreateProcessA(exe,&command[0],0,0,FALSE,CREATE_NO_WINDOW,0,0,&si,&pi));
    assert(WaitForSingleObject(pi.hProcess,30000)==WAIT_OBJECT_0);
    DWORD code=0; assert(GetExitCodeProcess(pi.hProcess,&code)&&code==expected);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
}
static void mappedRecoveryFixture() {
    char temp[MAX_PATH],dir[MAX_PATH],system[MAX_PATH],exe[MAX_PATH];
    assert(GetTempPathA(MAX_PATH,temp)); assert(GetTempFileNameA(temp,"kcm",0,dir)); assert(DeleteFileA(dir)); assert(CreateDirectoryA(dir,0));
    assert(GetSystemDirectoryA(system,MAX_PATH)); assert(GetModuleFileNameA(0,exe,MAX_PATH));
    const std::string root=std::string(dir)+"\\", pe=readFile(std::string(system)+"\\version.dll");
    assert(pe.size()>1024); // Genuine system PE, not an MZ-prefixed text fixture.
    const std::string original=pe+"\noriginal-fixture\n", promoted=pe+"\npromoted-fixture\n";
    put(root+"promoted.fixture",promoted);
    put(root+"coop_config.json","mapped-config"); put(root+"user.save","mapped-save");
    for(int i=0;i<4;++i) put(root+kComponentNames[i],i==0?original:std::string("MZold-")+kComponentNames[i]);
    child(exe,std::string("--mapped-crash \"")+dir+"\"",77);
    const std::string promotedHash=packageFileHash(root+kComponentNames[0]);
    assert(readFile(root+kComponentNames[0])==promoted);
    child(exe,std::string("--mapped-recover \"")+dir+"\" "+promotedHash,0);
    assert(readFile(root+kComponentNames[0])==original);
    assert(readFile(root+kComponentNames[0]+".quarantine")==promoted);
    assert(packageRecoveryPending(root)); // Evidence survives the mapped-image launch.
    HMODULE restored=LoadLibraryExA((root+kComponentNames[0]).c_str(),0,LOAD_WITH_ALTERED_SEARCH_PATH);
    assert(restored && GetProcAddress(restored,"GetFileVersionInfoSizeA"));
    bool restart=false; std::string error;
    assert(recoverPackageForStartup(root,packageFileHash(root+kComponentNames[0]),&restart,&error));
    assert(!restart && !packageRecoveryPending(root));
    assert(FreeLibrary(restored));
    assert(readFile(root+"coop_config.json")=="mapped-config" && readFile(root+"user.save")=="mapped-save");
    for(int i=0;i<4;++i) { assert(readFile(root+kComponentNames[i])==(i==0?original:std::string("MZold-")+kComponentNames[i])); assert(DeleteFileA((root+kComponentNames[i]).c_str())); }
    assert(DeleteFileA((root+"promoted.fixture").c_str())); assert(DeleteFileA((root+"coop_config.json").c_str())); assert(DeleteFileA((root+"user.save").c_str())); assert(RemoveDirectoryA(dir));
}
// Each lifecycle case gets a new process: successful apply is restart-only,
// even after transaction cleanup. Never share that state with the full suite.
struct RuntimeImageFixture {
    std::string root, original[4], payloads[4];
    PackageManifest package;
    RuntimeImageFixture() {
        char temp[MAX_PATH],dir[MAX_PATH],system[MAX_PATH];
        assert(GetTempPathA(MAX_PATH,temp)); assert(GetTempFileNameA(temp,"kcg",0,dir)); assert(DeleteFileA(dir)); assert(CreateDirectoryA(dir,0));
        assert(GetSystemDirectoryA(system,MAX_PATH));
        root=std::string(dir)+"\\";
        const std::string pe=readFile(std::string(system)+"\\version.dll");
        assert(pe.size()>1024);
        std::string hashes[4],error;
        for(int i=0;i<4;++i) {
            original[i]=pe+"\noriginal-gate-fixture-"+kComponentNames[i]+"\n";
            payloads[i]=pe+"\npromoted-gate-fixture-"+kComponentNames[i]+"\n";
            put(root+kComponentNames[i],original[i]);
            hashes[i]=packageHash(payloads[i].data(),payloads[i].size());
        }
        assert(parsePackageManifest(manifest(hashes),&package,&error));
        put(root+"coop_config.json","gate-private-config"); put(root+"user.save","gate-private-save");
    }
    void originalsIntact() const {
        for(int i=0;i<4;++i) assert(readFile(root+kComponentNames[i])==original[i]);
    }
    void promotedIntact() const {
        for(int i=0;i<4;++i) assert(readFile(root+kComponentNames[i])==payloads[i]);
    }
    ~RuntimeImageFixture() {
        packageTestFailure(-1,-1);
        std::string error;
        assert(recoverPackage(root,&error)); assert(!packageRecoveryPending(root));
        assert(readFile(root+"coop_config.json")=="gate-private-config" && readFile(root+"user.save")=="gate-private-save");
        for(int i=0;i<4;++i) assert(DeleteFileA((root+kComponentNames[i]).c_str()));
        assert(DeleteFileA((root+"coop_config.json").c_str())); assert(DeleteFileA((root+"user.save").c_str()));
        assert(RemoveDirectoryA(root.substr(0,root.size()-1).c_str()));
    }
};
static HMODULE loadFixtureImage(const RuntimeImageFixture& fixture) {
    HMODULE image=LoadLibraryExA((fixture.root+kComponentNames[1]).c_str(),0,LOAD_WITH_ALTERED_SEARCH_PATH);
    assert(image && GetProcAddress(image,"GetFileVersionInfoSizeA"));
    return image;
}
static bool guardedFixtureLoad(const RuntimeImageFixture& fixture,bool uiLoad) {
    RuntimeImageLoadGuard load(uiLoad);
    if(!load.allowed()) return false;
    HMODULE image=loadFixtureImage(fixture);
    assert(FreeLibrary(image));
    return true;
}
static void assertRuntimeLoadsRefused(const RuntimeImageFixture& fixture) {
    assert(runtimeImagesRestartOnly());
    assert(!guardedFixtureLoad(fixture,false)); // Companion load or session start.
    assert(!guardedFixtureLoad(fixture,true)); // A later F2 attempt cannot mix builds.
}
struct RuntimeImageInstall {
    RuntimeImageFixture* fixture;
    HANDLE attempted;
    volatile LONG loadSettled;
};
static DWORD WINAPI runtimeImageInstallThread(void* context) {
    RuntimeImageInstall* work=static_cast<RuntimeImageInstall*>(context);
    assert(SetEvent(work->attempted));
    RuntimeImageApplyGuard apply;
    assert(apply.ready());
    assert(runtimeImagesRestartOnly()); // Fence is published before any rename.
    // Admission is after the actual UI/companion attempt, never just after its
    // thread was started. A ready apply guard owns the entire real transaction.
    assert(InterlockedCompareExchange(&work->loadSettled,0,0)==1);
    work->fixture->originalsIntact();
    assert(!packageRecoveryPending(work->fixture->root));
    std::string error;
    assert(installPackage(work->fixture->root,work->fixture->package,work->fixture->payloads,&error));
    work->fixture->promotedIntact();
    return 0;
}
static HANDLE startRuntimeImageInstall(RuntimeImageInstall* work) {
    work->attempted=CreateEventA(0,TRUE,FALSE,0); assert(work->attempted);
    HANDLE thread=CreateThread(0,0,runtimeImageInstallThread,work,0,0); assert(thread);
    assert(WaitForSingleObject(work->attempted,10000)==WAIT_OBJECT_0);
    return thread;
}
static void finishRuntimeImageInstall(RuntimeImageInstall* work,HANDLE thread) {
    // All finite waits here detect deadlock only; none infer that another
    // thread reached a particular instruction from a quiet interval.
    assert(WaitForSingleObject(thread,10000)==WAIT_OBJECT_0);
    DWORD code=1; assert(GetExitCodeThread(thread,&code) && code==0);
    assert(CloseHandle(thread)); assert(CloseHandle(work->attempted));
    work->fixture->promotedIntact();
    assertRuntimeLoadsRefused(*work->fixture);
    std::string error;
    assert(recoverPackage(work->fixture->root,&error));
    assert(!packageRecoveryPending(work->fixture->root));
    work->fixture->promotedIntact();
    assertRuntimeLoadsRefused(*work->fixture); // Cleanup is not a process restart.
}
static void runtimeImageUiSettles(bool uiSucceeds) {
    RuntimeImageFixture fixture;
    assert(!runtimeImagesRestartOnly());
    RuntimeImageInstall work; work.fixture=&fixture; work.loadSettled=0;
    HANDLE thread=startRuntimeImageInstall(&work);
    // The updater has been requested before the first UI attempt. These file
    // checks and admission ordering exercise the real gate, not a mock counter;
    // the attempted event alone is not proof that its worker is already parked.
    fixture.originalsIntact(); assert(!packageRecoveryPending(fixture.root));
    {
        RuntimeImageLoadGuard ui(true); assert(ui.allowed());
        if(uiSucceeds) {
            HMODULE image=loadFixtureImage(fixture);
            assert(FreeLibrary(image));
        } else {
            assert(!LoadLibraryExA((fixture.root+"missing-ui.dll").c_str(),0,LOAD_WITH_ALTERED_SEARCH_PATH));
        }
        fixture.originalsIntact(); assert(!packageRecoveryPending(fixture.root));
        InterlockedExchange(&work.loadSettled,1);
    } // Both successful and failed first UI attempts must wake the updater.
    finishRuntimeImageInstall(&work,thread);
}
static void runtimeImageCompanionSettles() {
    RuntimeImageFixture fixture;
    assert(guardedFixtureLoad(fixture,true)); // First UI attempt already settled.
    RuntimeImageInstall work; work.fixture=&fixture; work.loadSettled=0;
    HANDLE thread=0;
    {
        RuntimeImageLoadGuard companion; assert(companion.allowed());
        HMODULE image=loadFixtureImage(fixture);
        thread=startRuntimeImageInstall(&work);
        fixture.originalsIntact(); assert(!packageRecoveryPending(fixture.root));
        assert(GetProcAddress(image,"GetFileVersionInfoSizeA"));
        assert(FreeLibrary(image));
        InterlockedExchange(&work.loadSettled,1);
    } // Promotion must not overlap any real companion loader/session guard.
    finishRuntimeImageInstall(&work,thread);
}
static void runtimeImageRollback(bool incomplete) {
    RuntimeImageFixture fixture;
    assert(guardedFixtureLoad(fixture,true));
    {
        RuntimeImageApplyGuard apply; assert(apply.ready());
        assert(runtimeImagesRestartOnly());
        // Fail after promotion; optionally prevent restoration of one original.
        packageTestFailure(22,incomplete?1:-1);
        std::string error;
        assert(!installPackage(fixture.root,fixture.package,fixture.payloads,&error));
        packageTestFailure(-1,-1);
        if(incomplete) {
            assert(packageRecoveryPending(fixture.root));
            assert(readFile(fixture.root+kComponentNames[1])==fixture.payloads[1]);
            assert(readFile(fixture.root+kComponentNames[1]+".old")==fixture.original[1]);
            // An unresolved transaction must never reopen same-process loads.
        } else {
            fixture.originalsIntact(); assert(!packageRecoveryPending(fixture.root));
            apply.allowLoadsAfterRollback();
        }
    }
    if(incomplete) {
        assertRuntimeLoadsRefused(fixture);
        assert(packageRecoveryPending(fixture.root));
    } else {
        assert(!runtimeImagesRestartOnly());
        assert(guardedFixtureLoad(fixture,false));
        assert(guardedFixtureLoad(fixture,true));
        fixture.originalsIntact();
    }
}
static void runtimeImageFixtures() {
    char exe[MAX_PATH]; assert(GetModuleFileNameA(0,exe,MAX_PATH));
    const char* cases[]={"ui-success","ui-failure","companion-held","rollback-complete","rollback-pending"};
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i)
        child(exe,std::string("--runtime-image-gate ")+cases[i],0);
}
int main(int argc,char** argv) {
    if(argc==3 && strcmp(argv[1],"--runtime-image-gate")==0) {
        if(strcmp(argv[2],"ui-success")==0) runtimeImageUiSettles(true);
        else if(strcmp(argv[2],"ui-failure")==0) runtimeImageUiSettles(false);
        else if(strcmp(argv[2],"companion-held")==0) runtimeImageCompanionSettles();
        else if(strcmp(argv[2],"rollback-complete")==0) runtimeImageRollback(false);
        else if(strcmp(argv[2],"rollback-pending")==0) runtimeImageRollback(true);
        else return 2;
        return 0;
    }
    if(argc==2 && strcmp(argv[1],"--mapped-image")==0) { mappedRecoveryFixture(); puts("updater_tests: real mapped PE production startup recovery/restart barrier PASS"); return 0; }
    if(argc==3 && strcmp(argv[1],"--mapped-crash")==0) {
        const std::string root=std::string(argv[2])+"\\";
        std::string payloads[4],hashes[4],error;
        for(int i=0;i<4;++i) { payloads[i]=i==0?readFile(root+"promoted.fixture"):std::string("MZactual-new-")+kComponentNames[i]; hashes[i]=packageHash(payloads[i].data(),payloads[i].size()); }
        PackageManifest m; assert(parsePackageManifest(manifest(hashes),&m,&error));
        packageTestCrash(26); installPackage(root,m,payloads,&error); return 3;
    }
    if(argc==4 && strcmp(argv[1],"--mapped-recover")==0) {
        const std::string root=std::string(argv[2])+"\\", core=root+kComponentNames[0];
        HMODULE mapped=LoadLibraryExA(core.c_str(),0,LOAD_WITH_ALTERED_SEARCH_PATH);
        assert(mapped && GetProcAddress(mapped,"GetFileVersionInfoSizeA"));
        assert(packageFileHash(core)==argv[3]);
        bool restart=false; std::string error;
        assert(!recoverPackageForStartup(root,argv[3],&restart,&error) && restart);
        assert(packageFileHash(core)!=argv[3]);
        assert(packageFileHash(core+".quarantine")==argv[3] && packageRecoveryPending(root));
        restart=false; assert(!recoverPackageForStartup(root,argv[3],&restart,&error) && restart);
        // Exit while still mapped; the next process must finish cleanup and start.
        return 0;
    }
    if(argc==4 && strcmp(argv[1],"--crash")==0) {
        std::string payloads[4],hashes[4],error;
        for(int i=0;i<4;++i) { payloads[i]=std::string("MZactual-new-")+kComponentNames[i]; hashes[i]=packageHash(payloads[i].data(),payloads[i].size()); }
        PackageManifest m; assert(parsePackageManifest(manifest(hashes),&m,&error));
        packageTestCrash(atoi(argv[3]));
        installPackage(std::string(argv[2])+"\\",m,payloads,&error);
        return 3;
    }
    char temp[MAX_PATH],dir[MAX_PATH]; assert(GetTempPathA(MAX_PATH,temp)); assert(GetTempFileNameA(temp,"kcu",0,dir)); assert(DeleteFileA(dir)); assert(CreateDirectoryA(dir,0));
    std::string root=std::string(dir)+"\\", bytes[4], hashes[4], error;
    for(int i=0;i<4;++i) { bytes[i]=std::string("MZactual-new-")+kComponentNames[i]; hashes[i]=packageHash(bytes[i].data(),bytes[i].size()); }
    PackageManifest m; const std::string good=manifest(hashes);
    assert(parsePackageManifest(good,&m,&error));
    const char* invalid[]={"schema=1\n","transportAbi=2\n","sha256=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\n","url=https://github.com/kotetsyy/KenshiCoop4up/releases/download/v0.2.0/KenshiCoop.dll\n","coreSha256=bad\n","version=0.2.0\n","unknown=value\n"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) assert(!parsePackageManifest(good+invalid[i],&m,&error));
    for(char abi='1';abi<='3';abi+=2) {
        std::string incompatible=good;
        incompatible[incompatible.find("transportAbi=")+13]=abi;
        assert(!parsePackageManifest(incompatible,&m,&error));
    }
    std::string bad=good; bad.replace(bad.find("kotetsyy"),8,"attacker"); assert(!parsePackageManifest(bad,&m,&error));
    bad=good; bad.erase(bad.find("udpSha256=")); assert(!parsePackageManifest(bad,&m,&error));
    bad=good; bad.replace(bad.find("version=0.2.0"),13,"version=0..0"); assert(!parsePackageManifest(bad,&m,&error));
    const char* componentKeys[]={"core","ui","steam","udp"};
    for(int i=0;i<4;++i) {
        std::string key=std::string(componentKeys[i])+"Sha256=";
        bad=good; size_t pos=bad.find(key); bad.erase(pos,bad.find('\n',pos)-pos+1); assert(!parsePackageManifest(bad,&m,&error));
        bad=good; bad[pos+key.size()]='g'; assert(!parsePackageManifest(bad,&m,&error));
        key=std::string(componentKeys[i])+"Url="; bad=good; pos=bad.find(key); bad.erase(pos,bad.find('\n',pos)-pos+1); assert(!parsePackageManifest(bad,&m,&error));
        bad=good; pos=bad.find(key)+key.size(); bad.replace(pos,5,"http:"); assert(!parsePackageManifest(bad,&m,&error));
    }
    assert(!parsePackageManifest("version=0.1.31\nsha256="+hashes[0]+"\nurl=https://github.com/kotetsyy/KenshiCoop4up/releases/download/v0.1.31/KenshiCoop.dll\n",&m,&error));
    // Frozen published parser/gate executes against both old and new schemas.
    assert(legacyUpdaterSnapshot::reachesDownload("version=0.1.31\nsha256="+hashes[0]+"\nurl=old\nuiSha256="+hashes[1]+"\nuiUrl=old-ui\n"));
    assert(!legacyUpdaterSnapshot::reachesDownload(good));
    put(root+"coop_config.json","private-config"); put(root+"user.save","private-save");
    for(int i=0;i<4;++i) put(root+kComponentNames[i],std::string("MZold-")+kComponentNames[i]);
    assert(parsePackageManifest(good,&m,&error));
    std::string corrupted[4]; for(int i=0;i<4;++i) corrupted[i]=bytes[i];
    corrupted[3]+="tampered";
    assert(!installPackage(root,m,corrupted,&error));
    for(int i=0;i<4;++i) { assert(readFile(root+kComponentNames[i])==std::string("MZold-")+kComponentNames[i]); assert(GetFileAttributesA((root+kComponentNames[i]+".new").c_str())==INVALID_FILE_ATTRIBUTES); }
    for(int failure=0;failure<32;++failure) {
        for(int i=0;i<4;++i) put(root+kComponentNames[i],std::string("MZold-")+kComponentNames[i]);
        assert(parsePackageManifest(good,&m,&error));
        packageTestFailure(failure,-1);
        bool ok=installPackage(root,m,bytes,&error);
        packageTestFailure(-1,-1);
        if(!ok) { assert(recoverPackage(root,&error)); for(int i=0;i<4;++i) assert(readFile(root+kComponentNames[i])==std::string("MZold-")+kComponentNames[i]); }
        else {
            for(int i=0;i<4;++i) assert(readFile(root+kComponentNames[i])==bytes[i]);
            assert(!installPackage(root,m,bytes,&error)); // Must not erase previous backups.
            for(int i=0;i<4;++i) assert(readFile(root+kComponentNames[i]+".old")==std::string("MZold-")+kComponentNames[i]);
            assert(recoverPackage(root,&error));
        }
        assert(readFile(root+"coop_config.json")=="private-config" && readFile(root+"user.save")=="private-save");
    }
    // Promotion failure plus rollback failure must retain recovery material,
    // report failure, and recover on the next clean launch.
    for(int rollback=0;rollback<4;++rollback) {
        for(int i=0;i<4;++i) put(root+kComponentNames[i],std::string("MZold-")+kComponentNames[i]);
        packageTestFailure(22,rollback);
        assert(!installPackage(root,m,bytes,&error));
        assert(GetFileAttributesA((root+".kenshicoop-update.txn").c_str())!=INVALID_FILE_ATTRIBUTES);
        packageTestFailure(-1,-1); assert(recoverPackage(root,&error));
        for(int i=0;i<4;++i) assert(readFile(root+kComponentNames[i])==std::string("MZold-")+kComponentNames[i]);
    }
    // Real child process termination during journal/backup/promotion/commit.
    // Recovery runs the production implementation in the surviving parent.
    char exe[MAX_PATH]; assert(GetModuleFileNameA(0,exe,MAX_PATH));
    for(int crash=12;crash<28;++crash) {
        for(int i=0;i<4;++i) put(root+kComponentNames[i],std::string("MZold-")+kComponentNames[i]);
        char command[2048]; _snprintf(command,sizeof(command),"\"%s\" --crash \"%s\" %d",exe,dir,crash);
        STARTUPINFOA si; PROCESS_INFORMATION pi; ZeroMemory(&si,sizeof(si)); ZeroMemory(&pi,sizeof(pi)); si.cb=sizeof(si);
        assert(CreateProcessA(exe,command,0,0,FALSE,CREATE_NO_WINDOW,0,0,&si,&pi));
        assert(WaitForSingleObject(pi.hProcess,30000)==WAIT_OBJECT_0); DWORD code=0; assert(GetExitCodeProcess(pi.hProcess,&code)&&code==77); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        if(crash==12) {
            assert(!recoverPackage(root,&error)); // Orphaned stages are preserved, never guessed away.
            for(int i=0;i<4;++i) { assert(readFile(root+kComponentNames[i])==std::string("MZold-")+kComponentNames[i]); assert(DeleteFileA((root+kComponentNames[i]+".new").c_str())); }
        } else {
            assert(recoverPackage(root,&error));
            for(int i=0;i<4;++i) assert(readFile(root+kComponentNames[i])==(crash==27?bytes[i]:std::string("MZold-")+kComponentNames[i]));
        }
        assert(readFile(root+"coop_config.json")=="private-config" && readFile(root+"user.save")=="private-save");
    }
    for(int i=0;i<4;++i) put(root+kComponentNames[i],std::string("MZold-")+kComponentNames[i]);
    assert(installPackage(root,m,bytes,&error));
    put(root+kComponentNames[3],"corrupted-live-UDP");
    assert(!recoverPackage(root,&error));
    for(int i=0;i<4;++i) assert(readFile(root+kComponentNames[i]+".old")==std::string("MZold-")+kComponentNames[i]);
    put(root+kComponentNames[3],bytes[3]); assert(recoverPackage(root,&error));
    put(root+".kenshicoop-update.txn","corrupt-journal");
    put(root+kComponentNames[0]+".old","preserve-recovery-backup");
    assert(!recoverPackage(root,&error));
    assert(readFile(root+kComponentNames[0]+".old")=="preserve-recovery-backup");
    assert(readFile(root+".kenshicoop-update.txn")=="corrupt-journal");
    assert(DeleteFileA((root+kComponentNames[0]+".old").c_str())); // test-fixture disposal only
    assert(DeleteFileA((root+".kenshicoop-update.txn").c_str()));
    assert(readFile(root+"coop_config.json")=="private-config" && readFile(root+"user.save")=="private-save");
    put(root+kComponentNames[0]+".quarantine","orphan-quarantine-evidence");
    std::string beforeOrphan[4]; for(int i=0;i<4;++i) beforeOrphan[i]=packageFileHash(root+kComponentNames[i]);
    assert(packageRecoveryPending(root) && !recoverPackage(root,&error));
    assert(!installPackage(root,m,bytes,&error));
    assert(readFile(root+kComponentNames[0]+".quarantine")=="orphan-quarantine-evidence");
    for(int i=0;i<4;++i) assert(packageFileHash(root+kComponentNames[i])==beforeOrphan[i]);
    assert(DeleteFileA((root+kComponentNames[0]+".quarantine").c_str()));
    for(int i=0;i<4;++i) assert(DeleteFileA((root+kComponentNames[i]).c_str()));
    assert(DeleteFileA((root+"coop_config.json").c_str())); assert(DeleteFileA((root+"user.save").c_str())); assert(RemoveDirectoryA(dir));
    mappedRecoveryFixture();
    runtimeImageFixtures();
    puts("updater_tests: production schema/transaction/stage/rename/flush/rollback/recovery/runtime-image gate/config-save preservation PASS");
    return 0;
}
