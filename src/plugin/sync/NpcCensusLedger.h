#ifndef COOP_NPC_CENSUS_LEDGER_H
#define COOP_NPC_CENSUS_LEDGER_H
#include <map>

namespace coop {
// A reliable ordered channel orders each sender, not all senders together.
// Empty snapshots replace only their author's population, never another peer's.
template<class Snapshot> class NpcCensusLedger {
public:
    struct Entry {
        Snapshot snapshot;
        Snapshot previous;
        unsigned long previousReceived;
        bool havePrevious;
        unsigned long received;
        Entry() : previousReceived(0), havePrevious(false), received(0) {}
    };
    typedef std::map<unsigned int, Entry> Entries;
    void clear() { entries_.clear(); }
    void retire(unsigned int owner) { entries_.erase(owner); }
    void put(const Snapshot& snapshot, unsigned long now) {
        const bool existed = entries_.find(snapshot.ownerId) != entries_.end();
        Entry& entry = entries_[snapshot.ownerId];
        entry.havePrevious = existed;
        if (entry.havePrevious) {
            entry.previous = entry.snapshot;
            entry.previousReceived = entry.received;
        }
        entry.snapshot = snapshot;
        entry.received = now;
    }
    bool fresh(unsigned int owner, unsigned long now) const {
        typename Entries::const_iterator it = entries_.find(owner);
        return it != entries_.end() && now - it->second.received <= 5000;
    }
    const Entries& entries() const { return entries_; }
private:
    Entries entries_;
};
}
#endif
