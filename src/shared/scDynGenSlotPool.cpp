#include "scDynGenSlotPool.h"

#include "ofMain.h"

#include <bitset>
#include <mutex>

namespace {
constexpr int kSharedMaxSlots = 16;
std::bitset<kSharedMaxSlots> gDynGenSlotPool;
std::mutex gDynGenSlotMutex;
}

namespace scDynGenSlotPool {
int acquireSlot(const std::string& ownerTag, int maxSlots) {
    if(maxSlots != kSharedMaxSlots) {
        ofLogWarning(ownerTag) << "Slot pool compiled for " << kSharedMaxSlots
                               << " slots but requested " << maxSlots << ".";
    }

    std::lock_guard<std::mutex> lock(gDynGenSlotMutex);
    for(int i = 0; i < std::min(maxSlots, kSharedMaxSlots); i++) {
        if(!gDynGenSlotPool.test(i)) {
            gDynGenSlotPool.set(i);
            return i;
        }
    }

    ofLogError(ownerTag) << "DynGen slot pool exhausted.";
    return -1;
}

void releaseSlot(const std::string& ownerTag, int idx, int maxSlots) {
    if(idx < 0) return;

    if(maxSlots != kSharedMaxSlots) {
        ofLogWarning(ownerTag) << "Slot pool compiled for " << kSharedMaxSlots
                               << " slots but requested " << maxSlots << ".";
    }

    if(idx >= kSharedMaxSlots) {
        ofLogWarning(ownerTag) << "Ignoring out-of-range slot release for slot " << idx;
        return;
    }

    std::lock_guard<std::mutex> lock(gDynGenSlotMutex);
    gDynGenSlotPool.reset(idx);
}
}
