// Real DLL/real-process prerequisite tests. No game or Steam gameplay certification.
// Build: C++17, x64, link ws2_32 + advapi32 + crypt32. Do NOT link GNS or either provider.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <sddl.h>
#include <wincrypt.h>
#include "../TransportApi.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;
constexpr uint32_t kFrameLimit = 8u * 1024u * 1024u;
constexpr uint32_t kRpcTimeoutMs = 15000;
constexpr uint32_t kNetworkTimeoutMs = 45000;
constexpr uint64_t kHostGeneration = 701;
void require(bool condition, const char* failure);
void put32(Bytes& bytes, uint32_t value);
void put64(Bytes& bytes, uint64_t value);
void putString(Bytes& bytes, const std::string& value);
struct Reader {
    const Bytes& bytes;
    size_t at = 0;
    uint32_t u32();
    uint64_t u64();
    std::string string();
    Bytes rest();
    void done();
};
enum class Op : uint32_t { Prepare, Invite, Start, Poll, Send, Burst, Stats,
    Status, Disconnect, Stop, Modules, Exit, MalformedStart, Pump, HoldEvent, ReleaseHeld, InvalidSend,
    SetPolling, DelayedSend };
struct Reply { int32_t result = COOP_NET_INTERNAL; Bytes bytes; };
struct Event {
    uint32_t type = 0, connection = 0, traffic = 0;
    uint64_t generation = 0;
    int32_t reason = 0;
    std::string detail;
    Bytes data;
};
struct StartOptions {
    uint32_t mode = COOP_NET_UDP, port = 0;
    uint64_t generation = 0;
    std::string address = "127.0.0.1", invite;
};
struct Child {
    HANDLE process = nullptr, input = nullptr, output = nullptr;
    std::string label;
    bool exited = false;
    Child(const std::wstring& executable, const std::wstring& provider,
          const std::filesystem::path& directory, bool host, HANDLE job,
          const char* name);
    ~Child();
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Reply rpc(Op op, const Bytes& payload = {});
    int32_t start(const StartOptions& options);
    Event poll();
    int32_t send(uint32_t connection, uint32_t traffic, const Bytes& payload);
    std::vector<int32_t> burst(uint32_t connection, uint32_t traffic,
                               const std::vector<Bytes>& messages);
    CoopNetStatus status();
    CoopNetPeerStats stats(uint32_t connection);
    void stop();
    void finish();
    void proveModules(bool udp);
};
struct PrivateDirectory {
    std::filesystem::path path;
    PrivateDirectory();
    ~PrivateDirectory();
};
struct Envelope { Bytes root, leaf; };
Envelope decodeProfile(const Bytes& ciphertext);
Envelope decodeInvitation(const std::string& text);
std::string hostRoleInvitation(const Envelope& host);
struct WireAudit {
    std::vector<Bytes> privateKeys;
    std::atomic<uint64_t> clientCertificates{0}, hostCertificates{0};
    std::atomic<uint64_t> leakedFields{0}, leakedKeys{0}, malformed{0};
    void addKeys(const Envelope& envelope, bool privateRoot);
    void observe(const uint8_t* bytes, size_t size) noexcept;
    ~WireAudit();
};
struct Proxy {
    struct Datagram { Bytes bytes; sockaddr_in target{}; Clock::time_point due; };
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in host{}, client{};
    uint16_t port = 0;
    WireAudit& audit;
    std::atomic<bool> stopping{false};
    std::atomic<uint64_t> packets{0}, forwarded{0}, dropped{0}, duplicated{0}, reordered{0};
    std::thread worker;
    Proxy(uint16_t hostPort, WireAudit& wireAudit);
    ~Proxy();
    void run();
};
uint16_t reservePort();
std::string issueInvite(Child& child);
uint32_t waitConnected(Child& child, uint64_t generation);
void waitPeerCount(Child& host, uint32_t count);
void waitSendDrained(Child& sender, uint32_t connection);
uint64_t privateUsage(const Child& child);
void drainStateProbes(Child& receiver, uint64_t generation, const Bytes& fixture);
void rejectJoin(Child& child, const StartOptions& options);
Bytes record(uint32_t tag, uint32_t sequence, size_t size);
uint32_t expectMessages(Child& receiver, uint64_t generation, uint32_t traffic,
                    const std::vector<Bytes>& expected, uint32_t timeoutMs = kNetworkTimeoutMs);
void quietMessages(Child& receiver, uint64_t generation, uint32_t milliseconds);
void report(const char* name, uint64_t count);
Bytes readPrivateFile(const std::filesystem::path& path);
void writePrivateFile(const std::filesystem::path& path, const Bytes& data);
void observeSteamProcesses();
int childMain(bool host, const std::wstring& provider, const std::filesystem::path& directory);
int controller(const std::wstring& executable, const std::wstring& udp, const std::wstring& steam);

