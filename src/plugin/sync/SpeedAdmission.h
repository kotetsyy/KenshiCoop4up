// Speed packet admission shared by the real replicator and native consumers.
#ifndef KENSHICOOP_SPEED_ADMISSION_H
#define KENSHICOOP_SPEED_ADMISSION_H
#include "../../netproto/Wire.h"

namespace coop { namespace sync {
class SpeedAdmission {
public:
    enum Kind { DROP, HEARTBEAT, COMMAND, EFFECTIVE };
    SpeedAdmission() { reset(); }
    void reset() {
        for (unsigned i = 0; i < MAX_PLAYERS; ++i) requests_[i] = 0;
        effective_ = 0;
    }
    Kind accept(const SpeedPacket& p, u32 source, bool isHost) {
        if (source >= MAX_PLAYERS || p.ownerId != source || p.seq == 0) return DROP;
        if (isHost) {
            if (source == 0 || p.type != PKT_SPEED_REQ) return DROP;
            if (!newer(p.seq, requests_[source])) return DROP;
            requests_[source] = p.seq;
            // A stale consensus echo is state/diagnostics, NEVER a player vote.
            return (p.flags & SPEED_COMMAND) ? COMMAND : HEARTBEAT;
        }
        if (source != 0 || p.type != PKT_SPEED_SET || (p.flags & SPEED_COMMAND)) return DROP;
        if (!newer(p.seq, effective_)) return DROP;
        effective_ = p.seq;
        return EFFECTIVE;
    }
private:
    static bool newer(u32 seq, u32 seen) { return seen == 0 || (int)(seq - seen) > 0; }
    u32 requests_[MAX_PLAYERS];
    u32 effective_;
};
} }
#endif
