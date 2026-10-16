# Replication pitfalls and gate design

> **Purpose.** Hard-won failure modes in KenshiCoop4up's item/world replication,
> inherited from and developed on [KenshiCoop by nhoral](https://github.com/nhoral/KenshiCoop),
> and the testing habits that did or did not catch them. KenshiCoop4up is
> independently maintained by kotetsyy; the original project's authorship
> remains nhoral's. Each entry states a rule, the concrete bug that produced it,
> and the signature to grep for. This is engineering evidence, not a changelog
> or a claim that every historical fix/gate validates the current release.
>
> Historical protocol numbers, session results and fault-injection examples below
> retain their original context. The public interface reference is
> [API_REFERENCE.md](API_REFERENCE.md); current packet contracts live in
> [`src/netproto/Wire.h`](../src/netproto/Wire.h), which wins on conflict.
> No unavailable private protocol-history document is required to use this file.
> Most entries generalise past items: they are really about drawing conclusions
> from engine reads, and about writing gates that can fail.

---

## 1. Budget in milliseconds, not ticks

`mainLoop_hook` runs every engine tick and Kenshi's loop runs at roughly
**100–125 Hz**. Any budget expressed as "N consecutive reads" is therefore a
tolerance of `N × ~8–10 ms`, which is almost never what the author meant.

`WD_DEAD_READS_MAX = 3` was intended as "let the read agree with itself before
believing it". In a real session it retired a tracked ground item **29 ms after
the drop that created it**, and a second one 32 seconds before the peer actually
picked that item up. The author then had no handle to answer the peer's pickup
with, so its ground copy stayed on the floor next to the item the peer now held —
the duplicate players reported.

**Rule.** Pair every read-count budget with a real-time hold, and log the elapsed
duration in the line that acts on it, so the budget is auditable from a session
log instead of inferred. See `WD_DEAD_HOLD_MS` and the `ground-prune` line, which
now reports `(N consecutive reads over Xms, everLive=1)`.

## 2. Never conclude from a single engine read

The engine streams objects out and back. A cached `RootObject*` goes unreadable
for a frame, `isInInventory` flickers, a spatial query misses. Any of these is a
transient, and code that treats one of them as a verdict makes a permanent
mistake from a momentary one.

Three places did exactly this, all in the pickup path:

- `why=gone` erased a ground track on one unreadable read — the very read that
  `reconcileGroundGear` deliberately refuses to trust on its own.
- `why=untracked` gave up after a single attempt at the site-anchored spatial
  scan. That scan uses `getObjectsWithinSphere`, which this codebase repeatedly
  documents as unreliable **in towns** — so the one-shot fallback was weakest
  exactly where players hit it.
- The drop mirror fabricated an item the moment a top-level search missed (§4).

**Rule.** When an operation cannot be satisfied now, park it and retry against a
deadline rather than answering once. Unsatisfied identified pickups are now
`PendingPickup` entries retried each tick (named track first, site scan second)
until `WD_REHOME_MAX_MS`, then reported as `PICKUP-GAVEUP` rather than vanishing
quietly. Identity-less intents are still refused outright — without an instance
identity, "re-home the oldest same-sid copy" teleports an unrelated object.

## 3. Let the asymmetry of the cost pick the default

Forgetting a track that is still on the ground costs a **permanent duplicate**.
Keeping a track whose object is genuinely gone costs **a stale pointer until the
next read**. Those are not comparable, so the tie should not be broken evenly.

The same asymmetry runs the other way for destruction: the author may only
destroy its ground copy after it has *seen* the item in the target container,
because a wrong guess loses the only instance. Keeping a duplicate beats
destroying the last copy.

**Rule.** Write down which error is recoverable before choosing the threshold.

## 4. Fabrication must prove absence, not failure-to-find

`APPLY-HEALED` mints an item from the drop intent's provenance when the mirror
cannot find its own copy. That is safe only if the item is genuinely absent —
and "my search didn't find it" is a much weaker claim, because **search scope**
is part of the search.

`dropItemFromInventory` reads top level only, on purpose: a bagged item belongs
to a different `Inventory` and must not be counted as the character's loose kit
(the `includeNested` distinction). But hoovering
a pile of loot into one character overflows the grid and Kenshi stows the tail in
the worn backpack. The mirror then declared the item missing and fabricated one,
leaving the real item in the bag **and** a minted duplicate on the ground. Four
of these appeared in a single player session, three consecutively.

**Rule.** Before fabricating, exhaust every place the item could legitimately be.
`relocateWeaponToGround` now falls back to `dropItemFromNestedContainer` and logs
`RELOCATE-NESTED` when that reach is what saved the object.

## 5. Engine constraints that shape what a test can even do

Discovered the hard way while trying to build fixtures; each one silently returns
zero rather than failing loudly.

| Constraint | Symptom |
|---|---|
| A character has two weapon slots, and **both its grid and its worn bag refuse a third weapon**. | `[mk] tryAddItem-fail sid='...' type=2` — a scenario trying to mint a burst of weapons adds nothing. Prefer an armour template. |
| A character's loose storage **is** its worn backpack, so a second container cannot be added. | `[mk] tryAddItem-fail ... type=46`. The same-template multi-bag case is unreachable on a character. |
| `getObjectsWithinSphere` is unreliable in towns. | Spatial discovery and spatial recovery both silently miss. Query-free paths (the `dropItem` detour, handle-based liveness) exist for this reason; do not build a sole recovery path on a spatial scan. |
| A town-dropped item often reports its transform as `(0,0,0)` the frame it grounds. | Mirrored copies land at world origin unless the sentinel is filtered. |

## 6. Gate on a conservation ledger, not on presence

"Does the peer have it?" passes on a duplicate, because the peer does have it —
and so does the ground. A count taken only at the end also passes on a
destroy-and-recreate loop.

The invariant that catches both directions at once is a **per-template ledger
evaluated on each side**: every instance dumped must exist exactly once, either
on the ground or in the picker's bag.

```
ground + bag == dumped        # per template, per client
```

A template summing **above** what was dumped is the duplicate; **below** it is the
item that never arrived. `inv_dump_all` prints this as `dist='1+0/1,1+0/1'` so a
failure is readable without opening the logs.

Two traps in measuring it:

- Count nested contents (`includeNested`), or a successful transfer **into a
  backpack** reads as a loss.
- Measure a **delta** against each side's own baseline. Probe templates are
  ordinary kit the receiving character may already carry, and an absolute count
  passes on the save's own contents with nothing having crossed the wire. An
  early version of the nested-bag gate did exactly that (host 7, join 5, PASS).

## 7. "Tolerated but reported" is how a bug survives green runs

`APPLY-HEALED` was deliberately logged-and-allowed, on the reasoning that a heal
means the publish hold was outrun and the backstop covered it. It covered nothing
— it was minting duplicates — and it did so through several passing runs because
no gate would fail on it.

**Rule.** If a signature means the product did something it should not have, fail
on it. If it is genuinely acceptable, say why in the gate, not in a comment. The
oracles now fail on `APPLY-HEALED`, `PICKUP-GAVEUP`, any surviving one-shot
`why=untracked`, and any `ground-prune` that retired a once-live track inside
`WD_DEAD_HOLD_MS`.

## 8. A fault-injection lever must model the *actual* fault

This is the subtlest lesson here, and it cost three A/B attempts.

The goal was a gate that fails before a fix and passes after. Two levers failed
to discriminate:

1. **The plain scenario** passed on both builds — the fault is timing-dependent
   and does not reproduce on demand.
2. **`KENSHICOOP_WD_FORGET_TRACK`** (discard the author's track permanently) also
   passed pre-fix, because a *permanently* lost track is covered by the
   site-anchored recovery, which `inv_regear_forget` already gates. The injected
   fault was **more severe** than the real one, so it exercised a different
   recovery path and said nothing about the bug under test.

What discriminated was modelling the real transient:
`KENSHICOOP_WD_TRANSIENT_DEAD=N` makes the first N pickup-time resolutions report
the object gone and later reads succeed — the engine streaming an object out and
back. Pre-fix, the host ledger reads `2+0/1` (two ground copies of a template
dumped once); post-fix it reads `1+0/1`.

**Rule.** If a lever makes the failure permanent when reality makes it momentary,
a passing A/B proves only that some *other* recovery path works. Match the
severity, not just the shape.

## 9. Log unconditionally for anything a player can see

W1 ground-item diagnostics sit behind `KENSHICOOP_INV_DUMP`, so a real session log
of "I dropped it here and it never appeared there" contained nothing attributable
about the W1 path at all. The W2 path, whose key lines are unconditional, was
diagnosable from the same log in minutes.

**Rule.** A state the player can observe deserves an unconditional line. Keep the
verbose per-item dumps behind the flag; put the verdicts (`DROP-CAP-SKIP`,
`SEND-DEFER`, `PICKUP-GAVEUP`, `APPLY-LOST`) in front of it.

## 10. Gate the workflow, not the unit

Every gear gate was a one-item round trip: drop one thing, pick it up, assert. The
player's actual workflow was to **dump a character's entire inventory and hoover
all of it up with one other character**. That difference is where the bugs lived —
a burst mints many tracks at once, and one receiving grid fills up and overflows
into the backpack. Neither condition existed in any gate, which is why the suite
stayed green across three reported-bug sessions.

**Rule.** Ask how the feature is actually driven, and make at least one gate drive
it that way. `inv_dump_all` exists for this; `inv_dump_all_transient` is its
fault-injected twin (§8).

## 11. Put the camera where the player's camera would be

§10 is about the *action* a gate drives. This is about the *viewpoint* it drives
it from, which turns out to be load-bearing in its own right.

`park()` teleports bodies and does not touch the camera, and nothing else in the
harness moved it either — `cameraCenter()` could only read. So every automated
run watched wherever the save happened to leave the camera. In `split_far`, which
separates the two squads by ~5,200 u, that meant **both** clients stared at the
host's squad for the whole window while the join's characters stood 5,200 u
off-screen. No player has ever played that way, and the anchor count said so:
`anchors=3`, being the two tab leaders plus one camera hovering 167 u above the
host's leader (just outside the 100 u dedupe). There was never an anchor at the
far end.

Adding `cameraFocusOn` and pointing each side at its own tab leader changed the
run substantially. NPC population at the join's own squad, previously 5, ranged
**5–22** across five runs, and two of those runs failed existence-parity with
ghost runs of 7 consecutive samples — the reported "join sees NPCs the host
doesn't" symptom, which the parked-camera configuration never produced:

| popMover | 5 | 5 | 10 | 12 | 22 |
|---|---|---|---|---|---|
| existence-parity | PASS | PASS | PASS | FAIL | FAIL |

The failure tracks **population**, not the camera directly; the camera matters
because it is one of the things that drives population up. Smoothness went from
PASS to FAIL in 5/5 follow-camera runs (`zeroFrac ≈ 0.95`).

Two cautions on reading this:

- Only the **join's** camera was ever varied. The host's sat on the host's squad
  in every run including the parked one, so nothing here isolates the effect of
  the host looking away from its own people.
- One parked-camera sample is not a baseline. "The camera raises population" is
  consistent with the data but not established by it — two follow-camera runs
  also came in at 5.

**Rule.** State the viewpoint a scenario runs from as deliberately as it states
the positions, and re-point the camera after any teleport. A gate that never
renders the region it is making claims about is measuring enumeration only,
while the symptom it is chasing is something a player *sees*.

## 12. A symmetric swap is still a disjoint view

The first phased version of `split_far` ran `own → cross → back`: each side on
its own squad, then **both** cameras swapped to the peer's squad, then back.
That looks like it covers the space, and it does not. Swapping both at once
mirrors the arrangement without ever overlapping it — in all three phases the
two clients are drawing *different* characters.

That matters because a disagreement measured under disjoint views is
unattributable. If the join counts bodies at a spot and the host does not, the
host might have failed to replicate them, or the host might simply not be
looking. Nothing in the run separates those.

For two clients and two squads there are four arrangements, and the swap covers
neither of the interesting ones:

| | host → stay | host → mover |
|---|---|---|
| **join → mover** | `own` (disjoint) | `cross` (disjoint) |
| **join → stay** | **`both_stay`** (overlap) | **`both_mover`** (overlap) |

The scenario now runs `own → both_stay → both_mover → back`. `cross` is dropped:
it is disjoint like `own`, and `both_mover` already answers the question it was
there for (does the host's count rise when the host looks at the mover?) while
also giving overlap. Verified from the `[cam]` telemetry rather than assumed —
under `both_mover` the two cameras report identical centres
(`-50476.1,868.4,-2699.8` on both sides).

**Rule.** When a test compares two observers, check that its phases put them on
the same subject at least once. Otherwise every difference it reports has a
second explanation, and the reassuring result — the counts matching — is the
one most likely to be an artefact of neither side looking.

**Caveat carried forward.** The per-phase table compares *counts*
(`countNpcsNear`), not identities: two clients can hold five bodies each and
disagree about which five. `existence_parity` is the identity check, and it
stays advisory here precisely because this scenario exists to expose ghosts.

## 13. Both harness clients share one save folder, and the host writes to it

The two worlds disagree at PLATOON granularity, not per body. Diff the
`[platoon] first-sight` container ids from the two logs and the sets differ by
7–13 squads; at a settlement that reads as duplicated *roles* — two `Barman`
bodies where there is one bar, six `Ninja Guard` against four. The extra copies
go `drv → ghost → hid` and then run their AI invisibly, and the seconds before
suppression are what players report as "the join sees NPCs the host doesn't".
That much is solid, and it is worth knowing that census truncation, staleness,
zone loading and the mint path were all clean in every run that showed it.

What the harness **cannot** currently tell you is why. Both installs auto-load
from the same `%LOCALAPPDATA%\kenshi\save` folder, and the host's connect-push
bakes its live world *into that same folder* while the join is reading it —
`sync_save.cmd` says in its own header "close both Kenshi instances first so
save files aren't mid-write", which is exactly what the runner then does not
do. So what the join loads is decided by a race against the host's write.

This is easy to mistake for a causal result. Varying `-JoinDelaySec` produces a
clean monotone table — host solo before bake 15s / 68s / 128s giving 7–12 / 2 /
0 join-only platoons — which reads as "bake earlier, diverge more". It is not.
Deferring the bake by 120s *in the plugin*, so the host settles exactly as long
but the join launches at the usual moment, gave **13** join-only platoons, the
worst result measured. The stagger was moving the join's load relative to the
host's write, not changing what a settled world looks like.

**Rule.** Before drawing any conclusion about what the join loaded, establish
*which bytes* it loaded. On one machine with a shared save folder and a host
that writes mid-session, that is a race, and no amount of timing variation
turns it into evidence. Give the two installs separate save folders — or drive
the join's load entirely from `LOAD_GO` after the write has quiesced — before
re-running any experiment of this shape.

## 14. A predicate two clients must agree on has to be published, not derived

The attention gate stops both clients reconciling a region neither is watching:
if no attention centre is within `KENSHICOOP_ATTENTION_RADIUS`, the host omits
those bodies from its census and the join stops counting suppression frames
against them. That only works if the two sides reach the *same* verdict about
the same body. Wherever an input to the predicate is private, they cannot.

Three of the four centres were already common — both clients load the same save,
so both hold both squad tabs, and a tab leader's position is the same number on
each machine. The camera was the exception. `PKT_CAM_HINT` shipped join → host
only, because its original job was widening the host's interest spheres, and for
that one direction is enough. As a *gate* input it is not: with the host's camera
private, the join has to assume the host is always watching, which is the
assumption that keeps the ghost churn alive. The hint is now bidirectional.

The safety property the design rests on is that a squad is never dormant just
because no camera is on it — every tab leader is an interest centre, so bodies
beside a leader are observed whether or not anybody is looking. That is true by
construction in `interestCenters` (leaders fill the first slots; the four-slot
cap can only drop *camera* anchors), and it is measured rather than assumed:
`dormPc` on the `[audit] exist` line counts dormant bodies within the radius of
any player character, `existence_parity` FAILs on a non-zero, and `pcs` reports
the squad size it was measured against so a vacuous zero is visible. Note that
every harness save carries one character per tab, so the case the metric exists
to catch — a squad *member* that walked away from its leader and anchors nothing
— cannot be staged here yet.

**Rule.** Before gating behaviour on a predicate, list its inputs and ask which
of them the peer can see. An input only one side holds does not make the gate
approximate, it makes the two sides run different gates. Publish it, or drop it
from the predicate.

## 15. The engine owns every object's lifetime; a stored pointer expires when its block unloads

A `RootObject*` we minted is not ours. Kenshi destroys ground items when the zone
block they stand in deactivates, and travelling far is exactly what deactivates
blocks — so a map of proxy pointers is a map of pointers whose validity is a
function of where the players have walked since. `worldProxies_` held raw
pointers, and after a long trek every use of one was a use-after-free: the cull
called `GameWorld::destroy` on an object the engine had already destroyed (which
it notices — `Item <name> alredy has destroy reason ...` in `kenshi_info.log`),
the snapshot path called the VIRTUAL `setPositionRotation` through a dangling
vtable, and the claim path read `isInInventory` out of freed memory.

The reads are the subtle half. Freed heap usually still maps, so the SEH guards
around these calls do not fire; they return plausible garbage. A stale
`isInInventory` reading true makes the join tell the host "I picked your item up",
and the host destroys a real item nobody touched. A crash is the loud version of
this bug and duplicate/vanished items are the quiet version.

The fix is not a stronger guard, it is an identity: every proxy records the
engine `hand` it was minted with, verified at mint by resolving it back to the
same pointer, and `liveWorldProxy()` re-resolves before anything touches the
object. A hand is serial-checked, so a destroyed object's hand stops resolving
and a recycled table slot resolves to a *different* pointer — both read as "gone",
and gone means drop the mapping and call nothing. `groundItemLiveness(hand)`
already worked this way for our own tracked items; only the proxies, whose
`Engine.h` comment openly said "we hold only as a pointer", did not.

**Rule.** Never store a bare engine pointer across ticks. Store the hand, resolve
it at the point of use, and treat an unresolvable hand as an already-destroyed
object rather than an error. If a pointer must be cached, the question to answer
is not "can this be null" but "what does the engine do to this object when the
players walk away from it".

## 16. One value with two writers cannot be replicated as a snapshot

Every channel in this codebase publishes state and lets the newest value win,
which is correct when each value has exactly one author. The player's money has
two: Kenshi keeps ONE wallet per save (`Faction::factionOwnerships`), and both
players spend from it. Exchange that as a total and concurrent spends erase each
other — from 1000, one player buying for 200 and the other for 300 send 800 and
700, whichever lands last is the pool, and one purchase was free. The bug is
worst in the case players will actually create, both shopping in the same town.

So the join sends the CHANGE (`PKT_MONEY_DELTA`) and the host — whose wallet *is*
the pool — sends the total. Three things that fall out of that and are easy to
miss:

- **The delta channel must be reliable AND ordered.** A dropped or reordered
  total is self-healing (the next one is still correct); a dropped delta mints or
  burns cats permanently.
- **The publisher needs an ack, or the join's own purchase visibly bounces.**
  The host's total lags the join's local spend by a round trip, so adopting it
  blindly reverts the purchase and then re-applies it. `MoneyPacket::ackSeq`
  names the last delta folded in, and the join re-applies whatever is still
  pending on top.
- **Detection can be a sample, not a hook.** One wallet read per tick against a
  baseline catches every path that moves money — trade UI, sale, loot, bounty,
  hire, bar tab — with no per-path detours. The baseline must be updated on our
  own writes too, or applying the host's total reads back as a fresh local
  change and the pool oscillates.

**Rule.** Before writing a channel, ask how many authors the value has. If the
answer is more than one, replicate the operation rather than the state, and gate
it on CONSERVATION (`money_sync` asserts both clients end at
`base - joinSpend - hostSpend`) — a convergence gate cannot tell a lost update
from agreement, because both clients agreeing on the wrong number passes.

**Corollary on the negative control.** `money_persist` first waited for the
host's pool to move before saving. That is the right assertion in the wrong
place: with `KENSHICOOP_MONEY_SYNC=0` the fold never arrives, so the run stalled
before the save and the A/B proved only that a scenario can fail to set itself
up. Latching whatever the pool reads (recording `moved=0|1`) and saving anyway
carries the unfixed build to the post-load comparison, where the erase is
visible and named. A negative control has to REACH the assertion, so no step of
the script may be gated on a precondition that only the fix satisfies.

## 17. Split authority has to be total, or bodies fall between the halves

Cell authority (protocol 49) hands each side the NPC census for the 4608 u zone
cells its own squad stands in, instead of the host authoring everywhere. Asking
which of the two models is better turns out to be the wrong question: measured
per arm on the same build, host authority took `dual_drive` 4/4 against 1/4 and
`world_parity` 4/4 against 1/4 with no smoothness cost while the squads stood
together, and the same A/B run apart had the join hold a steady 11 NPCs at its
own town under cell authority against 5, 5 and 23 without it. Each model wins
decisively in one regime. So the rule switches on SEPARATION rather than
electing a global winner: while both squads claim the same cell, every cell
resolves to the host (`KENSHICOOP_CELL_COLLAPSE`, default on).

Two ways that went wrong, both instructive:

- **A proximity predicate is not a co-location predicate.** The first version
  collapsed on Chebyshev distance 1, reasoning that adjacent cells are the same
  neighbourhood. Cells are 4608 u; two towns a long walk apart can sit in
  adjacent cells, and `split_far2` — the scenario whose entire premise is that
  the pair is separated — collapsed onto the host and lost the population it
  exists to protect. Same cell or nothing.
- **Collapsing the CLAIMED cells is not collapsing the map.** A pair walking
  together leaves a trail of cells the join claimed a moment ago, and
  `authoritySrc` kept handing those to the join as last occupant
  (`AUTHSRC_VACATE`). But a collapsed join publishes no census at all, so bodies
  standing in that trail were corroborated by nobody, and the host froze them as
  census-absent — the visible symptom being a town going still behind the
  players. While collapsed, `authoritySrc` now returns the host for EVERY cell,
  claimed, vacated or open.

**Rule.** Any scheme that divides authorship must define an author for every
region, including the ones nobody currently occupies. A cell whose nominal
author has stopped publishing is worse than a cell with no author at all,
because the absence reads as a deliberate "this body is gone".

## 18. A marker written on one edge is a record of history, not of state

`KENSHICOOP_DEBUG_MARKERS` pins a colored label to each judged body — green DRV
for host-driven, red HID for suppressed, yellow LOC for a local-sim copy. The
DRV label is written every tick a body is driven, and nothing ever removes it
when the drive stops: the body is still standing there, so the pruner (which
destroys labels only for bodies that have vanished) keeps vouching for it. The
label therefore accumulates into "everything this client has driven at any point
this session", and since both clients accumulate their own, the same NPC ends up
green on BOTH screens. That reads on screen as two clients driving one body —
which is the exact fault the tag exists to let you rule out. It cost a long
detour chasing a dual-drive that the logs plainly denied: the host had refused
zero bodies, frozen one, and was receiving nothing but the join's own PC.

The fix is to re-derive the label from the live set rather than trust the edge:
the prune destroys any green label whose body is absent from `drivenChars_`,
which the drive rebuilds every tick, so a stopped drive loses its tag within the
2 s prune cadence and a resumed one re-creates it.

**Rule.** Liveness of the SUBJECT is not liveness of the CLAIM. A diagnostic
asserting a fact that can stop being true has to be re-asserted on a cadence and
withdrawn when it lapses — otherwise the instrument manufactures the failure it
was built to detect, and it does so most convincingly in exactly the long
sessions where you are least able to check it.

## 19. Reliable transport ordering does not imply ordered application across queues

On a join bootstrap, the host sent 34 files / 3,964,020 bytes, but the join
verified only 3,499,846 bytes (`badCrc=4`, `XFER-ACK ok=0`). Reconnecting
produced `XFER-COMMIT badCrc=0`. The first failure did not retry or issue
`READY`; the host remained paused until the reconnect.

`SAVE_BEGIN`, `SAVE_FILE` and `SAVE_DONE` arrive in order on reliable `CH_BULK`,
but `pumpSaveReceive()` used to drain three separate inbound queues one at a
time. The net thread could append the last FILE chunks after the file drain and
before the DONE drain; the game thread then verified and discarded the staging
folder before applying those chunks. It now snapshots the three queues under
one lock so every DONE is processed with its preceding FILE chunks.

**Rule.** When a completion marker gates a commit, dequeue its data and marker
atomically even if the transport itself guarantees ordering. This fixes the
identified queue race; reproducing the two-machine bootstrap after deploying
the same DLL to both players is still necessary to confirm that it caused this
particular failed transfer.

## 20. Save existence is not save-manager readiness

The UDP handshake completed (`WELCOME`, proto v63, RTT about 58 ms), and the
host baked and announced its world with `LOAD_GO`. The join stayed at the
title screen exchanging traffic without a load decision or a transfer request.

`titleUpdate_hook` gated `driveLoadSync` on `savesReady`, which called
`SaveManager::savesExist()`. With no local saves, this blocked the very NACK
that would request the host's save: the client could not obtain the content
required to pass its gate. `savesReady` now checks the SaveManager singleton,
independently of local content. The unused existence probe was removed.

Verification: a real game client with an isolated empty save folder received
`LOAD_GO`, replied `MISSING -> NACK`, committed 27 files / 3,376,186 bytes with
`badCrc=0`, loaded the world, and emitted `READY->host loadId=1 first-world-live`.
The sender used the actual NetLink / SaveXfer implementations and host save.
This local UDP smoke proves the missing-save path, not remote WAN conditions.

**Rule.** A bootstrap prerequisite must not depend on the content the bootstrap
itself is responsible for fetching.

## 21. A transfer cannot create its source shortfall

Two players taking the last unit must compete for one authoritative stock.
The old transfer apply accepted a request against an empty source by creating
the missing unit (`moved=0 fab=1`). A paired native-inventory regression reproduced
the extra item on v0.1.26.

Transfers now acknowledge only units actually detached from the authoritative
source. The optimistic sender returns rejected units from destination to source
before publishing its inventory. Client loot snapshots no longer overwrite the
host's stock, and an exhausted stock is not a permanent cap on later deposits
or production. `critical_stock` and `critical_npc` check the last-unit race,
returning the winning item, and subsequent replenishment on both game processes.

## 22. A backpack transfer includes its contents at the commit

Adding two items and immediately handing over their backpack used to transfer
an older, empty mirror; the next destination snapshot then erased both items.
The old build failed this paired-game regression.

`PKT_INV_XFER` now carries the complete captured bag contents, including an
explicit empty payload. When the receiver owns the source, its acknowledgement
carries the source's actual contents instead. An acquired foreign bag cannot
be edited until that acknowledgement is applied to the exact local bag instance.
`critical_bag` and `critical_bag_take` assert one destination bag, no source bag,
and both nested items after immediate transfers in either direction.

## 23. Loading a native ground item is not dropping an item

A newly streamed-in save-native item exists independently on both game copies.
Treating its first spatial discovery as a session drop minted a second copy on
the peer. Spatial discovery now seeds native ground stock without announcing a
drop; an actual native inventory-drop edge promotes the item into session stock.
`critical_ground` introduces a native item after the initial census and asserts
one, not two, on both copies.


## 24. A merchant's pockets are not the merchant's stock

The real Squin travel-shop regression found 109 stock units on the host and zero
on the client proxy of the same canonical merchant. The host's ordinary character
inventory contained only seven entries. Native trade aggregates furniture inventories
from the platoon's home building; a minted merchant had no home on the client.

`SpawnInfoPacket` carries the host merchant's shop home and platoon template SID.
Minted and adopted proxies borrow that template and bind the home through native
`Ownerships::setHomeBuilding`, deferring the description until the building loads.
This does not edit shared GameData or generate/replenish goods.

A clean-source repeat exposed a second failure: both client shopkeepers existed
but native `isATrader` returned false. Changing `SQ_ROAMING` to `SQ_RESIDENT`
did not fix it. The native predicate reads `platoon->squadTemplate`'s `is trader`
flag; an arbitrary mint template loses that flag. Preserving the actual host
template made both minted merchants valid and their native trade windows converge:
175 units / `B7D99E13` and 93 units / `7AD9CC7F`. Keep the strict canonical-key
and complete-content checks; a nearby unsynchronized local merchant is not a pass.

Shop shelves are `BCTYPE_USABLE`, not `BCTYPE_STORAGE`. Inventory-bearing usable
furniture now joins the authoritative container census; inventory-less beds and
seats do not. Fixture identities use the same 400-unit radius and 96-row limit
as inventory publication, so those runtime shelves can be resolved on the peer.

`critical_shops_squin` compares the complete native trade view under the same
canonical merchant key on both game processes, including item characteristics.
One measured run converged to 85 units and hash `DE53641B` on both peers; its
physical source inventories also had identical content fingerprints.

**Rule.** Trace the consumer's actual backing inventory, then publish both its
state and its identity. Nonempty but different shops are still a failed regression.

## 25. Native provenance and placement are part of stock reconstruction

Waystation regressions exposed different research blueprints with the same
`BLUEPRINT_ITEM` template. Grouping only by template preserved the wrong research
when quantities happened to match. Native manufacturer/material fields also
carry research and ordinary trade-item data, not only weapon provenance.

Reconcile groups, in-place gear moves and removals now include both native
provenance fields. Reconstruction resolves those records without a weapon-only
category restriction. A blueprint is created from its native research record;
ordinary non-weapon metadata is restored after the correct native constructor.
Unresolved provenance is not replaced with an arbitrary blueprint or material.

A later clean-source smoke disproved the compaction workaround: a Waystation
shop held 84 units on the host but only 81 on the client. Three Water items were
rejected repeatedly; each occupies a 5x5 rectangle in an 18x18 shelf.
Native auto-arrange did not guarantee the host's valid packing.

Protocol 68 carries each loose stack's native section and grid position.
Complete building stock validates all target rectangles before mutation,
reuses matching native items, and places each physical stack at the host's
coordinates through native section callbacks. It does not increase capacity,
invent goods or rearrange player bags. The first paired-game run converged both
Waystation shops completely (152 units / `32AFC767`, 120 units / `D254F474`).

Snapshot relocation runs under the existing `g_invVetoSuspend` guard: native
detach/add callbacks must not capture mirror maintenance as a player's drag.

Native fabrication without supplied provenance preserves factory defaults;
explicit snapshot strings, including empty strings, restore the source fields.
Clearing factory defaults broke stacking of native-produced ore and bag contents.
The full paired-game regression caught this before distribution.

Critical fixtures choose the same item by native string ID on both peers rather
than localized names. Bag transfer fixtures empty the source bag explicitly,
then transfer two freshly added children immediately, without waiting for their
ordinary snapshot. The clean-build eleven-scenario suite passed on nonprimary
DISPLAY1, including Squin, both Waystation shops, simultaneous loot/deposit/
replenishment, native ore production and both backpack transfer directions.

**Rule.** Preserve the native identity and constructor semantics; matching total
counts alone cannot distinguish different blueprints. A valid host inventory can
still fail to fit a peer's fragmented native grid.

## 26. Snapshot maintenance is not a cursor transfer

The cursor lifetime gate added in v0.1.30 exposed an unbalanced capture path.
`unequipToLoose` and `correctWeaponSlot` detach through the hooked inventory
remove, then place directly through `InventorySection::addItem`. The section
add bypasses the inventory add hook, leaving a `PendingRemoval` even after
placement and with every panel closed. Later publication or reconciliation can
therefore remain deferred as if the player still held an item on the cursor.

All top-level and nested snapshot reconciliation now runs under
`g_invVetoSuspend`, restored with `__finally` even when a native call faults.
Real player transfers retain their capture and pending-verdict protection.

`inv_snapshot_cursor` exercises an actual native equipped-to-loose snapshot
move on both game processes. Before the fix both reported one preserved item
but `pending=1` and failed. After the fix both reported `pending=0` and passed,
with the item's quality, craft level, manufacturer and material preserved.

This gate covers snapshot maintenance, not every `InventoryGUI` exchange
callback. Passing primitive transfer probes must not be reported as proof that
mouse drag, right-click, portrait transfer and partial-stack exchange all work.

## 27. Host-only transaction controls are not ordinary inventory state

`CHECKPOINT_ACK` and native commerce/extraction requests are valid only on HOST.
Relaying them by packet type alone delivers a role-invalid packet to healthy
uninvolved JOINs. The relay now checks the snapshot flags or transfer operation;
ordinary inventory snapshots and legacy transfer operation zero still relay.
The real native HOST+3 consumer covers both paths and requires unchanged seats,
connected peers and receipt of an ordinary same-lane marker after the controls.

A host actor checkpoint must close the native transaction gate during
pre-engine inventory ingestion, not later during reconciliation. Otherwise an
extraction can start between those phases and block the very checkpoint whose
ACK is needed to finish the earlier operation.

Equal bag templates are not equal parent instances. Checkpoint comparison
matches container instances one-to-one with their child multisets. Moving two
identical units from two identical bags into one bag must fail equivalence;
row reordering and stack splits within the same matched parent remain valid.
These cases pass the native-toolchain protocol regression group. This is not
proof that partial-stack native commerce or every nested-bag UI path works.

## 28. Census motion history belongs to its source author

Merging census membership must not rotate a global previous-position map and
clock whenever any peer sends a census. An unrelated JOIN's arrival then
replaces another author's displacement or changes its measurement interval.
Previous positions and consecutive arrival times now retain the author
namespace; disconnect/reset retire that namespace, including DWORD clock wrap.
The standalone native regression covers interleaved equal keys from two
authors. The intermittent two-player hostile-NPC report still requires native
paired traces and is not certified by this arithmetic regression.

## 29. Incomplete provenance is an admission failure, not an empty world

The local `seven-sync-trade-native-20261015-2` game run completed streamed save
transfer and reconciled the 16-object procedural set in region `(20,32)`, but
HOST censuses for `(20,33)` and `(21,33)` stayed incomplete with 13 and 11
unknown-origin Camp Beds. JOIN did not send READY and native commerce was not
exercised. The run failed; a passing save-transfer or empty-region ACK cannot
override this outcome.

Observe a native factory's input provenance before invoking its original
function. Input save-state objects may be mutated or consumed there, and
delayed factory work may likewise be consumed before its return. Keep only the
captured generation/origin token across that call. Do not classify unknown
objects as procedural merely to unblock READY, and do not delete them.

The deferred nest path also needs native handle semantics. An absent
`insideBuilding` is `NULL_ITEM` with zero identity words, not an all-zero wire
sentinel. Comparing those representations rejected live, positively tracked
messages and made their generated beds unknown. Capture the constructor's
initialized payload, validate its queued lifetime and full parent-town handle,
and compare the native null representation at direct deferred virtual dispatch.
Player-owned births remain protected even inside a generation scope.

The post-correction `seven-sync-trade-native-20261015-9` run reconciled the
nonempty sets in `(20,32)`, `(20,33)` and `(21,33)` with 18, 12 and 12 objects,
respectively. HOST received matching count/digest ACKs before JOIN readiness.
That establishes initial procedural admission, not the unload/reload/reconnect
or shared-bed interaction acceptance. The commerce scenario still failed:
it selected actors before the first admitted remote actor/claim and performed
no purchase. Native transaction scenarios must wait for actual ownership and
stock within a bounded interval; missing prerequisites must remain a failure.

## 30. Reliable carry release outranks interpolated carry pose

The paired `seven-sync-carry-native-20261015-2` run reproduced a real native
pickup followed by placement in a Camp Bed. HOST received reliable DROP, then
ENTER several milliseconds later. Between those events, an older
`TASK_CARRY_BODY` sample issued `HEAL PICKUP`; the subsequent `HEAL DROP` tore
down bed occupancy. Checking only current/pending furniture occupancy did not
cover the DROP-to-ENTER interval.

Keep the released passenger's full canonical hand and authenticated carrier
author in the driven carrier state. That reliable release fences healing from
an older carry sample; only a later reliable PICKUP starts another carry
lifecycle. World/target teardown clears the state. Do not use a short timeout
that would let the old pose become authoritative again.

Native `setBedMode` is also insufficient for a conscious owned passenger:
the next AI tick can immediately get out of bed. Accepted owner-side bed
placement now supplies one native `USE_BED_ORDER` before attaching, after
clearing the previous action. Remote pose reproduction does not create player
orders. Native occupancy checks and explicit owner exit remain authoritative;
there is no repeating sleep order, teleport or forced sleeping-state flag.

`seven-sync-carry-native-20261015-6` passed in two actual Kenshi processes over
GNS UDP on the non-primary monitor: real cross-owner pickup/placement, 60 native
occupancy observations per side across 15 seconds, matching complete bed
descriptors and admitted actor identities, then explicit owner exit and
movement. Both engine logs reported zero double-destroy/zone-leak findings.
The scenario sampling cadence also stopped using elapsed zero as an
uninitialized sentinel: two engine ticks at zero had produced duplicate claim
observations and a correctly rejected oracle result in run 5.

## 31. A broad regression PASS is not seven-bug acceptance

The paired native series on 2026-10-15 produced additional limits:

- `seven-sync-extraction-native-20261015-1` did not exercise extraction. JOIN
  remained before READY while region `(19,31)` alternated native creation
  failure and removal; actual actor claims were consequently not admitted.
  Do not call this an item-conservation result or bypass procedural admission.
- `seven-sync-lock-native-20261015-1` observed the first native unlock and
  relock on both peers, but the second unlock failed. Its last periodic sample
  preceded the 120-second termination boundary. That is not reconnect/reload
  proof and must not be reported as a fixed lock lifecycle.
- `seven-sync-npc-native-20261015-1` passed the existing `assault_travel` gate,
  but fight parity was only 430/566 observations, with 136 JOIN-only fighting
  observations. The broad gate's PASS does not prove enemy-NPC state equality.
- `seven-sync-procedural-lifecycle-20261015-1` compared complete native
  populations, but the protected player-bed fixture failed creation before
  arm and region `(22,32)` contained no managed beds. Shared use and lifecycle
  acceptance therefore failed; an empty matching region is not a substitute.

Merchant runs 11/13 admitted the actual owner claims but still found no native
shop-source candidates. `ShopTrader` is nonphysical and its trade inventory is
an aggregate of home-furniture sections; a world-sphere query or the trader's
personal pockets cannot certify its stock. Native buy/sell, replay and wallet
acceptance remain unverified. Saved `BuildingItemGroup` item provenance also
remains unverified; unknown saved objects stay protected and environment-item
reconciliation remains Harness opt-in.

## 32. A native factory result still needs authoritative ownership

`seven-sync-extraction-native-20261015-2` reproduced an admission loop for
region `(19,31)`: a recreated outdoor Camp Bed had the exact requested native
position, quaternion, floor and construction flags, but owner `nofac` instead
of HOST's `200-gamedata.base`. Complete comparison correctly refused the
object, and the following reconciliation removed the positively identified
replica before another creation attempt.

Run 3 ruled out treating a non-null faction lookup as sufficient or attempting
to solve this by creating a faction: even with an exact validated faction
passed to the factory, the resulting bed still reported `nofac`. That attempted
get-or-create fallback was removed. Keep the exact-template/non-player lookup
check, then restore the authoritative owner using native `setFaction(owner,0)`
after native physical initialization when the resulting owner differs. Do not
write `owner` directly or ignore ownership in equality/digests.

Run 4 rebuilt core/UI and exercised the corrected path in actual paired
Kenshi processes over GNS UDP on monitor 2. Region `(19,31)` reconciled and
both peers acknowledged the same complete one-object snapshot, epoch 1,
digest `3536f095`, world `54572716898101`, load 1. The bed's native owner was
`200-gamedata.base`. JOIN then sent READY after all procedural controls;
HOST accepted it and resumed. Clock catch-up converged to 1x, and both native
engine-integrity checks passed.

The extraction scenario itself still failed: after actor admission the real
machine inventory had zero entries, so no real extraction was performed.
This run proves the corrected ownership/admission path, not mining-item
conservation, shared use, region `(22,32)` lifecycle, reconnect, AI or navigation
safety. Those acceptance checks must remain open.

## 33. Native preparation and diagnostic admission are part of the test contract

The environment-item scenario initially failed before launching Kenshi because
`KENSHICOOP_ENVIRONMENT_ITEMS` was missing from `CoopDiagEnvKeys`. It is now a
managed diagnostic key: the manifest can opt in, and the same reset clears it
before a subsequent free-play/scenario launch. The actual environment transition
in `AuditIntegration.Tests.ps1` failed before this change and passed afterward.
Do not leave the experimental category enabled through an inherited environment.

The subsequent paired `seven-sync-environment-native-20261015-2` run launched
successfully but still failed category acceptance: it did not pin a native
merchant room or emit an environment-item census sample. This is not proof of
saved ItemGroup provenance or reconciliation. Unknown saved items remain
protected.

`scripts/verify.ps1 -Configuration Harness -NativeBin <verified-provider-bin>`
then completed with OVERALL PASS (343 seconds), including the protocol/native
test binaries, PowerShell regression and safety suites, disposable updater
transactions, separate-process UDP provider/core consumers, reconnect/world
rotation and malformed endpoint isolation. Steam-unavailable rejection without
UDP fallback passed; an actual two-account Steam game session was not tested.
This gate preceded the later native mining preparation changes.

`seven-sync-extraction-native-20261015-5` issued an actual native mining goal to
the admitted HOST actor. Production rose from 0 to 0.303 while the physical
output inventory remained empty; the former 30-second preparation deadline
expired before a complete unit existed. The test now allows native 1x production
without injecting ore and measures its conservation leg from physical-stock
selection, not scenario arm. Completion still requires native stock, transfers
and paired state evidence. The lock scenario also emits a terminal native
sample at its 120-second boundary rather than ending before the oracle's final
observation window. These test corrections do not by themselves certify either
gameplay fix.

`seven-sync-hostile-native-20261015-1` passed the broad `mint_aggro` primary gate,
but its advisory mint-distance gate failed and sampled native fight parity was
only 31/38. Several JOIN combat orders precede the HOST scenario's explicit
close order, yet those JOIN orders already follow authoritative combat frames.
The 1 Hz HOST observations alone cannot establish that JOIN invented those
short-lived fights. Do not rewrite AI or revoke trust based only on that sampled
disagreement; source-frame/canonical-identity evidence is still required.

## 34. A conserved stack does not prove authoritative native production

`seven-sync-extraction-native-20261015-6` and `-7` produced actual copper at
native 1x through an admitted HOST actor, without injecting stock. HOST withdrew
the single physical stack; JOIN then observed source=0, own=0, peer=1 and its
cached second withdrawal returned false. This is a sequential winner/stale-retry
check, not simultaneous-request, replay, or reconnect evidence.

The first strict pairing attempt exposed a witness error: on HOST,
`invWireKeyForLocal` can name the peer's destination alias, whereas the actual
published inventory checkpoint uses `inventorySnapshotIdentity`. The Harness
query now reads that published identity on HOST and requires positive
round-trip resolution on JOIN. Do not require equal heap-local native hands or
equal process-local revision counters. Do require equal current world/load,
canonical source, canonical actor ownership, source contents and both endpoint
inventories.

The corrected `seven-sync-extraction-native-20261015-8` still failed the strict
gate, correctly. All 181 HOST samples were complete with progress=0.003; JOIN
settled at 0.024 for 176 of its 181 complete samples. The nearest-time paired
world/load, canonical source, physical stock, source hash and mirrored endpoint
units/hashes agreed; production progress differed in every such comparison.
Both local scenarios reported PASS, so those local labels must not override the
paired native gate. The captured scene shows the actors and mine, not an open
inventory panel; inventory evidence comes from native container reads.

Native logs confirmed that the operate detour installed and the mapped JOIN
root was admitted, but no native guard hit occurred. The output-mutation seam,
`ProductionBuilding::_NV_updateOutput`, is therefore being checked directly.
The guard is restricted to complete, positively paired, non-player production
roots without inputs; native AI/worker scheduling remains intact. Fractional
progress application uses the HOST float rather than treating values within
the old 0.01 publication quantum as equal. Compilation alone does not verify
this change: a subsequent native guard hit and paired progress/stock evidence
are required before claiming success.

The PowerShell oracle regressions passed with distinct local source hands and
revision counters, canonical-source/load mismatches, one-stack stale retries,
duplicate grants, missing attempts and malformed/overflowed numeric evidence.
Invalid evidence must fail the gate, not throw out of the test runner.

The earlier native lock fixture is also not a two-cycle unlock proof. Its HOST
actor was knocked unconscious after the first actual unlock; at the second
scheduled order the native task remained 65535. An order API returning true is
not proof that an incapacitated actor performed a second native unlock.

## 35. Pair native observations on one clock; inspect the actual virtual entry

`seven-sync-extraction-native-20261015-9` remained FAIL: HOST selected one actual
copper unit, whereas JOIN selected two. Do not reinterpret a later corrected
snapshot or a local scenario PASS as proof that this transient duplication is
fixed.

That run also exposed different scenario arm times. `t` and `preparedAt` are
process-local scenario clocks and must only be compared within their own leg.
The local Windows HOST/JOIN runner now records `sampleMs=GetTickCount64()` and
pairs observations on this shared boot clock. HOST's pre-JOIN completion window
ends at the actual JOIN attempt's sample time, not at an elapsed timestamp from
JOIN's unrelated arm clock. This is a same-machine runner contract, not a clock
synchronization scheme for logs captured on different PCs.

The PowerShell consumer regression reproduced the false rejection with a
20-second arm offset before the change and passed after it. Malformed sample
times and a regressing shared clock fail the gate. Canonical identities, complete
native contents, progress equality and single-winner conservation remain
required; timing correction must not weaken those checks.

The bounded `seven-sync-extraction-entry-20261015-10` diagnostic reached actual
native work, but its 55-second runtime did not produce a complete transaction.
Its FAIL verdict is not an acceptance run. JOIN logged SDK output RVA 0x298470
and instance virtual entry RVA 0x11A4A. The installed executable's latter entry
forwards through 0x58AB20 and an indirect jump; the RE_Kenshi reference executable
instead jumps directly to 0x298470. The on-disk images have different SHA-256
values. Inspect the instance's real virtual entry rather than assuming that
the reference-image function body is the active implementation.

The output guard now installs only after positive JOIN fixture admission and
targets that instance virtual entry. It does not claim a different implementation
is guarded, and HOST/freeplay still call the original. A native guard hit and a
successful paired stock/progress regression are still required to verify this
change.

For locks, the earlier actual HOST unlock appeared at scenario t=26015 and JOIN
t=26516; the HOST relock appeared at t=60015 and JOIN t=60516. This confirms that
first transition and its reversal, not the failed incapacitated second pick,
reconnect or region reload.

## 36. Steam connection lifecycle and admission are separate concerns

The supplied remote JOIN log records a null read in
`steamclient64.dll+0x116656d` one second after a second CONNECT, after the first
attempt was cancelled. That stack establishes the faulting module and retry
sequence, not the underlying Steam implementation bug.

The global-dispatch-only connection candidate was not sufficient. In the actual
Kenshi JOIN probe, the main-thread timer completed 1,552 successful provider pumps
while the native connection reached `ProblemDetectedLocally` with end reason
`5003`; no connection event reached the provider consumer. Authentication and
relay availability were both Current, and the connection's generation matched.
This distinguishes lost callback delivery from a missing main-thread pump.

The corrective candidate keeps the borrowed game's `SteamAPI_RunCallbacks` for
lobbies and explicitly drains the sockets interface's `RunCallbacks` on that
same thread. Core fences native startup as well as teardown against the pump.
The diagnostic probe records actual timer-pump results, and Steam provider
status includes the pending native state/end reason without credentials.

The paired before/after JOIN runs used real Kenshi on monitor2, the same real
Steam account/target and unused virtual port 213. With the explicit drain, the
consumer received `CONNECTING` after 63 ms and `DISCONNECTED` with native reason
`5003` after 30,032 ms. The same runtime disconnect-delivery assertion failed
before the change and passed afterwards. The echo probe correctly ended with
`failed=1`: no responding host was observed on that test port. This is callback
delivery proof, not a successful two-account connection or echo/reconnect pass.

Steam HOST no longer has a manual guest list in the provider, core, UI or host
runners. Identity authentication, encryption, expected JOIN host and capacity
checks remain. UDP invitations are authorization credentials, not allowlists.

The earlier global-only in-game HOST probe ran for 120 seconds on monitor2
without a guest list and without a guest. That observation did not test callback
delivery and must not certify JOIN, payload exchange or reconnect. A native
disconnect-callback regression can run against an unused virtual port with one
real account, but successful HOST/JOIN exchange and reconnect still require
two distinct Steam accounts.


## 37. Submitted Steam messages belong to Steam, not the caller's lease

The fresh remote `KenshiCoop_join (4).log` and local JOIN both faulted at
`steamclient64.dll+0x116656d`, reading null. In the installed local Steam DLL,
that instruction copies from the source pointer; the caller's assertion
literal identifies `steamnetworkingsockets_snp.cpp`. This establishes the
native sender path, not a complete remote register dump.

The provider's `SendLease` released an allocated message after `SendMessages`
when the caller's pointer remained non-null. That is unsafe with the borrowed
Steam interface: [Steamworks documents unconditional ownership transfer](https://partner.steamgames.com/doc/api/ISteamNetworkingSockets#SendMessages),
and the real native probe retained the caller's pointer after submission.
In contrast, the pinned OSS backend with its fourth argument set to `false`
clears successful caller pointers and leaves failed messages for the caller.
The previous UDP regressions therefore did not expose the native Steam defect.

The shared production sender now selects the OSS always-consume mode (`true`)
and never releases or inspects a submitted message on success or failure.
Validation, admission, traffic lanes, backpressure and result translation are
unchanged. This does not change the wire protocol or transport architecture.

Real Kenshi runs on monitor2 supplied a red/green regression. Before the fix,
`steam-probe-evidence/20261016-014106-641` delivered the native control message
then crashed with exit `0xC0000005` on the first production send. After the fix,
`steam-probe-evidence/20261016-014406-292` logged `LOOPBACK PASS pid=30280`:
26 exact deliveries, all three lanes in both directions, no extra deliveries,
and invalid-send recovery, using the same production sender and native Steam
library with deferred network-loopback delivery. The runner restored its
owned loader/probe/display changes and verified their hashes. This fixes the
reproduced message-lifetime crash; successful two-account P2P admission,
Internet routing, save bootstrap and reconnect remain unverified.

## 38. Native building population is a separate procedural birth path

The supplied `KenshiCoop_join (5).log` establishes successful two-account Steam
WELCOME and streamed-save commits with zero bad CRCs. Initial admission instead
stalled because HOST region `(20,33)` contained seven unknown-origin Camp Beds;
JOIN acknowledged the Hub set but never received that region's complete
snapshot. A transport connection or successful save transfer is not WORLD-READY.

The seven births originate inside native `RootObjectFactory::populateBuilding`,
outside the nest/deferred scopes. Their input has `firstTime=1`, base data source
`rebirth`, and live instance IDs. Track the native population scope before
calling its original, propagate its token through nested/queued work, and
unwind it in `__finally`. This added scope is bed-only: it must not activate
unverified ground-item reconciliation. Named save-state input alone does not
prove either procedural or persisted origin.

A generated HOST layout bed can become a saved-origin JOIN bed after the
authoritative save is streamed. Include such a positively traced, non-player
saved bed only when its full descriptor matches the wanted HOST object.
After complete-set/digest verification, adopt it as replica bookkeeping without
changing its native placement, layout/AI references or saved-origin destruction
protection. Preserve native journal identity; bind the current network identity
separately. Region readiness still gates interaction with these saved beds.
An existing bed at the requested physical placement blocks new creation if its
native state cannot be reconciled; a mismatch must not create a duplicate.

The first real `steam-world-ready-native-import-20261016` run acknowledged all
seven layout beds with digest `db90bbf9`, preserving the native saved objects.
It also exposed an independent creation failure: normalizing a HOST quaternion
again changed native yaw from `1.60785019` to `1.60784984`, crossing the canonical
`1e-4` boundary. Pass through the HOST's native quaternion rather than relaxing
the full state/digest comparison.

The `procedural_beds` HOST harness now imports its fixture through the existing
native import path, retaining the squad and player buildings while forcing
fresh world generation. Merely reloading a later autosave can mask missing birth
provenance because the objects have already become saved state. The paired
fresh-world runs above failed overall and do not certify WORLD-READY, shared
bed use or lifecycle acceptance. A run whose HOST arms on its timeout and exits
before JOIN finishes loading likewise provides no admission proof.

The serial `steam-world-ready-native-import-20261016-3` run passed the admission
path with the final binaries: all current native sets matched across peers
(53 managed beds plus 11 protected beds on each side), HOST committed the actual
requested region membership, JOIN emitted READY for load 1 at `03:08:12.645`,
and HOST accepted that READY at `03:08:13.844`. These are local UDP game-engine
results through the existing GNS provider, not a new two-account Steam run.
The final full game-independent verification gate also passed.

Do not relabel the whole game scenario PASS: same-bed native occupancy was not
observed before the 60-second scenario ended; the HOST actor was still moving
toward the selected bed. Unload/reload and reconnect were not exercised.
Rollback also encountered transient access denial removing two originally absent
test DLLs. Both were preserved, then removed after the owned processes exited;
the original failed verdict and recovery backups remain intact.

## 39. Reading merchant stock must not manufacture native garbage

`native_trade_watch` reproduced a 1–3 FPS HOST while JOIN remained above 200
FPS. Native frame profiling attributed about 410 ms per HOST frame to the
procedural census, not the native simulation. The native
`GameWorld::getIsInKillList` checks phase 1, then linearly scans phase 2; that
queue had grown into millions of entries. Batching those queries improved CPU
time but did not stop memory growth: run 8 added 1079.7 MiB of HOST private
memory over its final minute and ended at 9694.8 MiB.

The producer was the owned merchant view. `transactionInventory` rebuilt it
on every passive read. Native `ShopTraderInventory::updateInventory` clears
its display rows and copies physical shop items again; discarded copies enter
the engine's deferred-destruction queues. Repeated read-side refreshes therefore
created both memory pressure and expensive lifetime queries.

Retain the native view while its complete physical source rows are unchanged.
Compare source/physical full handles, section and item identity, exact quality
and remaining-charge bits, uniqueness, proper owner, uniform faction, quantity
and the complete supported item metadata. Reuse both comparison buffers.
Refresh through the native API when this set changes; do not replace
native buying/selling, synthesize stock, use a timer/hash as equality, or retain
the view across native world teardown. An already-open native shop window still
supplies its own view.

Run `other-todos-merchant-view-20261016-9-stock-source-cache` exercised the
original paired native scenario on monitor 2. HOST's final 40 timing windows
had median 216.5 frames and minimum 203; its final-minute private-memory change
was +0.375 MiB, with phase 2 empty. Over 100,000 reads reused a view refreshed
three times. Real native HOST purchase and resale remained successful.

This is performance/memory evidence, not merchant synchronization acceptance:
the strict paired trade gate still failed because JOIN did not certify the
stationary merchant's canonical inventory root. The temporary batched bed
lifetime workaround and detailed profiling probes were removed afterward;
the final candidate retains the engine's own lifetime checks.

The final minimal candidate was rebuilt and rerun as
`other-todos-merchant-view-20261016-10-minimal-native-lifetime` (207.69 seconds),
with the original native per-bed lifetime API and no profiling workaround.
Captured HUDs showed HOST 205 FPS and JOIN 229 FPS. HOST private memory ended
at 4634.9 MiB, changing +0.684 MiB in the last minute; JOIN changed +0.090 MiB
in its last 30 seconds and 0 MiB in its last 15 seconds. Both processes emitted
explicit post-verdict self-exit markers; health and native engine integrity
passed. This bounded run does not exclude every long-session leak or establish
the cause of an unrelated crash without its dump.

The source-matched, rebuilt `verify.ps1 -Configuration Harness` gate passed,
including real separate-process GNS/UDP and NetLink consumers. Paired native
trade acceptance remained FAIL for the JOIN merchant-root admission above;
neither that task nor two-account Steam gameplay is certified by these checks.

## 40. Detached item construction is not native item registration

`RootObjectFactory::createItem` and the exact `Sword` constructor accept the
handle supplied by their caller. Passing an empty handle does not register the
result, and adding it to an inventory does not repair that identity.

The paired native run `other-todos-merchant-view-20261016-12-source-lifetime`
observed a real merchant backing section at
`0,2069,11111,497,3900268544` retaining an item whose handle was `4,0,0,0,0`.
Resolving that handle returned null. About 2.5 seconds into the JOIN observation,
the complete physical-source guard consequently refused the merchant view.
Ignoring the guard or accepting that item would conceal the inventory defect.

`fabricateItem` now finishes detached construction through the SDK's native
`RootObjectFactory::copyItem` registration path. An already registered native
result is reused directly. The detached prototype is scoped and deleted, never
inserted into stock; no native handle-manager addresses or fabricated IDs are
introduced. This extra native construction occurs on item creation, not on
passive stock reads.

In `other-todos-merchant-view-20261016-13-registered-stock`, JOIN retained the
selected commodity and the complete stock hash `3953057378` in all 181 samples
over 90 seconds. Neither process reported an invalid physical merchant source.
HOST completed its actual native purchase and sale; native engine integrity
passed. The whole paired trade gate still failed because JOIN had not certified
the merchant root, so registration alone is not multiplayer trade acceptance.

Inventory-root evidence must use inventory authority, not require a stationary
merchant to participate in the NPC movement stream. The receive state retains
the authenticated wire root. The native witness requires its exact round trip,
HOST ownership, a nonzero applied revision, no dirty or truncated snapshot, and
an exact duplicate-aware comparison of all current supported inventory rows.
Matching only a raw local hand, a count or a hash cannot certify convergence.

`other-todos-merchant-view-20261016-15-root-difference` exercised the stronger
witness without weakening the PowerShell trade oracle. JOIN attempted an actual
native purchase and received an ACK with `applied=0/1`. At that point its source
revision was still 1, while HOST's completed purchase/sale had advanced stock to
revision 3. This proves stale client state, not the sole native rejection cause.
Later, full row comparison exposed a grid mismatch for
`95764-rebirth.mod`: byte 67 (`gridY`) was 0 locally and 1 in the received
revision-3 row. The legacy content hash excludes grid coordinates, so equal
`3953057378` hashes were not proof that the inventories fully agreed.

The paired trade gate therefore remains FAIL. Do not certify merchant
transactions, replay, reconnect or concurrent clients from the FPS fix or the
registered-item result. The temporary invalid-source and row-difference probes
were removed after preserving these logs; the full-state witness remains.

The final clean runtime was exercised again in
`other-todos-merchant-view-20261016-16-final-fps-registered-stock`: HOST/JOIN
screenshots showed 210/221 FPS. In 58 memory samples over the final 58.74 seconds
before the scenario verdict, private memory increased by 0.828/2.645 MiB
respectively. Native health and engine-integrity gates passed; both later exits
were explicitly marked Harness scenario-verdict exits. This is bounded
short-run evidence, not a long-session zero-leak proof or attribution of the
original user-reported crash.

The source-matched rebuilt `verify.ps1 -Configuration Harness` subsequently
passed all four summary gates (unit, contract, oracle and audit integration),
including the real separate-process GNS/UDP and NetLink consumers. The actual
paired native trade gate still failed and is not overridden by that suite.

## 41. Publishing equal contents must not discard a newer transaction revision

`InvPub::revision` records the last revision actually queued to the network.
Do not overwrite it merely by observing current native contents. A purchase
followed by resale can restore the previous content hash before the removal
settle window expires; comparing only that hash suppresses the newer revision
until the periodic refresh, up to 30 seconds for a large merchant inventory.

The publisher now compares the current native revision as well as contents,
and advances its recorded revision only when queueing that snapshot. Existing
cursor/drop/checkpoint fences remain in place.

In `other-todos-merchant-view-20261016-17-published-revision`, JOIN observed
revision 3 before its purchase at 30 seconds. HOST acknowledged that purchase
with `applied=1/1`; JOIN's commodity increased to one, merchant stock decreased
by one and its wallet decreased from 2398 to 1932. The next native sale was
refused, so the complete paired trade gate remained FAIL. A successful purchase
is not acceptance of resale, replay, simultaneous access or reconnect.

## 42. NPC buyItem is not the player commerce path

The loaded RE_Kenshi image's `Inventory::buyItem` body (RVA `0x749AF0`)
splits one unit and uses the receiver faction for pricing. It does **not**
guarantee delivery: with a non-player source, a native random branch can
bypass `giveItem` and still charge money. The observed direct caller is NPC
shopping code. Do not use this method to implement or certify player commerce.

The player interface uses `InventoryGUI::RClickAutoTrade` (observed native
RVA `0x713150`) and separate mouse/stack paths. The native transaction fixture
now opens a real money-trading partnership and invokes the actual right-click
member with its correct hidden-return-pointer ABI, rather than the NPC method.

`Inventory::hasItem(Item*)` is not a complete ownership witness: in
`other-todos-merchant-view-20261016-19-live-shop-view`, the purchased registered
weapon was in the player's inventory and automatically equipped, but
`hasItem` returned false. This caused JOIN's sale to be refused and the HOST
fixture to try inserting an already-transferred weapon again. Check physical
sections and held/equipped slots; require the actual item/root association
for an actor sale. A shop's native null handle (`91,0,0,0,0`) is shared by
different catalogs: obtain the actual backing trader instead of selecting
the first equal null handle.

A merchant catalog is a native projection over physical furniture inventories.
Do not independently rewrite both the catalog and those physical sources.
Refresh the selected live/owned view on exact physical source changes, and
certify its revision only after its full semantic contents match HOST.

Runs `other-todos-merchant-view-20261016-20-native-unit-contract` and
`...-21-native-purchase-hold` exercised the NPC method, not player commerce.
Run 21 passes the corrected paired temporal oracle for that exercised method;
this is not evidence that the player's shop interface synchronizes.

Run `...-22-native-stack-before-transfer-cleanup` exposed a duplicate: two
steel bars became three after resale. A second insertion of a native-returned
item can reinsert a pointer already merged and queued for destruction.
Removing that insertion eliminated this duplicate/double-destroy in run
`...-23-native-stack-single-writer`, but the NPC method then charged the
buyer without delivering a unit. That candidate fails commerce acceptance.

Run `...-24-native-player-trade-entry` exercises actual player right-clicks.
HOST bought and resold one real steel bar: source 2 -> 1 -> 2, actor 0 -> 1
-> 0, wallet 1000 -> 504 -> 1000. JOIN's purchase then produced actor=1 and
wallet=504, but HOST's source remained 2. JOIN's certified projection restored
its displayed source to 2 as well. The paired gate and JOIN terminal result
are FAIL. The NPC-only hook does not authorize the player's real mutation.
Merchant synchronization remains unresolved; no failed candidate is packaged.

An untagged native `addItem` double-destroy is still a failed lifetime gate,
not proof that the engine alone is responsible. Run 22 contains that error
at the duplicate resale. `engine_integrity` is now mandatory for native
commerce/extraction scenarios; attribution is reported separately from safety.
Regression fixtures reject both untagged double-destroy and dangling-zone
errors and accept complete clean archives. Original run verdicts are preserved;
separate rechecks classify run 22 lifetime FAIL and run 23 lifetime PASS.

Run `...-25-native-hidden-player-context` confirms HOST can invoke that same
player method using genuine hidden native trade windows while preserving the
existing trade-partner map and town context. The borrowed trader window must
not retire the global UI ShopTrader when destroyed. Construct the context
before preparing windows so failure restores the saved native UI state.

Run `...-26-native-authoritative-player-context` intercepts JOIN before local
mutation, executes its real purchase on HOST and delivers ACK 1/1: quote 496,
wallet 1000 -> 504, physical receiver one unit. Its overall verdict remains
FAIL: the native-action driver incorrectly treated the intentionally refused
local call as a failed submission and exited before observing the ACK.
A queued intent is not a successful purchase: the driver must keep observing
actual stock, actor contents and money, including the subsequent native sale.
Never change the native refusal into a fabricated successful TradeResult.

HOST price limits are checked against the actual native player price. Outside
the authorized call, price and money getters are unchanged. A quote mismatch
uses the native insufficient-funds rollback before insertion/payment rather
than reimplementing item splitting or changing the charged price.

Run `...-27-native-player-async-observation` passes the strict paired native
stack-trade gate: both participants bought and resold a real unit from the
two-unit steel-bar stack, final stock returned to two, actor units to zero,
and shared wallet to 1000. JOIN purchase and sale received separate ACK 1/1;
HOST committed op=1/id=1 and op=2/id=2, wallet 1000 -> 504 -> 1000.
Both terminal results, process exits and native lifetime gate pass.
This verifies single-right-click buy/resale, not replay/races, bulk/drag
gestures, merchant unload/reconnect or two-account Steam gameplay.

## 43. Resource-node faction adoption does not revoke admitted authority

Native work adopts the mine into the worker's faction. In
`other-todos-native-extraction-20261016-02-progress-diagnosis`, JOIN first
admits the complete zero-input production node while neutral, then its real
`updateOutput` hook sees the same full lifetime hand with `playerOwned=1`.
Reapplying the initial ownership filter silently disabled output authority.
Physical stock was conserved, but the next-unit progress diverged permanently:
HOST `0.002`, JOIN `0.021`. Both original failed runs remain preserved.

Initial admission still excludes player-owned production buildings and requires
the authenticated native fixture binding, complete production class, matching
output template and the hooked native virtual implementation. Subsequent calls
retain authority for that exact admitted lifetime hand across faction adoption;
unknown player-owned objects remain native. Worker AI is not disabled.
HOST's versioned inventory channel alone materializes output stock; scalar
progress updates never call the native item-producing setter.

`other-todos-native-extraction-20261016-03-faction-adoption` passes the strict
real HOST/JOIN gate. The native miner produces one copper unit, then stops;
HOST takes it and JOIN's later stale attempt returns refusal. Sustained complete
terminal samples show source zero on both sides, exactly one unit at HOST and
none at JOIN, and next-unit progress `0.002` on both sides (159 HOST and 119 JOIN
samples). Both terminal results, process exits, health and native lifetime gate
pass. No ore was seeded. This case does not certify simultaneous contenders,
replay, unload/reconnect or two-account Steam gameplay.

`other-todos-native-extraction-20261016-06-simultaneous-readiness` separately
passes a real two-process contention case. Both native extraction calls occur
at boot-clock millisecond `68868171`; both initially return success. HOST then
rejects JOIN's stale transfer and JOIN rolls back its speculative item. The
complete paired state retains one unit at HOST, zero at JOIN, an empty source
at revision 3, unchanged wallets, and output progress `0.004` on both sides
(168 HOST and 169 JOIN terminal samples). Native lifetime, health, process exits
and both terminal results pass. This is one observed contention, not general
replay, unload/reconnect or two-account Steam certification.

The first contention fixture failed honestly: a deadline guessed before game
launch expired while the native mine was only at progress `0.837`. Its FAIL
and memory evidence remain preserved. The corrected Harness-only fixture uses
a unique local file barrier after BOTH complete native stock/actor baselines,
then schedules a shared future boot-clock deadline. The oracle still requires
both actual native calls within 250 ms, independently matched full endpoint
state, source revision advance and a sustained single winner; marker files
are scheduling inputs, never synchronization or transaction proof.

## 44. Native runtime restoration must wait for file release, not hide failure

Two private runs failed rollback when removing a newly deployed runtime DLL,
even though the owned game processes had exited. A later independent check
found no Kenshi/WerFault processes and successfully opened the same file
exclusively. The owner and cause of the transient access denial are unproven.
Original rollback directories and complete failed-run evidence were retained.

`Restore-CoopRunState` now retries only Windows access/sharing/lock errors
(5/32/33), bounded by a monotonic five-second resource wait. Every attempt
revalidates destination topology and reads existing backup data before removal.
Owned-process closure is still mandatory. Other errors fail immediately;
permanent denial still fails and retains both original backups and recoverable
current files. A restored runtime never changes a failed gameplay verdict.

`HarnessSafetyRegression.Tests.ps1` reproduces the old failure with a real
Windows delete-sharing handle released by a background thread. With the fix,
exact original bytes are restored. A permanently held handle still rejects
restoration and preserves recovery data. Missing backups, junction redirection
and uncertain process ownership remain rejected. Private restoration regression
tests also pass; these are filesystem-safety checks, not gameplay certification.

## 45. Hostile NPC diagnosis needs canonical native/stream pairs

`other-todos-hostile-native-20261016-01-mint-aggro` fails the real proxy-drift
gate (maximum 463 units); its complete FAIL remains preserved. The largest
stationary-owner mismatch occurs immediately after minting the hostile squad.
The advisory fight-state check passes only 37/48 paired samples and does not
certify NPC synchronization. Screenshots capture initial gameplay, not combat.

`KENSHICOOP_NPC_PAIR_TRACE=1` enables the existing opt-in native/stream trace
through the canonical Harness diagnostics whitelist. The trace includes the
complete wire key, local native character-key fields, stream owner/freshness,
sample age, local/remote cells, transforms, tasks, body state and task subjects.
The local fields retain `readHand` character-key order
(index, serial, type, container, containerSerial), not object/wire-key order.
Free-play environment reset clears this diagnostic. Enabling a trace is not a
production NPC fix; actual before/after state and gameplay remain required.

`other-todos-hostile-native-20261016-02-pair-trace` also fails; fresh full-key
traces show noncombat authority alongside a locally fighting proxy. Native03
passes position bounds but fails enemy fight parity (5/13 JOIN-only samples).
The previous advisory classification incorrectly left that run overall PASS.
A native reactive-attack suspension candidate was built and exercised in
native04. It blocked real suspended-body attack calls but did not fix the NPC
scenario (463-unit drift, 10/28 JOIN-only fight samples). That candidate and its
diagnostic detour were removed, not distributed as an NPC fix; exact sources,
binaries, native logs and memory samples remain in the private evidence archive.

## 46. Native enemy parity must not certify unreadable or unrelated bodies

`Get-EnemyStateParity` requires readable native samples and a consistent complete
`canon5` identity; matching only index/serial aliases distinct containers or
generations. Feasible chronological pairing consumes each HOST sample once. `mint_aggro`
requires all spawned identities to have paired observations and at least one
actual paired native combat observation, so mutually inert bodies cannot certify
combat synchronization. Minted assault checks cannot substitute unrelated entity
telemetry when the required native enemy series is missing.

Nine adversarial cases previously passed incorrectly: changed type, container or
container generation; missing full identity; contradictory short/full identity;
unreadable native state; both sides inert; one reused HOST observation; and a
missing squad member. They now fail while readable full-identity combat passes.
Reanalysis of the unchanged native03 logs rejects the run. `mint_aggro` now gates
on native fight parity and native engine integrity in addition to proxy drift
and clock synchronization. This corrects test acceptance, not NPC gameplay.

Nearest-and-consume matching could discard an earlier usable HOST observation:
eight 1 Hz combat observations with JOIN sampled 900 ms later produced only seven
pairs despite the 1500 ms tolerance. The public gate reproduced that false
rejection. Match the earliest feasible ordered observations instead; discard only
expired HOST samples and retain future ones for a later JOIN. Selection does not
consult combat state or lower the minimum evidence count. Regression fixtures
cover both phase directions, inclusive/exclusive tolerance endpoints, unequal
cadence, repeated timestamps, one-use cardinality and opposing combat transitions.

Each temporal fixture uses distinct log paths: the analysis caches immutable log
timelines by path within one run. Rewriting a fixture file between direct gate
calls reuses the old timeline and invalidates boundary checks. The first combined
verification caught two such fixture-isolation failures; isolated fixtures reject
both out-of-tolerance cases. `verify.ps1` also lists failed additional regression
consumers by name instead of showing only its four older group summaries.

## 47. A native lock test needs a claimed, capable actor

The first native lock run reported zero lockpick chance without completing the
required paired transitions. A subsequent `sync` run pinned the same full HOST
actor identity on both sides and read back real LOCKPICKING stat 100, but still
failed final unlock: chance remained zero and the actor later entered combat
and a down state. `sync` is therefore not inherently a peaceful usable fixture.
Claimed identity and skill readback alone do not establish native capability.

Keep native order issuance, the engine's reported chance, task/subject readback
and paired lock transitions as distinct evidence. A composite test flag that
also requires positive chance cannot prove that `addOrder` was never called.
The failed run's first unlock window and matching task target need to be
distinguished from its failed final transition, not dismissed or certified.

In the inspected Steam 1.0.65 executable, native RVA `0x884dc0` for
`Character::getLockpickChance` ends by zeroing its return register. Remove that
unsupported query and its output parameter, not the actual lockpick requirement.
`order=x/y` now records native submissions; task 76 and full task subjects are
supplementary because instant native success can finish between samples.

The disposable fixture claims the exact chest and its actual enclosing building
through the existing native deed API and reads ownership back on both peers.
Only HOST stages the actor before each order. Healthy stealth/low postures are
allowed by semantic body predicates; down, carried, occupied and incapacitated
postures remain rejected. The scenario enables real native stealth to exercise
that distinction.

`ChestLockScenario` records actor ownership, full local/canonical identities,
skill and body state, plus the complete chest identity, template and position.
It still requires two genuine PICK_LOCK transitions with a native relock between
them; no forced unlock or synthetic success is permitted. The paired native
state windows and engine-integrity gate remain mandatory. Fixture preparation
alone is not successful lock replication, reconnect or cell-reload acceptance.

The scenario emits `CRIT LOCK` rather than generic `SCENARIO MEMBER/RECV` rows.
Its manifest supplies those actual HOST/JOIN screenshot anchors and a ten-second
delay. Waiting for absent generic markers let both processes exit before any
capture; no visual proof was obtained from that failed run.

The rebuilt `other-todos-native-lock-20261016-03-owned-exact-fixture` pair passed
all four native lock windows and engine integrity. Both submissions used the
same full chest `0.2069.11111.500.2016845696`; the claimed actor remained healthy
in body state 136, entered native task 76 against that exact chest, and completed
both unlock legs. This exercises the semantic posture guard rather than requiring
body state zero. Reconnect and unload/reload were not exercised by this run.
HOST/JOIN images exist, but the verdict's generic `captureOk` is false and the
close-up does not establish global navigation, AI or player-building safety.

## 48. Bounded NPC diagnostics must reserve evidence for delayed combat

`KENSHICOOP_NPC_PAIR_TRACE` records native capture, spawn stages and actual
driver/action branches without changing AI, targets or placement policy. Its
disabled path returns before diagnostic reads and slot searches; enabled records
use fixed arrays and process-lifetime limits, not growing maps.

Idle periodic output must not consume every record before a later combat edge.
Capture reserves 24 of 32 records per actor and 768 of 1024 globally for meaningful
transitions; idle actors use only the first 32 of 64 slots so late first-seen
combatants retain capacity. Native NULL_ITEM garbage is ignored only when deciding
diagnostic priority; the original payload and production target semantics remain.
Drive retains its 2048-admission, 24-per-actor/path and 256-slot hard bounds while
routine output stops at the lower half of those budgets. Episode eligibility is
captured before disarm clears gameplay state; a next-snap marker is consumed only
after its follow-up record is admitted, never instead of the immediate post-snap
observation. Lifetime exhaustion still intentionally stops recording.

These are causal observations, not an NPC synchronization fix. Bootstrap
placement, retained combat after HOST sends NONE, and later vertical drift remain
separate hypotheses until actual paired native traces identify the responsible
writer and action lifetime. Arrival clocks are not invented wire-capture IDs.

The subsequent `other-todos-hostile-native-20261016-05-causal-observation` run
passed the unchanged gates: 752 drift samples, median zero, maximum 135 units;
34 of 42 combat observations agreed, with four HOST-only and four JOIN-only.
That observational pass does not supersede earlier failures or certify an NPC
gameplay fix. The exact tested source predates the next correction wave.


## 49. Fresh noncombat state must eventually release retained NPC combat

Paired native traces showed JOIN retaining native combat for four seconds after
HOST had published `TASK_NONE`, despite continued accepted noncombat updates.
The NPC-only clear path now reuses the existing 1200 ms task-clear interval.
It requires an accepted noncombat receipt at or after that interval; silence,
an old receipt or a renewed combat observation cannot certify the transition.
The four-second drought fallback and existing native cleanup remain unchanged.

Use `Driven::lastSeenMs` for the local accepted-receipt clock. Despite its current
header comment, `Interp::lastArrivalMs` receives the mapped capture stamp from
`Replicator::ingest`; it is not a request generation or an independent receipt clock.
A bootstrap-buffer candidate relying on that interpretation was rejected and
removed, rather than treating delayed spawn replies as identified generations.

The regression cases cover the inclusive clear boundary, fresh versus stale
receipts, drought, renewed combat and clock wrap. Core/UI Harness builds and
the complete verification pipeline passed. The first post-change native pair
was deliberately terminated by its memory safety observer before JOIN loaded
the world; it supplies no post-change combat acceptance.

## 50. Retiring one owner must invalidate its native transaction admission

When one peer leaves while another remains, HOST can retain the departed
owner's admitted native inventory revision while a replacement owner publishes
a new revision sequence. Retire the translated native inventory admission
before removing that owner's received inventory records. Preserve the monotonic
checkpoint fence while clearing the current revision, hash and admitted state.

Only complete, untruncated native recapture equivalent to the received checkpoint
can readmit the source. Source revision checks reject zero and require equality.
Do not reset the whole session or add a second transport disconnect flush:
the existing last-peer reset already clears the entire transaction state.
This correction concerns partial departure, not ordinary two-player teardown.

The lifecycle and admission regressions passed in the complete verification run.
They do not replace a native multi-peer departure/rejoin exercise; earlier
two-player merchant and mining successes do not establish that acceptance.

## 51. A procedural READY result must certify the current native census

Native procedural work can change provenance between coordinator scans. A
cached coordinator READY must not remain usable after that native revision
changes. `NativeCensusFence` records the revision at scan start and certifies
only a complete scan that finishes at the same revision. Readiness, region
queries and canonical bed bindings require that certification to remain current.
Resetting native provenance also advances the revision.

The native bed fixture now waits for its real factory/protected baseline and
compares the complete current census rather than accepting an empty region.
JOIN must observe HOST using the selected canonical bed and then select that
same identity; disappearance cannot silently select a replacement. Descriptor
comparison retains type, position, rotation, floor, parent and required states.
Unchecked procedural categories remain disabled.

Fence, retirement and reload regressions passed. Native same-bed use and the
complete unload/reload/reconnect lifecycle remain separate, unverified gates
until a real paired run exercises them.

## 52. An incomplete native run does not identify a crash cause

`Test-LogHealth` rejects missing gameplay or clean completion evidence without
guessing that the process crashed. Actual process-exit failures and native
exception evidence remain separate gates; removing that guess does not turn
an incomplete or deliberately terminated run into a pass.

Two private post-change pairs were stopped by their own memory observer when
free system commit crossed its unchanged 2 GiB floor. Both processes were still
responding. The second pair reached save-transfer commit and JOIN native loading,
not complete procedural census or the two-actor lifecycle. Preserve these FAILs
and their safety records; neither accepts nor rejects the gameplay correction.

The observer must follow the owned runner's lifetime rather than expire after
900 seconds while a later JOIN remains inside its allowed deadline. The actual
old/new scriptblocks reproduced that expiry and continued sampling beyond it,
without launching games or admitting any process to their stop set.

Measure memory admission inside the observer before publishing its ready marker:
a parent's earlier snapshot can become stale before native startup. The next
private recipe requires 15 GiB free commit, approximately the previous successful
starting envelope, while retaining the existing runtime stop floor. Its actual
low-memory refusal was exercised without invoking the game-launching script.
This is a conservative test prerequisite, not a mod memory limit or leak proof.

## 53. Environmental-item readiness also needs a current native certificate

The environmental journal advances on queued/deferred creation, birth, exclusion,
removal and reset. Clearing a native permission map is insufficient if a later
coordinator update can republish an old cached READY. Reuse `NativeCensusFence`
for captures, post-reconciliation verification and loaded-scope certification.
Carry the exact successful certificate into native interaction permissions;
never certify an old capture against a newer revision.

Region, scope, item and peer readiness, snapshot sending and ACK acceptance
require current certificates. A transferred complete snapshot alone supplies no
native proof. Failed or partial captures revoke the old proof; reconciliation's
own native mutations can defer recovery to the next loaded-set scan. Queries
must not rescan the whole world merely to check certificate freshness.

The state regressions exercise invalidation, reset, incomplete capture and
recovery. They do not establish native coordinator scheduling, asynchronous
hook behavior, floor-item pickup or complete HOST/JOIN object-set equality.
Matching tolerances, safe-removal rules and enabled categories are unchanged.

## 54. A reused JOIN slot needs a new door-request sequence namespace

With HOST and multiple JOINs, partial departure preserves the live door cache.
A replacement process starts its request counter at one; retaining the old
slot's `seqSeen` can reject its legitimate OPEN requests. Retire only that
departed sender's guards. Preserve the HOST durable lock/open baseline, surviving
senders' replay guards, pending native state, translated identities and outbound
sequence counters.

The existing last-peer reset already clears the whole cache; ordinary paired
reconnect is not this defect. Stateful authority regressions cover restarted
requests and surviving replay guards. Native acceptance still requires a real
three-peer partial-departure/rejoin exercise, not a two-game lockpick run.

## 55. Client connected and HOST admitted are different bootstrap milestones

The standalone consumer fixture used to broadcast a one-shot reconnect
`LOAD_GO` after observing only client `NET_CONNECTED`. That transition follows
SESSION_ACK submission; HOST admission occurs when HOST consumes that ACK.
Publication fanout excludes unadmitted peers, so the fixture could omit the
rejoining recipient entirely. Normal game bootstrap instead starts from HOST's
post-admission connect event.

Wait for the actual HOST admitted roster before both reconnect publications,
using the original first-recipient GO deadline. Keep exact GO/READY, malformed
peer rejection, healthy-peer retention and old-world fencing checks unchanged.
Failure diagnostics record the real recipient, observed GO/world, phases,
errors and bounded queue state; do not guess the absent recipient or add retries.

The original failing log is preserved. The corrected actual NetLink consumer
passed its standalone smoke run, but its missing-recipient diagnostic was not
present in the old failure, so that episode's unique cause remains unproven.
Standalone provider/consumer success does not certify Kenshi gameplay or
two-account Steam connectivity.

## 56. Merchant drag is not a right-click with different coordinates

The field report distinguishes a working right-click purchase from a broken
drag. Native `pickupItemToMouse` detaches or splits the source before the target
is known; `placeItemFromMouse` has its own payment and section-callback path.
Hooking only `RClickAutoTrade` cannot authorize that gesture.

The drag adapter records the source before pickup, reads the actual held cursor
instead of the hover item, and preserves native merchant display bookkeeping
while suppressing JOIN write-through to physical backing stock. Before queuing
commerce it restores source ownership through native section insertion, checks
matching-unit conservation, clears the icon and reacquires a current source row.
Do not use `returnItem` as a rollback: its fallback can drop the item to the floor.
A normal insertion refusal retains the live remainder and cursor. Absence from
the world-object registry does not prove a merchant display copy was destroyed.

Keep native one-unit right-click calls one-unit-only. A selected stack uses one
prepared native trade context and checks each actual unit price against the full
selection quote. Readback counts the complete inventory tree, since native
auto-placement can insert a purchase into a worn backpack. Report only conserved
source decrement/destination increment; a partial native result is not a grant
of the whole request. Money, insertion and insufficient-funds rollback remain
native, and existing operation IDs prevent replaying an already committed trade.

Cursor deferral must also preserve the receiver's dirty actor baseline. An
engine apply refusal followed by revision admission would let a later purchase
checkpoint overwrite newer JOIN contents with stale HOST contents. Defer at
`applyInventories` before clearing dirty or releasing reservations, and keep
HOST commerce queued until that retained baseline has actually applied.

Rule/frame regressions and static ABI review do not certify the gesture.
Paired native acceptance still needs real buy/sell dragging, split and whole
stacks, partial funds/space, cancellation/UI close, backpack placement and the
working right-click path, with HOST/JOIN stock, actor units and money compared.
The private game pair remains subject to the existing memory and second-monitor
admission rules; no unavailable native run may be reported as passing.