// Expectations are authored here, not queried from provider internals. Mutations
// caught: wrong bytes/lane, silent reliable loss, accepted hostile credentials,
// stale epochs, missing admission bound, lane starvation and Steam fallback.
int controller(const std::wstring& executable, const std::wstring& udp, const std::wstring& steam) {
    PrivateDirectory directory;
    observeSteamProcesses();
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    require(job != nullptr, "create child cleanup job");
    struct CloseJob { HANDLE handle; ~CloseJob() { CloseHandle(handle); } } closeJob{job};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE,
            "set child cleanup job limits");
    Child host(executable, udp, directory.path / L"host", true, job, "host");
    Child alien(executable, udp, directory.path / L"alien", true, job, "wrong-host");
    require(host.rpc(Op::Prepare).result == COOP_NET_OK, "provision private host credential");
    const auto hostProfile = directory.path / L"host" / L"credential.bin";
    const Bytes originalProfile = readPrivateFile(hostProfile);
    require(!originalProfile.empty(), "provisioned host credential file is empty");
    const Envelope decodedHost = decodeProfile(originalProfile);
    WireAudit audit;
    audit.addKeys(decodedHost, true);
    const std::string wrongRoleInvite = hostRoleInvitation(decodedHost);
    require(host.rpc(Op::Prepare).result == COOP_NET_OK, "existing valid profile cannot be reused");
    require(readPrivateFile(hostProfile) == originalProfile, "prepareHost replaces valid persisted host credential");
    Child damaged(executable, udp, directory.path / L"damaged", true, job, "damaged-profile");
    const auto damagedProfile = directory.path / L"damaged" / L"credential.bin";
    const Bytes garbageProfile{0x44, 0x45, 0x41, 0x44, 0x00, 0xff, 0x19};
    writePrivateFile(damagedProfile, garbageProfile);
    require(damaged.rpc(Op::Prepare).result != COOP_NET_OK, "prepareHost silently replaces invalid existing profile");
    require(readPrivateFile(damagedProfile) == garbageProfile, "invalid existing profile was mutated");
    damaged.finish();
    report("credential-idempotence-and-invalid-profile-preservation", 2);
    require(alien.rpc(Op::Prepare).result == COOP_NET_OK, "provision independent wrong-host credential");
    const std::string wrongHostInvite = issueInvite(alien);
    audit.addKeys(decodeProfile(readPrivateFile(directory.path / L"alien" / L"credential.bin")), true);
    audit.addKeys(decodeInvitation(wrongHostInvite), false);
    alien.proveModules(true);
    alien.finish();
    const uint16_t hostPort = reservePort();
    StartOptions hosting;
    hosting.port = hostPort;
    hosting.generation = kHostGeneration;
    // Wrong selected mode must fail before any Steam initialization or network creation.
    StartOptions wrongMode = hosting;
    wrongMode.mode = COOP_NET_STEAM;
    require(host.start(wrongMode) == COOP_NET_INVALID, "UDP provider accepts Steam mode");
    host.proveModules(true);
    require(host.rpc(Op::MalformedStart).result == COOP_NET_OK, "malformed start creates network or succeeds");
    require(host.rpc(Op::Pump).result == COOP_NET_INVALID, "UDP main-thread Steam pump is not explicitly unsupported");
    require(host.start(hosting) == COOP_NET_OK, "create actual UDP listener");
    require(host.status().listening == 1, "host is not listening after network creation");
    StartOptions concurrent = hosting;
    concurrent.generation += 900;
    require(host.start(concurrent) == COOP_NET_INVALID, "active provider accepts concurrent start");
    require(host.status().generation == kHostGeneration && host.status().listening == 1,
            "rejected concurrent start mutates running network");
    require(readPrivateFile(hostProfile) == originalProfile, "network start mutates persisted credential");
    report("malformed-config-and-concurrent-start-rejection", 6);
    report("network-created", 1);

    std::vector<std::unique_ptr<Child>> clients;
    std::vector<std::unique_ptr<Proxy>> proxies;
    std::vector<std::string> invites;
    for (unsigned i = 0; i < 4; ++i) {
        invites.push_back(issueInvite(host));
        for (unsigned earlier = 0; earlier < i; ++earlier)
            require(invites[i] != invites[earlier], "host reissued identical per-client invitation");
        audit.addKeys(decodeInvitation(invites.back()), false);
    }
    // Secret-key vector is immutable before starting capture threads.
    for (unsigned i = 0; i < 4; ++i) {
        proxies.emplace_back(std::make_unique<Proxy>(hostPort, audit));
        clients.emplace_back(std::make_unique<Child>(executable, udp,
            directory.path / (L"client" + std::to_wstring(i)), false, job, "client"));
    }
    StartOptions joining;
    joining.port = proxies[0]->port;
    joining.generation = 101;
    joining.invite = "not-an-invitation";
    rejectJoin(*clients[0], joining);
    joining.generation = 102;
    joining.invite = wrongHostInvite;
    rejectJoin(*clients[0], joining);
    joining.generation = 103;
    joining.invite = invites[0];
    // Change a byte without changing credential length. Both replacement
    // characters are valid in hex/base64, so this is not just a size test.
    const size_t corruptAt = joining.invite.size() * 3 / 4;
    joining.invite[corruptAt] = joining.invite[corruptAt] == '0' ? '1' : '0';
    rejectJoin(*clients[0], joining);
    joining.generation = 104;
    joining.invite = wrongRoleInvite;
    rejectJoin(*clients[0], joining);
    waitPeerCount(host, 0);
    report("invalid-wrong-host-corrupted-wrong-role-invites-rejected", 4);

    std::array<uint32_t, 3> connections{};
    std::array<uint64_t, 3> generations{{201, 301, 401}};
    for (unsigned i = 0; i < 3; ++i) {
        joining.port = proxies[i]->port;
        joining.generation = generations[i];
        joining.invite = invites[i];
        require(clients[i]->start(joining) == COOP_NET_OK, "start invited UDP client");
        connections[i] = waitConnected(*clients[i], generations[i]);
        waitPeerCount(host, i + 1);
    }
    // Child four has a genuinely valid, unique host-issued certificate, not a bad invite.
    joining.port = proxies[3]->port;
    joining.generation = 501;
    joining.invite = invites[3];
    rejectJoin(*clients[3], joining);
    waitPeerCount(host, 3);
    report("three-admitted-fourth-rejected", 4);
    Bytes invalidSendConnection;
    put32(invalidSendConnection, connections[0]);
    require(clients[0]->rpc(Op::InvalidSend, invalidSendConnection).result == COOP_NET_OK,
            "invalid sends are not rejected safely");
    quietMessages(host, kHostGeneration, 200);
    report("invalid-send-preserves-connected-stream", 4);

    // Literal fixture includes zero bytes, non-ASCII bytes and CR/LF.
    const Bytes exact{0x00, 0xff, 0x80, 0x01, 0x0d, 0x0a, 0x4b, 0x43, 0x00};
    std::array<uint32_t, 3> hostConnections{};
    for (unsigned i = 0; i < 3; ++i) {
        require(clients[i]->send(connections[i], GAME_RELIABLE, exact) == COOP_NET_OK,
                "send exact reliable fixture");
        hostConnections[i] = expectMessages(host, kHostGeneration, GAME_RELIABLE, {exact});
        const Bytes reverse{0x19, 0x00, 0x7f, 0xff, static_cast<uint8_t>(i)};
        require(host.send(hostConnections[i], GAME_RELIABLE, reverse) == COOP_NET_OK,
                "host reverse-direction send fails");
        expectMessages(*clients[i], generations[i], GAME_RELIABLE, {reverse});
    }
    report("independent-client-exact-payloads", 3);
    const Bytes oversizedState(COOP_NET_MAX_STATE_MESSAGE + 1, uint8_t(0x5e));
    require(clients[0]->send(connections[0], GAME_STATE, oversizedState) == COOP_NET_INVALID,
            "oversized GAME_STATE silently becomes native reliable traffic");

    std::vector<Bytes> reliable, bulk;
    for (uint32_t i = 0; i < 24; ++i) reliable.push_back(record(0x31415926, i, 73 + i));
    for (const auto& bytes : reliable)
        require(clients[0]->send(connections[0], GAME_RELIABLE, bytes) == COOP_NET_OK,
                "queue reliable order fixtures");
    expectMessages(host, kHostGeneration, GAME_RELIABLE, reliable);
    // Deliberately non-power-of-two and beyond the old 256 KiB save ceiling.
    for (uint32_t i = 0; i < 4; ++i) bulk.push_back(record(0x27182818, i, 262151 + i * 7));
    for (const auto& bytes : bulk) {
        require(clients[0]->send(connections[0], SAVE_BULK, bytes) == COOP_NET_OK,
                "queue large save fixture");
        expectMessages(host, kHostGeneration, SAVE_BULK, {bytes});
    }
    report("reliable-order", reliable.size());
    report("large-save-exact-byte-order", bulk.size());

    // All sends happen in one child command without polling between calls. A
    // bounded budget must report BACKPRESSURE, never OK then lose reliable data.
    std::vector<Bytes> bulkFlood;
    for (uint32_t i = 0; i < 16; ++i) bulkFlood.push_back(record(0xb001b001, i, 262151));
    const auto bulkResults = clients[0]->burst(connections[0], SAVE_BULK, bulkFlood);
    std::vector<Bytes> acceptedBulk;
    unsigned bulkPressure = 0;
    for (size_t i = 0; i < bulkResults.size(); ++i) {
        if (bulkResults[i] == COOP_NET_OK) acceptedBulk.push_back(bulkFlood[i]);
        else { require(bulkResults[i] == COOP_NET_BACKPRESSURE, "bulk flood returns unexpected error"); ++bulkPressure; }
    }
    require(!acceptedBulk.empty() && bulkPressure != 0, "bulk budget does not expose BACKPRESSURE");
    require(clients[0]->stats(connections[0]).pendingBulkBytes != 0, "bulk not queued for class probes");
    const Bytes reliableProbe{0x52, 0x00, 0xff, 0x19};
    const Bytes stateProbe{0x53, 0x00, 0xfe, 0x29};
    require(clients[0]->send(connections[0], GAME_RELIABLE, reliableProbe) == COOP_NET_OK,
            "bulk starves reliable enqueue");
    for (unsigned i = 0; i < 8; ++i)
        require(clients[0]->send(connections[0], GAME_STATE, stateProbe) == COOP_NET_OK,
                "bulk starves state enqueue");
    bool sawReliable = false, sawState = false;
    size_t bulkReceived = 0;
    bool reliableWhileBulk = false, stateWhileBulk = false;
    auto deadline = Clock::now() + std::chrono::milliseconds(kNetworkTimeoutMs);
    while (Clock::now() < deadline && (!sawReliable || !sawState || bulkReceived < acceptedBulk.size())) {
        const Event event = host.poll();
        if (event.type == 0) { Sleep(1); continue; }
        require(event.generation == kHostGeneration, "bulk class probe stale host generation");
        if (event.type != COOP_NET_EVENT_MESSAGE) {
            require(event.type != COOP_NET_EVENT_ERROR && event.type != COOP_NET_EVENT_DISCONNECTED,
                    "network fails during bulk class probes");
            continue;
        }
        if (event.traffic == SAVE_BULK) {
            require(bulkReceived < acceptedBulk.size(), "bulk duplicated accepted record");
            require(event.data == acceptedBulk[bulkReceived++], "bulk accepted record corrupted or reordered");
        } else if (event.traffic == GAME_RELIABLE) {
            require(!sawReliable && event.data == reliableProbe, "reliable probe duplicated or corrupted");
            sawReliable = true;
            reliableWhileBulk = bulkReceived < acceptedBulk.size();
        } else {
            require(event.traffic == GAME_STATE && event.data == stateProbe, "state probe corrupted or mislabeled");
            sawState = true;
            stateWhileBulk = stateWhileBulk || bulkReceived < acceptedBulk.size();
        }
    }
    require(sawReliable && sawState && bulkReceived == acceptedBulk.size(), "bulk/class probes timed out or lost accepted data");
    require(reliableWhileBulk && stateWhileBulk, "game classes delivered only after bulk drained");
    drainStateProbes(host, kHostGeneration, stateProbe);
    // Backpressured bytes were not consumed. Retry the first rejected original
    // record after drain; it must arrive exactly once, byte-for-byte.
    const auto rejectedBulk = std::find(bulkResults.begin(), bulkResults.end(), COOP_NET_BACKPRESSURE);
    const Bytes& retry = bulkFlood[static_cast<size_t>(rejectedBulk - bulkResults.begin())];
    require(clients[0]->send(connections[0], SAVE_BULK, retry) == COOP_NET_OK, "bulk retry after drain fails");
    expectMessages(host, kHostGeneration, SAVE_BULK, {retry});
    report("bulk-backpressure", bulkPressure);
    report("per-class-probes-before-bulk-completion", 2);

    std::vector<Bytes> reliableFlood;
    for (uint32_t i = 0; i < 40; ++i) reliableFlood.push_back(record(0xface1234, i, 65537));
    const auto reliableResults = clients[0]->burst(connections[0], GAME_RELIABLE, reliableFlood);
    std::vector<Bytes> acceptedReliable;
    unsigned reliablePressure = 0;
    for (size_t i = 0; i < reliableResults.size(); ++i) {
        if (reliableResults[i] == COOP_NET_OK) acceptedReliable.push_back(reliableFlood[i]);
        else { require(reliableResults[i] == COOP_NET_BACKPRESSURE, "reliable flood returns unexpected error"); ++reliablePressure; }
    }
    require(!acceptedReliable.empty() && reliablePressure != 0, "reliable buffer does not expose BACKPRESSURE");
    expectMessages(host, kHostGeneration, GAME_RELIABLE, acceptedReliable);
    const auto rejectedReliable = std::find(reliableResults.begin(), reliableResults.end(), COOP_NET_BACKPRESSURE);
    const Bytes& reliableRetry = reliableFlood[static_cast<size_t>(rejectedReliable - reliableResults.begin())];
    require(clients[0]->send(connections[0], GAME_RELIABLE, reliableRetry) == COOP_NET_OK,
            "reliable retry after drain fails");
    expectMessages(host, kHostGeneration, GAME_RELIABLE, {reliableRetry});
    quietMessages(host, kHostGeneration, 750);
    report("reliable-backpressure-and-no-silent-loss", reliablePressure);
    Bytes disconnectConnection;
    put32(disconnectConnection, hostConnections[2]);
    require(host.rpc(Op::Disconnect, disconnectConnection).result == COOP_NET_OK, "explicit peer disconnect fails");
    bool observedDisconnect = false;
    const auto disconnectDeadline = Clock::now() + std::chrono::milliseconds(kNetworkTimeoutMs);
    while (Clock::now() < disconnectDeadline) {
        const Event event = clients[2]->poll();
        require(!event.type || event.generation == generations[2], "disconnect event has stale generation");
        require(event.type != COOP_NET_EVENT_MESSAGE, "unexpected payload during explicit disconnect");
        if (event.type == COOP_NET_EVENT_DISCONNECTED) {
            require(event.reason == 1000, "disconnect does not preserve caller reason");
            observedDisconnect = true;
            break;
        }
        Sleep(2);
    }
    require(observedDisconnect, "explicit disconnect not visible to remote consumer");
    waitPeerCount(host, 2);
    clients[2]->stop();
    joining.port = proxies[2]->port; joining.invite = invites[2]; joining.generation = 402;
    require(clients[2]->start(joining) == COOP_NET_OK, "restart explicitly disconnected client");
    connections[2] = waitConnected(*clients[2], 402);
    waitPeerCount(host, 3);
    report("explicit-remote-disconnect-and-rejoin", 1);

    // Same process/context and same imported identity, new consumer generation.
    const uint32_t staleConnection = connections[0];
    clients[0]->stop();
    require(clients[0]->status().connections == 0, "client stop leaves connections active");
    require(clients[0]->send(staleConnection, GAME_RELIABLE, exact) != COOP_NET_OK,
            "stopped connection still sends");
    waitPeerCount(host, 2);
    joining.port = proxies[0]->port;
    joining.invite = invites[0];
    joining.generation = 202;
    require(clients[0]->start(joining) == COOP_NET_OK, "reconnect same context/profile");
    connections[0] = waitConnected(*clients[0], 202);
    waitPeerCount(host, 3);
    require(clients[0]->send(connections[0], GAME_RELIABLE, exact) == COOP_NET_OK, "reconnected reliable send");
    expectMessages(host, kHostGeneration, GAME_RELIABLE, {exact});
    report("fresh-and-reconnected-session-generations", 2);
    require(host.rpc(Op::HoldEvent).result == COOP_NET_OK, "arm provider-owned held message");
    const Bytes heldFixture{0x68, 0x00, 0xff, 0x65, 0x6c, 0x64};
    require(clients[0]->send(connections[0], GAME_RELIABLE, heldFixture) == COOP_NET_OK, "send held-event fixture");
    const uint32_t healthyHostConnection = expectMessages(host, kHostGeneration, GAME_RELIABLE, {heldFixture});
    // Native networking remains live while the consumer intentionally does
    // not poll: a terminal peer must not close a different healthy target.
    Bytes pause; put32(pause, 0);
    require(host.rpc(Op::SetPolling, pause).result == COOP_NET_OK, "pause only host consumer polling");
    Bytes terminal; put32(terminal, connections[2]);
    require(clients[2]->rpc(Op::Disconnect, terminal).result == COOP_NET_OK, "close independent client before host polls");
    const Bytes survivor{0x73, 0x75, 0x72, 0x00, 0xff};
    Bytes delayed;
    put32(delayed, healthyHostConnection); put32(delayed, GAME_RELIABLE); put32(delayed, 1500);
    delayed.insert(delayed.end(), survivor.begin(), survivor.end());
    const int32_t survivingSend = host.rpc(Op::DelayedSend, delayed).result;
    Bytes resume; put32(resume, 1);
    require(host.rpc(Op::SetPolling, resume).result == COOP_NET_OK, "resume host consumer polling");
    require(survivingSend == COOP_NET_OK, "terminal unrelated peer makes healthy target report CLOSED");
    expectMessages(*clients[0], 202, GAME_RELIABLE, {survivor});
    report("unpolled-terminal-peer-preserves-healthy-send", 1);
    const Bytes maximumState(COOP_NET_MAX_STATE_MESSAGE, uint8_t(0x4b));
    require(clients[0]->send(connections[0], GAME_STATE, maximumState) == COOP_NET_OK,
            "maximum genuinely unreliable GAME_STATE is rejected");
    report("unreliable-state-size-boundaries", 2);

    for (auto& client : clients) { client->stop(); client->proveModules(true); client->finish(); }
    waitPeerCount(host, 0);
    host.stop();
    require(host.status().listening == 0 && host.status().connections == 0,
            "host stop leaves listener or peers active");
    hosting.generation = kHostGeneration + 1;
    require(host.start(hosting) == COOP_NET_OK, "restart host on released UDP port");
    require(host.status().generation == kHostGeneration + 1 && host.status().listening == 1,
            "host restart has stale generation or no listener");
    require(host.rpc(Op::ReleaseHeld).result == COOP_NET_OK, "held provider message invalid after stop/start");
    quietMessages(host, kHostGeneration + 1, 200);
    report("provider-owned-message-release-after-stop-restart", 1);
    host.stop();
    host.proveModules(true);
    host.finish();
    report("network-stop-restart-destroy", 3);

    uint64_t packets = 0, drops = 0, duplicates = 0, reorders = 0;
    for (const auto& proxy : proxies) {
        packets += proxy->packets.load(); drops += proxy->dropped.load();
        duplicates += proxy->duplicated.load(); reorders += proxy->reordered.load();
    }
    require(packets > 0 && drops > 0 && duplicates > 0 && reorders > 0,
            "real UDP impairment was not actually exercised");
    report("actual-loopback-UDP-packets", packets);
    report("proxy-dropped-datagrams", drops);
    report("proxy-duplicated-datagrams", duplicates);
    report("proxy-reordered-datagrams", reorders);
    proxies.clear(); // Join capture threads before final security accounting.
    require(audit.clientCertificates.load() > 0 && audit.hostCertificates.load() > 0,
            "actual client and host handshake certificates were not captured");
    require(audit.malformed.load() == 0, "bounded native wire certificate decoding failed");
    require(audit.leakedFields.load() == 0 && audit.leakedKeys.load() == 0,
            "actual UDP datagram leaked SignedCert.private_key_data or native private key bytes");
    report("actual-client-handshake-certificates-with-no-private-key-field", audit.clientCertificates.load());
    report("actual-host-handshake-certificates-with-no-private-key-field", audit.hostCertificates.load());
    report("native-private-key-non-disclosure-on-actual-UDP-wire", audit.privateKeys.size());
    // Direct native UDP children isolate ownership/lifecycle from the wire
    // impairment fixture. The same DLL/context/process is reused throughout.
    {
        Child lifecycleHost(executable, udp, directory.path / L"lifecycle-host", true, job, "lifecycle-host");
        Child lifecycleClient(executable, udp, directory.path / L"lifecycle-client", false, job, "lifecycle-client");
        require(lifecycleHost.rpc(Op::Prepare).result == COOP_NET_OK, "provision lifecycle host");
        StartOptions hostCycle, clientCycle;
        hostCycle.port = reservePort(); hostCycle.generation = 10000;
        clientCycle.port = hostCycle.port; clientCycle.generation = 20000;
        clientCycle.invite = issueInvite(lifecycleHost);
        require(lifecycleHost.start(hostCycle) == COOP_NET_OK, "start lifecycle host");
        require(lifecycleClient.start(clientCycle) == COOP_NET_OK, "start lifecycle client");
        uint32_t clientConnection = waitConnected(lifecycleClient, clientCycle.generation);
        waitPeerCount(lifecycleHost, 1);
        uint32_t hostConnection = 0;
        for (unsigned boundary = 0; boundary < 2; ++boundary) {
            // Small messages hit the 64-loan ceiling; eight maximum messages
            // reach 4 MiB without conflating it with the count ceiling.
            const uint32_t count = boundary == 0 ? 64 : 8;
            Bytes arm; put32(arm, count);
            require(lifecycleHost.rpc(Op::HoldEvent, arm).result == COOP_NET_OK, "arm native receive-loan boundary");
            for (uint32_t i = 0; i < count; ++i) {
                const Bytes fixture = record(0x10a00000u + boundary, i, boundary == 0 ? 31 : COOP_NET_MAX_MESSAGE);
                require(lifecycleClient.send(clientConnection, GAME_RELIABLE, fixture) == COOP_NET_OK,
                        "send native receive-loan boundary fixture");
                hostConnection = expectMessages(lifecycleHost, hostCycle.generation, GAME_RELIABLE, {fixture}, 10000);
                waitSendDrained(lifecycleClient, clientConnection);
            }
            const Bytes pending{0x70, 0x00, 0xff, static_cast<uint8_t>(boundary)};
            require(lifecycleClient.send(clientConnection, GAME_RELIABLE, pending) == COOP_NET_OK,
                    "send reliable message beyond receive-loan bound");
            // Native acknowledgement establishes arrival independently of
            // consumer polling; the pending message must not be lost or lent.
            waitSendDrained(lifecycleClient, clientConnection);
            quietMessages(lifecycleHost, hostCycle.generation, 200);
            Bytes one; put32(one, 1);
            require(lifecycleHost.rpc(Op::ReleaseHeld, one).result == COOP_NET_OK, "release one boundary loan");
            expectMessages(lifecycleHost, hostCycle.generation, GAME_RELIABLE, {pending}, 10000);
            lifecycleClient.stop(); lifecycleHost.stop();
            ++hostCycle.generation; ++clientCycle.generation;
            require(lifecycleHost.start(hostCycle) == COOP_NET_OK, "restart host with outstanding boundary loans");
            require(lifecycleClient.start(clientCycle) == COOP_NET_OK, "restart client after boundary teardown");
            clientConnection = waitConnected(lifecycleClient, clientCycle.generation);
            waitPeerCount(lifecycleHost, 1);
            require(lifecycleHost.rpc(Op::ReleaseHeld).result == COOP_NET_OK,
                    "boundary-loan data or ownership invalid after native restart");
            const Bytes fresh{0x66, 0x00, 0x80, static_cast<uint8_t>(boundary)};
            require(lifecycleClient.send(clientConnection, GAME_RELIABLE, fresh) == COOP_NET_OK,
                    "boundary restart reliable send fails");
            hostConnection = expectMessages(lifecycleHost, hostCycle.generation, GAME_RELIABLE, {fresh}, 10000);
            require(lifecycleHost.send(hostConnection, GAME_RELIABLE, fresh) == COOP_NET_OK,
                    "boundary restart reverse reliable send fails");
            expectMessages(lifecycleClient, clientCycle.generation, GAME_RELIABLE, {fresh}, 10000);
            quietMessages(lifecycleHost, hostCycle.generation, 50);
            quietMessages(lifecycleClient, clientCycle.generation, 50);
            report(boundary == 0 ? "native-64-loan-bound-release-resumes-exact-delivery-and-restart" :
                                  "native-4MiB-loan-bound-release-resumes-exact-delivery-and-restart", count);
        }
        lifecycleClient.stop(); lifecycleHost.stop();
        uint64_t hostBaseline = 0, clientBaseline = 0, hostPeak = 0, clientPeak = 0;
        constexpr unsigned cycles = 16, warmupCycles = 4;
        constexpr uint64_t cacheTolerance = 16ull * 1024ull * 1024ull;
        for (unsigned cycle = 0; cycle < cycles; ++cycle) {
            const uint32_t oldHostConnection = hostConnection, oldClientConnection = clientConnection;
            hostCycle.generation = 11000 + cycle; clientCycle.generation = 21000 + cycle;
            require(lifecycleHost.start(hostCycle) == COOP_NET_OK, "repeated native host restart fails");
            require(lifecycleClient.start(clientCycle) == COOP_NET_OK, "repeated native client restart fails");
            clientConnection = waitConnected(lifecycleClient, clientCycle.generation);
            waitPeerCount(lifecycleHost, 1);
            if (cycle) {
                require(lifecycleHost.rpc(Op::ReleaseHeld).result == COOP_NET_OK, "host loan invalid across repeated restart");
                require(lifecycleClient.rpc(Op::ReleaseHeld).result == COOP_NET_OK, "client loan invalid across repeated restart");
            }
            const Bytes stale{0x73, 0x74, 0x00, static_cast<uint8_t>(cycle)};
            require(lifecycleHost.send(oldHostConnection, GAME_RELIABLE, stale) == COOP_NET_CLOSED &&
                    lifecycleClient.send(oldClientConnection, GAME_RELIABLE, stale) == COOP_NET_CLOSED,
                    "previous-session connection sends into fresh native session");
            require(lifecycleHost.rpc(Op::HoldEvent).result == COOP_NET_OK, "arm repeated host loan");
            require(lifecycleClient.rpc(Op::HoldEvent).result == COOP_NET_OK, "arm repeated client loan");
            const std::vector<Bytes> forward{
                {0x00, 0xff, 0x81, static_cast<uint8_t>(cycle), 0x01},
                {0x48, 0x00, static_cast<uint8_t>(cycle), 0x02, 0x0d, 0x0a},
                {0x19, static_cast<uint8_t>(cycle), 0xfe, 0x03}};
            const std::vector<Bytes> reverse{
                {0x43, 0x00, 0x80, static_cast<uint8_t>(cycle), 0x04},
                {0xff, static_cast<uint8_t>(cycle), 0x00, 0x05},
                {0x0a, 0x0d, static_cast<uint8_t>(cycle), 0x06, 0x00}};
            for (const auto& fixture : forward)
                require(lifecycleClient.send(clientConnection, GAME_RELIABLE, fixture) == COOP_NET_OK,
                        "repeated native reliable forward send fails");
            hostConnection = expectMessages(lifecycleHost, hostCycle.generation, GAME_RELIABLE, forward, 10000);
            for (const auto& fixture : reverse)
                require(lifecycleHost.send(hostConnection, GAME_RELIABLE, fixture) == COOP_NET_OK,
                        "repeated native reliable reverse send fails");
            expectMessages(lifecycleClient, clientCycle.generation, GAME_RELIABLE, reverse, 10000);
            quietMessages(lifecycleHost, hostCycle.generation, 50);
            quietMessages(lifecycleClient, clientCycle.generation, 50);
            // Leave acknowledged native receives UNPOLLED in both directions.
            // stop/restart must fence these, not merely clear our copied queue.
            Bytes pauseConsumers; put32(pauseConsumers, 0);
            require(lifecycleHost.rpc(Op::SetPolling, pauseConsumers).result == COOP_NET_OK &&
                    lifecycleClient.rpc(Op::SetPolling, pauseConsumers).result == COOP_NET_OK, "pause lifecycle consumers");
            require(lifecycleClient.send(clientConnection, GAME_RELIABLE, stale) == COOP_NET_OK &&
                    lifecycleHost.send(hostConnection, GAME_RELIABLE, stale) == COOP_NET_OK, "queue old-session native receives");
            waitSendDrained(lifecycleClient, clientConnection); waitSendDrained(lifecycleHost, hostConnection);
            if (cycle & 1) { lifecycleHost.stop(); lifecycleClient.stop(); }
            else { lifecycleClient.stop(); lifecycleHost.stop(); }
            const CoopNetStatus stoppedHost = lifecycleHost.status(), stoppedClient = lifecycleClient.status();
            require(!stoppedHost.listening && !stoppedHost.connections &&
                    !stoppedClient.listening && !stoppedClient.connections, "repeated stop leaves native resources active");
            require(lifecycleHost.send(hostConnection, GAME_RELIABLE, stale) == COOP_NET_CLOSED &&
                    lifecycleClient.send(clientConnection, GAME_RELIABLE, stale) == COOP_NET_CLOSED,
                    "stopped native session still accepts sends");
            Bytes resumeConsumers; put32(resumeConsumers, 1);
            require(lifecycleHost.rpc(Op::SetPolling, resumeConsumers).result == COOP_NET_OK &&
                    lifecycleClient.rpc(Op::SetPolling, resumeConsumers).result == COOP_NET_OK, "resume lifecycle consumers");
            require(!lifecycleHost.poll().type && !lifecycleClient.poll().type, "stopped provider exposes old native receive");
            const uint64_t hostBytes = privateUsage(lifecycleHost), clientBytes = privateUsage(lifecycleClient);
            if (cycle == warmupCycles - 1) { hostBaseline = hostBytes; clientBaseline = clientBytes; }
            if (cycle >= warmupCycles) {
                hostPeak = (std::max)(hostPeak, hostBytes); clientPeak = (std::max)(clientPeak, clientBytes);
                require(hostPeak <= hostBaseline + cacheTolerance && clientPeak <= clientBaseline + cacheTolerance,
                        "native lifecycle child PrivateUsage exceeds post-warmup cache-growth tolerance");
            }
        }
        require(lifecycleHost.rpc(Op::ReleaseHeld).result == COOP_NET_OK &&
                lifecycleClient.rpc(Op::ReleaseHeld).result == COOP_NET_OK, "final stopped native loans cannot be released");
        lifecycleHost.stop(); lifecycleClient.stop(); // Idempotent teardown before owned process exit.
        lifecycleHost.proveModules(true); lifecycleClient.proveModules(true);
        lifecycleHost.finish(); lifecycleClient.finish();
        report("native-repeated-bidirectional-exact-session-stop-restart-generations", cycles);
        report("native-unpolled-old-session-receives-fenced-both-directions", cycles * 2);
        report("native-message-loans-immutable-across-repeated-stop-restart", cycles * 2);
        std::printf("PASS native-child-PrivateUsage-bounded-growth post-warmup-samples-per-child=%u warmup-cycles=%u "
                    "allocator-cache-tolerance-bytes=%llu host-baseline=%llu host-peak=%llu "
                    "client-baseline=%llu client-peak=%llu; NOT zero-leak proof.\n",
                    cycles - warmupCycles, warmupCycles, static_cast<unsigned long long>(cacheTolerance),
                    static_cast<unsigned long long>(hostBaseline), static_cast<unsigned long long>(hostPeak),
                    static_cast<unsigned long long>(clientBaseline), static_cast<unsigned long long>(clientPeak));
    }

    Child steamChild(executable, steam, directory.path / L"steam", false, job, "steam-unavailable");
    steamChild.proveModules(false);
    StartOptions steamOptions;
    steamOptions.mode = COOP_NET_STEAM;
    steamOptions.generation = 901;
    require(steamChild.start(steamOptions) == COOP_NET_UNAVAILABLE,
            "Steam provider without initialized game runtime does not report UNAVAILABLE");
    const CoopNetStatus unavailable = steamChild.status();
    require(unavailable.mode == COOP_NET_STEAM && unavailable.connections == 0 && unavailable.listening == 0,
            "Steam unavailability falls back to UDP or creates network");
    require(steamChild.rpc(Op::Prepare).result == COOP_NET_INVALID,
            "Steam provider offers UDP credential provisioning");
    require(steamChild.rpc(Op::Invite).result == COOP_NET_INVALID, "Steam provider offers UDP invitation creation");
    steamChild.proveModules(false);
    steamChild.finish();
    report("steam-absent-runtime-explicitly-unavailable-no-fallback", 1);
    std::error_code cleanupError;
    std::filesystem::remove_all(directory.path, cleanupError);
    require(!cleanupError && !std::filesystem::exists(directory.path), "private credential cleanup failed");
    report("private-credential-cleanup", 1);
    std::puts("PASS standalone transport prerequisites only; NOT Steam connectivity or four-player gameplay certification.");
    return 0;
}

