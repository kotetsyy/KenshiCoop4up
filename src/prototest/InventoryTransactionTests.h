#ifndef KENSHICOOP_INVENTORY_TRANSACTION_TESTS_H
#define KENSHICOOP_INVENTORY_TRANSACTION_TESTS_H
#include "../plugin/core/InventoryTransactionRules.h"
#include "../netproto/Wire.h"
#include <cstdio>
#include <climits>
inline int testInventoryTransactionRules() {
    using namespace coop::inventory_transaction;
    int failures=0;
#define TX_CHECK(name, value) do { if (!(value)) { std::printf("FAIL inventory transaction: %s\n",name); ++failures; } } while (0)
    TX_CHECK("buy validates exact quote and funds", validate(1,4,4,3,3,25,75,75)==VALID);
    TX_CHECK("unagreed stock is not spendable", validate(1,0,0,3,3,25,75,75)==STALE);
    TX_CHECK("old stock cannot grant remaining units", validate(3,4,5,3,3,0,0,0)==STALE);
    TX_CHECK("quantity is all or nothing", validate(3,4,4,4,3,0,0,0)==QUANTITY);
    TX_CHECK("zero quantity cannot consume an operation", validate(3,4,4,0,3,0,0,0)==QUANTITY);
    TX_CHECK("client cannot choose its price", validate(1,4,4,3,3,25,74,75)==PRICE);
    TX_CHECK("concurrent purchase rechecks cash", validate(1,4,4,3,3,25,75,74)==FUNDS);
    TX_CHECK("negative price rejected", validate(2,4,4,1,1,-1,-1,100)==PRICE);
    TX_CHECK("quote multiplication cannot overflow", validate(1,4,4,2,2,INT_MAX,0,INT_MAX)==PRICE);
    TX_CHECK("sell requires merchant funds", validate(2,4,4,2,2,25,50,49)==FUNDS);
    TX_CHECK("native purchase may improve quote without requiring quoted funds",
        validate(1,4,4,1,1,20,25,20)==VALID);
    TX_CHECK("native resale may improve minimum receipt through buyback pricing",
        validate(2,4,4,1,1,30,25,30)==VALID);
    TX_CHECK("native purchase cannot exceed buyer ceiling",
        validate(1,4,4,1,1,26,25,100)==PRICE);
    TX_CHECK("native resale cannot underpay seller floor",
        validate(2,4,4,1,1,24,25,100)==PRICE);
    TX_CHECK("improved native resale still requires actual merchant cash",
        validate(2,4,4,1,1,30,25,29)==FUNDS);
    TX_CHECK("extract does not debit money", validate(3,4,4,2,2,0,0,0)==VALID);
    TX_CHECK("native commerce cannot grant two units from one native call", !nativeQuantitySupported(1,2,5));
    TX_CHECK("native sale cannot consume a whole larger actor stack", !nativeQuantitySupported(2,5,5));
    TX_CHECK("native purchase splits one unit from an existing stack", nativeQuantitySupported(1,1,5));
    TX_CHECK("native sale splits one unit from an existing actor stack", nativeQuantitySupported(2,1,5));
    TX_CHECK("one existing unit can use native buyItem", nativeQuantitySupported(1,1,1));
    TX_CHECK("native extraction admits real partial remove quantity", nativeQuantitySupported(3,2,5));
    TX_CHECK("native extraction never overdraws physical stack", !nativeQuantitySupported(3,6,5));
    TX_CHECK("no native action admits zero quantity", !nativeQuantitySupported(1,0,0));
    TX_CHECK("native operation kind is never inferred from quantity", !nativeQuantitySupported(0,1,1));
    TX_CHECK("bulk buy compares each native unit with full selected ceiling",
        nativeBatchPriceWithinQuote(1,20,3,60) && !nativeBatchPriceWithinQuote(1,21,3,60));
    TX_CHECK("bulk sale cannot reuse total floor as a single unit quote",
        nativeBatchPriceWithinQuote(2,20,3,60) && !nativeBatchPriceWithinQuote(2,19,3,60));
    TX_CHECK("bulk actual quote multiplication cannot overflow",
        !nativeBatchPriceWithinQuote(1,INT_MAX,2,INT_MAX));
    TX_CHECK("bulk quote refuses zero quantity and negative native price",
        !nativeBatchPriceWithinQuote(1,1,0,0) && !nativeBatchPriceWithinQuote(2,-1,3,0));
    TX_CHECK("one native call counts only a conserved physical unit",
        nativeUnitMoved(5,4,2,3)==1);
    TX_CHECK("native failure with unchanged stock never grants selected quantity",
        nativeUnitMoved(5,5,2,2)==0);
    TX_CHECK("removal without destination insertion is not a committed unit",
        nativeUnitMoved(5,4,2,2)==0);
    TX_CHECK("one native call never acknowledges a multi-unit grant",
        nativeUnitMoved(5,2,2,5)==0);
    TX_CHECK("split stack remainder does not count as transferred",
        nativeUnitMoved(10,9,7,8)+nativeUnitMoved(9,9,8,8)==1);
    TX_CHECK("wire quantity boundary remains all-or-nothing", validate(3,4,4,65536,65536,0,0,0)==QUANTITY);
    Revision sourceVersion;
    sourceVersion.adopt(8); sourceVersion.adopt(3);
    TX_CHECK("late source publication never rewinds quote namespace", sourceVersion.current()==8);
    sourceVersion.adopt(0);
    TX_CHECK("accepted sale invalidates quote without rewinding version", sourceVersion.current()==0 && sourceVersion.revision==8);
    sourceVersion.adopt(8);
    TX_CHECK("equal old actor replay cannot re-enable sold stock", sourceVersion.current()==0);
    sourceVersion.adopt(9);
    TX_CHECK("new actor publication reopens source with newer revision", sourceVersion.current()==9);
    TX_CHECK("unknown operation rejected", validate(9,4,4,2,2,0,0,0)==OPERATION);
    Revision revision;
    TX_CHECK("real empty stock receives a nonzero initial revision", revision.observe(0)==1);
    TX_CHECK("periodic identical snapshot does not advance version", revision.observe(0)==1);
    TX_CHECK("stack quantity change advances version", revision.observe(19)==2);
    TX_CHECK("actor checkpoint has its own monotonic namespace", revision.checkpoint()==3 && revision.revision==2);
    revision.revision=1;
    TX_CHECK("later peer publication cannot rewind host checkpoint", revision.checkpoint()==4);
    revision.revision=100;
    TX_CHECK("checkpoint fence clears known author version", revision.checkpoint()==101);
    revision.revision=0;
    TX_CHECK("invalidating sold source does not rewind checkpoints", revision.checkpoint()==102);
    std::map<std::pair<unsigned int,unsigned int>,coop::InvXferPacket> journal;
    coop::InvXferPacket operation; std::memset(&operation,0,sizeof(operation));
    operation.ownerId=1; operation.xferId=17; operation.quantity=1;
    TX_CHECK("first operation ID is admitted once", remember(journal,operation)==FIRST);
    TX_CHECK("reliable repeated delivery keeps one immutable operation", remember(journal,operation)==RETRY && journal.size()==1);
    operation.quantity=2;
    TX_CHECK("same ID cannot change quantity", remember(journal,operation)==CONFLICT);
    operation.quantity=1; operation.dIndex=42;
    TX_CHECK("same ID cannot redirect the grant", remember(journal,operation)==CONFLICT);
    operation.dIndex=0; operation.quotedTotal=9;
    TX_CHECK("same ID cannot change price", remember(journal,operation)==CONFLICT);
    operation.quotedTotal=0; operation.actorRevision=7;
    TX_CHECK("same operation cannot replace its frozen actor checkpoint", remember(journal,operation)==CONFLICT);
    operation.actorRevision=0; operation.ownerId=2;
    TX_CHECK("different authenticated senders have separate operation IDs", remember(journal,operation)==FIRST && journal.size()==2);
    coop::InvItemEntry desired[2],actual[3];
    std::memset(desired,0,sizeof(desired)); std::memset(actual,0,sizeof(actual));
    std::strcpy(desired[0].stringID,"copper"); desired[0].quantity=10;
    actual[0]=desired[0]; actual[0].quantity=4; actual[1]=desired[0]; actual[1].quantity=6;
    TX_CHECK("actor checkpoint accepts equal physical units split across native stacks", equivalentCheckpoint(desired,1,actual,2));
    actual[1].quantity=7;
    TX_CHECK("checkpoint never acknowledges an extra physical unit", !equivalentCheckpoint(desired,1,actual,2));
    actual[1].quantity=6; actual[1].quality=1;
    TX_CHECK("checkpoint preserves quality identity", !equivalentCheckpoint(desired,1,actual,2));
    std::memset(desired,0,sizeof(desired)); std::memset(actual,0,sizeof(actual));
    std::strcpy(desired[0].stringID,"bag"); desired[0].quantity=1;
    std::strcpy(desired[1].stringID,"copper"); desired[1].quantity=2; desired[1].parentIdx=1;
    actual[0]=desired[1]; actual[0].parentIdx=2; actual[1]=desired[0];
    TX_CHECK("parent row reorder does not strand applied checkpoint", equivalentCheckpoint(desired,2,actual,2));
    actual[0].parentIdx=0;
    TX_CHECK("bagged resource is not equivalent to loose resource", !equivalentCheckpoint(desired,2,actual,2));
    coop::InvItemEntry bags[4],packed[5];
    std::memset(bags,0,sizeof(bags)); std::memset(packed,0,sizeof(packed));
    bags[0]=desired[0]; bags[1]=desired[0];
    bags[2]=desired[1]; bags[2].quantity=1;
    bags[3]=bags[2]; bags[3].parentIdx=2;
    std::memcpy(packed,bags,sizeof(bags));
    packed[2].quantity=2;
    TX_CHECK("identical bags cannot redistribute children and acknowledge checkpoint",
        !equivalentCheckpoint(bags,4,packed,3));
    packed[2].quantity=1; packed[3]=bags[3];
    TX_CHECK("distinct identical bags retain their own equal child multisets",
        equivalentCheckpoint(bags,4,packed,4));
    bags[2].quantity=3; bags[3].quantity=2;
    packed[0]=bags[3]; packed[0].parentIdx=4;
    packed[1]=bags[2]; packed[1].quantity=1; packed[1].parentIdx=5;
    packed[2]=packed[1]; packed[2].quantity=2;
    packed[3]=bags[1]; packed[4]=bags[0];
    TX_CHECK("identical parent reorder allows child stack splits inside matched parent",
        equivalentCheckpoint(bags,4,packed,5));
    packed[0].quantity=1; packed[2].quantity=3;
    TX_CHECK("same total child units cannot cross identical parent boundaries",
        !equivalentCheckpoint(bags,4,packed,5));
    packed[0].parentIdx=1;
    TX_CHECK("malformed parent cannot certify checkpoint",
        !equivalentCheckpoint(bags,4,packed,5));
    // Native consumer boundary: a reconnect restarts the author's revision, not
    // its physical stock. Only a complete matching recapture may reopen it.
    Revision reconnect;
    reconnect.adopt(8);
    TX_CHECK("established peer sale can consume its agreed physical source",
        sourceRevisionMatches(8,reconnect.current()));
    TX_CHECK("established mining checkpoint fences the actor sequence", reconnect.checkpoint()==9);
    reconnect.retire();
    TX_CHECK("disconnected mining stock cannot admit a stale extraction",
        validate(3,8,reconnect.current(),1,2,0,0,0)==STALE);
    TX_CHECK("disconnected sale cannot spend a zero source namespace",
        !sourceRevisionMatches(0,reconnect.current()));
    std::memset(desired,0,sizeof(desired)); std::memset(actual,0,sizeof(actual));
    std::strcpy(desired[0].stringID,"copper"); desired[0].quantity=2;
    actual[0]=desired[0]; actual[0].quantity=1;
    TX_CHECK("incomplete native restore cannot reopen restarted mining or shop source",
        !admitCheckpoint(reconnect,1,desired,1,actual,1,false,false) &&
        validate(3,1,reconnect.current(),1,2,0,0,0)==STALE &&
        !sourceRevisionMatches(1,reconnect.current()));
    actual[0]=desired[0];
    TX_CHECK("truncated native recapture cannot certify even matching visible rows",
        !admitCheckpoint(reconnect,1,desired,1,actual,1,false,true) && reconnect.current()==0);
    TX_CHECK("truncated wire checkpoint cannot certify matching visible native rows",
        !admitCheckpoint(reconnect,1,desired,1,actual,1,true,false) && reconnect.current()==0);
    TX_CHECK("complete restarted peer stock reopens native resale and extraction",
        admitCheckpoint(reconnect,1,desired,1,actual,1,false,false) &&
        sourceRevisionMatches(1,reconnect.current()) &&
        validate(3,1,reconnect.current(),1,2,0,0,0)==VALID);
    TX_CHECK("old pre-disconnect quote cannot spend restarted stock",
        !sourceRevisionMatches(8,reconnect.current()) &&
        validate(3,8,reconnect.current(),1,2,0,0,0)==STALE);
    TX_CHECK("reconnect preserves host actor checkpoint monotonicity", reconnect.checkpoint()==10);
    TX_CHECK("new complete publication advances the live source",
        admitCheckpoint(reconnect,2,desired,1,actual,1,false,false));
    TX_CHECK("older complete replay cannot rewind a live source",
        !admitCheckpoint(reconnect,1,desired,1,actual,1,false,false) &&
        sourceRevisionMatches(2,reconnect.current()));
    reconnect.adopt(0);
    TX_CHECK("equal actor replay cannot reopen an invalidated sale even with matching rows",
        !admitCheckpoint(reconnect,2,desired,1,actual,1,false,false) &&
        !sourceRevisionMatches(2,reconnect.current()));
#undef TX_CHECK
    return failures;
}
#endif
