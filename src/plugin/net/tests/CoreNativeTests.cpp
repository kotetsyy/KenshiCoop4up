// Real v100 NetLink consumers in isolated processes. No engine, fake sockets,
// fake Steam users or provider replacement; this is not multiplayer-game proof.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS 1
#include <winsock2.h>
#include <windows.h>
#include <sddl.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <stdexcept>
#include "../NetLink.h"
#include "../../../transport/TransportApi.h"
#include "../../sync/SpeedAdmission.h"
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {
using namespace coop;
typedef std::vector<unsigned char> Bytes;
const DWORD RPC_MS = 20000, NETWORK_MS = 45000;
const unsigned ENTITY_COUNT = 64, SAVE_CHUNKS = 512;
const u32 SAVE_ID = 701, LOAD_ID = 702;
enum Op { START = 1, STATUS, PUBLISH, EVENTS, SAVE, SAMPLE,
          STOP, RESET_WORLD, READY_WORLD, SIM, EXIT_ACTOR, LOAD_WORLD, BUILD_REPLAY, INVALID_EVENT, CENSUS, SPAWN_LOOKUP, SPAWN_REPLY,
          SPEED_CLICK, SPEED_EFFECTIVE, SPEED_HEARTBEAT, INVENTORY_CONTROLS, DEBUG_STATS };
void require(bool value, const char* error) {
    if (!value) throw std::runtime_error(error);
}
template<class T> void put(Bytes& out, const T& value) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(&value);
    out.insert(out.end(), p, p + sizeof(value));
}
void putString(Bytes& out, const std::string& value) {
    put(out, u32(value.size())); out.insert(out.end(), value.begin(), value.end());
}
struct Reader {
    const Bytes& data; size_t at;
    Reader(const Bytes& b) : data(b), at(0) {}
    template<class T> T get() {
        require(at <= data.size() && sizeof(T) <= data.size() - at, "short private RPC reply");
        T value; std::memcpy(&value, &data[at], sizeof(value)); at += sizeof(value); return value;
    }
    std::string string() {
        u32 n = get<u32>(); require(n <= data.size() - at, "short private RPC string");
        std::string value(data.begin() + at, data.begin() + at + n); at += n; return value;
    }
    void done() { require(at == data.size(), "extra private RPC bytes"); }
};
void writeAll(HANDLE h, const void* data, DWORD size) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    while (size) { DWORD n = 0; require(WriteFile(h, p, size, &n, 0) && n, "private pipe write failed"); p += n; size -= n; }
}
void readAll(HANDLE h, void* data, DWORD size, bool timed, DWORD timeout = RPC_MS) {
    unsigned char* p = static_cast<unsigned char*>(data); DWORD begin = GetTickCount();
    while (size) {
        if (timed) {
            DWORD available = 0; require(PeekNamedPipe(h, 0, 0, 0, &available, 0) != FALSE, "actor pipe closed");
            require(GetTickCount() - begin < timeout, "actor RPC timeout");
            if (!available) { Sleep(2); continue; }
        }
        DWORD n = 0; require(ReadFile(h, p, size, &n, 0) && n, "private pipe read failed"); p += n; size -= n;
    }
}
Bytes readFrame(HANDLE h, bool timed, DWORD timeout = RPC_MS) {
    u32 size = 0; readAll(h, &size, sizeof(size), timed, timeout);
    require(size <= 8u * 1024u * 1024u, "oversized private RPC frame");
    Bytes bytes(size); if (size) readAll(h, &bytes[0], size, timed, timeout); return bytes;
}
void writeFrame(HANDLE h, const Bytes& bytes) {
    u32 size = u32(bytes.size()); writeAll(h, &size, sizeof(size));
    if (size) writeAll(h, &bytes[0], size);
}
std::wstring executablePath() {
    wchar_t path[32768]; DWORD n = GetModuleFileNameW(0, path, 32768);
    require(n && n < 32768, "test executable path unavailable"); return std::wstring(path, n);
}
struct PrivateRuntime {
    std::wstring path;
    PrivateRuntime() {
        wchar_t temp[MAX_PATH], name[80]; require(GetTempPathW(MAX_PATH, temp) != 0, "private test temp unavailable");
        _snwprintf(name, 79, L"KenshiCoop-core-native-%lu-%lu", GetCurrentProcessId(), GetTickCount()); name[79] = 0;
        path = std::wstring(temp) + name;
        PSECURITY_DESCRIPTOR sd = 0;
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;OW)",
            SDDL_REVISION_1, &sd, 0) != FALSE, "private test DACL failed");
        SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };
        BOOL made = CreateDirectoryW(path.c_str(), &sa); LocalFree(sd);
        require(made != FALSE, "private test directory creation failed");
    }
    ~PrivateRuntime() {
        const wchar_t* files[] = { L"core_native_tests.exe", L"KenshiCoopNetUDP.dll", L"KenshiCoopNetSteam.dll" };
        for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); ++i) DeleteFileW((path + L"\\" + files[i]).c_str());
        RemoveDirectoryW(path.c_str());
    }
};
struct Child {
    HANDLE process, input, output; DWORD rpcTimeout;
    Child(const std::wstring& executable, const std::wstring& cwd, HANDLE job) : process(0), input(0), output(0), rpcTimeout(RPC_MS) {
        HANDLE childIn = 0, childOut = 0; PROCESS_INFORMATION pi; std::memset(&pi, 0, sizeof(pi));
        SECURITY_ATTRIBUTES sa = { sizeof(sa), 0, TRUE };
        require(CreatePipe(&childIn, &input, &sa, 0) != FALSE, "actor input pipe creation failed");
        if (!CreatePipe(&output, &childOut, &sa, 0)) { CloseHandle(childIn); CloseHandle(input); input = 0; throw std::runtime_error("actor output pipe creation failed"); }
        require(SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0) && SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0), "actor pipe isolation failed");
        SIZE_T attrSize = 0; InitializeProcThreadAttributeList(0, 1, 0, &attrSize);
        std::vector<unsigned char> attrStorage(attrSize);
        LPPROC_THREAD_ATTRIBUTE_LIST attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(&attrStorage[0]);
        require(InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) != FALSE, "actor handle-list creation failed");
        HANDLE inherited[] = { childIn, childOut };
        BOOL listed = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), 0, 0);
        STARTUPINFOEXW si; std::memset(&si, 0, sizeof(si)); si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES; si.StartupInfo.hStdInput = childIn;
        si.StartupInfo.hStdOutput = childOut; si.StartupInfo.hStdError = childOut; si.lpAttributeList = attrs;
        std::wstring command = L"\"" + executable + L"\" --actor";
        BOOL started = listed && CreateProcessW(executable.c_str(), &command[0], 0, 0, TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, 0, cwd.c_str(), &si.StartupInfo, &pi);
        DeleteProcThreadAttributeList(attrs); CloseHandle(childIn); CloseHandle(childOut);
        if (!started) { CloseHandle(input); CloseHandle(output); input = output = 0; throw std::runtime_error("actor process creation failed"); }
        process = pi.hProcess;
        if (!AssignProcessToJobObject(job, process)) { TerminateProcess(process, 1); CloseHandle(pi.hThread); throw std::runtime_error("actor cleanup job assignment failed"); }
        ResumeThread(pi.hThread); CloseHandle(pi.hThread);
    }
    ~Child() { if (input) CloseHandle(input); if (output) CloseHandle(output); if (process) CloseHandle(process); }
    void beginRpc(Op op, const Bytes& payload = Bytes()) {
        rpcTimeout = op == SAVE ? NETWORK_MS : RPC_MS;
        Bytes frame; put(frame, u32(op)); frame.insert(frame.end(), payload.begin(), payload.end()); writeFrame(input, frame);
    }
    Bytes finishRpc() {
        Bytes reply = readFrame(output, true, rpcTimeout); Reader r(reply); u32 ok = r.get<u32>();
        if (!ok) throw std::runtime_error(r.string());
        return Bytes(reply.begin() + sizeof(u32), reply.end());
    }
    Bytes rpc(Op op, const Bytes& payload = Bytes()) { beginRpc(op, payload); return finishRpc(); }
    NetStatus status() { Bytes b = rpc(STATUS); Reader r(b); NetStatus s = r.get<NetStatus>(); r.done(); return s; }
    NetDebugStats debugStats(u32* playerId = 0) { Bytes b = rpc(DEBUG_STATS); Reader r(b); u32 id = r.get<u32>(); if (playerId) *playerId = id; NetDebugStats s = r.get<NetDebugStats>(); r.done(); return s; }
    void start(bool host, bool steam, u32 port) {
        Bytes b; put(b, u32(host)); put(b, u32(steam)); put(b, port);
        Bytes reply = rpc(START, b); Reader r(reply); require(r.get<u32>() == 1, "NetLink start not accepted"); r.done();
    }
    void publish(u32 base) { Bytes b; put(b, base); rpc(PUBLISH, b); }
    void events(u32 base, u32 count) { Bytes b; put(b, base); put(b, count); rpc(EVENTS, b); }
    void sim(u32 delay) { Bytes b; put(b, delay); rpc(SIM, b); }
    void finish() { rpc(EXIT_ACTOR); require(WaitForSingleObject(process, 5000) == WAIT_OBJECT_0, "actor shutdown timeout"); DWORD exit = 1; require(GetExitCodeProcess(process, &exit) && exit == 0, "actor shutdown failed"); }
};
struct StateKey {
    u32 owner, index;
    bool operator<(const StateKey& other) const { return owner != other.owner ? owner < other.owner : index < other.index; }
};
struct Observation {
    std::map<StateKey, float> states;
    std::vector<u64> events;
    u32 begins, files, dones, gos, readyLoad, lastGo, buildPlaces, buildRemoves;
    unsigned __int64 bytes, lastGoWorld;
    bool saveGood, goOrdered, buildGood;
    u32 censusPackets[MAX_PLAYERS], censusRows[MAX_PLAYERS];
    bool censusGood;
    u32 spawnReqOwners, spawnInfoOwners;
    sync::SpeedAdmission speedAdmission;
    u32 speedCommands, speedHeartbeats, speedSets;
    float speedMult;
    bool speedPaused;
    u32 checkpointAcks, ordinaryInventories, inventoryOperations[4];
    Observation() : begins(0), files(0), dones(0), gos(0), readyLoad(0), lastGo(0), buildPlaces(0), buildRemoves(0), bytes(0), lastGoWorld(0), saveGood(true), goOrdered(true), buildGood(true), censusGood(true), spawnReqOwners(0), spawnInfoOwners(0),
        speedCommands(0), speedHeartbeats(0), speedSets(0), speedMult(1.0f), speedPaused(false) {
        std::memset(censusPackets, 0, sizeof(censusPackets)); std::memset(censusRows, 0, sizeof(censusRows));
        checkpointAcks=ordinaryInventories=0;
        std::memset(inventoryOperations,0,sizeof(inventoryOperations));
    }
};
void collect(Inbound& inbound, Observation& result, bool isHost) {
    std::deque<InboundInv> inventories; std::deque<InboundInvXferAck> verdicts;
    inbound.drainInventory(inventories,verdicts);
    while (!inventories.empty()) {
        if (inventories.front().flags & INV_FLAG_CHECKPOINT_ACK) ++result.checkpointAcks;
        else ++result.ordinaryInventories;
        inventories.pop_front();
    }
    std::deque<InboundInvXfer> transfers; inbound.drainInvXfers(transfers);
    while (!transfers.empty()) {
        require(transfers.front().pkt.operation<4,"invalid inventory operation delivered");
        ++result.inventoryOperations[transfers.front().pkt.operation]; transfers.pop_front();
    }
    std::deque<InboundEntity> entities; inbound.drainEntities(entities);
    std::deque<InboundSpeed> speeds; inbound.drainSpeed(speeds);
    while (!speeds.empty()) {
        const InboundSpeed& s = speeds.front();
        sync::SpeedAdmission::Kind accepted = result.speedAdmission.accept(s.pkt, s.ownerId, isHost);
        if (accepted == sync::SpeedAdmission::COMMAND || accepted == sync::SpeedAdmission::EFFECTIVE) {
            result.speedMult = s.pkt.speed;
            result.speedPaused = (s.pkt.flags & SPEED_PAUSED) != 0;
            if (accepted == sync::SpeedAdmission::COMMAND) ++result.speedCommands;
            else ++result.speedSets;
        } else if (accepted == sync::SpeedAdmission::HEARTBEAT) ++result.speedHeartbeats;
        speeds.pop_front();
    }
    while (!entities.empty()) { const InboundEntity& e = entities.front(); StateKey key = { e.ownerId, e.e.hIndex }; result.states[key] = e.e.x; entities.pop_front(); }
    std::deque<InboundEvent> events; inbound.drainEvents(events);
    while (!events.empty()) { result.events.push_back((u64(events.front().ownerId) << 32) | events.front().ev.eventId); events.pop_front(); }
    std::deque<InboundNpcCensus> censuses; inbound.drainNpcCensus(censuses);
    while (!censuses.empty()) {
        const InboundNpcCensus& c = censuses.front();
        require(c.ownerId < MAX_PLAYERS, "census origin escaped authenticated player range");
        result.censusGood = result.censusGood && (c.hands.empty() ? c.pos.empty() :
            c.hands.size() == 5 && c.pos.size() == 3 && c.hands[0] == 2 && c.hands[1] == 41 &&
            c.hands[2] == 3 && c.hands[3] == 9001 && c.hands[4] == 7 &&
            c.pos[0] == 11 && c.pos[1] == 2 && c.pos[2] == -3);
        ++result.censusPackets[c.ownerId]; result.censusRows[c.ownerId] = u32(c.hands.size() / 5);
        censuses.pop_front();
    }
    std::deque<InboundSpawnReq> requests; inbound.drainSpawnReqs(requests);
    while (!requests.empty()) {
        const InboundSpawnReq& r = requests.front();
        require(r.ownerId < MAX_PLAYERS, "invalid spawn request origin");
        result.spawnReqOwners |= 1u << r.ownerId;
        requests.pop_front();
    }
    std::deque<InboundSpawnInfo> descriptions; inbound.drainSpawnInfos(descriptions);
    while (!descriptions.empty()) {
        const InboundSpawnInfo& r = descriptions.front();
        require(r.ownerId < MAX_PLAYERS, "invalid spawn description origin");
        result.spawnInfoOwners |= 1u << r.ownerId;
        descriptions.pop_front();
    }
    // GO is drained first; a taken GO guarantees earlier same-lane DONE is
    // already enqueued before the following atomic transfer drain.
    std::deque<InboundLoadGo> gos; inbound.drainLoadGos(gos);
    std::deque<InboundSaveBegin> begins; std::deque<InboundSaveFile> files; std::deque<InboundSaveDone> dones;
    inbound.drainSaveTransfer(begins, files, dones);
    while (!begins.empty()) { const SaveBeginPacket& p = begins.front().pkt;
        result.saveGood = result.saveGood && p.ownerId == 0 && p.xferId == SAVE_ID && p.fileCount == 1 && p.totalBytes == u64(SAVE_CHUNKS) * SAVE_CHUNK_MAX;
        ++result.begins; begins.pop_front(); }
    while (!files.empty()) { const InboundSaveFile& p = files.front();
        result.saveGood = result.saveGood && result.begins == 1 && p.hdr.xferId == SAVE_ID && p.hdr.fileIdx == 0 &&
            p.path == "frame.save" && p.hdr.offset == result.files * SAVE_CHUNK_MAX && p.data.size() == SAVE_CHUNK_MAX;
        for (size_t i = 0; i < p.data.size(); ++i) result.saveGood = result.saveGood && p.data[i] == (result.files % 251);
        result.bytes += p.data.size(); ++result.files; files.pop_front(); }
    while (!dones.empty()) { const InboundSaveDone& p = dones.front();
        result.saveGood = result.saveGood && p.hdr.xferId == SAVE_ID && p.crcs.size() == 1 && p.crcs[0] == 0x12345678 && result.files == SAVE_CHUNKS;
        ++result.dones; dones.pop_front(); }
    while (!gos.empty()) { const LoadGoPacket& p = gos.front().pkt;
        result.lastGo = p.loadId;
        result.lastGoWorld = p.worldToken;
        if (p.loadId == LOAD_ID) {
            result.goOrdered = result.goOrdered && result.dones == 1 && p.worldToken != 0;
            ++result.gos;
        }
        gos.pop_front(); }
    std::deque<InboundTime> times; inbound.drainTime(times);
    while (!times.empty()) { if (times.front().pkt.readyLoadId) result.readyLoad = times.front().pkt.readyLoadId; times.pop_front(); }
    std::deque<InboundBuildPlace> places; inbound.drainBuildPlace(places);
    while (!places.empty()) {
        const BuildPlacePacket& p = places.front().pkt;
        result.buildGood = result.buildGood && p.ownerId == 0 && p.seq == 1 && p.key[0] == 9000 &&
            std::strcmp(p.sid, "native-core-fixture") == 0 && p.x == 1 && p.y == 2 && p.z == 3;
        ++result.buildPlaces; places.pop_front();
    }
    std::deque<InboundBuildRemove> removals; inbound.drainBuildRemove(removals);
    while (!removals.empty()) {
        const BuildRemovePacket& p = removals.front().pkt;
        result.buildGood = result.buildGood && p.ownerId == 0 && p.seq == 2 && p.key[0] == 9000;
        ++result.buildRemoves; removals.pop_front();
    }
}
Bytes serialize(const Observation& result) {
    Bytes out; put(out, u32(result.states.size()));
    for (std::map<StateKey, float>::const_iterator it = result.states.begin(); it != result.states.end(); ++it) {
        put(out, it->first.owner); put(out, it->first.index); put(out, it->second);
    }
    put(out, u32(result.events.size())); for (size_t i = 0; i < result.events.size(); ++i) put(out, result.events[i]);
    put(out, result.begins); put(out, result.files); put(out, result.dones); put(out, result.gos); put(out, result.readyLoad); put(out, result.lastGo);
    put(out, result.lastGoWorld);
    put(out, result.bytes); put(out, u32(result.saveGood)); put(out, u32(result.goOrdered));
    put(out, result.buildPlaces); put(out, result.buildRemoves); put(out, u32(result.buildGood));
    for (u32 i = 0; i < MAX_PLAYERS; ++i) { put(out, result.censusPackets[i]); put(out, result.censusRows[i]); }
    put(out, u32(result.censusGood)); put(out, result.spawnReqOwners); put(out, result.spawnInfoOwners);
    put(out, result.speedCommands); put(out, result.speedHeartbeats); put(out, result.speedSets);
    put(out,result.checkpointAcks); put(out,result.ordinaryInventories);
    for (u32 i=0;i<4;++i) put(out,result.inventoryOperations[i]);
    put(out, result.speedMult); put(out, u32(result.speedPaused)); return out;
}
Observation sample(Child& child) {
    Bytes b = child.rpc(SAMPLE); Reader r(b); Observation out;
    u32 n = r.get<u32>(); require(n <= ENTITY_COUNT * MAX_PLAYERS, "unexpected consumer entity count");
    for (u32 i = 0; i < n; ++i) { StateKey key; key.owner = r.get<u32>(); key.index = r.get<u32>(); out.states[key] = r.get<float>(); }
    n = r.get<u32>(); require(n <= 1024, "unexpected consumer event count"); for (u32 i = 0; i < n; ++i) out.events.push_back(r.get<u64>());
    out.begins = r.get<u32>(); out.files = r.get<u32>(); out.dones = r.get<u32>(); out.gos = r.get<u32>(); out.readyLoad = r.get<u32>(); out.lastGo = r.get<u32>();
    out.lastGoWorld = r.get<unsigned __int64>();
    out.bytes = r.get<unsigned __int64>(); out.saveGood = r.get<u32>() != 0; out.goOrdered = r.get<u32>() != 0;
    out.buildPlaces = r.get<u32>(); out.buildRemoves = r.get<u32>(); out.buildGood = r.get<u32>() != 0;
    for (u32 i = 0; i < MAX_PLAYERS; ++i) { out.censusPackets[i] = r.get<u32>(); out.censusRows[i] = r.get<u32>(); }
    out.censusGood = r.get<u32>() != 0; out.spawnReqOwners = r.get<u32>(); out.spawnInfoOwners = r.get<u32>();
    out.speedCommands = r.get<u32>(); out.speedHeartbeats = r.get<u32>(); out.speedSets = r.get<u32>();
    out.checkpointAcks=r.get<u32>(); out.ordinaryInventories=r.get<u32>();
    for (u32 i=0;i<4;++i) out.inventoryOperations[i]=r.get<u32>();
    out.speedMult = r.get<float>(); out.speedPaused = r.get<u32>() != 0; r.done(); return out;
}
bool hasStates(const Observation& s, u32 owner, u32 base) {
    for (u32 i = 0; i < ENTITY_COUNT; ++i) { StateKey key = { owner, 1000 + i }; std::map<StateKey, float>::const_iterator it = s.states.find(key);
        if (it == s.states.end() || it->second != float(base + i)) return false; } return true;
}
void expectStates(Child& child, u32 owner, u32 base) {
    DWORD begin = GetTickCount(); while (!hasStates(sample(child), owner, base)) {
        require(GetTickCount() - begin < NETWORK_MS, "missing independent entity stream/chunk or stale position"); Sleep(10);
    }
}
void expectEvents(Child& child, u32 owner, u32 base, u32 count) {
    DWORD begin = GetTickCount();
    for (;;) {
        Observation s = sample(child); std::vector<u32> ids;
        for (size_t i = 0; i < s.events.size(); ++i) if (u32(s.events[i] >> 32) == owner) ids.push_back(u32(s.events[i]));
        require(ids.size() <= count, "one-shot reliable event duplicated");
        if (ids.size() == count) { for (u32 i = 0; i < count; ++i) require(ids[i] == base + i, "reliable event FIFO corrupted"); return; }
        require(GetTickCount() - begin < NETWORK_MS, "reliable event lost under native backpressure"); Sleep(10);
    }
}
void waitPhase(Child& child, NetPhase phase) {
    DWORD begin = GetTickCount(); for (;;) { NetStatus s = child.status(); if (s.phase == phase) return;
        if (s.phase == NET_FAILED) {
            std::fprintf(stderr, "native lifecycle failure: expected=%d phase=%d error=%d arg=%u detail=%s\n",
                         int(phase), int(s.phase), int(s.error), s.errorArg, s.errorRaw);
            throw std::runtime_error("native NetLink failed before expected phase");
        }
        require(GetTickCount() - begin < NETWORK_MS, "native NetLink phase timeout"); Sleep(10); }
}
void bootstrapFailure(Child& host, Child& child, u32 clientIndex, u32 loadId, const char* reason) {
    NetStatus h = host.status(), p = child.status();
    u32 playerId = 0; NetDebugStats hs = host.debugStats(), ps = child.debugStats(&playerId);
    Observation seen = sample(child);
    char detail[1024];
    _snprintf(detail, sizeof(detail) - 1,
        "%s: clientIndex=%u playerId=%u targetLoad=%u observedGo=%u observedWorld=%I64u readyLoad=%u begins=%u files=%u dones=%u "
        "host(phase=%d error=%d arg=%u members=%u handshakes=%u reliable=%lu bulk=%lu detail=%s) "
        "client(phase=%d error=%d arg=%u members=%u attempts=%u reliable=%lu bulk=%lu detail=%s)",
        reason, clientIndex, playerId, loadId, seen.lastGo, seen.lastGoWorld, seen.readyLoad, seen.begins, seen.files, seen.dones,
        int(h.phase), int(h.error), h.errorArg, h.memberMask, h.pendingHandshakes, hs.pendingReliableBytes, hs.pendingBulkBytes, h.errorRaw,
        int(p.phase), int(p.error), p.errorArg, p.memberMask, p.connectAttempts, ps.pendingReliableBytes, ps.pendingBulkBytes, p.errorRaw);
    detail[sizeof(detail) - 1] = 0;
    for (u32 i = 0; i < hs.peerStatsCount && i < MAX_PLAYERS; ++i) {
        const NetDebugPeer& peer = hs.peers[i];
        std::fprintf(stderr, "native bootstrap host peer: connection=%lu player=%lu reliable=%lu bulk=%lu unacked=%lu rtt=%lu\n",
            peer.slot, peer.playerId, peer.pendingReliableBytes, peer.pendingBulkBytes, peer.unackedReliableBytes, peer.rttMs);
    }
    throw std::runtime_error(detail);
}
void waitHostAdmission(Child& host, Child& child, u32 clientIndex, u32 loadId, DWORD begin) {
    // Client NET_CONNECTED means WELCOME/ACK submission, not host ACK receipt.
    // A one-shot LOAD fanout only includes host-admitted peers. This fence
    // consumes the first GO wait's existing deadline, rather than adding one.
    for (;;) {
        NetStatus s = host.status();
        if (s.phase == NET_LISTENING && s.memberMask == 15) return;
        if (s.phase == NET_FAILED || child.status().phase != NET_CONNECTED || GetTickCount() - begin >= NETWORK_MS)
            bootstrapFailure(host, child, clientIndex, loadId, "host SESSION_ACK admission incomplete before LOAD");
        Sleep(10);
    }
}
void waitBootstrapGo(Child& host, Child& child, u32 clientIndex, u32 loadId, DWORD begin) {
    while (sample(child).lastGo != loadId) {
        if (host.status().phase == NET_FAILED || child.status().phase == NET_FAILED || GetTickCount() - begin >= NETWORK_MS)
            bootstrapFailure(host, child, clientIndex, loadId, "native reconnect LOAD_GO not observed");
        Sleep(10);
    }
}
void expectCensus(Child& child, u32 owner, u32 packets, u32 rows) {
    DWORD begin = GetTickCount();
    for (;;) {
        Observation s = sample(child);
        require(s.censusGood && s.censusPackets[owner] <= packets, "census contents/origin changed or duplicated");
        if (s.censusPackets[owner] == packets) {
            require(s.censusRows[owner] == rows, "empty census did not clear previous existence claim"); return;
        }
        require(GetTickCount() - begin < NETWORK_MS, "client census rejected/lost after READY"); Sleep(10);
    }
}
void expectSpawn(Child& child, u32 owner, bool description) {
    DWORD begin = GetTickCount();
    for (;;) {
        Observation s = sample(child);
        if ((description ? s.spawnInfoOwners : s.spawnReqOwners) & (1u << owner)) return;
        require(child.status().phase != NET_FAILED, "legitimate distributed spawn packet rejected native link");
        require(GetTickCount() - begin < NETWORK_MS, "distributed spawn lookup/description not relayed"); Sleep(10);
    }
}
void expectSpeed(Child& child, u32 commands, u32 sets, bool paused) {
    DWORD begin = GetTickCount();
    for (;;) {
        Observation s = sample(child);
        require(s.speedCommands <= commands && s.speedSets <= sets, "speed command/SET duplicated");
        if (s.speedCommands == commands && s.speedSets == sets) {
            require(s.speedPaused == paused && s.speedMult == 1.0f, "wrong admitted speed command/effective"); return;
        }
        require(GetTickCount() - begin < NETWORK_MS, "independent sender speed command/SET rejected or lost"); Sleep(10);
    }
}
u32 reservePort() {
    WSADATA data; require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "reserve port startup failed");
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); require(s != INVALID_SOCKET, "reserve port socket failed");
    sockaddr_in addr; std::memset(&addr, 0, sizeof(addr)); addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "reserve port bind failed"); int len = sizeof(addr);
    require(getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0, "reserve port address failed");
    u32 port = ntohs(addr.sin_port); closesocket(s); WSACleanup(); return port;
}
int actor() {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
    Inbound inbound; NetLink net; Observation observed; u32 timeSequence = 0, speedSequence = 0;
    bool isHost = false;
    for (;;) {
        Bytes frame = readFrame(input, false); Reader r(frame); Op op = Op(r.get<u32>()); Bytes result; bool finish = false;
        try {
            switch (op) {
            case START: {
                NetStartOptions options; options.isHost = r.get<u32>() != 0;
                isHost = options.isHost;
                options.transport = r.get<u32>() ? NET_TRANSPORT_STEAM : NET_TRANSPORT_UDP; options.port = int(r.get<u32>());
                options.address = options.transport == NET_TRANSPORT_UDP ? "127.0.0.1" : "";
                r.done();
                net.setLocalName(options.isHost ? "CoreHost" : "CoreJoin"); put(result, u32(net.start(options, &inbound)));
                break;
            }
            case STATUS: { r.done(); NetStatus s; net.copyStatus(&s); put(result, s); break; }
            case DEBUG_STATS: { r.done(); put(result, net.localId()); NetDebugStats s; net.copyDebugStats(&s); put(result, s); break; }
            case PUBLISH: {
                u32 base = r.get<u32>(); r.done(); EntityState entities[ENTITY_COUNT]; std::memset(entities, 0, sizeof(entities));
                for (u32 i = 0; i < ENTITY_COUNT; ++i) { entities[i].hType = 1; entities[i].hContainer = 41; entities[i].hContainerSerial = 3;
                    entities[i].hIndex = 1000 + i; entities[i].hSerial = 7; entities[i].x = float(base + i); }
                net.setOwnedEntities(net.localId(), entities, ENTITY_COUNT); break;
            }
            case EVENTS: {
                u32 base = r.get<u32>(), count = r.get<u32>(); r.done(); require(count <= 128, "oversized event fixture");
                for (u32 i = 0; i < count; ++i) { EventPacket p; std::memset(&p, 0, sizeof(p)); p.type = PKT_EVENT;
                    p.event = EVT_KNOCKOUT; p.ownerId = net.localId(); p.eventId = base + i; p.sType = 1; p.sIndex = 1000; p.sSerial = 7; net.queueEvent(p); }
                break;
            }
            case BUILD_REPLAY: {
                r.done(); BuildPlacePacket place; std::memset(&place, 0, sizeof(place));
                place.type = PKT_BUILD_PLACE; place.ownerId = net.localId(); place.seq = 1; place.key[0] = 9000;
                std::strcpy(place.sid, "native-core-fixture"); place.x = 1; place.y = 2; place.z = 3; net.queueBuildPlace(place);
                BuildRemovePacket remove; std::memset(&remove, 0, sizeof(remove));
                remove.type = PKT_BUILD_REMOVE; remove.ownerId = net.localId(); remove.seq = 2; remove.key[0] = 9000;
                net.queueBuildRemove(remove); break;
            }
            case INVALID_EVENT: {
                r.done(); EventPacket p; std::memset(&p, 0, sizeof(p));
                p.type = PKT_EVENT; p.event = 255; p.ownerId = net.localId(); p.eventId = 700;
                p.sType = 1; p.sIndex = 1000; p.sSerial = 7; net.queueEvent(p); break;
            }
            case CENSUS: {
                u32 rows = r.get<u32>(); r.done(); require(rows <= 1, "oversized census fixture");
                const u32 hands[] = { 2, 41, 3, 9001, 7 }; const float pos[] = { 11, 2, -3 };
                net.queueNpcCensus(net.localId(), hands, pos, rows); break;
            }
            case SPAWN_LOOKUP: {
                u32 target=r.get<u32>(); r.done(); SpawnReqPacket p; std::memset(&p, 0, sizeof(p)); p.type = PKT_SPAWN_REQ; p.ownerId = net.localId();
                p.targetOwner=target;
                p.hType = 1; p.hContainer = 41; p.hContainerSerial = 3; p.hIndex = 9301; p.hSerial = 7;
                net.queueSpawnReq(p); break;
            }
            case SPAWN_REPLY: {
                u32 requestor=r.get<u32>(); r.done(); SpawnInfoPacket p; std::memset(&p, 0, sizeof(p)); p.type = PKT_SPAWN_INFO; p.ownerId = net.localId();
                p.requestorId=requestor;
                p.hType = 1; p.hContainer = 41; p.hContainerSerial = 3; p.hIndex = 9301; p.hSerial = 7;
                p.found = 1; p.dead = SPAWN_BODY_KO; p.age = 0.75f; p.x = 55; p.y = 2; p.z = -3;
                std::strcpy(p.charSid, "native-npc"); std::strcpy(p.facSid, "native-faction"); net.queueSpawnInfo(p); break;
            }
            case INVENTORY_CONTROLS: {
                r.done(); const u32 key[5]={1,41,3,9301,7};
                net.queueInvSnapshot(net.localId(),0,key,0,0,INV_FLAG_CHECKPOINT_ACK,1);
                for (u32 operation=0;operation<4;++operation) {
                    InvXferPacket p; std::memset(&p,0,sizeof(p));
                    p.type=PKT_INV_XFER; p.ownerId=net.localId(); p.xferId=800+operation;
                    p.operation=u8(operation); p.quantity=1;
                    p.sourceRevision=1; p.actorRevision=1;
                    p.sType=p.dType=1; p.sContainer=p.dContainer=41;
                    p.sContainerSerial=p.dContainerSerial=3;
                    p.sIndex=9301; p.dIndex=9302; p.sSerial=p.dSerial=7;
                    std::strcpy(p.stringID,"copper");
                    std::vector<InvItemEntry> contents; net.queueInvXfer(p,contents);
                }
                // Same-lane marker follows every control; observing it proves the
                // uninvolved guest survived any preceding erroneous relay.
                net.queueInvSnapshot(net.localId(),0,key,0,0,0,2);
                break;
            }
            case SPEED_CLICK:
            case SPEED_EFFECTIVE:
            case SPEED_HEARTBEAT: {
                // A local host command can precede the peer-drain this tick,
                // exactly as syncSpeed captures local intent before peer REQs.
                if (op != SPEED_EFFECTIVE) collect(inbound, observed, isHost);
                bool paused = op == SPEED_HEARTBEAT ? observed.speedPaused : r.get<u32>() != 0;
                r.done(); SpeedPacket p; std::memset(&p, 0, sizeof(p));
                p.type = op == SPEED_EFFECTIVE ? PKT_SPEED_SET : PKT_SPEED_REQ;
                p.ownerId = net.localId(); p.seq = ++speedSequence; p.speed = observed.speedMult;
                p.flags = (paused ? SPEED_PAUSED : 0) | (op == SPEED_CLICK ? SPEED_COMMAND : 0);
                if (op == SPEED_EFFECTIVE) observed.speedPaused = paused;
                net.queueSpeed(p); put(result, u32(paused)); break;
            }
            case SAVE: {
                r.done(); SaveBeginPacket begin; std::memset(&begin, 0, sizeof(begin)); begin.type = PKT_SAVE_BEGIN;
                begin.ownerId = net.localId(); begin.xferId = SAVE_ID; begin.fileCount = 1; begin.totalBytes = u64(SAVE_CHUNKS) * SAVE_CHUNK_MAX;
                std::strcpy(begin.name, "native-test"); net.queueSaveBegin(begin);
                u8 chunk[SAVE_CHUNK_MAX]; const char path[] = "frame.save";
                DWORD pacedSince = GetTickCount();
                for (u32 i = 0; i < SAVE_CHUNKS; ++i) {
                    while (!net.canQueueSaveBytes(sizeof(SaveFileHeader) + sizeof(path) - 1 + sizeof(chunk))) {
                        NetStatus status; net.copyStatus(&status);
                        if (status.phase == NET_FAILED || GetTickCount() - pacedSince >= NETWORK_MS - 1000) {
                            NetDebugStats stats; net.copyDebugStats(&stats); char detail[512];
                            _snprintf(detail, sizeof(detail) - 1,
                                      "bulk producer stalled: chunk=%u phase=%d error=%d arg=%u peers=%u pendingBulk=%lu pendingState=%lu sentBytesPerSec=%lu rtt=%lu detail=%s",
                                      i, int(status.phase), int(status.error), status.errorArg,
                                      status.memberMask, stats.pendingBulkBytes, stats.pendingStateBytes,
                                      stats.sentBytesPerSec, stats.maxRttMs, status.errorRaw);
                            detail[sizeof(detail) - 1] = 0; throw std::runtime_error(detail);
                        }
                        Sleep(2);
                    }
                    std::memset(chunk, i % 251, sizeof(chunk)); SaveFileHeader p; std::memset(&p, 0, sizeof(p));
                    p.type = PKT_SAVE_FILE; p.ownerId = net.localId(); p.xferId = SAVE_ID; p.pathLen = sizeof(path) - 1;
                    p.offset = i * SAVE_CHUNK_MAX; p.dataLen = SAVE_CHUNK_MAX; net.queueSaveFile(p, path, chunk, sizeof(chunk));
                }
                SaveDoneHeader done; std::memset(&done, 0, sizeof(done)); done.type = PKT_SAVE_DONE; done.ownerId = net.localId(); done.xferId = SAVE_ID;
                done.fileCount = 1; u32 crc = 0x12345678; net.queueSaveDone(done, &crc, 1);
                break;
            }
            case LOAD_WORLD: {
                u32 loadId = r.get<u32>(); r.done(); LoadGoPacket go; std::memset(&go, 0, sizeof(go));
                go.type = PKT_LOAD_GO; go.ownerId = net.localId(); go.loadId = loadId;
                go.fingerprint = 0x34567890; std::strcpy(go.name, "native-test"); net.queueLoadGo(go); break;
            }
            case SAMPLE: {
                r.done(); collect(inbound, observed, isHost); result = serialize(observed);
                require(GetModuleHandleW(L"steam_api64.dll") == 0, "UDP core consumer loaded Steam API");
                require(GetModuleHandleW(L"KenshiCoopNetSteam.dll") == 0, "UDP core consumer loaded Steam provider/fallback");
                break;
            }
            case STOP: r.done(); net.stop(); inbound.flushWorldState(); observed = Observation(); break;
            case RESET_WORLD: {
                u32 rotate = r.get<u32>(); r.done(); inbound.flushWorldState();
                net.bumpSessionEpoch(rotate != 0); observed = Observation(); break;
            }
            case READY_WORLD: {
                u32 load = r.get<u32>(); r.done(); net.queueWorldReady(load);
                TimePacket p; std::memset(&p, 0, sizeof(p)); p.type = PKT_TIME; p.ownerId = net.localId(); p.seq = ++timeSequence; p.gameHours = 37.5; p.readyLoadId = load; net.queueTime(p); break;
            }
            case SIM: {
                u32 delay = r.get<u32>(); r.done();
                require(!net.isRunning(), "simulation configured only before native session launch");
                net.setNetSim(delay, 0, 0); break;
            }
            case EXIT_ACTOR: r.done(); net.stop(); finish = true; break;
            default: throw std::runtime_error("unknown private actor command");
            }
            Bytes reply; put(reply, u32(1)); reply.insert(reply.end(), result.begin(), result.end()); writeFrame(output, reply);
            if (!reply.empty()) SecureZeroMemory(&reply[0], reply.size());
        } catch (const std::exception& error) { Bytes reply; put(reply, u32(0)); putString(reply, error.what()); writeFrame(output, reply); }
        if (!frame.empty()) SecureZeroMemory(&frame[0], frame.size());
        if (!result.empty()) SecureZeroMemory(&result[0], result.size());
        if (finish) return 0;
    }
}
int controller(const std::wstring& udp, const std::wstring& steam) {
    PrivateRuntime runtime; const std::wstring exe = runtime.path + L"\\core_native_tests.exe";
    require(CopyFileW(executablePath().c_str(), exe.c_str(), TRUE) != FALSE, "copy exact core consumer failed");
    require(CopyFileW(udp.c_str(), (runtime.path + L"\\KenshiCoopNetUDP.dll").c_str(), TRUE) != FALSE, "copy exact UDP provider failed");
    require(CopyFileW(steam.c_str(), (runtime.path + L"\\KenshiCoopNetSteam.dll").c_str(), TRUE) != FALSE, "copy exact Steam provider failed");
    HANDLE job = CreateJobObjectW(0, 0); require(job != 0, "core consumer cleanup job unavailable");
    struct JobLease { HANDLE h; ~JobLease() { CloseHandle(h); } } lease = { job };
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits; std::memset(&limits, 0, sizeof(limits)); limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE, "core consumer cleanup job configuration failed");
    Child host(exe, runtime.path, job); const u32 port = reservePort();
    host.start(true, false, port); waitPhase(host, NET_LISTENING);
    Child first(exe, runtime.path, job), second(exe, runtime.path, job), third(exe, runtime.path, job);
    Child* clients[MAX_JOINS] = { &first, &second, &third };
    for (u32 i = 0; i < MAX_JOINS; ++i) {
        Bytes simulation; put(simulation, u32(i == 0 ? 125 : 0)); clients[i]->rpc(SIM, simulation);
        clients[i]->start(false, false, port); waitPhase(*clients[i], NET_CONNECTED);
    }
    DWORD rosterStart = GetTickCount(); NetStatus hs = host.status();
    while (hs.memberMask != 15) {
        require(GetTickCount() - rosterStart < NETWORK_MS, "native host+3 gameplay roster incomplete"); Sleep(10); hs = host.status();
    }
    require(hs.active == NET_TRANSPORT_UDP, "host did not use native UDP");
    for (u32 i = 0; i < MAX_JOINS; ++i) require(clients[i]->status().active == NET_TRANSPORT_UDP, "client did not use native UDP");
    std::printf("PASS actual NetLink native UDP HELLO/WELCOME host+3\n");
    host.rpc(BUILD_REPLAY); host.events(98, 2); Sleep(100);
    for (u32 i = 0; i < MAX_JOINS; ++i) {
        Observation pending = sample(*clients[i]);
        require(pending.events.empty() && pending.buildPlaces == 0 && pending.buildRemoves == 0,
                "unready world received reliable gameplay before completed load");
    }
    Bytes initialGo; put(initialGo, u32(500)); host.rpc(LOAD_WORLD, initialGo);
    for (u32 i = 0; i < MAX_JOINS; ++i) {
        DWORD begin = GetTickCount();
        while (sample(*clients[i]).lastGo != 500) {
            require(GetTickCount() - begin < NETWORK_MS, "bootstrap LOAD_GO lost"); Sleep(10);
        }
        if (i == 0) { clients[i]->events(199, 1); Sleep(100); }
        clients[i]->rpc(READY_WORLD, initialGo);
    }
    for (u32 i = 0; i < MAX_JOINS; ++i) {
        expectEvents(*clients[i], 0, 98, 2); DWORD start = GetTickCount();
        for (;;) {
            Observation replay = sample(*clients[i]); require(replay.buildGood && replay.buildPlaces <= 1 && replay.buildRemoves <= 1, "reliable building replay changed or duplicated");
            if (replay.buildPlaces == 1 && replay.buildRemoves == 1) break;
            require(GetTickCount() - start < NETWORK_MS, "connect-time building replay lost before READY"); Sleep(10);
        }
    }
    expectEvents(host, 1, 199, 1); expectEvents(second, 1, 199, 1); expectEvents(third, 1, 199, 1);
    std::printf("PASS pre-READY building/event retention and client READY bypass without deadlock\n");
    first.rpc(INVENTORY_CONTROLS);
    DWORD inventoryStart=GetTickCount();
    for (;;) {
        Observation h=sample(host), s=sample(second), t=sample(third);
        require(s.checkpointAcks==0 && t.checkpointAcks==0,
                "host-only checkpoint ACK escaped to uninvolved guests");
        for (u32 operation=1;operation<4;++operation)
            require(s.inventoryOperations[operation]==0 && t.inventoryOperations[operation]==0,
                    "host-only inventory request escaped to uninvolved guests");
        if (h.checkpointAcks==1 && h.ordinaryInventories==1 &&
            h.inventoryOperations[0]==1 && h.inventoryOperations[1]==1 &&
            h.inventoryOperations[2]==1 && h.inventoryOperations[3]==1 &&
            s.ordinaryInventories==1 && t.ordinaryInventories==1 &&
            s.inventoryOperations[0]==1 && t.inventoryOperations[0]==1) break;
        require(GetTickCount()-inventoryStart<NETWORK_MS,"inventory controls or ordinary relay lost");
        Sleep(10);
    }
    require(host.status().memberMask==15,"inventory control retired a gameplay seat");
    for (u32 i=0;i<MAX_JOINS;++i)
        require(clients[i]->status().phase==NET_CONNECTED && clients[i]->status().connectAttempts==1,
                "valid host-only inventory control disconnected a guest");
    std::printf("PASS host+3 host-only inventory controls and ordinary inventory/op0 relay\n");
    // Empty -> populated -> empty exercises the exact post-load disconnect and
    // clearing a prior existence claim, through real encrypted star relay.
    Child* censusPeers[MAX_PLAYERS] = { &host, &first, &second, &third };
    for (u32 round = 0; round < 3; ++round) {
        const u32 rows = round == 1 ? 1 : 0; Bytes census; put(census, rows);
        for (u32 owner = 0; owner < MAX_PLAYERS; ++owner) {
            censusPeers[owner]->rpc(CENSUS, census);
            for (u32 receiver = 0; receiver < MAX_PLAYERS; ++receiver)
                if (receiver != owner) expectCensus(*censusPeers[receiver], owner, round + 1, rows);
        }
    }
    require(host.status().memberMask == 15 && host.status().error == NET_ERR_NONE,
            "valid client census retired a gameplay seat");
    for (u32 i = 0; i < MAX_JOINS; ++i)
        require(clients[i]->status().phase == NET_CONNECTED && clients[i]->status().connectAttempts == 1,
                "valid client census triggered a reconnect after completed world READY");
    std::printf("PASS post-READY host+3 bidirectional empty/populated/cleared NPC census without reconnect\n");
    // After cell authority moves to a guest, the host must be able to ask
    // about that guest's runtime NPCs without retiring the native session.
    Bytes targetFirst; put(targetFirst,u32(1)); host.rpc(SPAWN_LOOKUP,targetFirst);
    expectSpawn(first,0,false);
    require(sample(second).spawnReqOwners==0 && sample(third).spawnReqOwners==0,
            "non-author guest consumed host spawn query");
    Bytes targetHost; put(targetHost,u32(0)); first.rpc(SPAWN_REPLY,targetHost); expectSpawn(host,1,true);
    require(sample(second).spawnInfoOwners==0 && sample(third).spawnInfoOwners==0,
            "unrelated guest consumed authoritative spawn answer");
    // A guest may also need the NPC owned by another guest through the star.
    Bytes targetThird; put(targetThird,u32(3)); first.rpc(SPAWN_LOOKUP,targetThird); expectSpawn(third,1,false);
    require(sample(host).spawnReqOwners==0 && sample(second).spawnReqOwners==0,
            "host or unrelated guest consumed another author's query");
    third.rpc(SPAWN_REPLY,targetFirst); expectSpawn(first,3,true);
    require(sample(host).spawnInfoOwners==(1u<<1) && sample(second).spawnInfoOwners==0,
            "non-requestor consumed guest-to-guest spawn answer");
    require(host.status().memberMask == 15 && host.status().error == NET_ERR_NONE, "spawn lookup retired an admitted guest");
    for (u32 i = 0; i < MAX_JOINS; ++i)
        require(clients[i]->status().phase == NET_CONNECTED && clients[i]->status().connectAttempts == 1,
                "cell-authority spawn lookup caused a reconnect");
    std::printf("PASS author-targeted distributed SPAWN_REQ/INFO, recipient isolation and no reconnect\n");
    // Real encrypted NetLink packets, consumed by the SAME admission gate
    // syncSpeed uses. No mocked sockets or fabricated receive/echo callbacks.
    Bytes resumed, paused; put(resumed, u32(0)); put(paused, u32(1));
    host.rpc(SPEED_EFFECTIVE, resumed);
    for (u32 i = 0; i < MAX_JOINS; ++i) expectSpeed(*clients[i], 0, 1, false);
    for (u32 i = 0; i < 12; ++i) first.rpc(SPEED_CLICK, resumed);
    expectSpeed(host, 12, 0, false);
    // Generate a real heartbeat from first's last RECEIVED host SET before the
    // host's next local command. Leave its reliable packet queued for the host
    // consumer to drain AFTER that command (the real stale-echo race).
    Bytes oldEcho=first.rpc(SPEED_HEARTBEAT); Reader echoed(oldEcho);
    require(echoed.get<u32>() == 0, "stale heartbeat precondition: join did not echo prior running SET"); echoed.done();
    host.rpc(SPEED_EFFECTIVE, paused);
    DWORD speedHold = GetTickCount(); u32 previousHeartbeats = sample(host).speedHeartbeats;
    while (GetTickCount() - speedHold < 8200) {
        for (u32 i = 0; i < MAX_JOINS; ++i) clients[i]->rpc(SPEED_HEARTBEAT);
        Observation heldPause = sample(host);
        require(heldPause.speedPaused && heldPause.speedCommands == 12,
                "periodic heartbeat undid explicit host pause");
        Sleep(3000);
    }
    require(sample(host).speedHeartbeats >= previousHeartbeats + 6, "no real heartbeat traffic during pause hold");
    for (u32 i = 0; i < MAX_JOINS; ++i) expectSpeed(*clients[i], 0, 2, true);
    // Each other peer is still at seq 3/4 while first is already above 12.
    second.rpc(SPEED_CLICK, resumed); expectSpeed(host, 13, 0, false);
    third.rpc(SPEED_CLICK, paused); expectSpeed(host, 14, 0, true);
    std::printf("PASS real speed heartbeat cannot undo host pause; three independent REQ sequences and host SET scope\n");
    host.publish(10000); for (u32 i = 0; i < MAX_JOINS; ++i) expectStates(*clients[i], 0, 10000);
    const u32 firstId = first.status().memberMask & 2 ? 1 : 0;
    require(firstId == 1, "first join roster missing assigned seat");
    first.publish(20000); expectStates(host, firstId, 20000); expectStates(second, firstId, 20000); expectStates(third, firstId, 20000);
    std::printf("PASS 64 independent entity streams and encrypted star relay\n");
    first.events(200, 24); expectEvents(host, firstId, 199, 25); expectEvents(second, firstId, 199, 25); expectEvents(third, firstId, 199, 25);
    first.events(200, 24); first.events(224, 1);
    expectEvents(host, firstId, 199, 26); expectEvents(second, firstId, 199, 26); expectEvents(third, firstId, 199, 26);
    Bytes repeatedWorld; put(repeatedWorld, u32(501)); host.rpc(LOAD_WORLD, repeatedWorld);
    for (u32 i = 0; i < MAX_JOINS; ++i) {
        DWORD begin = GetTickCount();
        while (sample(*clients[i]).lastGo != 501) {
            require(GetTickCount() - begin < NETWORK_MS, "same-world coordinated reload control lost"); Sleep(10);
        }
    }
    // The new event is a same-lane barrier after the repeated EVENT IDs.
    // Fresh native frames must not replay those declared one-shot IDs.
    Bytes captureReset; put(captureReset, u32(0)); first.rpc(RESET_WORLD, captureReset);
    for (u32 i = 0; i < MAX_JOINS; ++i) clients[i]->rpc(READY_WORLD, repeatedWorld);
    first.events(1, 3);
    Child* eventReceivers[] = { &host, &second, &third };
    for (u32 n = 0; n < 3; ++n) {
        DWORD begin = GetTickCount();
        for (;;) {
            Observation observed = sample(*eventReceivers[n]); std::vector<u32> ids;
            for (size_t i = 0; i < observed.events.size(); ++i)
                if (u32(observed.events[i] >> 32) == firstId) ids.push_back(u32(observed.events[i]));
            require(ids.size() <= 29, "fresh-frame EVENT replay crossed one-shot boundary");
            if (ids.size() == 29) {
                for (u32 i = 0; i < 26; ++i) require(ids[i] == 199 + i, "prior EVENT FIFO changed after capture reset");
                for (u32 i = 0; i < 3; ++i) require(ids[26 + i] == 1 + i, "legitimate EVENT counter reset was rejected");
                break;
            }
            require(GetTickCount() - begin < NETWORK_MS, "new capture-epoch EVENT scope did not reach consumer"); Sleep(10);
        }
    }
    std::printf("PASS fresh-frame EVENT replay rejection and legitimate capture-epoch counter reset\n");
    host.rpc(SAVE); host.events(100, 48);
    // First reset its consumer at LOAD 501; the other guests retained events 98/99.
    for (u32 i = 0; i < MAX_JOINS; ++i) expectEvents(*clients[i], 0, i == 0 ? 100 : 98, i == 0 ? 48 : 50);
    for (u32 i = 0; i < MAX_JOINS; ++i) { DWORD begin = GetTickCount(); Observation s;
        do { s = sample(*clients[i]); require(s.saveGood, "bulk bytes/BEGIN/FILE/DONE order corrupted");
            require(GetTickCount() - begin < NETWORK_MS, "bulk reliable data lost under host fanout pressure"); if (s.dones != 1) Sleep(10); } while (s.dones != 1);
        require(s.begins == 1 && s.files == SAVE_CHUNKS && s.dones == 1 && s.bytes == u64(SAVE_CHUNKS) * SAVE_CHUNK_MAX, "bulk transfer incomplete/duplicated");
    }
    std::printf("PASS reliable one-shot FIFO and exact 2MiB bulk per guest under fanout backpressure\n");
    Bytes loadGo; put(loadGo, LOAD_ID); host.rpc(LOAD_WORLD, loadGo);
    for (u32 i = 0; i < MAX_JOINS; ++i) {
        DWORD begin = GetTickCount();
        Observation s;
        do { s = sample(*clients[i]); require(s.goOrdered, "LOAD_GO passed consumer before completed bulk");
            require(GetTickCount() - begin < NETWORK_MS, "ordered LOAD_GO lost"); if (!s.gos) Sleep(10); } while (!s.gos);
    }
    Bytes ready; put(ready, LOAD_ID); first.rpc(READY_WORLD, ready);
    DWORD begin = GetTickCount(); while (sample(host).readyLoad != LOAD_ID) { require(GetTickCount() - begin < NETWORK_MS, "specific-load READY control lost"); Sleep(10); }
    require(second.status().phase == NET_CONNECTED && third.status().phase == NET_CONNECTED, "latejoin GO rotated other guests out of native session");
    std::printf("PASS specific LOAD_GO/READY token consumer (not engine world-live/pause proof)\n");
    first.rpc(STOP); begin = GetTickCount();
    while (host.status().memberMask & 2) { require(GetTickCount() - begin < NETWORK_MS, "departed native guest seat not released"); Sleep(10); }
    Bytes reconnectSimulation; put(reconnectSimulation, u32(750)); first.rpc(SIM, reconnectSimulation);
    host.publish(30000); first.start(false, false, port); waitPhase(first, NET_CONNECTED);
    DWORD rejoinStarted = GetTickCount(); waitHostAdmission(host, first, 0, 703, rejoinStarted);
    Bytes rejoinGo; put(rejoinGo, u32(703)); host.rpc(LOAD_WORLD, rejoinGo);
    for (u32 i = 0; i < MAX_JOINS; ++i) {
        waitBootstrapGo(host, *clients[i], i, 703, i == 0 ? rejoinStarted : GetTickCount());
        clients[i]->rpc(READY_WORLD, rejoinGo);
    }
    expectStates(first, 0, 30000);
    require(host.status().memberMask == 15, "native guest reconnect roster incomplete");
    first.publish(40000); expectStates(host, 1, 40000); expectStates(second, 1, 40000);
    std::printf("PASS fresh native reconnect and reused gameplay seat\n");
    DWORD delayFixtureStart = GetTickCount(); host.publish(50000); expectStates(second, 0, 50000);
    Observation held = sample(first);
    for (unsigned i = 0; i < ENTITY_COUNT; ++i) {
        StateKey key = { 0, 1000 + i };
        std::map<StateKey, float>::const_iterator found = held.states.find(key);
        if (found == held.states.end() || found->second != 30000 + float(i)) {
            char detail[192]; _snprintf(detail, sizeof(detail) - 1,
                "old-world delay precondition: entity=%u value=%.1f elapsed=%lu states=%u",
                i, found == held.states.end() ? -1.0 : double(found->second),
                GetTickCount() - delayFixtureStart, unsigned(held.states.size()));
            detail[sizeof(detail) - 1] = 0; throw std::runtime_error(detail);
        }
    }
    Bytes hostReload; put(hostReload, u32(1)); host.rpc(RESET_WORLD, hostReload);
    Bytes nextWorld; put(nextWorld, u32(704)); host.rpc(LOAD_WORLD, nextWorld);
    for (u32 i = 0; i < MAX_JOINS; ++i) {
        DWORD start = GetTickCount();
        while (sample(*clients[i]).lastGo != 704) {
            require(GetTickCount() - start < NETWORK_MS, "coordinated new-world GO lost"); Sleep(10);
        }
        if (i == 0) { clients[i]->events(17, 1); Sleep(100); }
        clients[i]->rpc(RESET_WORLD, captureReset); clients[i]->rpc(READY_WORLD, nextWorld);
    }
    Sleep(900);
    require(sample(first).states.empty(), "delayed old-world entities resurrected after coordinated token rotation");
    host.publish(60000);
    for (u32 i = 0; i < MAX_JOINS; ++i) expectStates(*clients[i], 0, 60000);
    host.events(1, 3);
    for (u32 i = 0; i < MAX_JOINS; ++i) expectEvents(*clients[i], 0, 1, 3);
    first.events(1, 3); expectEvents(host, 1, 1, 3); expectEvents(second, 1, 1, 3); expectEvents(third, 1, 1, 3);
    std::printf("PASS coordinated host-world rotation, delayed ghost rejection and new-world EVENT counter reset\n");
    host.beginRpc(SAVE);
    begin = GetTickCount();
    while (sample(second).files == 0) {
        require(GetTickCount() - begin < NETWORK_MS, "second native bulk never started before departure"); Sleep(10);
    }
    first.rpc(STOP); host.finishRpc();
    begin = GetTickCount();
    while (host.status().memberMask & 2) {
        require(host.status().phase != NET_FAILED && GetTickCount() - begin < NETWORK_MS, "one departing bulk recipient failed host session"); Sleep(10);
    }
    host.events(4, 3); expectEvents(second, 0, 1, 6); expectEvents(third, 0, 1, 6);
    for (u32 i = 1; i < MAX_JOINS; ++i) {
        DWORD start = GetTickCount(); Observation remaining;
        do {
            remaining = sample(*clients[i]); require(remaining.saveGood, "healthy peer bulk corrupted by another guest departure");
            require(GetTickCount() - start < NETWORK_MS, "healthy peer bulk stalled after another guest departure");
            if (remaining.dones != 1) Sleep(10);
        } while (remaining.dones != 1);
        require(remaining.bytes == u64(SAVE_CHUNKS) * SAVE_CHUNK_MAX && remaining.files == SAVE_CHUNKS,
                "healthy peer bulk lost/duplicated on per-recipient departure");
    }
    require(host.status().phase == NET_LISTENING && second.status().phase == NET_CONNECTED && third.status().phase == NET_CONNECTED,
            "native remaining-peer lifecycle failed after departure during save");
    std::printf("PASS departure during bulk preserves healthy guests' exact data and reliable gameplay\n");
    first.start(false, false, port); waitPhase(first, NET_CONNECTED);
    DWORD rejectionStarted = GetTickCount(); waitHostAdmission(host, first, 0, 705, rejectionStarted);
    Bytes rejectionWorld; put(rejectionWorld, u32(705)); host.rpc(LOAD_WORLD, rejectionWorld);
    for (u32 i = 0; i < MAX_JOINS; ++i) {
        waitBootstrapGo(host, *clients[i], i, 705, i == 0 ? rejectionStarted : GetTickCount());
        clients[i]->rpc(READY_WORLD, rejectionWorld);
    }
    first.rpc(INVALID_EVENT); begin = GetTickCount();
    for (;;) {
        NetStatus rejected = host.status();
        require(rejected.phase != NET_FAILED && GetTickCount() - begin < NETWORK_MS,
                "malformed connected guest was not retired without failing host");
        if (rejected.error == NET_ERR_PROTOCOL && !(rejected.memberMask & 2)) break;
        Sleep(10);
    }
    first.rpc(STOP); host.events(7, 3);
    expectEvents(second, 0, 1, 9); expectEvents(third, 0, 1, 9);
    require(second.status().phase == NET_CONNECTED && third.status().phase == NET_CONNECTED,
            "rejecting one malformed connected guest disconnected healthy guests");
    std::printf("PASS malformed-event rejection frees seat and preserves host/healthy guests\n");
    Child awaiting(exe, runtime.path, job);
    awaiting.start(false, false, reservePort());
    waitPhase(awaiting, NET_CONNECTING);
    DWORD attemptStarted = GetTickCount(); bool observedConnecting = false;
    do {
        NetStatus pending = awaiting.status();
        require(pending.phase != NET_FAILED && pending.connectAttempts == 1,
                "live native connection attempt was restarted before its configured initial deadline");
        observedConnecting = observedConnecting || pending.phase == NET_CONNECTING;
        Sleep(50);
    } while (GetTickCount() - attemptStarted < 2300);
    require(observedConnecting, "real pending native connection attempt was not observed");
    awaiting.finish();
    std::printf("PASS real native connection attempt remains live beyond the old two-second restart boundary\n");
    first.finish(); second.finish(); third.finish(); host.finish();
    Child unavailable(exe, runtime.path, job); unavailable.start(true, true, port);
    begin = GetTickCount(); while (unavailable.status().phase != NET_FAILED) { require(GetTickCount() - begin < RPC_MS, "unavailable Steam did not fail explicitly"); Sleep(10); }
    require(unavailable.status().active == NET_TRANSPORT_NONE, "unavailable Steam silently fell back to UDP"); unavailable.finish();
    std::printf("PASS unavailable real Steam explicit failure, no UDP fallback\n");
    std::printf("Core native consumer PASS; real two-account Steam and multiplayer engine gameplay NOT certified.\n"); return 0;
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 2 && std::wcscmp(argv[1], L"--actor") == 0) return actor();
        if (argc == 6 && std::wcscmp(argv[1], L"--controller") == 0 && std::wcscmp(argv[2], L"--udp") == 0 && std::wcscmp(argv[4], L"--steam") == 0)
            return controller(argv[3], argv[5]);
        std::fprintf(stderr, "usage: core_native_tests --controller --udp <exact DLL> --steam <exact DLL>\n"); return 2;
    } catch (const std::exception& error) { std::fprintf(stderr, "Core native consumer FAIL: %s\n", error.what()); return 1; }
}
