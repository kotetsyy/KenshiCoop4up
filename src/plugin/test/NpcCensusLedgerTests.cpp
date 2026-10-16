// Standalone native regression target; no SDK, game process or transport needed.
#include "../sync/NpcCensusLedger.h"
#include <cassert>
#include <vector>
struct Snapshot {
    unsigned int ownerId;
    std::vector<unsigned int> hands;
    std::vector<float> pos;
};
int main() {
    coop::NpcCensusLedger<Snapshot> ledger;
    Snapshot host, join;
    host.ownerId=0; host.hands.push_back(101);
    join.ownerId=1;
    ledger.put(host,100);
    ledger.put(join,101); // empty other-author snapshot cannot erase host
    assert(ledger.entries().find(0)->second.snapshot.hands.size()==1);
    assert(ledger.entries().find(1)->second.snapshot.hands.empty());
    ledger.clear();
    ledger.put(join,100); ledger.put(host,101); // inter-author order is irrelevant
    assert(ledger.entries().size()==2);
    assert(ledger.entries().find(0)->second.snapshot.hands[0]==101);
    host.hands.clear(); ledger.put(host,200); // same-author retirement is real
    assert(ledger.entries().find(0)->second.snapshot.hands.empty());
    assert(ledger.fresh(0,5200));
    assert(!ledger.fresh(0,5201));
    ledger.retire(0);
    assert(!ledger.fresh(0,200));
    assert(ledger.fresh(1,200));
    assert(!ledger.fresh(2,200));
    ledger.clear(); // session reset must not retain another session's owners
    assert(!ledger.fresh(0,200));
    ledger.put(host,0xfffffff0UL); // DWORD clock rollover
    assert(ledger.fresh(0,0x10UL));
    ledger.clear();
    host.hands.push_back(100); host.pos.push_back(100.0f);
    join.hands=host.hands; join.pos.push_back(900.0f); // identical key, different author
    ledger.put(host,100);
    ledger.put(join,1090);
    host.pos[0]=120.0f;
    ledger.put(host,1100);
    const coop::NpcCensusLedger<Snapshot>::Entry& motion=ledger.entries().find(0)->second;
    assert(motion.havePrevious);
    assert(motion.snapshot.pos[0]-motion.previous.pos[0]==20.0f);
    assert(motion.received-motion.previousReceived==1000);
    const float speed=(motion.snapshot.pos[0]-motion.previous.pos[0]) /
        ((float)(motion.received-motion.previousReceived)/1000.0f);
    const float destination=motion.snapshot.pos[0] +
        (motion.snapshot.pos[0]-motion.previous.pos[0]);
    assert(speed==20.0f && destination==140.0f);
    join.pos[0]=950.0f; ledger.put(join,1110);
    assert(motion.snapshot.pos[0]-motion.previous.pos[0]==20.0f);
    assert(motion.received-motion.previousReceived==1000);
    assert((motion.snapshot.pos[0]-motion.previous.pos[0]) /
        ((float)(motion.received-motion.previousReceived)/1000.0f)==speed);
    assert(motion.snapshot.pos[0]+
        (motion.snapshot.pos[0]-motion.previous.pos[0])==destination);
    ledger.retire(1); // unrelated disconnect must preserve motion
    assert(motion.received-motion.previousReceived==1000);
    ledger.retire(0); ledger.put(host,1200);
    assert(!ledger.entries().find(0)->second.havePrevious);
    ledger.clear(); ledger.put(host,0xfffffff0UL);
    host.pos[0]=140.0f; ledger.put(host,0x10UL);
    assert(ledger.entries().find(0)->second.havePrevious);
    assert(ledger.entries().find(0)->second.received-
           ledger.entries().find(0)->second.previousReceived==32UL);
    ledger.clear(); ledger.put(host,2000);
    assert(!ledger.entries().find(0)->second.havePrevious);
    return 0;
}
