#include "UpdatePackage.h"
#include <windows.h>
#include <wincrypt.h>
#include <map>
#include <stdio.h>
#include <string.h>
#pragma comment(lib,"advapi32.lib")
namespace coop { namespace updater {
const char* const kComponentNames[4]={"KenshiCoop.dll","KenshiCoopUI.dll","KenshiCoopNetSteam.dll","KenshiCoopNetUDP.dll"};
namespace {
#ifdef KENSHICOOP_UPDATER_TEST
int failOperation=-1, failRollback=-1, crashOperation=-1;
bool operation(int n) { if(n==crashOperation) ExitProcess(77); return n!=failOperation; }
bool rollbackOperation(int n) { return n!=failRollback; }
#else
bool operation(int) { return true; }
bool rollbackOperation(int) { return true; }
#endif
std::string trim(const std::string& s) {
    size_t a=0,b=s.size(); while(a<b && (s[a]==' '||s[a]=='\t'||s[a]=='\r')) ++a;
    while(b>a && (s[b-1]==' '||s[b-1]=='\t'||s[b-1]=='\r')) --b;
    return s.substr(a,b-a);
}
bool hex(const std::string& s) { if(s.size()!=64) return false; for(size_t i=0;i<s.size();++i) if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f'))) return false; return true; }
bool version(const std::string& s) {
    int dots=0,digits=0; unsigned value=0;
    for(size_t i=0;i<s.size();++i) { char c=s[i]; if(c=='.') { if(!digits||++dots>2) return false; digits=0; value=0; }
        else if(c>='0'&&c<='9') { if(++digits>6) return false; value=value*10+(c-'0'); if(value>100000) return false; }
        else return false; }
    return dots==2 && digits!=0;
}
bool exists(const std::string& p) {
    if(GetFileAttributesA(p.c_str())!=INVALID_FILE_ATTRIBUTES) return true;
    DWORD error=GetLastError();
    return error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND;
}
bool removeFile(const std::string& p) { if(DeleteFileA(p.c_str())) return true; return GetLastError()==ERROR_FILE_NOT_FOUND; }
bool moveFile(const std::string& a,const std::string& b,bool replace) { return MoveFileExA(a.c_str(),b.c_str(),MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0))!=0; }
bool readFile(const std::string& p,std::string* out,DWORD limit) {
    HANDLE f=CreateFileA(p.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);
    if(f==INVALID_HANDLE_VALUE) return false;
    DWORD hi=0,n=GetFileSize(f,&hi),got=0; bool ok=hi==0&&n!=INVALID_FILE_SIZE&&n>0&&n<=limit;
    if(ok) { out->resize(n); ok=ReadFile(f,&(*out)[0],n,&got,0)!=0&&got==n; }
    if(!CloseHandle(f)) ok=false;
    return ok;
}
bool writeFile(const std::string& p,const std::string& bytes,int writeOp,int flushOp) {
    if(!operation(writeOp)) return false;
    HANDLE f=CreateFileA(p.c_str(),GENERIC_WRITE,0,0,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,0);
    if(f==INVALID_HANDLE_VALUE) return false;
    DWORD wrote=0; bool ok=WriteFile(f,bytes.data(),(DWORD)bytes.size(),&wrote,0)!=0&&wrote==bytes.size();
    if(ok) ok=operation(flushOp)&&FlushFileBuffers(f)!=0;
    if(!CloseHandle(f)) ok=false;
    return ok;
}
std::string journalPath(const std::string& root) { return root+".kenshicoop-update.txn"; }
std::string commitPath(const std::string& root) { return root+".kenshicoop-update.committed"; }
bool decodeJournal(const std::string& text,std::string oldHash[4],std::string newHash[4]) {
    if(text.size()!=7+8*65 || text.substr(0,7)!="KCU2/1\n") return false;
    for(int i=0;i<8;++i) { std::string h=text.substr(7+i*65,64); if(!hex(h)||text[7+i*65+64]!='\n') return false; (i<4?oldHash[i]:newHash[i-4])=h; }
    return true;
}
bool rollback(const std::string& root,const std::string oldHash[4],const std::string newHash[4],std::string* error) {
    bool ok=true;
    for(int i=3;i>=0;--i) {
        const std::string live=root+kComponentNames[i],backup=live+".old",quarantine=live+".quarantine";
        if(exists(quarantine) && packageFileHash(quarantine)!=newHash[i]) { ok=false; continue; }
        if(exists(backup)) {
            if(packageFileHash(backup)!=oldHash[i] || !rollbackOperation(i)) { ok=false; continue; }
            const std::string current=packageFileHash(live);
            if(current!=oldHash[i]) {
                // A mapped Windows image can be renamed, but cannot be replaced.
                // Never overwrite either live or preserved quarantine evidence.
                if(exists(live) && (current!=newHash[i] || exists(quarantine) || !moveFile(live,quarantine,false))) { ok=false; continue; }
                if(!moveFile(backup,live,false)) { ok=false; continue; }
            }
        }
        if(packageFileHash(live)!=oldHash[i]) ok=false;
    }
    if(!ok) { *error="rollback incomplete; preserved transaction/backups/quarantine, manual recovery required"; return false; }
    for(int i=0;i<4;++i) if(!removeFile(root+kComponentNames[i]+".new") || !removeFile(root+kComponentNames[i]+".old")) { *error="rollback restored originals but staging/backup cleanup failed; preserved journal"; return false; }
    bool quarantinesRemoved=true;
    for(int i=0;i<4;++i) if(!removeFile(root+kComponentNames[i]+".quarantine")) quarantinesRemoved=false;
    // Originals are verified and usable on the next launch. Mapped promoted
    // images keep their journal and quarantine until that process has exited.
    if(!quarantinesRemoved) { *error="rollback restored originals; mapped quarantine cleanup deferred until restart"; return true; }
    if(!removeFile(commitPath(root)) || !removeFile(journalPath(root))) { *error="rollback restored originals but journal cleanup failed"; return false; }
    return true;
}
bool finishCommit(const std::string& root,const std::string newHash[4],std::string* error) {
    for(int i=0;i<4;++i) if(packageFileHash(root+kComponentNames[i])!=newHash[i]) { *error="committed image hash mismatch; preserved backups"; return false; }
    for(int i=0;i<4;++i) if(!removeFile(root+kComponentNames[i]+".old")||!removeFile(root+kComponentNames[i]+".new")) { *error="committed backup cleanup deferred (image mapped or read-only)"; return false; }
    // Remove journal first: a marker without a journal only means cleanup finished.
    if(!removeFile(journalPath(root))||!removeFile(commitPath(root))) { *error="committed journal cleanup failed"; return false; }
    return true;
}
}
#ifdef KENSHICOOP_UPDATER_TEST
void packageTestCrash(int operationNumber) { crashOperation=operationNumber; }
void packageTestFailure(int operationNumber,int rollbackComponent) { failOperation=operationNumber; failRollback=rollbackComponent; }
#endif
bool parsePackageManifest(const std::string& text,PackageManifest* out,std::string* error) {
    if(!out||!error) return false;
    *out=PackageManifest(); out->allowDowngrade=false;
    if(text.empty()||text.size()>65536||text.find('\0')!=std::string::npos) { *error="invalid manifest size/content"; return false; }
    std::map<std::string,std::string> values;
    const char* allowed[]={"schema","transportAbi","version","proto","notes","allowDowngrade","coreSha256","coreUrl","uiSha256","uiUrl","steamSha256","steamUrl","udpSha256","udpUrl"};
    for(size_t pos=0;pos<text.size();) {
        size_t end=text.find('\n',pos); std::string line=trim(text.substr(pos,end==std::string::npos?end:end-pos)); pos=end==std::string::npos?text.size():end+1;
        if(line.empty()||line[0]=='#') continue;
        size_t eq=line.find('='); if(eq==std::string::npos) { *error="malformed manifest line"; return false; }
        std::string key=trim(line.substr(0,eq)),value=trim(line.substr(eq+1)); bool known=false;
        for(size_t i=0;i<sizeof(allowed)/sizeof(allowed[0]);++i) if(key==allowed[i]) known=true;
        if(!known||values.find(key)!=values.end()) { *error="unknown or duplicate manifest field"; return false; }
        values[key]=value;
    }
    if(values["schema"]!="2"||values["transportAbi"]!="2"||!version(values["version"])) { *error="requires schema=2, transportAbi=2 and exact MAJOR.MINOR.PATCH"; return false; }
    out->version=values["version"]; out->proto=values["proto"]; out->notes=values["notes"];
    if(!out->proto.empty()) for(size_t i=0;i<out->proto.size();++i) if(out->proto[i]<'0'||out->proto[i]>'9') { *error="invalid protocol"; return false; }
    if(values.count("allowDowngrade")) { if(values["allowDowngrade"]!="0"&&values["allowDowngrade"]!="1") { *error="invalid allowDowngrade"; return false; } out->allowDowngrade=values["allowDowngrade"]=="1"; }
    const char* keys[]={"core","ui","steam","udp"};
    for(int i=0;i<4;++i) {
        out->hash[i]=values[std::string(keys[i])+"Sha256"]; out->url[i]=values[std::string(keys[i])+"Url"];
        std::string expected="https://github.com/kotetsyy/KenshiCoop4up/releases/download/v"+out->version+"/"+kComponentNames[i];
        if(!hex(out->hash[i])||out->url[i]!=expected) { *error="invalid component hash or fixed release/asset URL"; return false; }
    }
    return true;
}
std::string packageHash(const void* data,size_t n) {
    if(n>32*1024*1024) return std::string();
    HCRYPTPROV p=0; HCRYPTHASH h=0; std::string result;
    if(!CryptAcquireContextA(&p,0,0,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)) return result;
    if(CryptCreateHash(p,CALG_SHA_256,0,0,&h)) { if(CryptHashData(h,(const BYTE*)data,(DWORD)n,0)) { BYTE digest[32]; DWORD len=32;
        if(CryptGetHashParam(h,HP_HASHVAL,digest,&len,0)&&len==32) { char b[65]; for(int i=0;i<32;++i) _snprintf(b+i*2,3,"%02x",digest[i]); b[64]=0; result=b; } } CryptDestroyHash(h); }
    CryptReleaseContext(p,0); return result;
}
std::string packageFileHash(const std::string& path) {
    HANDLE f=CreateFileA(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);
    if(f==INVALID_HANDLE_VALUE) return std::string();
    DWORD high=0,size=GetFileSize(f,&high);
    HCRYPTPROV provider=0; HCRYPTHASH hash=0; std::string result;
    bool ok=high==0&&size!=INVALID_FILE_SIZE&&size>0&&size<=32*1024*1024;
    if(ok) ok=CryptAcquireContextA(&provider,0,0,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)!=0;
    if(ok) ok=CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)!=0;
    BYTE chunk[32768];
    for(DWORD left=size;ok&&left;) {
        DWORD wanted=left<sizeof(chunk)?left:(DWORD)sizeof(chunk),got=0;
        ok=ReadFile(f,chunk,wanted,&got,0)!=0&&got==wanted&&CryptHashData(hash,chunk,got,0)!=0;
        left-=got;
    }
    if(ok) { BYTE digest[32]; DWORD length=32; if(CryptGetHashParam(hash,HP_HASHVAL,digest,&length,0)&&length==32) {
        char b[65]; for(int i=0;i<32;++i) _snprintf(b+i*2,3,"%02x",digest[i]); b[64]=0; result=b;
    } }
    if(hash) CryptDestroyHash(hash); if(provider) CryptReleaseContext(provider,0);
    if(!CloseHandle(f)) result.clear();
    return result;
}
bool packageRecoveryPending(const std::string& root) {
    if(exists(journalPath(root))||exists(commitPath(root))) return true;
    for(int i=0;i<4;++i) if(exists(root+kComponentNames[i]+".old")||exists(root+kComponentNames[i]+".new")||exists(root+kComponentNames[i]+".quarantine")) return true;
    return false;
}
bool recoverPackage(const std::string& root,std::string* error) {
    if(!exists(journalPath(root))) {
        for(int i=0;i<4;++i) if(exists(root+kComponentNames[i]+".old")||exists(root+kComponentNames[i]+".new")||exists(root+kComponentNames[i]+".quarantine")) { *error="orphaned update files; preserved for inspection"; return false; }
        if(!removeFile(commitPath(root))) { *error="cannot remove completed transaction marker"; return false; }
        return true;
    }
    std::string journal,oldHash[4],newHash[4];
    if(!readFile(journalPath(root),&journal,4096)||!decodeJournal(journal,oldHash,newHash)) { *error="invalid recovery journal; preserved backups"; return false; }
    std::string marker;
    if(readFile(commitPath(root),&marker,4096)&&marker=="committed\n"+packageHash(journal.data(),journal.size())+"\n") return finishCommit(root,newHash,error);
    return rollback(root,oldHash,newHash,error);
}
bool recoverPackageForStartup(const std::string& root,const std::string& loadedCoreHash,bool* restart,std::string* error) {
    if(!restart || !error) return false;
    *restart=false;
    if(loadedCoreHash.empty()) { *error="cannot hash loaded core"; return false; }
    if(!recoverPackage(root,error)) return false;
    if(loadedCoreHash!=packageFileHash(root+kComponentNames[0])) {
        *restart=true; *error="interrupted update rolled back - RESTART before loading companions"; return false;
    }
    return true;
}
bool installPackage(const std::string& root,const PackageManifest& manifest,const std::string payloads[4],std::string* error) {
    if(packageRecoveryPending(root)) { *error="pending recovery; restart before another update"; return false; }
    std::string oldHash[4],journal="KCU2/1\n";
    for(int i=0;i<4;++i) {
        std::string live=root+kComponentNames[i]; oldHash[i]=packageFileHash(live);
        if(!hex(oldHash[i])) { *error="full four-image install required"; return false; }
        if(payloads[i].size()<2||payloads[i][0]!='M'||payloads[i][1]!='Z'||packageHash(payloads[i].data(),payloads[i].size())!=manifest.hash[i]) { *error="payload verification failed before mutation"; return false; }
        journal+=oldHash[i]+"\n";
    }
    for(int i=0;i<4;++i) journal+=manifest.hash[i]+"\n";
    bool staged=true;
    for(int i=0;i<4&&staged;++i) { std::string path=root+kComponentNames[i]+".new"; staged=writeFile(path,payloads[i],i,4+i)&&operation(8+i)&&packageFileHash(path)==manifest.hash[i]; }
    if(!staged) { bool clean=true; for(int i=0;i<4;++i) if(!removeFile(root+kComponentNames[i]+".new")) clean=false; *error=clean?"stage/write/flush/hash failed; originals untouched":"stage failed; originals untouched, staging cleanup incomplete"; return false; }
    if(!writeFile(journalPath(root),journal,12,13)) { bool clean=removeFile(journalPath(root)); for(int i=0;i<4;++i) if(!removeFile(root+kComponentNames[i]+".new")) clean=false; *error=clean?"journal write/flush failed; originals untouched":"journal failed; originals untouched, cleanup incomplete"; return false; }
    bool ok=true;
    for(int i=0;i<4&&ok;++i) ok=operation(14+i)&&moveFile(root+kComponentNames[i],root+kComponentNames[i]+".old",false);
    for(int i=0;i<4&&ok;++i) ok=operation(18+i)&&moveFile(root+kComponentNames[i]+".new",root+kComponentNames[i],false);
    for(int i=0;i<4&&ok;++i) ok=operation(22+i)&&packageFileHash(root+kComponentNames[i])==manifest.hash[i];
    if(ok) ok=writeFile(commitPath(root),"committed\n"+packageHash(journal.data(),journal.size())+"\n",26,27);
    if(!ok) { std::string recoveryError; if(!rollback(root,oldHash,manifest.hash,&recoveryError)) *error=recoveryError; else *error="promotion failed; all originals restored"; return false; }
    // Backups remain until the next launch, when the mapped images are released.
    return true;
}
} }
