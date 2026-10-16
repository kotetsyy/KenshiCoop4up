#ifndef KENSHICOOP_INVENTORY_TRANSACTION_BRIDGE_H
#define KENSHICOOP_INVENTORY_TRANSACTION_BRIDGE_H
#include "Engine.h"
class Inventory;
class RootObject;
class InventoryGUI;
namespace coop { namespace engine {
struct NativeShopIntent {
    unsigned int source[5], destination[5];
    InvItemEntry item;
    unsigned int revision, actorRevision;
    int quotedTotal;
    unsigned char operation;
};
unsigned int enumMerchantInventoryHands(GameWorld* gw,unsigned int (*out)[5],unsigned int capacity);
Inventory* transactionInventory(const unsigned int hand[5]);
bool isMerchantInventory(const unsigned int hand[5]);
bool inventoryTransactionRemovalReady(Inventory* source);
void configureInventoryTransactions(bool host, bool enabled, bool commerce);
void clearInventoryTransactionState();
// Only a positively paired, complete HOST world-production fixture may suppress
// JOIN's native production mutation. Native worker/pose AI remains scheduled.
bool noteHostProductionReplica(const unsigned int localHand[5]);
void noteInventoryRevision(const unsigned int hand[5], unsigned int revision);
// A departed writer loses source admission, but keeps HOST checkpoint ordering.
void retireInventoryRevision(const unsigned int hand[5]);
bool inventoryCheckpointPending(const unsigned int hand[5]);
bool admitInventoryCheckpoint(const unsigned int hand[5],unsigned int revision,
    const InvItemEntry* wanted,unsigned int wantCount,
    const InvItemEntry* actual,unsigned int actualCount,bool wantedTruncated,bool actualTruncated);
unsigned int observeInventoryRevision(const unsigned int hand[5], unsigned int hash);
unsigned int inventoryRevision(const unsigned int hand[5]);
unsigned int nextInventoryCheckpoint(const unsigned int hand[5]);
void setInventoryCheckpointPending(const unsigned int hand[5],bool pending);
bool inventoryCheckpointReady(Inventory* inventory);
// Called before player UI commerce. JOIN submits an intent without transferring
// a physical item or debiting money; NPC shopping does not enter this path.
bool nativeShopTransactionsDeferred();
bool deferNativeShopTransaction(Inventory* source, Item* item, Inventory* destination,
    unsigned int quantity);
bool nativePlayerTradeReady();
int observeNativeTradePrice(int nativePrice);
int constrainNativeTradeFunds(Inventory* payer,int nativeFunds);
unsigned int drainNativeShopTransactions(NativeShopIntent* out, unsigned int capacity);
// Executes only existing physical stock. Never reconstructs an item from a quote.
int commitNativeShopTransaction(GameWorld* gw, const unsigned int source[5],
    const unsigned int destination[5], const InvXferPacket& request, int* wallet);
int commitNativeExtraction(GameWorld* gw,const unsigned int source[5],
    const unsigned int destination[5],const InvXferPacket& request);
// Harness drives the same resolved native shop/cursor methods as gameplay,
// with hooks enabled. Zero means unavailable/refused, never fabricated success.
bool exerciseNativeTransaction(GameWorld* gw,const unsigned int source[5],
    const unsigned int destination[5],const InvItemEntry& item,bool commerce);
} }
#endif