// Test helpers follow the independently authored scenarios above.
void require(bool condition, const char* failure) {
    if (!condition) throw std::runtime_error(failure);
}
// Independent bounded readers for the pinned public wire formats, not linked
// provider/protobuf helpers. No decoded credential or packet bytes are printed.
struct ProtoField {
    uint32_t number = 0, wire = 0;
    size_t begin = 0, end = 0, data = 0, length = 0;
    uint64_t integer = 0;
};
uint64_t protoVarint(const uint8_t* bytes, size_t size, size_t& at) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 10; ++i) {
        require(at < size, "truncated bounded protobuf varint");
        const uint8_t byte = bytes[at++];
        require(i != 9 || byte <= 1, "overflowing bounded protobuf varint");
        value |= uint64_t(byte & 0x7f) << (i * 7);
        if (!(byte & 0x80)) {
            require(i == 0 || byte != 0, "noncanonical bounded protobuf varint");
            return value;
        }
    }
    throw std::runtime_error("oversized bounded protobuf varint");
}
std::vector<ProtoField> protoFields(const uint8_t* bytes, size_t size) {
    require(bytes && size > 0 && size <= 65535, "invalid bounded protobuf input");
    std::vector<ProtoField> fields;
    size_t at = 0;
    while (at < size) {
        require(fields.size() < 64, "bounded protobuf field count exceeded");
        ProtoField field;
        field.begin = at;
        const uint64_t tag = protoVarint(bytes, size, at);
        require((tag >> 3) > 0 && (tag >> 3) < (uint64_t(1) << 29), "invalid bounded protobuf field number");
        field.number = static_cast<uint32_t>(tag >> 3);
        field.wire = static_cast<uint32_t>(tag & 7);
        if (field.wire == 0) {
            field.data = at; field.integer = protoVarint(bytes, size, at); field.length = at - field.data;
        } else if (field.wire == 1 || field.wire == 5) {
            field.data = at; field.length = field.wire == 1 ? 8 : 4;
            require(field.length <= size - at, "truncated fixed-width protobuf field");
            at += field.length;
        } else if (field.wire == 2) {
            const uint64_t length = protoVarint(bytes, size, at);
            require(length <= size - at, "truncated bounded protobuf byte field");
            field.data = at; field.length = static_cast<size_t>(length); at += field.length;
        } else throw std::runtime_error("unsupported bounded protobuf wire type");
        field.end = at;
        fields.push_back(field);
    }
    return fields;
}
const ProtoField* findField(const std::vector<ProtoField>& fields, uint32_t number) {
    const ProtoField* found = nullptr;
    for (const auto& field : fields) {
        if (field.number == number) {
            require(!found, "duplicate singleton credential protobuf field");
            found = &field;
        }
    }
    return found;
}
Bytes fieldBytes(const Bytes& bytes, const std::vector<ProtoField>& fields, uint32_t number, size_t exactSize = 0) {
    const auto* field = findField(fields, number);
    require(field && field->wire == 2 && (!exactSize || field->length == exactSize),
            "required credential protobuf byte field absent or malformed");
    return Bytes(bytes.begin() + field->data, bytes.begin() + field->data + field->length);
}
Bytes privateKey(const Bytes& signedCert) {
    require(signedCert.size() <= 1024, "signed credential exceeds bound");
    const auto fields = protoFields(signedCert.data(), signedCert.size());
    Bytes key = fieldBytes(signedCert, fields, 1, 32);
    const Bytes certificate = fieldBytes(signedCert, fields, 4);
    const auto certFields = protoFields(certificate.data(), certificate.size());
    const auto* keyType = findField(certFields, 1);
    require(keyType && keyType->wire == 0 && keyType->integer == 1, "credential is not native Ed25519");
    require(fieldBytes(certificate, certFields, 2, 32) != key, "private/public credential key fixture is invalid");
    return key;
}
Envelope decodeEnvelope(const Bytes& bytes) {
    require(bytes.size() >= 12 && bytes.size() <= 2048 &&
            bytes[0] == 'K' && bytes[1] == 'C' && bytes[2] == 'U' && bytes[3] == '1',
            "invalid bounded native credential envelope");
    Reader reader{bytes};
    reader.at = 4;
    const uint32_t rootLength = reader.u32(), leafLength = reader.u32();
    require(rootLength > 0 && rootLength <= 1024 && leafLength > 0 && leafLength <= 1024 &&
            uint64_t(rootLength) + leafLength + 12 == bytes.size(), "invalid bounded credential envelope lengths");
    Envelope envelope;
    envelope.root.assign(bytes.begin() + 12, bytes.begin() + 12 + rootLength);
    envelope.leaf.assign(bytes.begin() + 12 + rootLength, bytes.end());
    const auto leafFields = protoFields(envelope.leaf.data(), envelope.leaf.size());
    require(fieldBytes(envelope.leaf, leafFields, 6, 64).size() == 64, "native leaf certificate is not signed");
    return envelope;
}
Envelope decodeProfile(const Bytes& ciphertext) {
    require(!ciphertext.empty() && ciphertext.size() <= 8192, "DPAPI profile exceeds strict bound");
    DATA_BLOB encrypted{static_cast<DWORD>(ciphertext.size()), const_cast<BYTE*>(ciphertext.data())};
    DATA_BLOB plain{};
    require(CryptUnprotectData(&encrypted, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain) != FALSE,
            "decrypt current-user private host test fixture");
    struct FreePlain {
        DATA_BLOB& blob;
        ~FreePlain() { if (blob.pbData) { SecureZeroMemory(blob.pbData, blob.cbData); LocalFree(blob.pbData); } }
    } freePlain{plain};
    require(plain.pbData && plain.cbData >= 12 && plain.cbData <= 2048, "decrypted host profile exceeds strict bound");
    Bytes bytes(plain.pbData, plain.pbData + plain.cbData);
    struct WipeBytes { Bytes& bytes; ~WipeBytes() { if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size()); } } wipe{bytes};
    return decodeEnvelope(bytes);
}
std::string encodeBase64(const Bytes& bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t value = uint32_t(bytes[i]) << 16 |
            (i + 1 < bytes.size() ? uint32_t(bytes[i + 1]) << 8 : 0) |
            (i + 2 < bytes.size() ? uint32_t(bytes[i + 2]) : 0);
        result += alphabet[(value >> 18) & 63]; result += alphabet[(value >> 12) & 63];
        result += i + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=';
        result += i + 2 < bytes.size() ? alphabet[value & 63] : '=';
    }
    return result;
}
Envelope decodeInvitation(const std::string& text) {
    require(text.size() > 5 && text.size() < COOP_NET_INVITE_CAPACITY && text.compare(0, 5, "KCU1:") == 0,
            "invalid bounded invitation prefix");
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const std::string encoded = text.substr(5);
    require(encoded.size() % 4 == 0 && encoded.size() <= 2732, "invalid bounded invitation base64 length");
    Bytes bytes;
    bytes.reserve(encoded.size() / 4 * 3);
    for (size_t i = 0; i < encoded.size(); i += 4) {
        uint32_t value = 0;
        unsigned padding = 0;
        for (unsigned j = 0; j < 4; ++j) {
            const char ch = encoded[i + j];
            if (ch == '=') {
                require(i + 4 == encoded.size() && j >= 2, "invalid invitation base64 padding");
                ++padding; value <<= 6;
            } else {
                require(!padding, "invitation base64 data after padding");
                const char* found = std::strchr(alphabet, ch);
                require(ch != '\0' && found != nullptr, "invalid invitation base64 character");
                value = (value << 6) | static_cast<uint32_t>(found - alphabet);
            }
        }
        require(padding <= 2 && (padding != 1 || (value & 0xffu) == 0) &&
                (padding != 2 || (value & 0xffffu) == 0), "noncanonical invitation base64 pad bits");
        bytes.push_back(static_cast<uint8_t>(value >> 16));
        if (padding < 2) bytes.push_back(static_cast<uint8_t>(value >> 8));
        if (!padding) bytes.push_back(static_cast<uint8_t>(value));
    }
    require(encodeBase64(bytes) == encoded, "noncanonical invitation base64 encoding");
    return decodeEnvelope(bytes);
}
std::string hostRoleInvitation(const Envelope& host) {
    const auto fields = protoFields(host.root.data(), host.root.size());
    Bytes publicRoot;
    for (const auto& field : fields) {
        if (field.number != 1)
            publicRoot.insert(publicRoot.end(), host.root.begin() + field.begin, host.root.begin() + field.end);
    }
    require(!publicRoot.empty() && publicRoot.size() < host.root.size(), "strip host root private credential for wrong-role fixture");
    Bytes envelope{'K', 'C', 'U', '1'};
    put32(envelope, static_cast<uint32_t>(publicRoot.size()));
    put32(envelope, static_cast<uint32_t>(host.leaf.size()));
    envelope.insert(envelope.end(), publicRoot.begin(), publicRoot.end());
    envelope.insert(envelope.end(), host.leaf.begin(), host.leaf.end());
    require(envelope.size() <= 2048, "wrong-role native credential envelope exceeds bound");
    const std::string text = "KCU1:" + encodeBase64(envelope);
    return text;
}
void WireAudit::addKeys(const Envelope& envelope, bool privateRoot) {
    const auto rootFields = protoFields(envelope.root.data(), envelope.root.size());
    require(privateRoot == (findField(rootFields, 1) != nullptr), "profile/invitation root private-key contract mismatch");
    if (privateRoot) privateKeys.push_back(privateKey(envelope.root));
    privateKeys.push_back(privateKey(envelope.leaf));
}
WireAudit::~WireAudit() {
    for (auto& key : privateKeys) if (!key.empty()) SecureZeroMemory(key.data(), key.size());
}
void WireAudit::observe(const uint8_t* bytes, size_t size) noexcept {
    try {
        for (const auto& key : privateKeys) {
            if (std::search(bytes, bytes + size, key.begin(), key.end()) != bytes + size) ++leakedKeys;
        }
        if (size <= 1 || (bytes[0] != 34 && bytes[0] != 35)) return;
        // Pinned native UDP SendMsg is message-ID byte + unpadded protobuf.
        // ConnectRequest.cert is field4; ConnectOK.cert is field8.
        const auto outer = protoFields(bytes + 1, size - 1);
        const ProtoField* cert = findField(outer, bytes[0] == 34 ? 4 : 8);
        require(cert && cert->wire == 2 && cert->length > 0 && cert->length <= 1024,
                "actual UDP handshake has no bounded native SignedCert");
        const auto signedFields = protoFields(bytes + 1 + cert->data, cert->length);
        if (findField(signedFields, 1)) ++leakedFields;
        const ProtoField* payload = findField(signedFields, 4);
        require(payload && payload->wire == 2 && payload->length > 0, "actual UDP handshake SignedCert has no public certificate");
        if (bytes[0] == 34) ++clientCertificates;
        else ++hostCertificates;
    } catch (...) { ++malformed; }
}
Bytes readPrivateFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(file.good(), "open private credential fixture");
    const auto length = file.tellg();
    require(length >= 0 && length <= static_cast<std::streamoff>(1024 * 1024), "private credential fixture exceeds bound");
    Bytes result(static_cast<size_t>(length));
    file.seekg(0);
    if (!result.empty()) file.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
    require(file.good(), "read private credential fixture");
    return result;
}
void writePrivateFile(const std::filesystem::path& path, const Bytes& data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    require(file.good(), "open damaged private credential fixture");
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    file.close();
    require(file.good(), "write damaged private credential fixture");
}
void observeSteamProcesses() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    require(snapshot != INVALID_HANDLE_VALUE, "observe external Steam process condition");
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    unsigned steamProcesses = 0;
    BOOL present = Process32FirstW(snapshot, &entry);
    while (present) {
        if (_wcsicmp(entry.szExeFile, L"steam.exe") == 0) ++steamProcesses;
        present = Process32NextW(snapshot, &entry);
    }
    const DWORD error = GetLastError();
    CloseHandle(snapshot);
    require(error == ERROR_NO_MORE_FILES, "observe Steam process condition completely");
    std::printf("OBSERVED external-steam.exe-processes=%u; online/offline/account state NOT inferred. "
                "Child module snapshots are independent process observations.\n", steamProcesses);
}
void put32(Bytes& bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
void put64(Bytes& bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
void putString(Bytes& bytes, const std::string& value) {
    require(value.size() <= kFrameLimit, "control string exceeds bound");
    put32(bytes, static_cast<uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}
uint32_t Reader::u32() {
    require(at <= bytes.size() && bytes.size() - at >= 4, "truncated control integer");
    uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i) result |= uint32_t(bytes[at++]) << (i * 8);
    return result;
}
uint64_t Reader::u64() {
    const uint64_t low = u32();
    return low | (uint64_t(u32()) << 32);
}
std::string Reader::string() {
    const uint32_t size = u32();
    require(at <= bytes.size() && size <= bytes.size() - at, "truncated control string");
    std::string result(reinterpret_cast<const char*>(bytes.data() + at), size);
    at += size;
    return result;
}
Bytes Reader::rest() {
    require(at <= bytes.size(), "invalid control cursor");
    Bytes result(bytes.begin() + at, bytes.end());
    at = bytes.size();
    return result;
}
void Reader::done() { require(at == bytes.size(), "unexpected control payload"); }

struct OwnerSecurity {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    OwnerSecurity() {
        HANDLE token = nullptr;
        require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE, "read account security token");
        DWORD length = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &length);
        Bytes storage(length);
        const BOOL got = GetTokenInformation(token, TokenUser, storage.data(), length, &length);
        CloseHandle(token);
        require(got != FALSE, "read account SID");
        LPWSTR sid = nullptr;
        require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid) != FALSE,
                "encode account SID");
        const std::wstring sddl = L"D:P(A;OICI;FA;;;" + std::wstring(sid) + L")(A;OICI;FA;;;SY)";
        LocalFree(sid);
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                    &descriptor, nullptr) != FALSE, "create private account-only ACL");
        attributes.lpSecurityDescriptor = descriptor;
    }
    ~OwnerSecurity() { if (descriptor) LocalFree(descriptor); }
};
PrivateDirectory::PrivateDirectory() {
    std::array<wchar_t, 32768> temporary{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
    require(length > 0 && length < temporary.size(), "locate temporary directory");
    OwnerSecurity security;
    // No credentials in the directory name or process command line.
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        path = std::filesystem::path(temporary.data()) / (L"KenshiCoop-transport-private-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) +
            L"-" + std::to_wstring(attempt));
        if (CreateDirectoryW(path.c_str(), &security.attributes)) return;
        require(GetLastError() == ERROR_ALREADY_EXISTS, "create private temporary directory");
    }
    throw std::runtime_error("allocate unique private temporary directory");
}
PrivateDirectory::~PrivateDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    if (error) std::fputs("WARNING private temporary directory cleanup failed (path/credentials withheld).\n", stderr);
}

