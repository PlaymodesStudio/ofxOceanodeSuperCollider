# Event-Driven FXP Loading Improvements for scVST

## Overview
This document describes the improvements made to the scVST node to replace timer-based FXP loading with a fully event-driven approach, resulting in better performance and reliability.

## Problem Statement
The original implementation used timers and blocking delays (`ofSleepMillis`) to wait for VST instances to be ready and for FXP preset loading to complete. This approach had several issues:

1. **Performance Impact**: Blocking delays could slow down the entire application
2. **Unreliability**: Fixed timeouts might be too short for complex plugins or too long for simple ones
3. **Error Prone**: Timer-based polling could miss events or timeout prematurely
4. **Inefficiency**: Continuous polling wasted CPU cycles

## Solution: Event-Driven Architecture

### Key Changes

#### 1. Removed Timer-Based Polling
**Before:**
```cpp
// Parameter timer logic - polling approach
if(parameterTimerActive) {
    if(currentTime - parameterTimerStart >= parameterTimerDelay) {
        if(areAllInstancesReady()) {
            // Apply parameters
        } else {
            // Extend timer
        }
    }
}
```

**After:**
```cpp
// EVENT-DRIVEN: No more timer polling - handled by event callbacks
// The parameter application is now triggered by VST ready events
```

#### 2. Parallel FXP Loading Without Delays
**Before:**
```cpp
// Sequential loading with delays
for(auto synth : serverInstances.second) {
    applyFXPToInstance(synth->nodeID);
    ofSleepMillis(100); // Blocking delay
}
```

**After:**
```cpp
// Parallel loading without delays
std::vector<std::pair<ofxSCServer*, int>> loadTargets;
for(auto& serverInstances : synthInstances) {
    for(auto synth : serverInstances.second) {
        if(synth != nullptr) {
            loadTargets.push_back({serverInstances.first, synth->nodeID});
        }
    }
}

// Send all commands without delays
for(const auto& [server, nodeID] : loadTargets) {
    // Send FXP load command
    server->sendMsg(readMsg);
    fxpAppliedInstances.insert(nodeID);
}
```

#### 3. Event-Based Completion Tracking
**Before:**
```cpp
// Blocking wait for completion
ofSleepMillis(500); // Wait for parameter updates
isFXPLoading.store(false);
```

**After:**
```cpp
// Non-blocking event-driven completion
void scVST::handleVSTPresetRead(ofxOscMessage& msg) {
    if(success) {
        fxpAppliedInstances.insert(nodeID);
        
        // Check completion without blocking
        if(fxpAppliedInstances.size() >= totalExpected) {
            // All done - trigger completion events
            isFXPLoading.store(false, std::memory_order_release);
            
            // Schedule parameter application for next update cycle
            if(hasPendingPresetData) {
                // Flag will be checked in next update() call
            }
        }
    }
}
```

#### 4. Deprecated Timer Setup
```cpp
void scVST::setupParameterTimer(int delayMs) {
    // DEPRECATED: This method is no longer used in event-driven approach
    // Kept for backward compatibility but does nothing
    ofLogWarning("scVST") << "setupParameterTimer called but is deprecated in event-driven mode";
}
```

## Benefits

### 1. Performance Improvements
- **No Blocking**: Eliminates all `ofSleepMillis()` calls
- **Parallel Processing**: All VST instances load FXP data simultaneously
- **Reduced Latency**: Events are processed immediately when ready
- **CPU Efficiency**: No polling loops wasting cycles

### 2. Reliability Enhancements
- **Event Guarantees**: VST ready events ensure operations only proceed when safe
- **No Timeout Issues**: Event-driven approach adapts to actual plugin load times
- **Better Error Handling**: Failed loads are detected immediately via events

### 3. Code Simplification
- **Cleaner Logic**: Event handlers replace complex timer state machines
- **Maintainability**: Event-driven code is easier to understand and debug
- **Extensibility**: New events can be added without affecting existing logic

## Implementation Details

### Event Flow
1. **VST Instance Creation**: Multiple instances created in parallel
2. **VST Open Event**: Each instance sends `/vst_open` when ready
3. **FXP Load Command**: Send `/program_read` to all instances simultaneously
4. **FXP Load Event**: Each instance sends `/vst_program_read` when complete
5. **Completion Check**: When all instances report success, apply parameters

### Key Components

#### Event Tracking
- `readyInstances`: Set of VST instances that have opened successfully
- `fxpAppliedInstances`: Set of instances that have loaded FXP data
- `isFXPLoading`: Atomic flag for FXP loading state (no longer blocks)

#### Non-Blocking Operations
- FXP file I/O happens once, shared by all instances
- OSC messages sent in rapid succession without delays
- Parameter application scheduled for next update cycle

## Migration Guide

### For Users
No changes required - the improvements are transparent to users. The node will:
- Load presets faster
- Be more responsive during loading
- Handle complex plugins more reliably

### For Developers
If extending scVST:
1. Don't use `setupParameterTimer()` - it's deprecated
2. Don't use blocking delays (`ofSleepMillis`)
3. Use event callbacks for async operations
4. Check flags in `update()` for deferred operations

## Testing Recommendations

### Performance Testing
1. Load complex VST plugins with many parameters
2. Create nodes with multiple instances (8+)
3. Load large preset banks
4. Monitor CPU usage during loading

### Reliability Testing
1. Test with slow-loading plugins
2. Test with plugins that fail to load
3. Test rapid preset switching
4. Test with network latency (remote servers)

## Future Improvements

### Potential Enhancements
1. **Promise-Based API**: Use C++ promises/futures for cleaner async code
2. **Event Queue**: Implement priority queue for event processing
3. **Batch Operations**: Group multiple parameter changes
4. **Progress Callbacks**: Provide loading progress to UI

### Event Manager Integration
The `scVST_EventDriven.h` header provides a foundation for a more sophisticated event management system that could be integrated in future versions.

## Conclusion

The event-driven approach significantly improves the scVST node's performance and reliability by:
- Eliminating blocking operations
- Enabling true parallel processing
- Providing deterministic event-based flow
- Reducing CPU usage
- Improving error handling

These changes make the node more suitable for professional audio applications where performance and reliability are critical.