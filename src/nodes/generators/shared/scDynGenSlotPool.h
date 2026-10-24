#pragma once

#include <string>

namespace scDynGenSlotPool {
int acquireSlot(const std::string& ownerTag, int maxSlots);
void releaseSlot(const std::string& ownerTag, int idx, int maxSlots);
}