// Parent pipe handles are overlapped: even a hung or crashed child cannot leave
// a blocking WriteFile/ReadFile behind. Child endpoints use synchronous stdio.
void transfer(HANDLE pipe, void* bytes, DWORD length, bool writing, bool overlapped) {
    auto* cursor = static_cast<uint8_t*>(bytes);
    const auto deadline = Clock::now() + std::chrono::milliseconds(kRpcTimeoutMs);
    while (length) {
        DWORD transferred = 0;
        OVERLAPPED operation{};
        if (overlapped) {
            operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            require(operation.hEvent != nullptr, "create bounded control IO event");
        }
        BOOL result = writing ? WriteFile(pipe, cursor, length, &transferred, overlapped ? &operation : nullptr)
                              : ReadFile(pipe, cursor, length, &transferred, overlapped ? &operation : nullptr);
        if (!result && overlapped && GetLastError() == ERROR_IO_PENDING) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            const DWORD wait = WaitForSingleObject(operation.hEvent, remaining > 0 ? static_cast<DWORD>(remaining) : 0);
            if (wait != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &operation);
                WaitForSingleObject(operation.hEvent, INFINITE);
                CloseHandle(operation.hEvent);
                throw std::runtime_error("child control IO timeout");
            }
            result = GetOverlappedResult(pipe, &operation, &transferred, FALSE);
        }
        if (operation.hEvent) CloseHandle(operation.hEvent);
        require(result != FALSE && transferred != 0, "child control pipe closed or failed");
        cursor += transferred;
        length -= transferred;
    }
}
void writeFrame(HANDLE pipe, const Bytes& frame, bool overlapped) {
    require(!frame.empty() && frame.size() <= kFrameLimit, "invalid outbound control frame bound");
    Bytes header;
    put32(header, static_cast<uint32_t>(frame.size()));
    transfer(pipe, header.data(), 4, true, overlapped);
    transfer(pipe, const_cast<uint8_t*>(frame.data()), static_cast<DWORD>(frame.size()), true, overlapped);
}
Bytes readFrame(HANDLE pipe, bool overlapped) {
    Bytes header(4);
    transfer(pipe, header.data(), 4, false, overlapped);
    Reader reader{header};
    const uint32_t length = reader.u32();
    require(length >= 4 && length <= kFrameLimit, "invalid inbound control frame bound");
    Bytes frame(length);
    transfer(pipe, frame.data(), length, false, overlapped);
    return frame;
}
std::wstring quote(const std::wstring& value) {
    // Windows command-line escaping, including paths ending in backslash.
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += ch;
    }
    result.append(slashes * 2, L'\\');
    result += L'\"';
    return result;
}
Child::Child(const std::wstring& executable, const std::wstring& provider,
             const std::filesystem::path& directory, bool host, HANDLE job, const char* name) : label(name) {
    std::filesystem::create_directories(directory);
    static std::atomic<unsigned> counter{0};
    const std::wstring pipeName = L"\\\\.\\pipe\\KenshiCoopTransport-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(counter++);
    OwnerSecurity security;
    input = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536,
        kRpcTimeoutMs, &security.attributes);
    require(input != INVALID_HANDLE_VALUE, "create private bounded child control pipe");
    output = input;
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), security.descriptor, TRUE};
    HANDLE endpoint = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, &inherit, OPEN_EXISTING, 0, nullptr);
    if (endpoint == INVALID_HANDLE_VALUE) { CloseHandle(input); input = output = nullptr; throw std::runtime_error("open child control endpoint"); }
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    Bytes attributes(attributeSize);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    const BOOL initialized = InitializeProcThreadAttributeList(list, 1, 0, &attributeSize);
    if (!initialized) { CloseHandle(endpoint); CloseHandle(input); input = output = nullptr; throw std::runtime_error("initialize isolated child handles"); }
    const BOOL updated = UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                  &endpoint, sizeof(endpoint), nullptr, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = endpoint;
    startup.StartupInfo.hStdOutput = endpoint;
    startup.StartupInfo.hStdError = endpoint;
    startup.lpAttributeList = list;
    std::wstring command = quote(executable) + (host ? L" --host" : L" --client") +
        L" --provider " + quote(provider) + L" --work " + quote(directory.wstring());
    PROCESS_INFORMATION info{};
    const BOOL created = updated && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &info);
    DeleteProcThreadAttributeList(list);
    CloseHandle(endpoint);
    if (!created) { CloseHandle(input); input = output = nullptr; throw std::runtime_error("launch isolated transport child"); }
    process = info.hProcess;
    if (!AssignProcessToJobObject(job, process)) {
        TerminateProcess(process, 2); WaitForSingleObject(process, kRpcTimeoutMs);
        CloseHandle(info.hThread); CloseHandle(process); CloseHandle(input); process = input = output = nullptr;
        throw std::runtime_error("assign child cleanup job");
    }
    if (ResumeThread(info.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(process, 2); WaitForSingleObject(process, kRpcTimeoutMs);
        CloseHandle(info.hThread); CloseHandle(process); CloseHandle(input); process = input = output = nullptr;
        throw std::runtime_error("resume transport child");
    }
    CloseHandle(info.hThread);
}
Child::~Child() {
    if (process) {
        if (!exited) { TerminateProcess(process, 2); WaitForSingleObject(process, kRpcTimeoutMs); }
        CloseHandle(process);
    }
    if (input && input != INVALID_HANDLE_VALUE) CloseHandle(input);
}
Reply Child::rpc(Op op, const Bytes& payload) {
    require(!exited, "command sent after child teardown");
    Bytes frame;
    put32(frame, static_cast<uint32_t>(op));
    frame.insert(frame.end(), payload.begin(), payload.end());
    writeFrame(input, frame, true);
    const Bytes answer = readFrame(output, true);
    Reader reader{answer};
    Reply reply;
    reply.result = static_cast<int32_t>(reader.u32());
    reply.bytes = reader.rest();
    return reply;
}
int32_t Child::start(const StartOptions& options) {
    Bytes payload;
    put32(payload, options.mode); put32(payload, options.port); put64(payload, options.generation);
    putString(payload, options.address); putString(payload, options.invite);
    const Reply response = rpc(Op::Start, payload);
    if (response.result != COOP_NET_OK && !response.bytes.empty()) {
        Reader diagnostic{response.bytes};
        const std::string detail = diagnostic.string();
        diagnostic.done();
        std::fprintf(stderr, "START result=%d detail=%s\n", response.result, detail.c_str());
    }
    return response.result;
}
Event Child::poll() {
    const Reply reply = rpc(Op::Poll);
    require(reply.result >= 0, "provider poll failed");
    if (!reply.result) { require(reply.bytes.empty(), "empty poll carries data"); return {}; }
    Reader reader{reply.bytes};
    Event event;
    event.type = reader.u32(); event.connection = reader.u32(); event.traffic = reader.u32();
    event.generation = reader.u64(); event.reason = static_cast<int32_t>(reader.u32());
    event.detail = reader.string();
    if (event.type == COOP_NET_EVENT_ERROR || event.type == COOP_NET_EVENT_DISCONNECTED)
        std::fprintf(stderr, "CONNECTION type=%u reason=%d detail=%s\n",
                     event.type, event.reason, event.detail.c_str());
    event.data = reader.rest();
    return event;
}
int32_t Child::send(uint32_t connection, uint32_t traffic, const Bytes& payload) {
    Bytes command;
    put32(command, connection); put32(command, traffic);
    command.insert(command.end(), payload.begin(), payload.end());
    return rpc(Op::Send, command).result;
}
std::vector<int32_t> Child::burst(uint32_t connection, uint32_t traffic, const std::vector<Bytes>& messages) {
    Bytes command;
    put32(command, connection); put32(command, traffic); put32(command, static_cast<uint32_t>(messages.size()));
    for (const auto& bytes : messages) {
        put32(command, static_cast<uint32_t>(bytes.size()));
        command.insert(command.end(), bytes.begin(), bytes.end());
    }
    const Reply reply = rpc(Op::Burst, command);
    require(reply.result == COOP_NET_OK, "bulk control command failed");
    Reader reader{reply.bytes};
    std::vector<int32_t> results;
    for (size_t i = 0; i < messages.size(); ++i) results.push_back(static_cast<int32_t>(reader.u32()));
    reader.done();
    return results;
}
CoopNetStatus Child::status() {
    const Reply reply = rpc(Op::Status);
    require(reply.result == COOP_NET_OK, "provider status fails");
    Reader reader{reply.bytes};
    CoopNetStatus status{};
    status.size = sizeof(status); status.mode = reader.u32(); status.generation = reader.u64();
    status.selfSteamId = reader.u64(); status.listening = reader.u32(); status.connections = reader.u32();
    reader.done();
    return status;
}
CoopNetPeerStats Child::stats(uint32_t connection) {
    Bytes payload; put32(payload, connection);
    const Reply reply = rpc(Op::Stats, payload);
    require(reply.result == COOP_NET_OK, "connected peerStats fails");
    Reader reader{reply.bytes};
    CoopNetPeerStats stats{};
    stats.size = sizeof(stats); stats.connection = connection;
    stats.pendingReliableBytes = reader.u32(); stats.pendingStateBytes = reader.u32();
    stats.pendingBulkBytes = reader.u32(); stats.unackedReliableBytes = reader.u32();
    reader.done();
    return stats;
}
void Child::stop() { require(rpc(Op::Stop).result == COOP_NET_OK, "provider stop command fails"); }
void Child::finish() {
    require(rpc(Op::Exit).result == COOP_NET_OK, "provider teardown command fails");
    require(WaitForSingleObject(process, kRpcTimeoutMs) == WAIT_OBJECT_0, "child teardown timeout");
    DWORD code = 2;
    require(GetExitCodeProcess(process, &code) && code == 0, "child exits unsuccessfully");
    exited = true;
}
void Child::proveModules(bool udp) {
    const Reply reply = rpc(Op::Modules);
    require(reply.result == COOP_NET_OK, "capture loaded modules");
    Reader reader{reply.bytes};
    const uint32_t steamLoads = reader.u32(), otherProviderLoads = reader.u32();
    require(steamLoads == 0 && otherProviderLoads == 0, "loader notifications observed transient Steam/fallback DLL load");
    const uint32_t count = reader.u32();
    require(count != 0 && count <= 512, "invalid loaded module count");
    std::printf("MODULES %s count=%u steam-load-notifications=%u other-provider-load-notifications=%u",
                label.c_str(), count, steamLoads, otherProviderLoads);
    bool selected = false;
    for (uint32_t i = 0; i < count; ++i) {
        const std::string name = reader.string();
        require(name.find(char(13)) == std::string::npos && name.find(char(10)) == std::string::npos, "invalid module name");
        std::printf(" %s", name.c_str());
        require(name != "steam_api64.dll" && name != "steamclient.dll" && name != "steamclient64.dll",
                "child loaded Steam runtime despite absent game context");
        if (udp) {
            require(name != "kenshicoopnetsteam.dll", "UDP child loaded Steam provider");
            if (name == "kenshicoopnetudp.dll") selected = true;
        } else {
            require(name != "kenshicoopnetudp.dll", "Steam child loaded UDP fallback");
            if (name == "kenshicoopnetsteam.dll") selected = true;
        }
    }
    std::puts("");
    reader.done();
    require(selected, "selected provider absent from module snapshot");
}
std::string issueInvite(Child& child) {
    const Reply reply = child.rpc(Op::Invite);
    require(reply.result == COOP_NET_OK, "issue per-client invitation");
    Reader reader{reply.bytes};
    const std::string invitation = reader.string();
    reader.done();
    require(!invitation.empty() && invitation.size() < COOP_NET_INVITE_CAPACITY, "invalid invitation length");
    return invitation;
}
void report(const char* name, uint64_t count) {
    std::printf("PASS %s count=%llu\n", name, static_cast<unsigned long long>(count));
}
uint32_t waitConnected(Child& child, uint64_t generation) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(kNetworkTimeoutMs);
    while (Clock::now() < deadline) {
        const Event event = child.poll();
        if (!event.type) { Sleep(2); continue; }
        require(event.generation == generation, "fresh/reconnected client receives old-generation event");
        require(event.type != COOP_NET_EVENT_DISCONNECTED && event.type != COOP_NET_EVENT_ERROR,
                "invited client connection fails");
        if (event.type == COOP_NET_EVENT_CONNECTED) {
            require(event.connection != 0, "connected event has no connection handle");
            return event.connection;
        }
        require(event.type != COOP_NET_EVENT_MESSAGE, "message arrives before client connection event");
    }
    throw std::runtime_error("invited client connection timeout");
}
void waitPeerCount(Child& host, uint32_t count) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(kNetworkTimeoutMs);
    while (Clock::now() < deadline) {
        const Event event = host.poll();
        require(!event.type || event.generation == host.status().generation, "host event from stale generation");
        require(event.type != COOP_NET_EVENT_MESSAGE, "unexpected message during admission/teardown");
        if (host.status().connections == count) return;
        Sleep(2);
    }
    throw std::runtime_error("host peer admission/teardown count timeout");
}
void waitSendDrained(Child& sender, uint32_t connection) {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (Clock::now() < deadline) {
        const CoopNetPeerStats stats = sender.stats(connection);
        if (!stats.pendingReliableBytes && !stats.pendingBulkBytes && !stats.unackedReliableBytes) return;
        Sleep(2);
    }
    throw std::runtime_error("native reliable acknowledgement did not drain before lifecycle deadline");
}
uint64_t privateUsage(const Child& child) {
    using Query = BOOL (WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo"));
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    require(query && query(child.process, reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters), sizeof(counters)),
            "sample actual native child PrivateUsage");
    return static_cast<uint64_t>(counters.PrivateUsage);
}
void rejectJoin(Child& child, const StartOptions& options) {
    const int32_t start = child.start(options);
    if (start != COOP_NET_OK) {
        require(start == COOP_NET_INVALID || start == COOP_NET_AUTH_FAILED, "bad invitation fails for unrelated reason");
        child.stop();
        return;
    }
    bool rejected = false;
    const auto deadline = Clock::now() + std::chrono::milliseconds(kNetworkTimeoutMs);
    while (Clock::now() < deadline) {
        const Event event = child.poll();
        if (!event.type) { Sleep(2); continue; }
        require(event.generation == options.generation, "rejected join has stale generation");
        require(event.type != COOP_NET_EVENT_CONNECTED && event.type != COOP_NET_EVENT_MESSAGE,
                "unadmitted/wrong-host/corrupt invitation connects or receives data");
        if (event.type == COOP_NET_EVENT_DISCONNECTED || event.type == COOP_NET_EVENT_ERROR) {
            require(event.reason != 0, "rejected join supplies no failure reason");
            rejected = true;
            break;
        }
    }
    require(rejected, "bad/fourth join never reports rejection");
    child.stop();
}
Bytes record(uint32_t tag, uint32_t sequence, size_t size) {
    require(size >= 8, "fixture too short");
    Bytes bytes;
    bytes.reserve(size);
    put32(bytes, tag); put32(bytes, sequence);
    for (size_t i = 8; i < size; ++i)
        bytes.push_back(static_cast<uint8_t>((i * 73u + sequence * 19u + (i >> 8)) & 0xffu));
    return bytes;
}
uint32_t expectMessages(Child& receiver, uint64_t generation, uint32_t traffic,
                       const std::vector<Bytes>& expected, uint32_t timeoutMs) {
    size_t received = 0;
    uint32_t connection = 0;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (received < expected.size() && Clock::now() < deadline) {
        const Event event = receiver.poll();
        if (!event.type) { Sleep(1); continue; }
        require(event.generation == generation, "message has stale consumer-visible generation");
        if (event.type != COOP_NET_EVENT_MESSAGE) {
            require(event.type != COOP_NET_EVENT_ERROR && event.type != COOP_NET_EVENT_DISCONNECTED,
                    "reliable stream fails while draining");
            continue;
        }
        require(event.traffic == traffic, "received message on wrong traffic class");
        require(event.data == expected[received++], "accepted reliable data lost, corrupted, duplicated or reordered");
        connection = event.connection;
    }
    require(received == expected.size(), "accepted reliable data missing at deterministic deadline");
    return connection;
}
void quietMessages(Child& receiver, uint64_t generation, uint32_t milliseconds) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(milliseconds);
    while (Clock::now() < deadline) {
        const Event event = receiver.poll();
        require(!event.type || event.generation == generation, "quiet interval has stale generation");
        require(event.type != COOP_NET_EVENT_MESSAGE, "duplicate or silently-consumed backpressure data arrived");
        require(event.type != COOP_NET_EVENT_ERROR && event.type != COOP_NET_EVENT_DISCONNECTED,
                "stream fails in quiet interval");
        Sleep(2);
    }
}
void drainStateProbes(Child& receiver, uint64_t generation, const Bytes& fixture) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(1200);
    while (Clock::now() < deadline) {
        const Event event = receiver.poll();
        require(!event.type || event.generation == generation, "state drain has stale generation");
        if (event.type == COOP_NET_EVENT_MESSAGE)
            require(event.traffic == GAME_STATE && event.data == fixture, "state drain reveals extra reliable/bulk/corrupt data");
        require(event.type != COOP_NET_EVENT_ERROR && event.type != COOP_NET_EVENT_DISCONNECTED,
                "network fails after bulk class probes");
        Sleep(2);
    }
}

