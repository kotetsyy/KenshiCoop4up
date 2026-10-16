#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "NativeBackend.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cerrno>
#include <array>

#include <new>
extern "C" int32_t COOP_NET_CALL KenshiCoopNet_GetApi(uint32_t, uint32_t, CoopNetApi*);
namespace coopnet {
int32_t inspectSteamBackend(NativeBackend&, char*, uint32_t);
}
namespace {
volatile LONG started = 0;
volatile LONG workerFinished = 0;
volatile LONG timerPumpCount = 0;
volatile LONG timerPumpResult = COOP_NET_UNAVAILABLE;
CoopNetApi mainThreadApi{};
ULONGLONG pumpDeadline = 0;
void CALLBACK pumpTimer(HWND, UINT, UINT_PTR timer, DWORD) {
    if (InterlockedCompareExchange(&workerFinished, 0, 0) || GetTickCount64() >= pumpDeadline) {
        KillTimer(nullptr, timer);
        return;
    }
    // TIMERPROC runs only on the game thread which installed this timer.
    InterlockedExchange(&timerPumpResult, mainThreadApi.pumpMainThread());
    InterlockedIncrement(&timerPumpCount);
}
struct WorkerArgs { HMODULE module; uint64_t counterpart; bool host; bool loopback; };
void logLine(FILE* log, const char* format, ...) {
    if (!log) return;
    std::fprintf(log, "[%llu] ", static_cast<unsigned long long>(GetTickCount64()));
    va_list args;
    va_start(args, format);
    std::vfprintf(log, format, args);
    va_end(args);
    std::fputc('\n', log);
    std::fflush(log);
}
void inspect(FILE* log, coopnet::NativeBackend& backend) {
    char error[COOP_NET_ERROR_CAPACITY] = {};
    // Behavioral negative-mode checks require no Steam initialization.
    coopnet::NativeBackend rejected;
    CoopNetConfig wrongMode{};
    wrongMode.size = sizeof(wrongMode);
    wrongMode.mode = COOP_NET_UDP;
    const bool negativeChecks =
        coopnet::initializeBackend(rejected, wrongMode, error, sizeof(error)) == COOP_NET_INVALID &&
        !rejected.initialized && !rejected.sockets && !rejected.utils;
    logLine(log, "negative transport mode check=%s", negativeChecks ? "PASS" : "FAIL");
    if (!negativeChecks) return;
    const int result = coopnet::inspectSteamBackend(backend, error, sizeof(error));
    logLine(log, "inspect result=%d detail=%s", result, error);
    if (result != COOP_NET_OK) return;
    SteamNetAuthenticationStatus_t auth{};
    SteamRelayNetworkStatus_t relay{};
    backend.sockets->GetAuthenticationStatus(&auth);
    backend.utils->GetRelayNetworkStatus(&relay);
    logLine(log, "interfaces=SteamNetworkingSockets012/SteamNetworkingUtils004 legacy=SteamUser019/SteamUtils007 AppID=233860 loggedOn=1 self=%llu auth=%d detail=%s relay=%d detail=%s",
        static_cast<unsigned long long>(backend.selfSteamId), static_cast<int>(auth.m_eAvail),
        auth.m_debugMsg, static_cast<int>(relay.m_eAvail), relay.m_debugMsg);
}
// Distinct fixed payloads prove each traffic lane without publishing any credential.
const char reliablePayload[] = "KC-GNS-PROBE reliable generation-scoped echo";
const char statePayload[] = "KC-GNS-PROBE state generation-scoped echo";

bool runLoopback(FILE* log, coopnet::NativeBackend& backend) {
    auto* sockets = backend.sockets;
    struct Pair {
        ISteamNetworkingSockets* sockets;
        HSteamNetConnection handles[2]{};
        ~Pair() { for (auto handle : handles) if (handle) sockets->CloseConnection(handle, 1000, "probe complete", false); }
    } pair{sockets};
    if (!sockets->CreateSocketPair(&pair.handles[0], &pair.handles[1], true, nullptr, nullptr)) {
        logLine(log, "LOOPBACK FAIL native network socket pair creation"); return false;
    }
    const int priorities[3] = {0, 0, 1};
    const uint16_t weights[3] = {4, 1, 1};
    for (auto handle : pair.handles) if (sockets->ConfigureConnectionLanes(handle, 3, priorities, weights) != k_EResultOK) {
        logLine(log, "LOOPBACK FAIL native three-lane setup"); return false;
    }
    struct LagRestore {
        ISteamNetworkingUtils* utils;
        int value = 0;
        bool changed = false;
        ~LagRestore() { if (changed) utils->SetConfigValue(k_ESteamNetworkingConfig_FakePacketLag_Send,
            k_ESteamNetworkingConfig_Global, 0, k_ESteamNetworkingConfig_Int32, &value); }
    } lag{backend.utils};
    size_t lagSize = sizeof(lag.value);
    ESteamNetworkingConfigDataType lagType{};
    const auto lagResult = backend.utils->GetConfigValue(k_ESteamNetworkingConfig_FakePacketLag_Send,
        k_ESteamNetworkingConfig_Global, 0, &lagType, &lag.value, &lagSize);
    if ((lagResult != k_ESteamNetworkingGetConfigValue_OK && lagResult != k_ESteamNetworkingGetConfigValue_OKInherited) ||
        lagType != k_ESteamNetworkingConfig_Int32) {
        logLine(log, "LOOPBACK FAIL native lag configuration read"); return false;
    }
    const int delayMs = 150;
    lag.changed = backend.utils->SetConfigValue(k_ESteamNetworkingConfig_FakePacketLag_Send,
        k_ESteamNetworkingConfig_Global, 0, k_ESteamNetworkingConfig_Int32, &delayMs);
    if (!lag.changed) { logLine(log, "LOOPBACK FAIL native deferred-send setup"); return false; }
    std::array<unsigned char, 65536> payload{};
    unsigned delivered = 0;
    auto exchange = [&](unsigned direction, unsigned lane, bool characterize) {
        const unsigned bytes = lane == GAME_STATE ? 512 : unsigned(payload.size());
        for (unsigned i = 0; i < bytes; ++i) payload[i] = static_cast<unsigned char>(i * 31 + delivered * 17 + lane * 7);
        int result;
        if (characterize) {
            auto* message = backend.utils->AllocateMessage(int(bytes));
            if (!message) return false;
            memcpy(message->m_pData, payload.data(), bytes);
            message->m_conn = pair.handles[direction];
            message->m_idxLane = uint16_t(lane);
            message->m_nFlags = k_nSteamNetworkingSend_ReliableNoNagle;
            auto* submitted = message;
            int64_t number = 0;
            sockets->SendMessages(1, &message, &number, false);
            logLine(log, "Steam native SendMessages false: number=%lld callerPointerRetained=%d; no caller Release",
                static_cast<long long>(number), message == submitted);
            // The actual Steam runtime owns all submitted messages, including failures.
            result = number > 0 ? COOP_NET_OK : COOP_NET_INTERNAL;
        } else {
            logLine(log, "production send begin direction=%u lane=%u bytes=%u delivered=%u", direction, lane, bytes, delivered);
            result = coopnet::sendNativePayload(backend, pair.handles[direction], lane, payload.data(), bytes);
        }
        if (result != COOP_NET_OK) { logLine(log, "LOOPBACK FAIL send result=%d", result); return false; }
        const ULONGLONG deadline = GetTickCount64() + 10000;
        while (GetTickCount64() < deadline) {
            SteamNetworkingMessage_t* message = nullptr;
            const int received = sockets->ReceiveMessagesOnConnection(pair.handles[1 - direction], &message, 1);
            if (received < 0) break;
            if (received == 1) {
                const bool exact = message && message->m_pData && message->m_cbSize == int(bytes) &&
                    message->m_idxLane == lane && !memcmp(message->m_pData, payload.data(), bytes);
                if (message) message->Release();
                if (!exact) { logLine(log, "LOOPBACK FAIL exact payload/lane mismatch"); return false; }
                ++delivered;
                return true;
            }
            Sleep(1);
        }
        logLine(log, "LOOPBACK FAIL receive deadline"); return false;
    };
    if (!exchange(0, GAME_RELIABLE, true)) return false;
    for (unsigned round = 0; round < 4; ++round)
        for (unsigned direction = 0; direction < 2; ++direction)
            for (unsigned lane = 0; lane < 3; ++lane)
                if (!exchange(direction, lane, false)) return false;
    const int rejected = coopnet::sendNativePayload(backend, 0, GAME_RELIABLE, payload.data(), 64);
    if (rejected != COOP_NET_INVALID || !exchange(0, GAME_RELIABLE, false)) {
        logLine(log, "LOOPBACK FAIL invalid-send ownership/recovery result=%d", rejected); return false;
    }
    for (auto handle : pair.handles) {
        SteamNetworkingMessage_t* extra = nullptr;
        const int received = sockets->ReceiveMessagesOnConnection(handle, &extra, 1);
        if (extra) extra->Release();
        if (received != 0) { logLine(log, "LOOPBACK FAIL unexpected extra delivery"); return false; }
    }
    logLine(log, "LOOPBACK PASS pid=%lu delivered=%u lanes=3 invalidSendRecovery=1 nativeNetworkLoopback=1",
        GetCurrentProcessId(), delivered);
    return true;
}
DWORD WINAPI runProbe(void* pointer) {
    const WorkerArgs args = *static_cast<WorkerArgs*>(pointer);
    delete static_cast<WorkerArgs*>(pointer);
    FILE* log = nullptr;
    fopen_s(&log, "KenshiCoop_gns_probe.log", "a");
    coopnet::NativeBackend diagnostics;
    inspect(log, diagnostics);
    if (args.loopback) {
        const bool passed = diagnostics.initialized && runLoopback(log, diagnostics);
        if (!passed) logLine(log, "LOOPBACK FAIL production message-lifetime regression");
        coopnet::shutdownBackend(diagnostics);
        if (log) std::fclose(log);
        InterlockedExchange(&workerFinished, 1);
        FreeLibraryAndExitThread(args.module, passed ? 0 : 1);
    }
    CoopNetApi api{};
    void* context = nullptr;
    bool echoReliable = false, echoState = false, sentReliable = false, sentState = false;
    uint32_t connection = 0;
    char error[COOP_NET_ERROR_CAPACITY] = {};
    CoopNetConfig config{};
    config.size = sizeof(config);
    config.mode = COOP_NET_STEAM;
    config.host = args.host;
    config.port = 213;
    config.generation = GetTickCount64();
    if (!config.generation) config.generation = 1;
    config.remoteSteamId = args.counterpart;
    config.maxPeers = 1;
    config.connectTimeoutMs = 30000;
    config.connectedTimeoutMs = 15000;
    config.sendBufferBytes = 1024 * 1024;
    config.bulkQueueBytes = 512 * 1024;
    if (!diagnostics.initialized || (args.counterpart && args.counterpart == diagnostics.selfSteamId)) {
        logLine(log, "probe rejected: counterpart must be distinct and Steam acquisition must succeed");
    } else if (KenshiCoopNet_GetApi(COOP_NET_ABI_VERSION, sizeof(api), &api) != COOP_NET_OK) {
        logLine(log, "provider ABI unavailable");
    } else if (!(context = api.create(error, sizeof(error)))) {
        logLine(log, "create failed: %s", error);
    } else {
        const int result = api.start(context, &config, error, sizeof(error));
        logLine(log, "start role=%s virtualport=213 generation=%llu result=%d detail=%s",
            args.host ? "host" : "join", static_cast<unsigned long long>(config.generation), result, error);
        const ULONGLONG deadline = GetTickCount64() + 120000;
        ULONGLONG nextStatus = 0;
        bool failed = result != COOP_NET_OK;
        while (!failed && GetTickCount64() < deadline) {
            CoopNetEvent event{};
            event.size = sizeof(event);
            const int polled = api.poll(context, &event);
            if (polled < 0) { logLine(log, "poll failed=%d", polled); failed = true; }
            if (polled > 0) {
                // Always release even rejected/stale events; provider owns message storage.
                if (event.generation != config.generation ||
                    (args.counterpart && event.remoteSteamId && event.remoteSteamId != args.counterpart)) {
                    logLine(log, "rejected event generation/identity mismatch");
                    failed = true;
                } else {
                    if (event.type != COOP_NET_EVENT_MESSAGE)
                        logLine(log, "event=%u connection=%u remote=%llu reason=%d detail=%s",
                            event.type, event.connection, static_cast<unsigned long long>(event.remoteSteamId), event.reason, event.detail);
                    if (event.type == COOP_NET_EVENT_CONNECTED) {
                        connection = event.connection;
                        SteamNetConnectionInfo_t info{};
                        char detail[16384] = {};
                        if (diagnostics.sockets->GetConnectionInfo(connection, &info)) {
                            logLine(log, "actual route flags=%d relayPOP=%u remotePOP=%u authenticated=%d encrypted=%d",
                                info.m_nFlags, info.m_idPOPRelay, info.m_idPOPRemote,
                                !(info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Unauthenticated),
                                !(info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Unencrypted));
                            diagnostics.sockets->GetDetailedConnectionStatus(connection, detail, sizeof(detail));
                            logLine(log, "actual connection detail: %s", detail);
                        } else { logLine(log, "native connection detail unavailable"); failed = true; }
                    } else if (event.type == COOP_NET_EVENT_MESSAGE) {
                        const char* expected = event.traffic == GAME_RELIABLE ? reliablePayload : statePayload;
                        const size_t size = event.traffic == GAME_RELIABLE ? sizeof(reliablePayload) : sizeof(statePayload);
                        if (event.connection != connection || (args.counterpart && event.remoteSteamId != args.counterpart) ||
                            (event.traffic != GAME_RELIABLE && event.traffic != GAME_STATE) ||
                            event.dataSize != size || !event.data || std::memcmp(event.data, expected, size)) {
                            logLine(log, "invalid probe message"); failed = true;
                        } else if (args.host) {
                            const int sent = api.send(context, connection, event.traffic, event.data, event.dataSize);
                            logLine(log, "host echo lane=%u result=%d", event.traffic, sent);
                            if (sent != COOP_NET_OK) failed = true;
                        } else {
                            if (event.traffic == GAME_RELIABLE) echoReliable = true;
                            else echoState = true;
                            logLine(log, "verified echo lane=%u", event.traffic);
                        }
                    } else if (event.type == COOP_NET_EVENT_ERROR || event.type == COOP_NET_EVENT_DISCONNECTED) {
                        failed = true;
                    }
                }
                api.releaseEvent(context, &event);
            }
            if (!args.host && connection && !failed) {
                if (!sentReliable) {
                    const int sent = api.send(context, connection, GAME_RELIABLE, reliablePayload, sizeof(reliablePayload));
                    if (sent == COOP_NET_OK) sentReliable = true;
                    else if (sent != COOP_NET_BACKPRESSURE) failed = true;
                }
                // State delivery is not guaranteed; retransmit the same test payload until observed.
                if (!echoState) {
                    const int sent = api.send(context, connection, GAME_STATE, statePayload, sizeof(statePayload));
                    if (sent == COOP_NET_OK) sentState = true;
                    else if (sent != COOP_NET_BACKPRESSURE) failed = true;
                }
                if (echoReliable && echoState) { logLine(log, "PASS actual distinct-account reliable/state echo"); break; }
            }
            if (GetTickCount64() >= nextStatus) {
                nextStatus = GetTickCount64() + 1000;
                CoopNetStatus status{}; status.size = sizeof(status);
                if (api.status(context, &status) == COOP_NET_OK)
                    logLine(log, "status auth=%d relay=%d connections=%u timerPumps=%ld pumpResult=%ld detail=%s",
                        status.authentication, status.relayAvailability, status.connections,
                        InterlockedCompareExchange(&timerPumpCount, 0, 0),
                        InterlockedCompareExchange(&timerPumpResult, 0, 0), status.detail);
                if (connection) {
                    CoopNetPeerStats stats{}; stats.size = sizeof(stats);
                    if (api.peerStats(context, connection, &stats) == COOP_NET_OK)
                        logLine(log, "actual RTT=%d state=%u pendingReliable=%u pendingState=%u", stats.rttMs, stats.state, stats.pendingReliableBytes, stats.pendingStateBytes);
                }
            }
            Sleep(20);
        }
        logLine(log, "probe end failed=%d reliableSent=%d stateSent=%d reliableEcho=%d stateEcho=%d (host does not claim client receipt)",
            failed, sentReliable, sentState, echoReliable, echoState);
        api.stop(context);
        api.destroy(context);
    }
    coopnet::shutdownBackend(diagnostics);
    if (log) std::fclose(log);
    InterlockedExchange(&workerFinished, 1);
    FreeLibraryAndExitThread(args.module, 0);
}
}

