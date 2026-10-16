#ifndef KENSHICOOP_LOCK_AUTHORITY_TESTS_H
#define KENSHICOOP_LOCK_AUTHORITY_TESTS_H
#include "../plugin/sync/LockAuthority.h"
#include "../plugin/sync/ChangeGate.h"
#include <cstdio>
inline int testLockAuthority() {
    int failures = 0;
#define LOCK_CHECK(name, condition) do { if (!(condition)) { std::printf("FAIL lock authority: %s\n", name); ++failures; } } while (0)
    using namespace coop::sync;
    LOCK_CHECK("host ignores remote durable state", !doorAcceptHostState(true, 0, 0, 1));
    LOCK_CHECK("join ignores other join lock state", !doorAcceptHostState(false, 2, 0, 1));
    LOCK_CHECK("initial host locked snapshot", doorAcceptHostState(false, 0, 0, 1));
    LOCK_CHECK("host native unlock revision accepted", doorAcceptHostState(false, 0, 1, 2));
    LOCK_CHECK("stale locked snapshot rejected", !doorAcceptHostState(false, 0, 2, 1));
    LOCK_CHECK("duplicate unlock is idempotent", !doorAcceptHostState(false, 0, 2, 2));
    LOCK_CHECK("revision wrap remains ordered", doorAcceptHostState(false, 0, 0xffffffffu, 1));
    LOCK_CHECK("zero revision rejected", !doorAcceptHostState(false, 0, 0, 0));
    LOCK_CHECK("reload relock is corrected at same accepted revision", doorNeedsLockApply(true, 1, 0));
    LOCK_CHECK("unloaded lock stays retryable", !doorNeedsLockApply(false, 1, 0));
    LOCK_CHECK("native host relock remains authoritative", doorNeedsLockApply(true, 0, 1));
    LOCK_CHECK("converged snapshot does not write", !doorNeedsLockApply(true, 0, 0));
    // With another JOIN still connected, only the departed player's slot is
    // reusable. Exercise the same request gate the HOST consumes, not a
    // last-peer reset (which already discards the complete door cache).
    unsigned int seen[4] = { 42, 18, 37, 9 };
    LOCK_CHECK("old departed request sequence rejects a restarted peer",
        !gateSeqAccept(seen[1], 1));
    doorRetirePeerSequences(seen, 1, false);
    LOCK_CHECK("reused player slot can request OPEN from sequence one",
        gateSeqAccept(seen[1], 1));
    LOCK_CHECK("partial departure preserves HOST durable baseline",
        seen[0] == 42 && !doorAcceptHostState(false, 0, seen[0], 41) &&
        !doorAcceptHostState(false, 0, seen[0], 42) &&
        doorAcceptHostState(false, 0, seen[0], 43));
    LOCK_CHECK("surviving JOIN cannot replay its accepted OPEN request",
        seen[2] == 37 && !gateSeqAccept(seen[2], 37) &&
        !gateSeqAccept(seen[2], 36) && gateSeqAccept(seen[2], 38));
    LOCK_CHECK("other surviving JOIN request baseline stays intact",
        seen[3] == 9 && !gateSeqAccept(seen[3], 9));
    seen[1] = 1; // HOST accepted the restarted process's first native OPEN.
    LOCK_CHECK("restarted peer duplicate OPEN remains idempotent",
        !gateSeqAccept(seen[1], 1));
    LOCK_CHECK("restarted peer next OPEN can advance normally",
        gateSeqAccept(seen[1], 2));
    seen[1] = 2;
    LOCK_CHECK("restarted peer stale OPEN cannot undo a newer request",
        !gateSeqAccept(seen[1], 1));
    doorRetirePeerSequences(seen, 4, false);
    LOCK_CHECK("out of range departure cannot release any sequence guard",
        seen[0] == 42 && seen[1] == 2 && seen[2] == 37 && seen[3] == 9);
    doorRetirePeerSequences(seen, 0xffffffffu, true);
    LOCK_CHECK("complete departure retires every old sender namespace",
        doorAcceptHostState(false, 0, seen[0], 1) &&
        gateSeqAccept(seen[1], 1) && gateSeqAccept(seen[2], 1) &&
        gateSeqAccept(seen[3], 1));
#undef LOCK_CHECK
    return failures;
}
#endif