uint16_t reservePort() {
    SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    require(socket != INVALID_SOCKET, "allocate real loopback UDP socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(socket); throw std::runtime_error("bind ephemeral loopback UDP socket");
    }
    int length = sizeof(address);
    const int got = getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length);
    closesocket(socket);
    require(got == 0, "read ephemeral loopback UDP port");
    return ntohs(address.sin_port);
}
Proxy::Proxy(uint16_t hostPort, WireAudit& wireAudit) : audit(wireAudit) {
    socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    require(socket != INVALID_SOCKET, "create actual impairment UDP socket");
    sockaddr_in local{};
    local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        closesocket(socket); socket = INVALID_SOCKET; throw std::runtime_error("bind impairment UDP socket");
    }
    int size = sizeof(local);
    require(getsockname(socket, reinterpret_cast<sockaddr*>(&local), &size) == 0, "read impairment UDP endpoint");
    port = ntohs(local.sin_port);
    host.sin_family = AF_INET; host.sin_addr.s_addr = htonl(INADDR_LOOPBACK); host.sin_port = htons(hostPort);
    u_long nonblocking = 1;
    require(ioctlsocket(socket, FIONBIO, &nonblocking) == 0, "set impairment UDP nonblocking");
    // Old client endpoints can close during reconnect. Do not let ICMP for
    // those stale endpoint datagrams disable the real proxy socket.
    BOOL reportUdpReset = FALSE;
    DWORD ioctlBytes = 0;
    require(WSAIoctl(socket, SIO_UDP_CONNRESET, &reportUdpReset, sizeof(reportUdpReset), nullptr, 0,
                     &ioctlBytes, nullptr, nullptr) == 0, "disable stale-endpoint UDP reset notifications");
    worker = std::thread([this] { run(); });
}
Proxy::~Proxy() {
    stopping = true;
    if (worker.joinable()) worker.join();
    if (socket != INVALID_SOCKET) closesocket(socket);
}
void Proxy::run() {
    std::vector<Datagram> pending;
    std::array<uint8_t, 65536> buffer{};
    std::array<Clock::time_point, 2> nextSend{{Clock::now(), Clock::now()}};
    while (!stopping.load()) {
        for (unsigned batch = 0; batch < 128; ++batch) {
            sockaddr_in source{};
            int length = sizeof(source);
            const int size = recvfrom(socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
                                      reinterpret_cast<sockaddr*>(&source), &length);
            if (size == SOCKET_ERROR) {
                if (WSAGetLastError() != WSAEWOULDBLOCK) stopping = true;
                break;
            }
            if (source.sin_addr.s_addr != htonl(INADDR_LOOPBACK) || size <= 0) continue;
            const bool fromHost = source.sin_port == host.sin_port;
            if (!fromHost) client = source;
            if (fromHost && !client.sin_port) continue;
            const uint64_t ordinal = ++packets;
            audit.observe(buffer.data(), static_cast<size_t>(size));
            if (ordinal % 17 == 0) { ++dropped; continue; }
            // Bounded 128 KiB/s in each direction, plus real lag/loss/duplicates/
            // reordering of encrypted packets. Packet contents remain opaque.
            const unsigned direction = fromHost ? 1 : 0;
            const auto now = Clock::now();
            auto due = std::max(now + std::chrono::milliseconds(12), nextSend[direction]);
            nextSend[direction] = due + std::chrono::microseconds((uint64_t(size) * 1000000u) / 131072u + 1u);
            if (ordinal % 11 == 0) { due += std::chrono::milliseconds(70); ++reordered; }
            if (pending.size() >= 8192) { stopping = true; break; }
            Datagram packet{Bytes(buffer.begin(), buffer.begin() + size), fromHost ? client : host, due};
            if (ordinal % 13 == 0) {
                pending.push_back(packet);
                packet.due += std::chrono::milliseconds(7);
                ++duplicated;
            }
            pending.push_back(std::move(packet));
        }
        const auto now = Clock::now();
        for (size_t i = 0; i < pending.size();) {
            if (pending[i].due > now) { ++i; continue; }
            const auto& packet = pending[i];
            const int sent = sendto(socket, reinterpret_cast<const char*>(packet.bytes.data()),
                static_cast<int>(packet.bytes.size()), 0, reinterpret_cast<const sockaddr*>(&packet.target), sizeof(packet.target));
            if (sent == static_cast<int>(packet.bytes.size())) ++forwarded;
            else stopping = true;
            pending.erase(pending.begin() + i);
        }
        Sleep(1);
    }
}

