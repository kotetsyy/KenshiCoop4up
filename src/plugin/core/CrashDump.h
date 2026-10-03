// CrashDump - write a real minidump AT FAULT TIME.
//
// Why this exists: RE_Kenshi installs its own unhandled-exception filter and
// packages a dump from it, but that runs late enough that the faulting frames
// are gone. The 2026-08-03 mid-session-load crash produced a 1.1 MB dump whose
// captured stack held two code addresses and not one engine frame, so the only
// thing recoverable was the exception record (an access violation writing 0x80
// from inside msvcp100) - which cannot say whose code was running. The host's
// dump write failed outright and left a zero-byte file.
//
// Windows' own LocalDumps has the same problem in reverse: it only fires if the
// exception reaches WER, and RE_Kenshi's filter handles it first.
//
// So we take our own, first: our filter runs before RE_Kenshi's (we are loaded
// by it, and SetUnhandledExceptionFilter is last-writer-wins), writes a full
// dump, then CHAINS to whatever filter was installed before us so RE_Kenshi's
// emergency save still happens.
#ifndef KENSHICOOP_CRASHDUMP_H
#define KENSHICOOP_CRASHDUMP_H

namespace coop {
namespace crashdump {

// Install the filter. Dumps land next to the plugin log as
// KenshiCoop_crash_<mode>_<pid>.dmp. Safe to call once at startup; a second
// call is ignored.
void install(const char* dir, const char* modeTag);

} // namespace crashdump
} // namespace coop

#endif