// RE_Kenshi resolves ?startPlugin@@YAXXZ; deliberately C++, not extern C.
__declspec(dllexport) void startPlugin() {
    if (InterlockedCompareExchange(&started, 1, 0)) return;
    FILE* log = nullptr;
    fopen_s(&log, "KenshiCoop_gns_probe.log", "a");
    char role[16] = {}, counterpart[32] = {};
    const DWORD roleLength = GetEnvironmentVariableA("KC_GNS_PROBE_ROLE", role, sizeof(role));
    if (!roleLength) {
        coopnet::NativeBackend backend;
        inspect(log, backend);
        coopnet::shutdownBackend(backend);
        if (log) std::fclose(log);
        return;
    }
    const DWORD idLength = GetEnvironmentVariableA("KC_GNS_PROBE_STEAMID", counterpart, sizeof(counterpart));
    char* end = nullptr;
    errno = 0;
    const unsigned long long id = std::strtoull(counterpart, &end, 10);
    bool digits = idLength > 0 && idLength < sizeof(counterpart);
    for (DWORD i = 0; digits && i < idLength; ++i) digits = counterpart[i] >= '0' && counterpart[i] <= '9';
    const bool host = !std::strcmp(role, "host");
    const bool loopback = !std::strcmp(role, "loopback");
    if (roleLength >= sizeof(role) || (!host && !loopback && std::strcmp(role, "join")) ||
        (idLength && (!digits || errno || !end || *end || !id)) || (!host && !loopback && !idLength)) {
        logLine(log, "invalid role/counterpart environment; JOIN requires a decimal host SteamID64");
        if (log) std::fclose(log);
        return;
    }
    HMODULE module = nullptr;
    // RE_Kenshi plugins stay resident. Permanent pin also prevents a queued
    // TIMERPROC from ever pointing into an unloaded DLL after worker exit.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&startPlugin), &module)) {
        logLine(log, "cannot retain probe module for bounded worker");
        if (log) std::fclose(log);
        return;
    }
    if (KenshiCoopNet_GetApi(COOP_NET_ABI_VERSION, sizeof(mainThreadApi), &mainThreadApi) != COOP_NET_OK ||
        !mainThreadApi.pumpMainThread) {
        logLine(log, "main-thread callback pump ABI unavailable");
        if (log) std::fclose(log);
        return;
    }
    // Bind the main thread before the worker publishes any Steam interface.
    mainThreadApi.pumpMainThread();
    pumpDeadline = GetTickCount64() + 125000;
    const UINT_PTR timer = SetTimer(nullptr, 0, 20, pumpTimer);
    if (!timer) {
        logLine(log, "main-thread network callback timer failed");
        if (log) std::fclose(log);
        return;
    }
    auto* args = new (std::nothrow) WorkerArgs{module, id, host, loopback};
    HANDLE thread = args ? CreateThread(nullptr, 0, runProbe, args, 0, nullptr) : nullptr;
    if (!thread) {
        delete args;
        KillTimer(nullptr, timer);
        InterlockedExchange(&workerFinished, 1);
        FreeLibrary(module);
        logLine(log, "worker creation failed");
    }
    else CloseHandle(thread);
    if (log) std::fclose(log);
}
