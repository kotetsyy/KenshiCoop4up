#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#include <sddl.h>
#include "Credentials.h"
#include <steam/isteamnetworkingsockets.h>
#include "steamnetworkingsockets_certstore.h"
#include "clientlib/steamnetworkingsockets_lowlevel.h"
#include "crypto.h"
#include "crypto_25519.h"
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <vector>
#include <limits>
#include <charconv>

namespace coopnet {
using namespace SteamNetworkingSocketsLib;
namespace {
constexpr size_t kMaxBlob = 2048;
constexpr size_t kMaxProfile = 8192;
constexpr uint32_t kLifetime = 730u * 86400u;
void wipe(std::string& s) { if (!s.empty()) SecureZeroMemory(&s[0], s.size()); s.clear(); }
struct Secret : std::string { ~Secret() { wipe(*this); } };
struct WipeCert {
    CMsgSteamDatagramCertificateSigned& cert;
    ~WipeCert() { if (cert.has_private_key_data()) wipe(*cert.mutable_private_key_data()); }
};
struct Handle { HANDLE h = INVALID_HANDLE_VALUE; ~Handle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
struct LocalBuffer { void* p = nullptr; ~LocalBuffer() { if (p) LocalFree(p); } };
struct ProtectedBuffer { DATA_BLOB b{}; ~ProtectedBuffer() { if (b.pbData) { SecureZeroMemory(b.pbData, b.cbData); LocalFree(b.pbData); } } };
void detail(char* out, uint32_t cap, const char* text) { if (out && cap) { const size_t n = (std::min)(size_t(cap-1), strlen(text)); memcpy(out, text, n); out[n] = 0; } }
bool bounded(const char* s, size_t max, size_t& n) { if (!s) return false; n = strnlen_s(s, max+1); return n > 0 && n <= max; }
bool pathName(const char* path, std::wstring& out) {
    size_t n; if (!bounded(path, 32700, n)) return false;
    int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, int(n), nullptr, 0);
    if (!chars) return false;
    out.resize(chars); return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, int(n), &out[0], chars) == chars;
}
std::string idText(uint64_t id) {
    // Base36 retains all 64 fingerprint bits in at most 13 characters.
    char text[13];
    const auto converted = std::to_chars(text, text + sizeof(text), id, 36);
    return std::string(text, converted.ptr);
}
static_assert(3 + 13 + 1 + 13 < SteamNetworkingIdentity::k_cchMaxGenericString,
              "Signed role identities must fit native generic-string storage");
bool base64(const std::string& bytes, std::string& out, const char* prefix = "") {
    const size_t skip = strlen(prefix);
    out.resize(skip + CCrypto::Base64EncodeMaxOutput(bytes.size(), "") + 1);
    memcpy(&out[0], prefix, skip); uint32_t n = uint32_t(out.size()-skip);
    if (!CCrypto::Base64Encode(bytes.data(), bytes.size(), &out[skip], &n, "")) return false;
    out.resize(skip + strlen(out.c_str()+skip)); return true;
}
bool unbase64(const char* text, size_t n, std::string& out) {
    if (!n || n > COOP_NET_INVITE_CAPACITY-6) return false;
    out.resize(CCrypto::Base64DecodeMaxOutput(n)); uint32_t count = uint32_t(out.size());
    if (!CCrypto::Base64Decode(text, n, &out[0], &count, false)) return false;
    out.resize(count); Secret canonical;
    return count <= kMaxBlob && base64(out,canonical) && canonical.size()==n && memcmp(canonical.data(),text,n)==0;
}
void append32(std::string& out, uint32_t x) { for (int i=0; i<4; ++i) out.push_back(char(x >> (i*8))); }
void pack(const std::string& root, const std::string& leaf, std::string& out) {
    out.reserve(12+root.size()+leaf.size()); out.assign("KCU1",4);
    append32(out,uint32_t(root.size())); append32(out,uint32_t(leaf.size())); out+=root; out+=leaf;
}
bool unpack(const std::string& data, UdpCredential& out) {
    if (data.size() < 12 || data.size() > kMaxBlob || memcmp(data.data(), "KCU1",4)) return false;
    uint32_t a=0,b=0; for(int i=0;i<4;++i) { a |= uint32_t(uint8_t(data[4+i])) << (8*i); b |= uint32_t(uint8_t(data[8+i])) << (8*i); }
    if (!a || !b || a>1024 || b>1024 || size_t(a)+b+12 != data.size()) return false;
    out.root.assign(data.data()+12,a); out.leaf.assign(data.data()+12+a,b); return true;
}
bool parse(const std::string& bytes, CMsgSteamDatagramCertificateSigned& signedCert, CMsgSteamDatagramCertificate& cert) {
    if (bytes.empty() || bytes.size()>1024 || !signedCert.ParseFromString(bytes) || signedCert.unknown_fields().field_count() ||
        signedCert.cert().empty() || signedCert.cert().size()>768 || !cert.ParseFromString(signedCert.cert()) || cert.unknown_fields().field_count()) return false;
    const auto now = std::time(nullptr);
    return cert.key_type() == CMsgSteamDatagramCertificate_EKeyType_ED25519 && cert.key_data().size()==32 &&
        signedCert.ca_key_id()!=0 && signedCert.ca_signature().size()==sizeof(CryptoSignature_t) &&
        (!signedCert.has_private_key_data() || signedCert.private_key_data().size()==32) &&
        cert.has_time_created() && cert.has_time_expiry() && cert.time_created()<=now+30 && cert.time_expiry()>now &&
        cert.time_expiry()>cert.time_created() && cert.time_expiry()-cert.time_created()<=kLifetime &&
        !cert.has_legacy_identity_binary() && !cert.has_legacy_steam_id() &&
        cert.gameserver_datacenter_ids_size()==0 && cert.ip_addresses_size()==0;
}
bool verify(const CMsgSteamDatagramCertificateSigned& cert, const CECSigningPublicKey& signer) {
    CryptoSignature_t signature; memcpy(signature, cert.ca_signature().data(), sizeof(signature));
    return cert.ca_key_id()==CalculatePublicKeyID(signer) && signer.VerifySignature(cert.cert().data(),cert.cert().size(),signature);
}
bool privateMatches(const CMsgSteamDatagramCertificateSigned& cert, const CMsgSteamDatagramCertificate& payload) {
    CECSigningPrivateKey priv; CECSigningPublicKey pub;
    return cert.has_private_key_data() && priv.SetRawDataFromStdString(cert.private_key_data()) &&
        pub.SetRawDataFromStdString(payload.key_data()) && priv.MatchesPublicKey(pub);
}
bool validate(UdpCredential& out, bool host) {
    CMsgSteamDatagramCertificateSigned root,leaf; CMsgSteamDatagramCertificate rootCert,leafCert;
    WipeCert rootWipe{root},leafWipe{leaf};
    if (!parse(out.root,root,rootCert) || !parse(out.leaf,leaf,leafCert) || rootCert.has_identity_string() ||
        !leafCert.has_identity_string() || rootCert.time_expiry()<leafCert.time_expiry() ||
        rootCert.app_ids_size()!=0 || leafCert.app_ids_size()!=1 || leafCert.app_ids(0)!=0) return false;
    CECSigningPublicKey rootKey,leafKey;
    if (!rootKey.SetRawDataFromStdString(rootCert.key_data()) || !leafKey.SetRawDataFromStdString(leafCert.key_data()) ||
        !verify(root,rootKey) || !verify(leaf,rootKey) || !privateMatches(leaf,leafCert) ||
        (host ? !privateMatches(root,rootCert) : root.has_private_key_data())) return false;
    auto id = idText(CalculatePublicKeyID(rootKey));
    out.hostIdentity = "kh-" + id; out.clientPrefix = "kc-" + id + "-";
    out.localIdentity = host ? out.hostIdentity : out.clientPrefix + idText(CalculatePublicKeyID(leafKey));
    return leafCert.identity_string() == "str:" + out.localIdentity;
}
bool readProfile(const char* path, UdpCredential& out) {
    std::wstring name; if (!pathName(path,name)) return false;
    Handle f; f.h=CreateFileW(name.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (f.h==INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(f.h,&info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) ||
        !GetFileSizeEx(f.h,&size) || size.QuadPart<1 || size.QuadPart>kMaxProfile) return false;
    std::vector<uint8_t> bytes(size_t(size.QuadPart)); DWORD read=0;
    if (!ReadFile(f.h,bytes.data(),DWORD(bytes.size()),&read,nullptr) || read!=bytes.size()) return false;
    DATA_BLOB encrypted{DWORD(bytes.size()),bytes.data()}; ProtectedBuffer plain;
    if (!CryptUnprotectData(&encrypted,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain.b) || plain.b.cbData>kMaxBlob) return false;
    Secret blob; blob.assign(reinterpret_cast<char*>(plain.b.pbData),plain.b.cbData);
    return unpack(blob,out) && validate(out,true);
}
bool signCertificate(CECSigningPrivateKey& signer, CECSigningPublicKey& pub, CECSigningPrivateKey& priv,
                     const std::string& identity, uint32_t expiry, std::string& out) {
    CECSigningPublicKey signerPub; if (!signer.GetPublicKey(&signerPub)) return false;
    CMsgSteamDatagramCertificate cert; cert.set_key_type(CMsgSteamDatagramCertificate_EKeyType_ED25519);
    if (!pub.GetRawDataAsStdString(cert.mutable_key_data())) return false;
    cert.set_time_created(uint32_t(std::time(nullptr))); cert.set_time_expiry(expiry);
    if (!identity.empty()) {
        cert.set_identity_string("str:"+identity);
        // Standalone GNS's native app scope is zero, not Kenshi's Steam AppID.
        // Identity-bearing certificates require a signed explicit app scope.
        cert.add_app_ids(0);
    }
    CMsgSteamDatagramCertificateSigned signedCert; WipeCert signedWipe{signedCert}; signedCert.set_cert(cert.SerializeAsString());
    signedCert.set_ca_key_id(CalculatePublicKeyID(signerPub)); CryptoSignature_t sig;
    signer.GenerateSignature(signedCert.cert().data(),signedCert.cert().size(),&sig); signedCert.set_ca_signature(sig,sizeof(sig));
    if (!priv.GetRawDataAsStdString(signedCert.mutable_private_key_data())) return false;
    return signedCert.SerializeToString(&out);
}
bool saveNew(const char* path, const UdpCredential& credential) {
    std::wstring name; if (!pathName(path,name)) return false;
    Secret plain; pack(credential.root,credential.leaf,plain);
    DATA_BLOB input{DWORD(plain.size()),reinterpret_cast<BYTE*>(&plain[0])}; ProtectedBuffer encrypted;
    if (!CryptProtectData(&input,L"KenshiCoop private UDP host",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&encrypted.b)) return false;
    Handle token; token.h=INVALID_HANDLE_VALUE; HANDLE rawToken=nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&rawToken)) return false; token.h=rawToken;
    DWORD needed=0; GetTokenInformation(token.h,TokenUser,nullptr,0,&needed); std::vector<BYTE> tokenBytes(needed);
    if (!needed || !GetTokenInformation(token.h,TokenUser,tokenBytes.data(),needed,&needed)) return false;
    LocalBuffer sid; if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(tokenBytes.data())->User.Sid,reinterpret_cast<LPWSTR*>(&sid.p))) return false;
    std::wstring dacl=L"D:P(A;;FA;;;SY)(A;;FA;;;"; dacl+=static_cast<wchar_t*>(sid.p); dacl+=L")";
    LocalBuffer descriptor; if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(dacl.c_str(),SDDL_REVISION_1,&descriptor.p,nullptr)) return false;
    SECURITY_ATTRIBUTES sa{sizeof(sa),descriptor.p,FALSE};
    std::wstring temporary=name+L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64())+L".private";
    bool ok=false;
    { Handle f; f.h=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,&sa,CREATE_NEW,FILE_ATTRIBUTE_HIDDEN|FILE_FLAG_WRITE_THROUGH,nullptr);
      if (f.h==INVALID_HANDLE_VALUE) return false;
      DWORD written=0; ok=WriteFile(f.h,encrypted.b.pbData,encrypted.b.cbData,&written,nullptr) && written==encrypted.b.cbData && FlushFileBuffers(f.h); }
    if (ok) ok=MoveFileExW(temporary.c_str(),name.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
    if (!ok) DeleteFileW(temporary.c_str()); return ok;
}
}
UdpCredential::~UdpCredential() { wipe(root); wipe(leaf); }
bool loadUdpCredential(const CoopNetConfig& config, UdpCredential& out) {
    if (config.host) return readProfile(config.profilePath,out);
    size_t n; if (!bounded(config.joinInvite,COOP_NET_INVITE_CAPACITY-1,n) || n<6 || memcmp(config.joinInvite,"KCU1:",5)) return false;
    Secret blob; return unbase64(config.joinInvite+5,n-5,blob) && unpack(blob,out) && validate(out,false);
}
bool installUdpRoot(const UdpCredential& credential) {
    CMsgSteamDatagramCertificateSigned root; WipeCert rootWipe{root}; if (!root.ParseFromString(credential.root)) return false;
    if (root.has_private_key_data()) wipe(*root.mutable_private_key_data());
    root.clear_private_key_data(); std::string encoded;
    if (!base64(root.SerializeAsString(),encoded)) return false;
    SteamNetworkingErrMsg error; SteamNetworkingGlobalLock lock("Coop UDP root"); CertStore_Reset();
    SteamNetworkingSockets_CertStore_SetExpiryGraceSeconds(0);
    if (!CertStore_AddCertFromBase64(encoded.c_str(),error)) return false;
    CMsgSteamDatagramCertificate payload; return CertStore_CheckCert(root,payload,std::time(nullptr),error)!=nullptr;
}
bool installUdpCertificate(ISteamNetworkingSockets* sockets, const UdpCredential& credential) {
    CMsgSteamDatagramCertificateSigned leaf; WipeCert leafWipe{leaf};
    CMsgSteamDatagramCertificate payload; SteamNetworkingErrMsg error;
    if (!sockets || !parse(credential.leaf,leaf,payload)) return false;
    {
        SteamNetworkingGlobalLock lock("Coop UDP leaf trust");
        if (!CertStore_CheckCert(leaf,payload,std::time(nullptr),error)) return false;
    }
    // The pinned runtime retains SignedCert verbatim and copies it into UDP
    // handshakes. First load the native private key; then replace its wire
    // certificate with the same signed public certificate before any sockets.
    if (!sockets->SetCertificate(credential.leaf.data(),int(credential.leaf.size()),error)) return false;
    wipe(*leaf.mutable_private_key_data()); leaf.clear_private_key_data();
    std::string publicCertificate;
    return leaf.SerializeToString(&publicCertificate) &&
        sockets->SetCertificate(publicCertificate.data(),int(publicCertificate.size()),error);
}
void clearUdpRoots() { SteamNetworkingGlobalLock lock("Coop UDP root reset"); CertStore_Reset(); }
int32_t provisionUdpHost(const char* path, char* error, uint32_t cap) {
    detail(error,cap,""); std::wstring name; if (!pathName(path,name)) { detail(error,cap,"Invalid private host profile path"); return COOP_NET_INVALID; }
    DWORD attributes=GetFileAttributesW(name.c_str());
    if (attributes!=INVALID_FILE_ATTRIBUTES) { UdpCredential old; if (readProfile(path,old)) return COOP_NET_OK;
        detail(error,cap,"Existing private host profile is invalid; it was not modified"); return COOP_NET_AUTH_FAILED; }
    if (GetLastError()!=ERROR_FILE_NOT_FOUND) { detail(error,cap,"Private host profile path is unavailable"); return COOP_NET_UNAVAILABLE; }
    CECSigningPrivateKey rootPriv,hostPriv; CECSigningPublicKey rootPub,hostPub;
    CCrypto::GenerateSigningKeyPair(&rootPub,&rootPriv); CCrypto::GenerateSigningKeyPair(&hostPub,&hostPriv);
    if (!rootPriv.IsValid() || !hostPriv.IsValid()) { detail(error,cap,"Native credential generation failed"); return COOP_NET_INTERNAL; }
    UdpCredential credential; const auto now=std::time(nullptr); if (now<0 || uint64_t(now)+kLifetime>UINT32_MAX) return COOP_NET_INTERNAL;
    const auto expiry=uint32_t(now)+kLifetime; const auto identity="kh-"+idText(CalculatePublicKeyID(rootPub));
    if (!signCertificate(rootPriv,rootPub,rootPriv,"",expiry,credential.root) ||
        !signCertificate(rootPriv,hostPub,hostPriv,identity,expiry,credential.leaf) || !validate(credential,true) || !saveNew(path,credential)) {
        detail(error,cap,"Cannot atomically provision private host profile; existing files were not replaced"); return COOP_NET_UNAVAILABLE; }
    return COOP_NET_OK;
}
int32_t issueUdpInvite(const char* path, char* invite, uint32_t inviteCap, char* error, uint32_t errorCap) {
    detail(error,errorCap,""); if (invite && inviteCap) invite[0]=0;
    if (!invite || !inviteCap) { detail(error,errorCap,"Invalid invitation output buffer"); return COOP_NET_INVALID; }
    UdpCredential profile; if (!readProfile(path,profile)) { detail(error,errorCap,"Invalid private host profile"); return COOP_NET_AUTH_FAILED; }
    CMsgSteamDatagramCertificateSigned root; CMsgSteamDatagramCertificate payload;
    WipeCert rootWipe{root};
    if (!parse(profile.root,root,payload)) return COOP_NET_AUTH_FAILED;
    CECSigningPrivateKey signer,priv; CECSigningPublicKey pub;
    if (!signer.SetRawDataFromStdString(root.private_key_data())) return COOP_NET_AUTH_FAILED;
    SecureZeroMemory(&(*root.mutable_private_key_data())[0],root.private_key_data().size()); root.clear_private_key_data();
    CCrypto::GenerateSigningKeyPair(&pub,&priv); if (!priv.IsValid()) return COOP_NET_INTERNAL;
    UdpCredential client; client.root=root.SerializeAsString();
    const auto expiry=(std::min)(payload.time_expiry(),uint32_t(std::time(nullptr))+7u*86400u);
    if (!signCertificate(signer,pub,priv,profile.clientPrefix+idText(CalculatePublicKeyID(pub)),expiry,client.leaf) || !validate(client,false)) return COOP_NET_INTERNAL;
    Secret binary; pack(client.root,client.leaf,binary); Secret encoded;
    if (!base64(binary,encoded,"KCU1:")) return COOP_NET_INTERNAL;
    if (encoded.size()+1>inviteCap || encoded.size()+1>COOP_NET_INVITE_CAPACITY) { detail(error,errorCap,"Invitation output buffer is too small"); return COOP_NET_INVALID; }
    memcpy(invite,encoded.c_str(),encoded.size()+1); return COOP_NET_OK;
}
}
