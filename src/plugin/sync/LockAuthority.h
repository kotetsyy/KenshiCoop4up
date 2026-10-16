#ifndef KENSHICOOP_LOCK_AUTHORITY_H
#define KENSHICOOP_LOCK_AUTHORITY_H
namespace coop { namespace sync {
// Only HOST versions durable lock state. Requests never advance this clock.
inline bool doorAcceptHostState(bool isHost, unsigned int owner,
                                unsigned int seen, unsigned int revision) {
    return !isHost && owner == 0 && revision != 0 &&
           (seen == 0 || (revision != seen && (revision - seen) < 0x80000000u));
}
// A live session may reuse a departed JOIN's player slot while other peers
// remain. Its new process restarts request sequences; only that sender's
// guard expires. Durable HOST state and surviving sender guards stay intact.
template <unsigned int Count>
inline void doorRetirePeerSequences(unsigned int (&seen)[Count],
                                   unsigned int owner, bool all) {
    if (all) {
        for (unsigned int i = 0; i < Count; ++i) seen[i] = 0;
    } else if (owner < Count) {
        seen[owner] = 0;
    }
}
// Keep the accepted value after convergence: native unload/load may restore a
// stale save lock without another network edge. No forced-unlocked exception.
inline bool doorNeedsLockApply(bool hasLock, int nativeLocked, int hostLocked) {
    return hasLock && nativeLocked != hostLocked;
}
} }
#endif