std::string utf8(const std::wstring& value) {
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    require(length > 0, "convert UTF-8 credential path");
    std::string result(length, '\0');
    require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), static_cast<int>(value.size()),
            result.data(), length, nullptr, nullptr) == length, "convert UTF-8 credential path bytes");
    return result;
}
Bytes moduleNames() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    require(snapshot != INVALID_HANDLE_VALUE, "snapshot child loaded modules");
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::vector<std::string> modules;
    BOOL present = Module32FirstW(snapshot, &entry);
    while (present) {
        std::string name = utf8(entry.szModule);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
            return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : static_cast<char>(ch);
        });
        modules.push_back(std::move(name));
        present = Module32NextW(snapshot, &entry);
    }
    const DWORD error = GetLastError();
    CloseHandle(snapshot);
    require(error == ERROR_NO_MORE_FILES && !modules.empty() && modules.size() <= 512, "enumerate child modules completely");
    std::sort(modules.begin(), modules.end());
    Bytes answer;
    put32(answer, static_cast<uint32_t>(modules.size()));
    for (const auto& name : modules) putString(answer, name);
    return answer;
}
// Ldr notifications catch transient loads that polling snapshots could miss.
// The callback is loader-lock safe: no allocation, IO, module lookup or locks.
struct NativeUnicodeString { USHORT length, maximumLength; PWSTR buffer; };
struct NativeDllNotice {
    ULONG flags;
    const NativeUnicodeString* fullName;
    const NativeUnicodeString* baseName;
    PVOID base;
    ULONG imageSize;
};
union NativeDllNoticeData { NativeDllNotice loaded, unloaded; };
using NativeDllCallback = void (CALLBACK*)(ULONG, const NativeDllNoticeData*, void*);
struct ModuleWatch {
    using Register = LONG (NTAPI*)(ULONG, NativeDllCallback, void*, void**);
    using Unregister = LONG (NTAPI*)(void*);
    std::atomic<uint32_t> steamLoads{0}, otherProviderLoads{0};
    bool udp;
    void* cookie = nullptr;
    Unregister unregister = nullptr;
    template<size_t N> static bool equals(const NativeUnicodeString* name, const wchar_t (&literal)[N]) noexcept {
        if (!name || !name->buffer || name->length != (N - 1) * sizeof(wchar_t)) return false;
        for (size_t i = 0; i < N - 1; ++i) {
            wchar_t ch = name->buffer[i];
            if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
            if (ch != literal[i]) return false;
        }
        return true;
    }
    static void CALLBACK notified(ULONG reason, const NativeDllNoticeData* data, void* context) noexcept {
        if (reason != 1 || !data || !context) return;
        auto& watch = *static_cast<ModuleWatch*>(context);
        const auto* name = data->loaded.baseName;
        if (equals(name, L"steam_api64.dll") || equals(name, L"steamclient.dll") || equals(name, L"steamclient64.dll"))
            ++watch.steamLoads;
        if ((watch.udp && equals(name, L"kenshicoopnetsteam.dll")) ||
            (!watch.udp && equals(name, L"kenshicoopnetudp.dll"))) ++watch.otherProviderLoads;
    }
    explicit ModuleWatch(bool selectedUdp) : udp(selectedUdp) {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        require(ntdll != nullptr, "locate existing loader notification runtime");
        const auto registration = reinterpret_cast<Register>(GetProcAddress(ntdll, "LdrRegisterDllNotification"));
        unregister = reinterpret_cast<Unregister>(GetProcAddress(ntdll, "LdrUnregisterDllNotification"));
        require(registration && unregister && registration(0, &notified, this, &cookie) >= 0 && cookie,
                "register actual child DLL-load notifications");
    }
    ~ModuleWatch() { if (cookie) unregister(cookie); }
    void requireExclusive() {
        require(!steamLoads.load() && !otherProviderLoads.load(), "child transiently loaded Steam runtime or wrong provider");
    }
};

