#ifndef KENSHICOOP_UI_PRESENTATION_TESTS_H
#define KENSHICOOP_UI_PRESENTATION_TESTS_H
#include "../ui/UiPrivacy.h"
#include "../ui/UiSession.h"
#include "../plugin/core/UiModule.h"

static CoopUiSnapshot uiHostSnapshot() {
    CoopUiSnapshot s = {};
    s.structSize = sizeof(s); s.running = s.busy = s.isHost = 1;
    s.phase = COOP_HOSTING; s.worldPhase = COOP_WORLD_READY;
    s.transportSel = s.activeTransport = 1; s.udpIp = "private.example.test"; s.udpPort = 30123;
    s.selfSteamId = 76561198012345678ull; s.playerCount = 2;
    s.players[0].local = 1; std::strcpy(s.players[0].name, "PrivateHost");
    s.players[1].id = 1; std::strcpy(s.players[1].name, "PrivateGuest");
    return s;
}
static void testUiPresentation() {
    std::printf("== native UI privacy/readiness/stop policy ==\n");
    CoopUiSnapshot s = uiHostSnapshot();
    const std::string text = "My Steam ID: 76561198012345678\nHost Steam ID: 42\nHost: private.example.test:30123\n"
        "Peer: 192.168.1.50:27800\n  id 1: 'PrivateGuest', ready: unknown\n"
        "RTT: 12 ms\nError: #10061\nAttempts: 42\nBytes: 1024 / 2048\nKenshiCoop v1.2.3.4 - proto 70\n";
    const std::string display = coopui::redact(text, s, false), report = coopui::redact(text, s, true);
    CHECK("display preserves existing last-four Steam mask", display.find("****5678") != std::string::npos);
    CHECK("display handles short typed Steam config ID", display.find("****42") != std::string::npos);
    CHECK("display never exposes full Steam ID", display.find("76561198012345678") == std::string::npos);
    CHECK("display masks host DNS and peer IPv4", display.find("private.example.test") == std::string::npos && display.find("192.168.1.50") == std::string::npos);
    CHECK("display preserves visible nickname", display.find("PrivateGuest") != std::string::npos);
    CHECK("report anonymizes Steam ID without retained suffix", report.find("76561198012345678") == std::string::npos && report.find("****5678") == std::string::npos);
    CHECK("report anonymizes typed short Steam ID", report.find("Host Steam ID: [Steam ID]") != std::string::npos);
    CHECK("report anonymizes address but keeps ports", report.find("[address]:30123") != std::string::npos && report.find("[address]:27800") != std::string::npos);
    CHECK("report aliases accepted participant by stable slot", report.find("id 1: 'Player 1'") != std::string::npos && report.find("PrivateGuest") == std::string::npos);
    CHECK("privacy keeps dotted version and protocol", report.find("v1.2.3.4 - proto 70") != std::string::npos);
    CHECK("privacy keeps WSA error, RTT and attempts", report.find("#10061") != std::string::npos && report.find("12 ms") != std::string::npos && report.find("Attempts: 42") != std::string::npos);
    CHECK("privacy keeps byte counters", report.find("1024 / 2048") != std::string::npos);
    CHECK("brand text does not exempt an error address from privacy",
        coopui::redact("Error: KenshiCoop peer 192.168.1.50 refused", s, true).find("192.168.1.50") == std::string::npos);
    std::strcpy(s.players[1].name, "1");
    const std::string numeric = coopui::redact("RTT: 1 ms\n  id 1: '1', ready: unknown", s, true);
    CHECK("numeric nickname is anonymous without rewriting counters", numeric.find("RTT: 1 ms") != std::string::npos && numeric.find("id 1: 'Player 1'") != std::string::npos);
    CHECK("unlisted diagnostic participant remains anonymous", coopui::redact("  id 3: 'UnknownGuest', ready: unknown", s, true).find("UnknownGuest") == std::string::npos);
    std::strcpy(s.players[0].name, "Same"); std::strcpy(s.players[1].name, "Same");
    CHECK("duplicate names do not collapse diagnostic slot aliases", coopui::redact("  id 0: 'Same', ready: yes\n  id 1: 'Same', ready: no", s, true).find("id 1: 'Player 1'") != std::string::npos);
    std::strcpy(s.players[0].name, "Player 1");
    CHECK("a nickname cannot rewrite an already anonymized diagnostic slot", coopui::redact("  id 1: 'Same', ready: no", s, true).find("id 1: 'Player 1'") != std::string::npos);
    std::strcpy(s.players[1].name, "O'Brien");
    CHECK("apostrophe in nickname is fully anonymized", coopui::redact("  id 1: 'O'Brien', ready: no", s, true).find("O'Brien") == std::string::npos);
    s = uiHostSnapshot();
    std::strcpy(s.players[1].name, "READY");
    CHECK("participant name cannot replace a technical world phase",
        coopui::redact("World: READY\n  id 1: 'READY', ready: yes", s, true) ==
        "World: READY\n  id 1: 'Player 1', ready: yes");
    std::strcpy(s.players[1].name, "UDP");
    CHECK("participant name cannot replace transport metadata",
        coopui::redact("Transport: UDP", s, true) == "Transport: UDP");
    std::strcpy(s.players[1].name, "failed");
    CHECK("participant name cannot replace a raw error",
        coopui::redact("Error: #10061 connection failed", s, true) == "Error: #10061 connection failed");
    CHECK("cached host DNS is masked independently of the current endpoint",
        coopui::redact("\xD0\x90\xD0\xB4\xD1\x80\xD0\xB5\xD1\x81 \xD1\x85\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0: old.private.test:30123", s, false) ==
        "\xD0\x90\xD0\xB4\xD1\x80\xD0\xB5\xD1\x81 \xD1\x85\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0: [address]:30123");
    s.udpIp = "27800";
    const std::string numericHost = "Host: 27800:27800\nRTT: 27800 ms\nError: #27800";
    const std::string numericSafe = "Host: [address]:27800\nRTT: 27800 ms\nError: #27800";
    CHECK("numeric host redaction preserves its port and unrelated metadata",
        coopui::redact(numericHost, s, true) == numericSafe &&
        coopui::redact(numericHost, s, false) == numericSafe);

    CHECK("cached IPv6 host is redacted without removing its port",
        coopui::redact("Host: [2001:db8::20]:27800", s, true) == "Host: [address]:27800");
    CHECK("diagnostic peer IPv6 is private even after endpoint edit",
        coopui::redact("Peer: [2001:db8::20]:27800", s, true) == "Peer: [address]:27800");
    CoopUiCommand command = {}; command.structSize = sizeof(command);
    command.kind = COOP_UI_CONNECT; command.settings.udpPort = 27800;
    std::strcpy(command.settings.udpIp, "127.0.0.1");
    CHECK("UDP join accepts numeric endpoint without credentials", coop::uimodule::validCommand(command));
    std::strcpy(command.settings.udpIp, "2001:db8::1");
    CHECK("UDP join accepts numeric IPv6 endpoint", coop::uimodule::validCommand(command));
    const char* badEndpoints[] = {"", "play.example.test", "256.1.2.3", "2001:::1"};
    for (size_t i = 0; i < sizeof(badEndpoints) / sizeof(badEndpoints[0]); ++i) {
        std::strcpy(command.settings.udpIp, badEndpoints[i]);
        CHECK("UDP join rejects invalid numeric endpoint", !coop::uimodule::validCommand(command));
    }
    command.settings.isHost = 1; command.settings.udpIp[0] = 0;
    CHECK("UDP host needs only its listening port", coop::uimodule::validCommand(command));
    const int badPorts[] = {0, -1, 65536};
    for (size_t i = 0; i < sizeof(badPorts) / sizeof(badPorts[0]); ++i) {
        command.settings.udpPort = badPorts[i];
        CHECK("UDP host rejects invalid listening port", !coop::uimodule::validCommand(command));
    }
    command.settings.udpPort = 65535;
    CHECK("UDP host accepts maximum listening port", coop::uimodule::validCommand(command));
    command.settings.isHost = 0; command.settings.udpPort = 1;
    std::strcpy(command.settings.udpIp, "127.0.0.1");
    CHECK("UDP join accepts minimum remote port", coop::uimodule::validCommand(command));
    command.settings.udpPort = 0;
    CHECK("UDP join rejects zero remote port", !coop::uimodule::validCommand(command));
    command.settings.udpPort = 27800; command.settings.isHost = 2;
    CHECK("invalid host mode cannot cross UI boundary", !coop::uimodule::validCommand(command));
    command.settings.isHost = 0; command.settings.useSteam = 2;
    CHECK("invalid transport mode cannot cross UI boundary", !coop::uimodule::validCommand(command));
    command.settings.useSteam = 0;
    std::memset(command.settings.udpIp, 'x', sizeof(command.settings.udpIp));
    CHECK("unterminated endpoint cannot cross UI boundary", !coop::uimodule::validCommand(command));
    std::strcpy(command.settings.udpIp, "127.0.0.1");
    command.kind = 999;
    CHECK("unknown UI command is rejected", !coop::uimodule::validCommand(command));
    s = uiHostSnapshot();
    CHECK("valid UI snapshot is consumable", coop::uimodule::validSnapshot(s));
    s.worldPhase = 999;
    CHECK("unknown world enum cannot fabricate READY", !coop::uimodule::validSnapshot(s));
    s = uiHostSnapshot(); std::memset(s.players[1].name, 'x', sizeof(s.players[1].name));
    CHECK("unterminated roster name cannot cross UI boundary", !coop::uimodule::validSnapshot(s));
    s = uiHostSnapshot(); s.playerCount = 1;
    CHECK("host session without gameplay guests is not READY", !coopui::gameReady(s));
    s = uiHostSnapshot(); s.isHost = 0; s.phase = COOP_HANDSHAKING;
    CHECK("client handshake cannot substitute for gameplay readiness", !coopui::gameReady(s));
    s = uiHostSnapshot();
    CHECK("host local world is not all-ready with unknown guest", !coopui::gameReady(s));
    s.players[1].worldReadyKnown = 1;
    CHECK("known loading guest is not all-ready", !coopui::gameReady(s));
    s.players[1].worldReady = 1;
    CHECK("host and acknowledged guests can be ready", coopui::gameReady(s));
    s.worldPhase = COOP_WORLD_NONE;
    CHECK("unknown host world never becomes ready from guest count", !coopui::gameReady(s));
    s.worldPhase = COOP_WORLD_FAILED;
    CHECK("failed host world never becomes ready from guest count", !coopui::gameReady(s));
    s.isHost = 0; s.phase = COOP_CONNECTED; s.worldPhase = COOP_WORLD_NONE;
    CHECK("client network welcome alone is not world readiness", !coopui::gameReady(s));
    s.worldPhase = COOP_WORLD_READY;
    CHECK("client observed live world and network are ready", coopui::gameReady(s));
    s.phase = COOP_RECONNECTING;
    CHECK("world retained after link loss is not ready", !coopui::gameReady(s));

    typedef coopui::StopConfirmation Stop;
    Stop membership; CoopUiSnapshot memberChange = uiHostSnapshot();
    membership.observe(memberChange); membership.request(); ++memberChange.peerSteamId;
    membership.observe(memberChange);
    CHECK("host guest membership change is not a session replacement", membership.open());
    Stop stop; s = uiHostSnapshot(); stop.observe(s);
    CHECK("accepted guest not yet READY still needs confirmation", stop.request() == Stop::PROMPT && stop.open());
    CHECK("repeated stop does not issue a second action", stop.request() == Stop::NONE);
    stop.cancel(); CHECK("Cancel/Esc cannot confirm or stop", !stop.open() && !stop.confirm() && !stop.stopping());
    stop.request(); s.playerName = "renamed"; s.players[1].worldReadyKnown = 1; stop.observe(s);
    CHECK("nickname and readiness updates preserve same-session prompt", stop.open());
    CHECK("explicit confirmation succeeds once", stop.confirm() && stop.stopping());
    CHECK("double confirm/repeated click cannot stop again", !stop.confirm() && stop.request() == Stop::NONE);
    s.running = s.busy = 0; s.phase = COOP_OFFLINE; stop.observe(s);
    s = uiHostSnapshot(); stop.observe(s); stop.request(); s.udpPort = 30124; stop.observe(s);
    CHECK("endpoint replacement invalidates old prompt", !stop.open() && !stop.confirm());
    stop.request(); ++s.sessionGeneration; stop.observe(s);
    CHECK("generation replacement invalidates prompt even without offline snapshot", !stop.open() && !stop.confirm());
    stop.request(); s.phase = COOP_STARTING; stop.observe(s);
    CHECK("observed restart invalidates old prompt", !stop.open() && !stop.confirm());
    s = uiHostSnapshot(); stop.observe(s); stop.request(); s.running = s.busy = 0; s.phase = COOP_OFFLINE; stop.observe(s);
    CHECK("session disappearance invalidates old prompt", !stop.open() && !stop.confirm());
    s = uiHostSnapshot(); s.playerCount = 1; s.peerPresent = 1; stop.observe(s);
    CHECK("solo host ignores stale game-thread peer flag", stop.request() == Stop::DISCONNECT && !stop.open());
    Stop client; s = uiHostSnapshot(); s.isHost = 0; s.phase = COOP_CONNECTED; client.observe(s);
    CHECK("client departure retains direct disconnect behavior", client.request() == Stop::DISCONNECT && !client.open());
}
#endif
