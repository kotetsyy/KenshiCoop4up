// Private harness provisioning utility, never included in a player runtime kit.
#include "../TransportApi.h"
#include <windows.h>
#include <sddl.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <string>

static std::string utf8(const wchar_t* value) {
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, &result[0], size, nullptr, nullptr)) return {};
    result.pop_back();
    return result;
}

static bool writePrivate(const wchar_t* path, const char* invitation, DWORD length) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    auto user = static_cast<TOKEN_USER*>(LocalAlloc(LPTR, bytes));
    LPWSTR sid = nullptr;
    bool ok = user && GetTokenInformation(token, TokenUser, user, bytes, &bytes) &&
              ConvertSidToStringSidW(user->User.Sid, &sid);
    CloseHandle(token);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ok) {
        std::wstring sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;";
        sddl += sid;
        sddl += L")";
        ok = ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) != FALSE;
    }
    if (sid) LocalFree(sid);
    if (user) LocalFree(user);
    if (!ok) return false;
    SECURITY_ATTRIBUTES security = {sizeof(security), descriptor, FALSE};
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(descriptor);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    ok = WriteFile(file, invitation, length, &written, nullptr) && written == length && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) DeleteFileW(path);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* dll = nullptr;
    const wchar_t* profile = nullptr;
    const wchar_t* output = nullptr;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) return 2;
        if (!wcscmp(argv[i], L"--udp") && !dll) dll = argv[i + 1];
        else if (!wcscmp(argv[i], L"--profile") && !profile) profile = argv[i + 1];
        else if (!wcscmp(argv[i], L"--invite-file") && !output) output = argv[i + 1];
        else return 2;
    }
    if (!dll || !profile) {
        fputs("Usage: transport_credentials --udp <absolute UDP DLL> --profile <private host file> [--invite-file <new private client file>]\n", stderr);
        return 2;
    }
    const auto absolute = [](const wchar_t* p) {
        const size_t n = p ? wcslen(p) : 0;
        return n >= 3 && ((p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) ||
                          (p[0] == L'\\' && p[1] == L'\\' && p[2] != L'\\'));
    };
    if (!absolute(dll) || !absolute(profile) || (output && !absolute(output))) return 2;
    HMODULE module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) { fputs("Cannot load selected UDP provider.\n", stderr); return 1; }
    auto getApi = reinterpret_cast<CoopNetGetApiFn>(GetProcAddress(module, "KenshiCoopNet_GetApi"));
    CoopNetApi api = {};
    bool ok = getApi && getApi(COOP_NET_ABI_VERSION, sizeof(api), &api) == COOP_NET_OK &&
              api.version == COOP_NET_ABI_VERSION && api.size == sizeof(api) && api.prepareHost && api.makeInvite;
    char error[COOP_NET_ERROR_CAPACITY] = {};
    char invitation[COOP_NET_INVITE_CAPACITY] = {};
    auto path = utf8(profile);
    if (ok) ok = !path.empty() && api.prepareHost(path.c_str(), error, sizeof(error)) == COOP_NET_OK;
    if (ok && output) {
        ok = api.makeInvite(path.c_str(), invitation, sizeof(invitation), error, sizeof(error)) == COOP_NET_OK;
        if (ok) {
            size_t length = strnlen(invitation, sizeof(invitation));
            ok = length > 5 && length < sizeof(invitation) &&
                 writePrivate(output, invitation, static_cast<DWORD>(length));
        }
    }
    SecureZeroMemory(invitation, sizeof(invitation));
    SecureZeroMemory(error, sizeof(error));
    FreeLibrary(module);
    if (!ok) { fputs("Native credential provisioning failed; no credential data is printed.\n", stderr); return 1; }
    puts(output ? "Private per-client invitation file created." : "Private host profile prepared.");
    return 0;
}