struct LoadedProvider {
    HMODULE module = nullptr;
    CoopNetApi api{};
    void* context = nullptr;
    explicit LoadedProvider(const std::wstring& path) {
        module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        require(module != nullptr, "load exactly selected provider DLL");
        const auto getApi = reinterpret_cast<CoopNetGetApiFn>(GetProcAddress(module, "KenshiCoopNet_GetApi"));
        if (!getApi || getApi(COOP_NET_ABI_VERSION, sizeof(api), &api) != COOP_NET_OK) {
            FreeLibrary(module); module = nullptr; throw std::runtime_error("selected provider API export/version unavailable");
        }
        CoopNetApi incompatible{};
        require(getApi(COOP_NET_ABI_VERSION + 1, sizeof(incompatible), &incompatible) != COOP_NET_OK &&
                getApi(COOP_NET_ABI_VERSION, sizeof(api), nullptr) != COOP_NET_OK,
                "consumer API handshake accepts unsupported version or NULL table");
        require(api.version == COOP_NET_ABI_VERSION && api.size >= sizeof(api) && api.create && api.start && api.poll &&
                api.send && api.releaseEvent && api.disconnect && api.stop && api.destroy && api.status &&
                api.peerStats && api.prepareHost && api.makeInvite && api.pumpMainThread,
                "selected provider lacks required consumer operations");
        char error[COOP_NET_ERROR_CAPACITY]{};
        context = api.create(error, sizeof(error));
        if (!context) { FreeLibrary(module); module = nullptr; throw std::runtime_error("selected provider context creation failed"); }
    }
    ~LoadedProvider() {
        if (context) { api.stop(context); api.destroy(context); }
        if (module) FreeLibrary(module);
    }
};
struct OwnedEvent {
    LoadedProvider& provider;
    CoopNetEvent event{};
    bool valid = false;
    explicit OwnedEvent(LoadedProvider& owner) : provider(owner) { event.size = sizeof(event); }
    ~OwnedEvent() { if (valid) provider.api.releaseEvent(provider.context, &event); }
};
Bytes encodeEvent(const CoopNetEvent& event) {
    Bytes answer;
    put32(answer, event.type); put32(answer, event.connection); put32(answer, event.traffic);
    put64(answer, event.generation); put32(answer, static_cast<uint32_t>(event.reason));
    putString(answer, event.detail);
    if (event.type == COOP_NET_EVENT_MESSAGE) {
        require(event.data && event.message && event.dataSize <= COOP_NET_MAX_MESSAGE,
                "provider receive ownership/payload violates contract");
        const auto* bytes = static_cast<const uint8_t*>(event.data);
        answer.insert(answer.end(), bytes, bytes + event.dataSize);
    } else require(event.dataSize == 0, "non-message event contains message bytes");
    return answer;
}
int childMain(bool hostRole, const std::wstring& providerPath, const std::filesystem::path& directory) {
    ModuleWatch moduleWatch(_wcsicmp(std::filesystem::path(providerPath).filename().c_str(), L"KenshiCoopNetUDP.dll") == 0);
    LoadedProvider provider(providerPath);
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
    const std::string profile = utf8((directory / L"credential.bin").wstring());
    std::deque<Bytes> queued;
    size_t queuedBytes = 0;
    const auto lifetimeDeadline = Clock::now() + std::chrono::seconds(240);
    struct HeldMessage {
        OwnedEvent owned;
        Bytes snapshot;
        explicit HeldMessage(LoadedProvider& owner) : owned(owner) {}
    };
    std::vector<std::unique_ptr<HeldMessage>> held;
    uint32_t holdRemaining = 0;
    bool running = true;
    bool polling = true;
    while (running && Clock::now() < lifetimeDeadline) {
        DWORD available = 0;
        require(PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr) != FALSE, "child control pipe lost");
        if (available) {
            Bytes command = readFrame(input, false);
            Reader reader{command};
            const Op op = static_cast<Op>(reader.u32());
            int32_t result = COOP_NET_OK;
            Bytes answer;
            char error[COOP_NET_ERROR_CAPACITY]{};
            switch (op) {
            case Op::Prepare:
                reader.done();
                result = provider.api.prepareHost(profile.c_str(), error, sizeof(error));
                break;
            case Op::Invite: {
                reader.done();
                std::array<char, COOP_NET_INVITE_CAPACITY> invitation{};
                result = provider.api.makeInvite(profile.c_str(), invitation.data(), static_cast<uint32_t>(invitation.size()),
                                                  error, sizeof(error));
                if (result == COOP_NET_OK) {
                    const auto end = std::find(invitation.begin(), invitation.end(), '\0');
                    require(end != invitation.end(), "invitation output has no terminating NUL");
                    putString(answer, std::string(invitation.begin(), end));
                }
                // Deliberately never emit provider error/detail: credentials can
                // appear in badly implemented diagnostics, including on failure.
                break;
            }
            case Op::Start: {
                CoopNetConfig config{};
                config.size = sizeof(config); config.mode = reader.u32(); config.host = hostRole ? 1 : 0;
                config.port = reader.u32(); config.generation = reader.u64();
                const std::string address = reader.string(), invitation = reader.string();
                reader.done();
                config.connectTimeoutMs = 7000; config.connectedTimeoutMs = 15000;
                config.sendBufferBytes = 1048576; config.bulkQueueBytes = 600000;
                config.maxPeers = 3;
                if (config.mode == COOP_NET_UDP) {
                    config.address = address.c_str();
                    config.profilePath = hostRole ? profile.c_str() : nullptr;
                    config.joinInvite = hostRole || invitation.empty() ? nullptr : invitation.c_str();
                } else {
                    // Valid selected-mode config: fail only because the
                    // game-owned Steam runtime is absent, not UDP fields.
                    config.remoteSteamId = 76561198000000001ull;
                }
                result = provider.api.start(provider.context, &config, error, sizeof(error));
                if (result != COOP_NET_OK) putString(answer, error);
                break;
            }
            case Op::Poll:
                reader.done();
                if (queued.empty()) result = 0;
                else {
                    result = 1; answer = std::move(queued.front()); queued.pop_front(); queuedBytes -= answer.size();
                }
                break;
            case Op::Send: {
                const uint32_t connection = reader.u32(), traffic = reader.u32();
                Bytes data = reader.rest();
                result = provider.api.send(provider.context, connection, traffic, data.data(), static_cast<uint32_t>(data.size()));
                // A real consumer may immediately overwrite/free its borrowed
                // buffer. Expected bytes live independently in the controller.
                std::fill(data.begin(), data.end(), uint8_t(0xa7));
                break;
            }
            case Op::SetPolling: {
                const uint32_t enabled = reader.u32(); reader.done();
                require(enabled <= 1, "invalid test consumer polling flag");
                polling = enabled != 0;
                break;
            }
            case Op::DelayedSend: {
                const uint32_t connection = reader.u32(), traffic = reader.u32(), delay = reader.u32();
                require(delay <= 5000, "test consumer delay exceeds bound");
                const Bytes data = reader.rest();
                Sleep(delay);
                result = provider.api.send(provider.context, connection, traffic, data.data(), static_cast<uint32_t>(data.size()));
                break;
            }
            case Op::Burst: {
                const uint32_t connection = reader.u32(), traffic = reader.u32(), count = reader.u32();
                require(count <= 128, "control burst count exceeds bound");
                for (uint32_t i = 0; i < count; ++i) {
                    const uint32_t length = reader.u32();
                    require(reader.at <= command.size() && length <= command.size() - reader.at, "truncated burst payload");
                    const int32_t sent = provider.api.send(provider.context, connection, traffic,
                                                          command.data() + reader.at, length);
                    std::fill(command.begin() + reader.at, command.begin() + reader.at + length, uint8_t(0xa7));
                    reader.at += length; put32(answer, static_cast<uint32_t>(sent));
                }
                reader.done();
                break;
            }
            case Op::Stats: {
                const uint32_t connection = reader.u32(); reader.done();
                CoopNetPeerStats stats{};
                stats.size = sizeof(stats);
                result = provider.api.peerStats(provider.context, connection, &stats);
                if (result == COOP_NET_OK) {
                    put32(answer, stats.pendingReliableBytes); put32(answer, stats.pendingStateBytes);
                    put32(answer, stats.pendingBulkBytes); put32(answer, stats.unackedReliableBytes);
                }
                break;
            }
            case Op::Status: {
                reader.done();
                CoopNetStatus status{};
                status.size = sizeof(status);
                result = provider.api.status(provider.context, &status);
                if (result == COOP_NET_OK) {
                    put32(answer, status.mode); put64(answer, status.generation); put64(answer, status.selfSteamId);
                    put32(answer, status.listening); put32(answer, status.connections);
                }
                break;
            }
            case Op::Disconnect: {
                const uint32_t connection = reader.u32(); reader.done();
                result = provider.api.disconnect(provider.context, connection, 1000, "integration requested disconnect");
                break;
            }
            case Op::InvalidSend: {
                const uint32_t connection = reader.u32(); reader.done();
                const Bytes bytes{0x00, 0xff, 0x19};
                require(provider.api.send(provider.context, connection, GAME_RELIABLE, nullptr, 9) == COOP_NET_INVALID,
                        "NULL borrowed send buffer accepted");
                require(provider.api.send(provider.context, connection, 777, bytes.data(),
                        static_cast<uint32_t>(bytes.size())) == COOP_NET_INVALID, "unknown traffic class accepted");
                const Bytes tooLarge(COOP_NET_MAX_MESSAGE + 1, 0x7f);
                require(provider.api.send(provider.context, connection, GAME_RELIABLE, tooLarge.data(),
                        static_cast<uint32_t>(tooLarge.size())) == COOP_NET_INVALID, "oversized message accepted");
                require(provider.api.send(provider.context, 0, GAME_RELIABLE, bytes.data(),
                        static_cast<uint32_t>(bytes.size())) != COOP_NET_OK, "invalid connection accepted");
                break;
            }
            case Op::MalformedStart: {
                reader.done();
                CoopNetConfig config{};
                config.size = sizeof(config); config.mode = COOP_NET_UDP; config.host = hostRole ? 1 : 0;
                config.port = 39001; config.generation = 9901;
                config.address = "127.0.0.1"; config.profilePath = profile.c_str();
                config.maxPeers = 3; config.sendBufferBytes = 1048576; config.bulkQueueBytes = 600000;
                config.connectTimeoutMs = 7000; config.connectedTimeoutMs = 15000;
                require(provider.api.start(provider.context, nullptr, error, sizeof(error)) == COOP_NET_INVALID,
                        "NULL config accepted");
                config.size = 1;
                require(provider.api.start(provider.context, &config, error, sizeof(error)) == COOP_NET_INVALID,
                        "truncated config accepted");
                config.size = sizeof(config); config.mode = 777;
                require(provider.api.start(provider.context, &config, error, sizeof(error)) == COOP_NET_INVALID,
                        "invalid transport mode accepted");
                config.mode = COOP_NET_UDP; config.port = 65536;
                require(provider.api.start(provider.context, &config, error, sizeof(error)) == COOP_NET_INVALID,
                        "overflowing UDP port accepted");
                config.port = 39001; config.address = "definitely-not-a-numeric-address";
                require(provider.api.start(provider.context, &config, error, sizeof(error)) == COOP_NET_INVALID,
                        "nonnumeric UDP address accepted");
                CoopNetStatus status{};
                status.size = sizeof(status);
                require(provider.api.status(provider.context, &status) == COOP_NET_OK &&
                        !status.listening && !status.connections, "malformed start leaves network active");
                break;
            }
            case Op::Pump:
                reader.done();
                require(provider.api.pumpMainThread != nullptr, "missing explicit Steam pump operation");
                result = provider.api.pumpMainThread();
                break;
            case Op::HoldEvent: {
                const uint32_t count = reader.at == command.size() ? 1 : reader.u32();
                reader.done();
                require(!holdRemaining && count && count <= 64 && held.size() + count <= 64,
                        "held-message request exceeds bounded consumer fixture");
                holdRemaining = count;
                break;
            }
            case Op::ReleaseHeld: {
                const uint32_t count = reader.at == command.size() ? static_cast<uint32_t>(held.size()) : reader.u32();
                reader.done();
                require(count && count <= held.size(), "no requested provider-owned messages held");
                for (uint32_t i = 0; i < count; ++i) {
                    auto& message = *held[i];
                    require(encodeEvent(message.owned.event) == message.snapshot,
                            "held message bytes/generation changed before release");
                    provider.api.releaseEvent(provider.context, &message.owned.event);
                    message.owned.valid = false;
                }
                held.erase(held.begin(), held.begin() + count);
                break;
            }
            case Op::Stop:
                reader.done(); provider.api.stop(provider.context);
                // Events already received/copied belong to this consumer. Clear
                // them; provider events NOT previously polled must be epoch-fenced.
                queued.clear(); queuedBytes = 0;
                break;
            case Op::Modules:
                reader.done();
                put32(answer, moduleWatch.steamLoads.load());
                put32(answer, moduleWatch.otherProviderLoads.load());
                {
                    const Bytes modules = moduleNames();
                    answer.insert(answer.end(), modules.begin(), modules.end());
                }
                break;
            case Op::Exit:
                require(held.empty() && !holdRemaining, "consumer failed to release held messages before destroy");
                reader.done(); provider.api.stop(provider.context);
                provider.api.destroy(provider.context); provider.context = nullptr;
                require(FreeLibrary(provider.module) != FALSE, "selected provider DLL unload fails");
                provider.module = nullptr;
                moduleWatch.requireExclusive();
                running = false;
                break;
            default: throw std::runtime_error("unknown child control operation");
            }
            Bytes response;
            put32(response, static_cast<uint32_t>(result));
            response.insert(response.end(), answer.begin(), answer.end());
            writeFrame(output, response, false);
        }
        if (!running) break;
        for (unsigned batch = 0; polling && batch < 64; ++batch) {
            OwnedEvent event(provider);
            const int32_t polled = provider.api.poll(provider.context, &event.event);
            require(polled >= 0, "selected provider poll returns error");
            if (!polled) break;
            event.valid = true;
            Bytes encoded = encodeEvent(event.event);
            if (holdRemaining && event.event.type == COOP_NET_EVENT_MESSAGE) {
                auto message = std::make_unique<HeldMessage>(provider);
                message->owned.event = event.event; message->owned.valid = true; event.valid = false;
                message->snapshot = encoded;
                held.push_back(std::move(message)); --holdRemaining;
            }
            require(queued.size() < 2048 && queuedBytes + encoded.size() <= 32u * 1024u * 1024u,
                    "bounded consumer event queue exceeded");
            queuedBytes += encoded.size(); queued.push_back(std::move(encoded));
        }
        Sleep(1);
    }
    require(!running, "child lifetime timeout");
    return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    // Credentials never occur in arguments. Only DLL/directory paths and mode.
    bool controllerMode = false, hostMode = false, clientMode = false;
    std::wstring udp, steam, provider;
    std::filesystem::path work;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--controller") controllerMode = true;
        else if (arg == L"--host") hostMode = true;
        else if (arg == L"--client") clientMode = true;
        else if (i + 1 < argc && arg == L"--udp") udp = std::filesystem::absolute(argv[++i]).wstring();
        else if (i + 1 < argc && arg == L"--steam") steam = std::filesystem::absolute(argv[++i]).wstring();
        else if (i + 1 < argc && arg == L"--provider") provider = std::filesystem::absolute(argv[++i]).wstring();
        else if (i + 1 < argc && arg == L"--work") work = std::filesystem::absolute(argv[++i]);
        else { std::fputs("Invalid transport integration arguments.\n", stderr); return 2; }
    }
    if (unsigned(controllerMode) + unsigned(hostMode) + unsigned(clientMode) != 1 ||
        (controllerMode && (udp.empty() || steam.empty())) || (!controllerMode && (provider.empty() || work.empty()))) {
        std::fputs("Usage: transport_integration --controller --udp <KenshiCoopNetUDP.dll> --steam <KenshiCoopNetSteam.dll>\n", stderr);
        return 2;
    }
    try {
        if (!controllerMode) return childMain(hostMode, provider, work);
        WSADATA winsock{};
        require(WSAStartup(MAKEWORD(2, 2), &winsock) == 0, "initialize controller UDP proxy sockets");
        struct Cleanup { ~Cleanup() { WSACleanup(); } } cleanup;
        std::array<wchar_t, 32768> path{};
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        require(length > 0 && length < path.size(), "locate integration executable");
        return controller(path.data(), udp, steam);
    } catch (const std::exception& error) {
        // Never echo a DLL-generated diagnostic, argument, path or invitation.
        if (controllerMode) std::fprintf(stderr, "FAIL transport prerequisite: %s\n", error.what());
        return 1;
    } catch (...) {
        if (controllerMode) std::fputs("FAIL transport prerequisite: unexpected exception\n", stderr);
        return 1;
    }
}
