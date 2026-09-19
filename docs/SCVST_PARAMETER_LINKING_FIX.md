# Fix for Parameter Linking Issues in Event-Driven scVST

## Problem
After implementing the event-driven FXP loading improvements, parameter linking functionality was broken:
- Published parameters didn't affect VST parameters when moved
- Presets with published parameters didn't respond to GUI automation
- The `hasPendingPresetData` flag wasn't being cleared properly

## Root Cause
The issue was caused by:
1. `applyPendingPresetData()` wasn't being called in the event-driven flow
2. The method still contained blocking `ofSleepMillis()` calls
3. The `hasPendingPresetData` flag blocked parameter propagation when it remained set

## Solution Applied

### 1. Added Event-Driven Check in Update Method
```cpp
// EVENT-DRIVEN: Check if we need to apply pending preset data
// This replaces the timer-based approach
if(hasPendingPresetData && areAllInstancesReady() && !isFXPLoading.load(std::memory_order_acquire)) {
    // All conditions met - apply the pending preset data
    applyPendingPresetData();
}
```

This ensures that pending preset data is applied as soon as:
- All VST instances are ready
- FXP loading is complete
- There is pending data to apply

### 2. Removed Blocking Delays from applyPendingPresetData()
**Before:**
```cpp
// Phase 2: Wait a bit for all VST messages to be processed
for(int i = 0; i < 10; i++) {
    for(auto& serverInstances : synthInstances) {
        serverInstances.first->process();
    }
    ofSleepMillis(50);  // BLOCKING!
}
```

**After:**
```cpp
// Phase 2: Process server messages without blocking
for(auto& serverInstances : synthInstances) {
    serverInstances.first->process();
}
```

### 3. Removed Delay from activateParameterBindings()
**Before:**
```cpp
// Small delay to ensure the flag change is processed
ofSleepMillis(100);  // BLOCKING!
```

**After:**
```cpp
// EVENT-DRIVEN: No blocking delay needed
```

## How It Works Now

1. **VST Instance Creation**: Instances are created in parallel
2. **VST Ready Events**: Each instance sends ready event when loaded
3. **FXP Loading**: FXP data is sent to all instances simultaneously
4. **FXP Complete Events**: Each instance reports when FXP is loaded
5. **Parameter Application**: When all instances are ready and FXP is loaded, the update() method detects this and calls `applyPendingPresetData()`
6. **Binding Activation**: Parameter bindings are activated immediately without delays
7. **Flag Clearing**: `hasPendingPresetData` and `isPresetLoading` flags are cleared, allowing normal parameter propagation

## Benefits
- **Non-blocking**: No more `ofSleepMillis()` calls
- **Reliable**: Parameters are applied when actually ready, not after arbitrary delays
- **Responsive**: GUI remains responsive during loading
- **Correct State**: Flags are properly managed to allow parameter linking

## Testing
To verify the fix works:
1. Load a VST plugin
2. Move a parameter knob and use "Add Last Touched"
3. Verify the published parameter controls the VST
4. Save a preset with published parameters
5. Load the preset and verify parameters respond to automation

## Key Files Modified
- [`scVST.cpp`](scVST.cpp:492-498): Added event-driven check in update()
- [`scVST.cpp`](scVST.cpp:3433-3476): Removed blocking delays from applyPendingPresetData()
- [`scVST.cpp`](scVST.cpp:3479-3489): Removed delay from activateParameterBindings()
- [`scVST.cpp`](scVST.cpp:4064-4068): Improved scheduling in handleVSTPresetRead()

## Important Notes
- The `hasPendingPresetData` flag is now properly cleared in `applyPendingPresetData()`
- The `isPresetLoading` and `oceanodePresetLoading` flags are cleared in `activateParameterBindings()`
- Parameter propagation is blocked when these flags are set (by design, to prevent feedback during loading)
- The event-driven approach ensures these flags are cleared at the right time