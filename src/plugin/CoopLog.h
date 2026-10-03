// CoopLog - tiny thread-safe file logger for KenshiCoop.
//
// Why a separate logger when KenshiLib already has DebugLog/ErrorLog?
//   * Those route into the engine's kenshi.log, which is overwritten each
//     launch and interleaved with heavy engine spam - awkward for an automated
//     test runner to evaluate.
//   * KenshiLib's UI/log helpers are NOT safe to call off the main thread,
//     whereas this logger guards a FILE* with a CRITICAL_SECTION so the net
//     thread can write too.
//
// Output is a dedicated, per-line-flushed file (so it survives a hard kill),
// with a timestamp + mode tag (HOST/JOIN) on every line. The high-level
// coopLog()/coopErr() wrappers in KenshiCoop.cpp call BOTH this and the
// KenshiLib helpers, so events still appear in kenshi.log as before.

#ifndef KENSHICOOP_COOPLOG_H
#define KENSHICOOP_COOPLOG_H

namespace coop {

// Open the log file at 'path' (truncating any previous run) and remember a
// short mode tag (e.g. "HOST"/"JOIN"). Safe to call once at plugin load.
void logInit(const char* path, const char* modeTag);

// Move logging to a different file mid-run, because the role this session
// actually plays is not known when logInit runs.
//
// The log name is derived from coop_config.json "role", which defaults to
// "host" and is only a REMEMBERED INTENT - the live role is whatever the player
// arms in the F2 panel, possibly minutes later. A fresh install that joins
// therefore wrote its whole session into KenshiCoop_host.log, and the join log
// people were asked for did not exist: not missing, never created under that
// name. Calling this at Connect, once the role is real, gives the session the
// file it will be looked for in.
//
// No-op when `path` is already the open file. Otherwise the old file gets a
// closing line naming the new one (so a half-read log points at its
// continuation) and the new file is opened truncating, like logInit. The mode
// tag on every subsequent line changes with it. Thread-safe.
// Returns true only when the file actually CHANGED, so the caller can re-emit
// a startup banner into the new file without duplicating it into the old one
// on every Connect (those lines are parsed by the log oracles).
bool logRetarget(const char* path, const char* modeTag);

// The wall clock every timestamp in this plugin derives from: milliseconds
// since local midnight PLUS the injected fake skew (see logSetFakeSkewMs).
// Both the log-line "[HH:MM:SS.mmm]" stamps AND the wire time-sync packets
// (NetLink CLOCKSYNC) read THIS function, so an injected skew shifts them
// together - which is exactly what the clock-skew validation relies on: the
// join's log stamps drift by +S while its estimated host-offset reads -S, and
// the oracles' offset correction must recover alignment. Thread-safe.
unsigned long wallClockMs();

// Inject a fake wall-clock skew (ms, may be negative). Set once at startup from
// KENSHICOOP_FAKE_CLOCK_SKEW_MS (join only) BEFORE logInit. 0 = real clock.
void logSetFakeSkewMs(long skewMs);

// Append one INFO/ERROR line (timestamped, tagged) and flush. Thread-safe.
void logLine(const char* msg);
void logErrLine(const char* msg);

#ifdef KENSHICOOP_NET_DIAG
// Client: start reading the current log from byte zero after WELCOME; stop on
// disconnect. Peek copies raw file bytes without advancing the logical offset.
// Commit only bytes whose reliable send succeeded. A new capture (or retarget)
// closes the old read handle and starts a new stream.
void logMirrorCapture(bool enabled);
unsigned logMirrorPeek(char* out, unsigned cap, unsigned __int64* offset);
void logMirrorCommit(unsigned bytes);

// Host: write an ordered stream per join to KenshiCoop_join_<id>_mirror.log
// alongside the host log. Offset zero starts/truncates a new stream. A file
// rotates before a chunk would exceed 16 MiB; offsets remain stream-relative.
bool logRemoteChunk(unsigned peerId, unsigned __int64 offset,
                    const char* data, unsigned bytes);
void logRemoteClose(unsigned peerId);
#endif

// Flush and close the file. Called right before ExitProcess on test self-exit.
void logClose();

} // namespace coop

#endif // KENSHICOOP_COOPLOG_H
