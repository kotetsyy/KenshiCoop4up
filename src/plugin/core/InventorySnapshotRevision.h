#ifndef KENSHICOOP_INVENTORY_SNAPSHOT_REVISION_H
#define KENSHICOOP_INVENTORY_SNAPSHOT_REVISION_H

namespace coop {
// A normal authoritative refresh may reassert a revision. A committed operation
// may override a locally owned pocket once only, even after its item moved away.
struct InventorySnapshotRevision {
    unsigned int received, committed;
    InventorySnapshotRevision() : received(0), committed(0) {}
    bool accept(unsigned int revision, bool transaction) {
        if (transaction && !revision) return false;
        if (received && revision != received &&
            (!revision || revision - received >= 0x80000000u)) return false;
        if (transaction && committed &&
            (revision == committed || revision - committed >= 0x80000000u)) return false;
        received = revision;
        if (transaction) committed = revision;
        return true;
    }
};
}
#endif
