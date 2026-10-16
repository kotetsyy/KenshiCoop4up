#ifndef KENSHICOOP_PROCEDURAL_WORLD_TESTS_H
#define KENSHICOOP_PROCEDURAL_WORLD_TESTS_H
#include "../plugin/core/ProceduralWorldState.h"
#include <cstdio>
#include <cstring>
#include <limits>

namespace procedural_test {
inline coop::ProceduralObject bed(coop::u64 id) {
    coop::ProceduralObject o = {}; o.id = id; o.category = coop::PROC_CATEGORY_CAMP_BED;
    std::strcpy(o.sid, "16845-nodes_otto1.mod"); o.flags = coop::PROC_OUTSIDE;
    o.rotationW = 1.0f;
    o.stateFlags = coop::PROC_COMPLETE; o.constructionProgress = 1.0f; return o;
}
inline coop::ProceduralPacket packet(coop::u8 op, coop::u32 epoch,
                                     const std::vector<coop::ProceduralObject>& rows) {
    coop::ProceduralPacket p = {}; p.type = coop::PKT_PROCEDURAL; p.worldToken = 11;
    p.loadId = 3; p.regionX = -7; p.regionZ = 5; p.epoch = epoch; p.operation = op;
    p.count = (coop::u32)rows.size(); p.digest = coop::procedural::snapshotDigest(rows); return p;
}
inline void check(bool ok, const char* name, int& failures) {
    if (!ok) { std::printf("FAIL procedural: %s\n", name); ++failures; }
}
}

