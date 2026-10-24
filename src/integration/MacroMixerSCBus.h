#pragma once
#include <vector>
class serverManager;
// Called alongside the ordinary ScBus type registration; no second node factory list.
void registerMacroMixerSCBus(const std::vector<serverManager*>& servers);
