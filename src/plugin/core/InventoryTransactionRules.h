#ifndef KENSHICOOP_INVENTORY_TRANSACTION_RULES_H
#define KENSHICOOP_INVENTORY_TRANSACTION_RULES_H
#include <climits>
#include <cstring>
#include <map>
#include <utility>
namespace coop { namespace inventory_transaction {
enum Result { VALID, STALE, QUANTITY, PRICE, FUNDS, OPERATION };
enum ReplayAdmission { FIRST, RETRY, CONFLICT };
template<class Packet>
inline ReplayAdmission remember(
    std::map<std::pair<unsigned int,unsigned int>,Packet>& journal,const Packet& request) {
    typedef std::pair<unsigned int,unsigned int> OperationId;
    OperationId id(request.ownerId,request.xferId);
    typename std::map<OperationId,Packet>::const_iterator old=journal.find(id);
    if (old!=journal.end())
        return std::memcmp(&old->second,&request,sizeof(request))==0 ? RETRY : CONFLICT;
    journal[id]=request;
    return FIRST;
}
struct Revision {
    unsigned int revision,hash,checkpointRevision;
    bool observed,admitted;
    Revision() : revision(0),hash(0),checkpointRevision(0),observed(false),admitted(true) {}
    unsigned int current() const { return admitted ? revision : 0; }
    // A departed author's next connection may restart its source namespace.
    // HOST checkpoint sequence is independent and must never rewind with it.
    void retire() {
        revision=hash=0; observed=false; admitted=false;
    }
    void adopt(unsigned int incoming) {
        if (!incoming) { admitted=false; return; }
        if (incoming>revision) { revision=incoming; admitted=true; }
    }
    unsigned int observe(unsigned int currentHash) {
        if (!observed || !admitted || hash!=currentHash) {
            ++revision; if (!revision) ++revision;
            hash=currentHash; observed=true; admitted=true;
        }
        return revision;
    }
    unsigned int checkpoint() {
        if (checkpointRevision<revision) checkpointRevision=revision;
        ++checkpointRevision; if (!checkpointRevision) ++checkpointRevision;
        return checkpointRevision;
    }
};
template<class Row>
inline bool sameIdentity(const Row& a,const Row& b) {
    return a.itemType==b.itemType && a.quality==b.quality && a.level==b.level &&
        a.equipped==b.equipped && a.slot==b.slot && a.locked==b.locked && a.section==b.section &&
        !std::strcmp(a.stringID,b.stringID) && !std::strcmp(a.manufacturer,b.manufacturer) &&
        !std::strcmp(a.material,b.material);
}
// Compare units only inside one particular parent (zero means loose roots).
template<class Row>
inline unsigned int checkpointUnits(const Row* rows,unsigned int count,
                                    unsigned int parent,const Row& key) {
    unsigned int units=0;
    for (unsigned int i=0;i<count;++i)
        if (rows[i].parentIdx==parent && sameIdentity(rows[i],key)) units+=rows[i].quantity;
    return units;
}
template<class Row>
inline bool checkpointChildrenEqual(const Row* a,unsigned int an,unsigned int ap,
                                     const Row* b,unsigned int bn,unsigned int bp) {
    for (unsigned int i=0;i<an;++i)
        if (a[i].parentIdx==ap &&
            checkpointUnits(a,an,ap,a[i])!=checkpointUnits(b,bn,bp,a[i])) return false;
    for (unsigned int i=0;i<bn;++i)
        if (b[i].parentIdx==bp &&
            checkpointUnits(b,bn,bp,b[i])!=checkpointUnits(a,an,ap,b[i])) return false;
    return true;
}
template<class Row>
inline bool checkpointContainerIdentity(const Row& key,const Row* rows,unsigned int count) {
    for (unsigned int i=0;i<count;++i)
        if (rows[i].parentIdx && sameIdentity(key,rows[rows[i].parentIdx-1])) return true;
    return false;
}
template<class Row>
inline bool checkpointParentsValid(const Row* rows,unsigned int count) {
    for (unsigned int i=0;i<count;++i) {
        unsigned int parent=rows[i].parentIdx;
        if (parent && (parent>count || parent==i+1 || rows[parent-1].parentIdx)) return false;
    }
    return true;
}
// Native packing can split/merge loose stacks and reorder parent rows. Confirm
// actual units and native identity, not incidental array order/grid positions.
// Actor checkpoints are rare; the exact bounded scan avoids per-frame heap work.
template<class Row>
inline bool equivalentCheckpoint(const Row* wanted,unsigned int wantCount,
                                  const Row* actual,unsigned int actualCount) {
    const unsigned int maxRows=4096;
    if (wantCount>maxRows || actualCount>maxRows ||
        !checkpointParentsValid(wanted,wantCount) || !checkpointParentsValid(actual,actualCount))
        return false;
    bool matched[maxRows]={false};
    for (unsigned int i=0;i<wantCount;++i) {
        if (wanted[i].parentIdx) continue;
        const bool container=checkpointContainerIdentity(wanted[i],wanted,wantCount) ||
            checkpointContainerIdentity(wanted[i],actual,actualCount);
        if (!container) {
            if (checkpointUnits(wanted,wantCount,0,wanted[i])!=
                checkpointUnits(actual,actualCount,0,wanted[i])) return false;
            continue;
        }
        unsigned int j=0;
        for (;j<actualCount;++j)
            if (!matched[j] && !actual[j].parentIdx && sameIdentity(wanted[i],actual[j]) &&
                wanted[i].quantity==actual[j].quantity &&
                checkpointChildrenEqual(wanted,wantCount,i+1,actual,actualCount,j+1)) break;
        if (j==actualCount) return false;
        matched[j]=true;
    }
    for (unsigned int i=0;i<actualCount;++i) {
        if (actual[i].parentIdx) continue;
        const bool container=checkpointContainerIdentity(actual[i],wanted,wantCount) ||
            checkpointContainerIdentity(actual[i],actual,actualCount);
        if (container ? !matched[i] :
            checkpointUnits(actual,actualCount,0,actual[i])!=
                checkpointUnits(wanted,wantCount,0,actual[i])) return false;
    }
    return true;
}
inline bool sourceRevisionMatches(unsigned int expected,unsigned int current) {
    return expected && expected==current;
}
// A retired source reopens only after complete wire/native stock agrees.
// Equal replay must still never undo a sale invalidation.
template<class Row>
inline bool admitCheckpoint(Revision& source,unsigned int incoming,
    const Row* wanted,unsigned int wantCount,const Row* actual,unsigned int actualCount,
    bool wantedTruncated,bool actualTruncated) {
    if (!incoming || wantedTruncated || actualTruncated ||
        !equivalentCheckpoint(wanted,wantCount,actual,actualCount)) return false;
    source.adopt(incoming);
    return sourceRevisionMatches(incoming,source.current());
}
// One player right-click transfers one unit. Extraction has an explicit quantity.
inline bool nativeQuantitySupported(unsigned int operation,int quantity,int available) {
    return quantity>0 && quantity<=available &&
        ((operation==1 || operation==2) ? quantity==1 : operation==3);
}
// Native buyback and trader context can improve the displayed quote. The HOST
// computes the real price: never spend above a buy ceiling or sell below a floor.
inline bool priceWithinQuote(unsigned int operation,int nativeTotal,int quote) {
    return nativeTotal>=0 && quote>=0 &&
        (operation==1 ? nativeTotal<=quote : operation==2 && nativeTotal>=quote);
}
// The hook observes a ONE-unit native price, never the selected stack total.
// Apply the same ceiling/floor to every unit, including a partially filled batch.
inline bool nativeBatchPriceWithinQuote(unsigned int operation,int nativeUnit,
                                       int quantity,int quote) {
    return quantity>0 && quantity<=65535 && nativeUnit>=0 &&
        nativeUnit<=INT_MAX/quantity &&
        priceWithinQuote(operation,nativeUnit*quantity,quote);
}
inline int nativeUnitMoved(unsigned int sourceBefore,unsigned int sourceAfter,
                           unsigned int destinationBefore,unsigned int destinationAfter) {
    return sourceBefore>0 && sourceAfter==sourceBefore-1 &&
        destinationBefore<UINT_MAX && destinationAfter==destinationBefore+1 ? 1 : 0;
}
// Values match InvXferPacket::operation. Zero remains the peer inventory move.
inline Result validate(unsigned int operation, unsigned int expectedRevision,
                       unsigned int currentRevision, int quantity, int available,
                       int unitPrice, int quotedTotal, int payerFunds) {
    if (operation<1 || operation>3) return OPERATION;
    if (!sourceRevisionMatches(expectedRevision,currentRevision)) return STALE;
    if (quantity<=0 || quantity>65535 || quantity>available) return QUANTITY;
    if (operation==3) return quotedTotal==0 ? VALID : PRICE;
    if (unitPrice<0 || unitPrice>INT_MAX/quantity ||
        !priceWithinQuote(operation,unitPrice*quantity,quotedTotal)) return PRICE;
    if (payerFunds<unitPrice*quantity) return FUNDS;
    return VALID;
}
} }
#endif