inline int testProceduralWorldState() {
    using namespace coop; using namespace coop::procedural; using namespace procedural_test;
    int failures = 0;
#define PROC_CHECK(name, condition) procedural_test::check((condition), name, failures)
    PROC_CHECK("native loaded region policy includes reported outside Hub cell", managedBedRegion(22,32));
    PROC_CHECK("native loaded region policy has no coordinate allowlist", managedBedRegion(-7,5) && managedBedRegion(0,0));
    {
        std::vector<ProceduralObject> outsideRows; outsideRows.push_back(bed(41));
        RegionSnapshot outside; outside.reset(11,3,22,32);
        ProceduralPacket p=packet(PROC_BEGIN,7,outsideRows); p.regionX=22; p.regionZ=32;
        PROC_CHECK("outside Hub snapshot begins", outside.accept(p) && !outside.complete());
        p.operation=PROC_ROW; p.object=outsideRows[0];
        PROC_CHECK("outside Hub snapshot row does not certify before END", outside.accept(p) && !outside.complete());
        ProceduralPacket wrong=p; wrong.regionX=20;
        PROC_CHECK("Hub traffic cannot contaminate outside snapshot", !outside.accept(wrong) && !outside.complete());
        p.operation=PROC_END;
        PROC_CHECK("outside Hub exact snapshot completes", outside.accept(p) && outside.complete());
        p.loadId++;
        PROC_CHECK("outside Hub reconnect fence rejects old assembly scope", !outside.accept(p) && outside.complete());
    }
    {
        // Consumer barrier: a queued native generation/reset between scans must
        // revoke READY even when wire epoch, ACK and membership are unchanged.
        NativeCensusFence census;
        PROC_CHECK("uncaptured native scope cannot certify", !census.current(7));
        census.begin(7); census.finish(true,7);
        PROC_CHECK("stable complete native scope certifies its revision", census.current(7));
        PROC_CHECK("deferred native work revokes an already committed scope immediately", !census.current(8));
        census.begin(8);
        PROC_CHECK("reload begins without inheriting the old complete census", !census.current(8));
        census.finish(false,8);
        PROC_CHECK("incomplete native loaded registry cannot recertify old ACKs", !census.current(8));
        census.begin(8); census.finish(true,9);
        PROC_CHECK("generation crossing the full native census cannot certify", !census.current(9));
        census.begin(9); census.finish(true,9);
        PROC_CHECK("complete stable reload can recertify the current scope", census.current(9));
        census.reset();
        PROC_CHECK("reconnect requires a new census even at the same native revision", !census.current(9));
        std::vector<ProceduralObject> nativeRows; nativeRows.push_back(bed(81));
        RegionSnapshot region; region.reset(11,3,-7,5);
        ProceduralPacket p=packet(PROC_BEGIN,4,nativeRows); region.accept(p);
        p.operation=PROC_ROW; p.object=nativeRows[0]; region.accept(p);
        p.operation=PROC_END; region.accept(p); region.markAcknowledged();
        PeerScope peer; peer.reset(11,3);
        p=packet(PROC_REQUEST,0,nativeRows); p.count=p.digest=0; peer.accept(p);
        p.operation=PROC_SCOPE_COMMIT; p.regionX=p.regionZ=0; p.count=1; peer.accept(p);
        census.begin(10); census.finish(true,10);
        PROC_CHECK("current nonempty acknowledged region and scope can certify",
            region.acknowledged() && peer.committed() && census.current(10));
        PROC_CHECK("unchanged wire ACK and commit cannot hide new native deferred work",
            region.acknowledged() && peer.committed() && !census.current(11));
        p=packet(PROC_RETIRE,0,nativeRows); p.count=p.digest=0;
        PROC_CHECK("native unload retires the old committed membership", peer.accept(p) && !peer.committed());
        region.reset(11,3,-7,5); census.reset();
        PROC_CHECK("native unload discards the old complete assembly and census",
            !region.complete() && !region.acknowledged() && !census.current(11));
        p=packet(PROC_REQUEST,0,nativeRows); p.count=p.digest=0; peer.accept(p);
        p=packet(PROC_BEGIN,5,nativeRows); region.accept(p);
        p.operation=PROC_ROW; p.object=nativeRows[0]; region.accept(p);
        p.operation=PROC_END; region.accept(p); region.markAcknowledged();
        p.operation=PROC_SCOPE_COMMIT; p.regionX=p.regionZ=0; p.epoch=p.digest=0; p.count=1; peer.accept(p);
        census.begin(11); census.finish(true,11);
        PROC_CHECK("reloaded nonempty epoch certifies only after native recapture",
            region.epoch()==5 && region.acknowledged() && peer.committed() && census.current(11));
        region.reset(11,4,-7,5); peer.reset(11,4); census.reset();
        p=packet(PROC_END,5,nativeRows);
        PROC_CHECK("reconnect rejects old-load completion even with the same native revision",
            !region.accept(p) && !region.acknowledged() && !peer.committed() && !census.current(11));
    }
    PeerScope scope; scope.reset(11,3);
    std::vector<ProceduralObject> noRows;
    ProceduralPacket request=packet(PROC_REQUEST,0,noRows);
    request.count=request.digest=0;
    ProceduralPacket commit=request; commit.operation=PROC_SCOPE_COMMIT;
    commit.regionX=commit.regionZ=0; commit.count=1;
    PROC_CHECK("old known set cannot certify before same-lane commit", scope.accept(request) && !scope.committed());
    PROC_CHECK("current set commit accepted", scope.accept(commit) && scope.committed());
    PROC_CHECK("identical request retry preserves certification", scope.accept(request) && scope.committed());
    PROC_CHECK("identical scope commit retry is idempotent", scope.accept(commit) && scope.committed());
    ProceduralPacket nextRequest=request; nextRequest.regionX++;
    PROC_CHECK("new membership invalidates old commit", scope.accept(nextRequest) && !scope.committed());
    PROC_CHECK("old set size cannot recommit expanded scope", !scope.accept(commit) && !scope.committed());
    commit.count=2; PROC_CHECK("expanded set commits", scope.accept(commit) && scope.committed());
    ProceduralPacket retire=request; retire.operation=PROC_RETIRE;
    PROC_CHECK("unloaded requested region retires", scope.accept(retire) && !scope.committed());
    commit.count=1; PROC_CHECK("remaining set commits without historical obligation", scope.accept(commit) && scope.committed());
    PROC_CHECK("identical retirement retry is idempotent", scope.accept(retire) && scope.committed());
    ProceduralPacket staleCommit=commit; staleCommit.loadId++;
    PROC_CHECK("scope commit load fence", !scope.accept(staleCommit) && scope.committed());
    retire.worldToken++; PROC_CHECK("retirement world fence", !scope.accept(retire) && scope.committed());
    scope.reset(11,4); PROC_CHECK("new load needs new membership and commit", !scope.committed() && !scope.accept(commit));
    scope.reset(11,3);
    PROC_CHECK("empty membership cannot certify", !scope.accept(commit) && !scope.committed());
    ProceduralPacket emptyCommit=commit; emptyCommit.count=0;
    PROC_CHECK("empty managed membership still requires explicit commit", !scope.committed());
    PROC_CHECK("explicit empty managed set can certify", scope.accept(emptyCommit) && scope.committed());
    PROC_CHECK("repeated empty commit is idempotent", scope.accept(emptyCommit) && scope.committed());
    PROC_CHECK("loading a managed region invalidates empty certification", scope.accept(request) && !scope.committed());
    PROC_CHECK("stale empty commit cannot hide a live managed obligation", !scope.accept(emptyCommit) && !scope.committed());
    ProceduralPacket finalRetire=request; finalRetire.operation=PROC_RETIRE;
    PROC_CHECK("last managed region retires before empty commit", scope.accept(finalRetire) && !scope.committed());
    PROC_CHECK("empty set recommits only after ordered retirement", scope.accept(emptyCommit) && scope.committed());
    scope.reset(11,3);
    bool bounded=true;
    for (unsigned region=0;region<256;++region) {
        nextRequest=request; nextRequest.regionX=(i32)region;
        bounded=scope.accept(nextRequest) && bounded;
    }
    nextRequest.regionX=256;
    PROC_CHECK("scope bounds the current region set", bounded && !scope.accept(nextRequest));
    nextRequest=request; nextRequest.regionX=0;
    PROC_CHECK("full scope still accepts identical request", scope.accept(nextRequest));
    commit.count=256; PROC_CHECK("full current scope commits", scope.accept(commit) && scope.committed());
    ProceduralObject a = bed(1), b = a;
    PROC_CHECK("valid native bed descriptor", validObject(a));
    ProceduralObject changedLifecycle=a; changedLifecycle.stateFlags^=PROC_PAUSED;
    changedLifecycle.constructionProgress=0.5f;
    PROC_CHECK("native reload preserves placement identity across mutable construction state",
        samePlacement(a,changedLifecycle) && !sameObject(a,changedLifecycle));
    changedLifecycle.x+=1.0f;
    PROC_CHECK("different native placement cannot inherit an unloaded logical ID", !samePlacement(a,changedLifecycle));
    ProceduralObject saved = a; saved.constructionProgress = 5;
    PROC_CHECK("native saved raw progress preserves unchanged state without weakening wire limits",
        sameNativeState(saved,saved) && !sameObject(saved,saved) && !validDescriptor(saved));
    ProceduralObject changedSaved = saved; changedSaved.constructionProgress = 1;
    PROC_CHECK("native preservation detects raw construction progress loss", !sameNativeState(saved,changedSaved));
    changedSaved = saved; changedSaved.floor++;
    PROC_CHECK("native preservation detects floor changes", !sameNativeState(saved,changedSaved));
    changedSaved = saved; changedSaved.rotationX = 0.6f; changedSaved.rotationW = 0.8f;
    PROC_CHECK("native preservation detects pitch changes", !sameNativeState(saved,changedSaved));
    changedSaved = saved; changedSaved.rotationW = -1;
    PROC_CHECK("native preservation accepts equivalent quaternion sign", sameNativeState(saved,changedSaved));
    changedSaved = saved; changedSaved.constructionProgress = -1;
    PROC_CHECK("native preservation rejects negative progress", !sameNativeState(saved,changedSaved));
    changedSaved.constructionProgress = std::numeric_limits<float>::quiet_NaN();
    PROC_CHECK("native preservation rejects nonfinite progress", !sameNativeState(saved,changedSaved));
    changedSaved = saved; changedSaved.constructionProgress = 1.0e15f;
    PROC_CHECK("native preservation rejects progress that overflows canonical integers", !sameNativeState(changedSaved,changedSaved));
    b.id = 9; PROC_CHECK("local logical identity ignored for equivalence", sameObject(a,b));
    b.x = 0.004f; PROC_CHECK("centimeter canonicalization", sameObject(a,b));
    b.x = 0.006f; PROC_CHECK("centimeter boundary changes geometry", !sameObject(a,b));
    b = a; b.id = 0; PROC_CHECK("equivalence ignores even absent local identity", sameObject(a,b) && !validObject(b));
    b = a; b.heading = -6.283185307179586f; PROC_CHECK("negative angular wrap", sameObject(a,b));
    b = a; b.heading = 0.00006f; PROC_CHECK("yaw canonical boundary", !sameObject(a,b));
    b = a; b.rotationX = 0.6f; b.rotationW = 0.8f;
    PROC_CHECK("pitch mismatch with identical heading", validObject(b) && !sameObject(a,b));
    b = a; b.rotationZ = 0.6f; b.rotationW = 0.8f;
    PROC_CHECK("roll mismatch with identical heading", validObject(b) && !sameObject(a,b));
    b = a; b.rotationY = 0.6f; b.rotationW = 0.8f;
    PROC_CHECK("quaternion yaw participates independently of heading", validObject(b) && !sameObject(a,b));
    ProceduralObject tilted = a; tilted.rotationX = 0.6f; tilted.rotationW = 0.8f;
    b = tilted; b.rotationX = -0.6f; b.rotationW = -0.8f;
    PROC_CHECK("quaternion sign equivalent", sameObject(tilted,b));
    b.rotationX = 6.0f; b.rotationW = 8.0f;
    PROC_CHECK("quaternion normalized scale equivalent", sameObject(tilted,b));
    signed __int64 canonical[4]; detail::rotation(tilted,canonical);
    PROC_CHECK("canonical quaternion integers ordered W X Y Z",
        canonical[0] == 8000 && canonical[1] == 6000 && canonical[2] == 0 && canonical[3] == 0);
    b = a; b.rotationW = FLT_MAX;
    PROC_CHECK("large finite quaternion normalizes in double", validObject(b) && sameObject(a,b));
    b.rotationW = FLT_MIN;
    PROC_CHECK("small nonzero quaternion normalizes in double", validObject(b) && sameObject(a,b));
    ProceduralObject nearPi = a; nearPi.rotationX = -1.0f; nearPi.rotationW = 0.000001f;
    b = nearPi; b.rotationX = 1.0f; b.rotationW = -0.000001f; b.rotationY = -0.0f;
    PROC_CHECK("near pi sign uses first nonzero quantized component", sameObject(nearPi,b));
    b = nearPi; b.rotationW = -0.000001f;
    PROC_CHECK("quantized zero W cannot determine quaternion sign", sameObject(nearPi,b));
    detail::rotation(nearPi,canonical);
    PROC_CHECK("near pi canonical sign is selected after quantization",
        canonical[0] == 0 && canonical[1] == 10000 && canonical[2] == 0 && canonical[3] == 0);
    b = a; b.rotationX = b.rotationY = b.rotationZ = -0.0f;
    PROC_CHECK("quaternion signed zeros preserve identity", sameObject(a,b));
    b = a; b.rotationW = 0;
    PROC_CHECK("zero quaternion rejected by validation and comparison", !validObject(b) && !sameObject(a,b));
    for (unsigned component = 0; component < 4; ++component) {
        b = a;
        float* parts[] = { &b.rotationW, &b.rotationX, &b.rotationY, &b.rotationZ };
        *parts[component] = std::numeric_limits<float>::infinity();
        PROC_CHECK("nonfinite quaternion component rejected", !validObject(b) && !sameObject(a,b));
        *parts[component] = std::numeric_limits<float>::quiet_NaN();
        PROC_CHECK("NaN quaternion component rejected", !validObject(b) && !sameObject(a,b));
    }
    b = a; b.constructionProgress = 0.99994f; PROC_CHECK("construction canonical boundary", !sameObject(a,b));
    b = a; b.buildingFunction++; PROC_CHECK("building function participates", !sameObject(a,b));
    b = a; b.stateFlags |= PROC_PAUSED; PROC_CHECK("native state participates", !sameObject(a,b));
    b = a; b.flags |= PROC_OWNER; std::strcpy(b.ownerSid,"owner");
    PROC_CHECK("owner identity participates", validObject(b) && !sameObject(a,b));
    b = a; b.flags = PROC_INSIDE; std::strcpy(b.insideSid,"interior"); b.insideY = 0.02f;
    PROC_CHECK("inside parent geometry participates", validObject(b) && !sameObject(a,b));
    b.flags |= PROC_OUTSIDE;
    PROC_CHECK("native outside-furniture flag may retain an inside parent", validObject(b) && !sameObject(a,b));
    b = a; b.heading = 6.283185307179586f; b.y = -0.0f;
    PROC_CHECK("angular wrap and signed zero", sameObject(a,b));
    b = a; b.floor = 1; PROC_CHECK("floor participates", !sameObject(a,b));
    b = a; b.flags |= PROC_TOWN; std::strcpy(b.townSid,"town"); b.townX = 12;
    PROC_CHECK("parent participates", validObject(b) && !sameObject(a,b));
    b = a; b.ownerSid[0] = 'x'; PROC_CHECK("owner flag matches metadata", !validObject(b));
    b = a; b.category = 2; PROC_CHECK("unknown category rejected", !validObject(b));
    b = a; b.x = std::numeric_limits<float>::infinity(); PROC_CHECK("infinite geometry rejected", !validObject(b));
    b = a; b.heading = std::numeric_limits<float>::quiet_NaN(); PROC_CHECK("NaN rejected", !validObject(b));
    b = a; std::memset(b.sid,'x',sizeof(b.sid)); PROC_CHECK("unterminated template rejected", !validObject(b));
    std::vector<ProceduralObject> rows; rows.push_back(a); rows.push_back(bed(2));
    std::vector<ProceduralObject> reversed(rows.rbegin(),rows.rend());
    PROC_CHECK("digest independent of row order", snapshotDigest(rows) == snapshotDigest(reversed));
    reversed[0].id = 3; PROC_CHECK("digest includes logical identity", snapshotDigest(rows) != snapshotDigest(reversed));
    reversed = rows; reversed[0].x = 0.004f;
    PROC_CHECK("digest uses canonical float representation", snapshotDigest(rows) == snapshotDigest(reversed));
    reversed = rows; reversed[0].townZ = 0.02f;
    PROC_CHECK("digest includes all parent geometry", snapshotDigest(rows) != snapshotDigest(reversed));
    reversed = rows; reversed[0].floor++;
    PROC_CHECK("digest includes floor", snapshotDigest(rows) != snapshotDigest(reversed));
    reversed = rows; reversed[1].id = reversed[0].id;
    PROC_CHECK("digest rejects duplicate logical identities", snapshotDigest(reversed) == 0);
    reversed = rows; reversed[0] = tilted;
    const u32 tiltedDigest = snapshotDigest(reversed);
    reversed[0].rotationX = -0.6f; reversed[0].rotationW = -0.8f;
    PROC_CHECK("digest identifies quaternion signs", tiltedDigest == snapshotDigest(reversed));
    reversed[0].rotationX = 6.0f; reversed[0].rotationW = 8.0f;
    PROC_CHECK("digest normalizes quaternion scale", tiltedDigest == snapshotDigest(reversed));
    reversed[0] = nearPi; const u32 nearPiDigest = snapshotDigest(reversed);
    reversed[0].rotationX = 1.0f; reversed[0].rotationW = -0.000001f; reversed[0].rotationZ = -0.0f;
    PROC_CHECK("digest canonical near pi sign and signed zero", nearPiDigest == snapshotDigest(reversed));
    reversed[0].rotationX = -1.0f; reversed[0].rotationW = -0.000001f;
    PROC_CHECK("digest ignores sign of quantized zero W", nearPiDigest == snapshotDigest(reversed));
    reversed = rows; reversed[0].rotationX = -0.0f; reversed[0].rotationZ = -0.0f;
    PROC_CHECK("digest canonical signed zeros", snapshotDigest(rows) == snapshotDigest(reversed));
    reversed[0] = tilted;
    PROC_CHECK("digest includes pitch", snapshotDigest(rows) != snapshotDigest(reversed));
    reversed[0] = a; reversed[0].rotationZ = 0.6f; reversed[0].rotationW = 0.8f;
    PROC_CHECK("digest includes roll", snapshotDigest(rows) != snapshotDigest(reversed));
    reversed[0].rotationW = reversed[0].rotationZ = 0;
    PROC_CHECK("digest rejects zero quaternion", snapshotDigest(reversed) == 0);
    reversed[0] = a; reversed[0].rotationY = std::numeric_limits<float>::quiet_NaN();
    PROC_CHECK("digest rejects nonfinite quaternion", snapshotDigest(reversed) == 0);
    RegionSnapshot s; s.reset(11,3,-7,5);
    ProceduralPacket begin = packet(PROC_BEGIN,1,rows), row = packet(PROC_ROW,1,rows), end = packet(PROC_END,1,rows);
    PROC_CHECK("END without BEGIN rejected", !s.accept(end) && !s.complete());
    PROC_CHECK("BEGIN accepted", s.accept(begin)); row.object = rows[0];
    PROC_CHECK("first row accepted", s.accept(row));
    PROC_CHECK("BEGIN retransmission preserves rows", s.accept(begin) && s.objects().size() == 1);
    PROC_CHECK("identical row replay idempotent", s.accept(row) && s.objects().size() == 1);
    PROC_CHECK("missing row cannot complete", !s.accept(end) && !s.complete());
    row.object = rows[1]; PROC_CHECK("remaining row accepted", s.accept(row));
    PROC_CHECK("full fenced END completes", s.accept(end) && s.complete());
    s.markAcknowledged(); PROC_CHECK("ACK only after complete", s.acknowledged());
    row.object=rows[1];
    PROC_CHECK("identical BEGIN and ROW preserve queued ACK", s.accept(begin) && s.accept(row) && s.acknowledged());
    ProceduralPacket foreignEnd=end; foreignEnd.worldToken++;
    PROC_CHECK("foreign END cannot disturb queued ACK", !s.accept(foreignEnd) && s.acknowledged());
    PROC_CHECK("repeated completed END requires exact re-ACK", s.accept(end) && s.complete() && !s.acknowledged());
    row.object.x += 1; PROC_CHECK("conflicting replay invalidates readiness", !s.accept(row) && !s.complete() && !s.acknowledged());
    PROC_CHECK("same epoch cannot heal conflict", !s.accept(begin));
    begin.epoch = row.epoch = end.epoch = 2; row.object = rows[0];
    PROC_CHECK("new epoch supersedes invalid assembly", s.accept(begin) && s.accept(row));
    ProceduralPacket stale = end; stale.epoch = 1;
    PROC_CHECK("stale END cannot complete", !s.accept(stale) && !s.complete());
    ProceduralPacket wrong = row; wrong.worldToken++;
    PROC_CHECK("world fence", !s.accept(wrong)); wrong = row; wrong.loadId++;
    PROC_CHECK("load fence", !s.accept(wrong)); wrong = row; wrong.regionZ++;
    PROC_CHECK("region fence", !s.accept(wrong));
    row.object = rows[1]; PROC_CHECK("fenced rejects preserve valid assembly", s.accept(row) && s.accept(end));
    s.reset(11,3,-7,5); s.markAcknowledged(); PROC_CHECK("reset clears readiness and ACK", !s.complete() && !s.acknowledged());
    begin.count = PROCEDURAL_OBJECTS_MAX + 1; PROC_CHECK("overcapacity rejected", !s.accept(begin));
    std::vector<ProceduralObject> empty;
    begin = packet(PROC_BEGIN,3,empty); end = packet(PROC_END,3,empty);
    PROC_CHECK("explicit empty set requires END", s.accept(begin) && !s.complete());
    PROC_CHECK("empty snapshot completes", s.accept(end) && s.complete() && s.count() == 0);
    end.digest++; PROC_CHECK("conflicting END invalidates completed snapshot", !s.accept(end) && !s.complete());
    s.reset(11,3,-7,5); begin = packet(PROC_BEGIN,4,rows); end = packet(PROC_END,4,rows);
    row = packet(PROC_ROW,4,rows); s.accept(begin); row.object = rows[0]; s.accept(row);
    row.object = rows[1]; row.object.floor++; s.accept(row);
    PROC_CHECK("full count with wrong descriptor fails digest", !s.accept(end) && !s.complete());
    std::vector<ProceduralObject> rotatedRows; rotatedRows.push_back(tilted);
    s.reset(11,3,-7,5); begin = packet(PROC_BEGIN,5,rotatedRows);
    row = packet(PROC_ROW,5,rotatedRows); end = packet(PROC_END,5,rotatedRows); row.object = tilted;
    PROC_CHECK("rotated snapshot begins", s.accept(begin) && s.accept(row));
    row.object.rotationX = -6.0f; row.object.rotationW = -8.0f;
    PROC_CHECK("sign and scale equivalent duplicate stays single row", s.accept(row) && s.objects().size() == 1);
    PROC_CHECK("equivalent rotation completes original digest", s.accept(end) && s.complete());
    s.markAcknowledged();
    PROC_CHECK("equivalent rotation replay preserves ACK", s.accept(row) && s.acknowledged());
    row.object.rotationX = 0; row.object.rotationZ = 0.6f; row.object.rotationW = 0.8f;
    PROC_CHECK("changed rotation replay invalidates snapshot and ACK", !s.accept(row) && !s.complete() && !s.acknowledged());
    s.reset(11,3,-7,5); s.accept(begin); s.accept(row);
    PROC_CHECK("rotation change with original digest cannot complete", !s.accept(end) && !s.complete());
    rows.assign(PROCEDURAL_OBJECTS_MAX, a);
    for (unsigned i = 0; i < rows.size(); ++i) rows[i].id = i + 1;
    s.reset(11,3,-7,5); begin = packet(PROC_BEGIN,5,rows); row = packet(PROC_ROW,5,rows); end = packet(PROC_END,5,rows);
    bool full = s.accept(begin);
    for (unsigned i = 0; i < rows.size(); ++i) { row.object = rows[i]; full = s.accept(row) && full; }
    PROC_CHECK("exact maximum unique rows completes", full && s.accept(end) && s.complete());
#undef PROC_CHECK
    return failures;
}
#endif
