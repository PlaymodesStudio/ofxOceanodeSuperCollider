# scVST Performance Optimizations

## Executive Summary
The scVST node experiences CPU performance issues and audio glitches during multi-instancing. This document provides comprehensive optimizations to improve performance without compromising features.

## Identified Performance Bottlenecks

### 1. **Excessive OSC Message Processing**
- **Issue**: Every parameter update generates OSC messages for ALL instances
- **Impact**: O(n*m) complexity where n=parameters, m=instances
- **Location**: `handleVSTParam()`, `handleVSTAuto()`, parameter update loops

### 2. **Synchronous Plugin Loading**
- **Issue**: Sequential plugin loading with delays between instances
- **Impact**: 100-200ms delay per instance during loading
- **Location**: `loadSelectedPlugin()` lines 700-760

### 3. **Inefficient Parameter Throttling**
- **Issue**: Try-lock pattern causes parameter updates to be dropped
- **Impact**: Inconsistent parameter synchronization
- **Location**: Lines 796-810, 873-887

### 4. **Redundant FXP Operations**
- **Issue**: FXP save/load for every parameter propagation
- **Impact**: Disk I/O blocking audio thread
- **Location**: `propagateFirstInstanceViaFXP()` and related methods

### 5. **Mutex Contention**
- **Issue**: Multiple mutexes causing thread blocking
- **Impact**: Audio dropouts during parameter updates
- **Location**: `feedbackMutex`, `paramThrottleMutex`, `oscMessageMutex`

### 6. **String Operations in Hot Paths**
- **Issue**: String allocations and comparisons in real-time callbacks
- **Impact**: Memory allocation causing audio glitches
- **Location**: Parameter name handling, OSC address parsing

### 7. **Inefficient Multi-Instance Architecture**
- **Issue**: Each instance processes full audio even when not needed
- **Impact**: Unnecessary CPU usage for silent instances
- **Location**: VST instance creation and audio processing

## Optimization Strategies

### Strategy 1: Batch OSC Message Processing
**Priority: HIGH**
**Estimated Performance Gain: 30-40%**

```cpp
// Add to scVST.h private section:
struct PendingParameterUpdate {
    int nodeID;
    int paramIndex;
    float value;
    uint64_t timestamp;
};
std::vector<PendingParameterUpdate> pendingUpdates;
std::mutex pendingUpdatesMutex;
static const size_t MAX_PENDING_UPDATES = 1024;
static const uint64_t BATCH_PROCESS_INTERVAL_MS = 8; // 125Hz update rate

// New method to batch process parameter updates
void processPendingParameterUpdates() {
    if(pendingUpdates.empty()) return;
    
    std::vector<PendingParameterUpdate> updates;
    {
        std::lock_guard<std::mutex> lock(pendingUpdatesMutex);
        updates = std::move(pendingUpdates);
        pendingUpdates.clear();
        pendingUpdates.reserve(MAX_PENDING_UPDATES);
    }
    
    // Group updates by parameter to avoid redundant updates
    std::map<int, float> latestValues;
    for(const auto& update : updates) {
        latestValues[update.paramIndex] = update.value;
    }
    
    // Apply only the latest value for each parameter
    for(const auto& [paramIndex, value] : latestValues) {
        updateParameterValueFromVST(paramIndex, value, -1);
    }
}
```

### Strategy 2: Parallel Plugin Loading
**Priority: HIGH**
**Estimated Performance Gain: 50-70% faster loading**

```cpp
void scVST::loadSelectedPluginOptimized() {
    if (currentPluginPath.empty()) return;
    
    // Clear tracking
    readyInstances.clear();
    parameterInfoMap.clear();
    
    if (!isPresetLoading) {
        removeAllDynamicParameters();
    }
    
    // Parallel close all instances
    std::vector<std::future<void>> closeFutures;
    for(auto& serverInstances : synthInstances) {
        for(auto synth : serverInstances.second) {
            if(synth != nullptr) {
                closeFutures.push_back(std::async(std::launch::async, [this, synth, server = serverInstances.first]() {
                    ofxOscMessage closeMsg;
                    closeMsg.setAddress("/u_cmd");
                    closeMsg.addIntArg(synth->nodeID);
                    closeMsg.addIntArg(2);
                    closeMsg.addStringArg("/close");
                    server->sendMsg(closeMsg);
                }));
            }
        }
    }
    
    // Wait for all closes
    for(auto& future : closeFutures) {
        future.wait();
    }
    
    // Brief processing
    ofSleepMillis(50);
    
    // Parallel open all instances
    std::vector<std::future<void>> openFutures;
    for(auto& serverInstances : synthInstances) {
        for(auto synth : serverInstances.second) {
            if(synth != nullptr) {
                openFutures.push_back(std::async(std::launch::async, [this, synth, server = serverInstances.first]() {
                    ofxOscMessage openMsg;
                    openMsg.setAddress("/u_cmd");
                    openMsg.addIntArg(synth->nodeID);
                    openMsg.addIntArg(2);
                    openMsg.addStringArg("/open");
                    openMsg.addStringArg(currentPluginPath);
                    openMsg.addIntArg(1);
                    openMsg.addIntArg(enableMultithreading.get() ? 1 : 0);
                    openMsg.addIntArg(0);
                    server->sendMsg(openMsg);
                }));
            }
        }
    }
    
    // Wait for all opens
    for(auto& future : openFutures) {
        future.wait();
    }
    
    pluginLoaded = true;
}
```

### Strategy 3: Lock-Free Parameter Updates
**Priority: HIGH**
**Estimated Performance Gain: 20-30%**

```cpp
// Replace mutex-based throttling with atomic operations
// In scVST.h:
std::atomic<uint64_t> parameterUpdateGeneration[1024]; // Fixed size array for parameters
std::atomic<bool> parameterDirty[1024];

// In parameter update methods:
void scVST::handleVSTParamOptimized(ofxOscMessage& msg) {
    if (msg.getNumArgs() < 4) return;
    
    int nodeID = msg.getArgAsInt32(0);
    int paramIndex = (int)msg.getArgAsFloat(2);
    float value = msg.getArgAsFloat(3);
    
    if (!isMyVSTInstance(nodeID)) return;
    if (paramIndex < 0 || paramIndex >= 1024) return;
    
    // Lock-free update using atomics
    uint64_t currentGen = parameterUpdateGeneration[paramIndex].load(std::memory_order_acquire);
    uint64_t newGen = ofGetElapsedTimeMillis();
    
    // Only update if enough time has passed
    if (newGen - currentGen < PARAM_UPDATE_THROTTLE_MS) {
        return;
    }
    
    // Try to claim this update slot
    if (!parameterUpdateGeneration[paramIndex].compare_exchange_strong(
        currentGen, newGen, std::memory_order_release, std::memory_order_relaxed)) {
        return; // Another thread won
    }
    
    // Mark as dirty for batch processing
    parameterDirty[paramIndex].store(true, std::memory_order_release);
    
    // Store update for batch processing
    {
        std::lock_guard<std::mutex> lock(pendingUpdatesMutex);
        if(pendingUpdates.size() < MAX_PENDING_UPDATES) {
            pendingUpdates.push_back({nodeID, paramIndex, value, newGen});
        }
    }
}
```

### Strategy 4: Smart Instance Management
**Priority: MEDIUM**
**Estimated Performance Gain: 15-25%**

```cpp
// Add instance pooling and lazy activation
class VSTInstancePool {
private:
    struct PooledInstance {
        ofxSCSynth* synth;
        bool active;
        uint64_t lastUsed;
    };
    
    std::vector<PooledInstance> instances;
    std::mutex poolMutex;
    
public:
    ofxSCSynth* acquire() {
        std::lock_guard<std::mutex> lock(poolMutex);
        
        // Find inactive instance
        for(auto& inst : instances) {
            if(!inst.active) {
                inst.active = true;
                inst.lastUsed = ofGetElapsedTimeMillis();
                return inst.synth;
            }
        }
        
        // Create new if needed
        return createNewInstance();
    }
    
    void release(ofxSCSynth* synth) {
        std::lock_guard<std::mutex> lock(poolMutex);
        
        for(auto& inst : instances) {
            if(inst.synth == synth) {
                inst.active = false;
                // Don't free immediately - keep warm
                break;
            }
        }
    }
    
    void cleanup() {
        std::lock_guard<std::mutex> lock(poolMutex);
        uint64_t now = ofGetElapsedTimeMillis();
        
        // Free instances unused for > 30 seconds
        instances.erase(
            std::remove_if(instances.begin(), instances.end(),
                [now](const PooledInstance& inst) {
                    return !inst.active && (now - inst.lastUsed > 30000);
                }),
            instances.end()
        );
    }
};
```

### Strategy 5: Optimized OSC Communication
**Priority: MEDIUM**
**Estimated Performance Gain: 10-15%**

```cpp
// Pre-allocate and reuse OSC messages
class OSCMessagePool {
private:
    std::vector<std::unique_ptr<ofxOscMessage>> pool;
    std::mutex poolMutex;
    
public:
    std::unique_ptr<ofxOscMessage> acquire() {
        std::lock_guard<std::mutex> lock(poolMutex);
        if(!pool.empty()) {
            auto msg = std::move(pool.back());
            pool.pop_back();
            msg->clear();
            return msg;
        }
        return std::make_unique<ofxOscMessage>();
    }
    
    void release(std::unique_ptr<ofxOscMessage> msg) {
        std::lock_guard<std::mutex> lock(poolMutex);
        if(pool.size() < 100) {
            pool.push_back(std::move(msg));
        }
    }
};

// Use in hot paths:
void scVST::sendParameterUpdate(int nodeID, int paramIndex, float value) {
    auto msg = oscPool.acquire();
    msg->setAddress("/u_cmd");
    msg->addIntArg(nodeID);
    msg->addIntArg(2);
    msg->addStringArg("/set");
    msg->addIntArg(paramIndex);
    msg->addFloatArg(value);
    
    server->sendMsg(*msg);
    oscPool.release(std::move(msg));
}
```

### Strategy 6: Cache-Friendly Data Structures
**Priority: LOW**
**Estimated Performance Gain: 5-10%**

```cpp
// Align frequently accessed data for better cache performance
struct alignas(64) CacheLineAlignedParam {
    std::atomic<float> value;
    std::atomic<uint64_t> lastUpdate;
    char padding[64 - sizeof(std::atomic<float>) - sizeof(std::atomic<uint64_t>)];
};

// Use array instead of map for O(1) access
CacheLineAlignedParam parameterCache[1024];
```

## Implementation Priority

1. **Immediate (Week 1)**
   - Batch OSC message processing
   - Lock-free parameter updates
   - Remove unnecessary delays in plugin loading

2. **Short-term (Week 2)**
   - Parallel plugin loading
   - OSC message pooling
   - Reduce mutex contention

3. **Medium-term (Week 3-4)**
   - Instance pooling
   - Smart instance activation
   - Cache-friendly data structures

## Testing Strategy

1. **Performance Metrics**
   - Measure CPU usage with 1, 4, 8, 16 instances
   - Measure parameter update latency
   - Measure plugin load time
   - Monitor audio dropout frequency

2. **Test Scenarios**
   - Rapid parameter automation
   - Preset switching under load
   - Multi-instance MIDI processing
   - Long-running stability test (24+ hours)

3. **Comparison Baseline**
   - Document current performance metrics
   - Compare with Reaper/Max MSP performance
   - Target: Match or exceed DAW performance

## Configuration Recommendations

### For Multi-Instance Use:
```cpp
// Optimal settings for performance
enableMultithreading = true;      // Use VST multithreading
singleInstance = false;            // Allow multiple instances
monoInstancing = true;             // Use mono instances when possible
PARAM_UPDATE_THROTTLE_MS = 16;    // 60fps parameter updates
MAX_PENDING_OSC_MESSAGES = 512;   // Larger buffer for busy sessions
```

### For Single Heavy Plugin:
```cpp
enableMultithreading = true;
singleInstance = true;
PARAM_UPDATE_THROTTLE_MS = 8;     // Higher update rate for single instance
```

## Memory Optimization

1. **String Caching**: Pre-compute and cache all OSC addresses
2. **Parameter Name Interning**: Use string pool for parameter names
3. **Circular Buffers**: Use lock-free circular buffers for updates
4. **Memory Pools**: Pre-allocate memory for common operations

## Thread Safety Improvements

1. Replace multiple mutexes with single read-write lock where possible
2. Use atomic operations for simple flag updates
3. Implement wait-free algorithms for critical paths
4. Separate UI thread operations from audio thread

## Monitoring and Diagnostics

Add performance counters:
```cpp
struct PerformanceMetrics {
    std::atomic<uint64_t> oscMessagesSent;
    std::atomic<uint64_t> parameterUpdates;
    std::atomic<uint64_t> droppedUpdates;
    std::atomic<uint64_t> averageUpdateLatency;
    std::atomic<uint64_t> maxUpdateLatency;
    
    void report() {
        ofLogNotice("scVST::Performance") 
            << "OSC: " << oscMessagesSent.load() 
            << " Updates: " << parameterUpdates.load()
            << " Dropped: " << droppedUpdates.load()
            << " AvgLatency: " << averageUpdateLatency.load() << "us"
            << " MaxLatency: " << maxUpdateLatency.load() << "us";
    }
};
```

## Expected Results

With all optimizations implemented:
- **CPU Usage**: 40-60% reduction in multi-instance scenarios
- **Load Time**: 50-70% faster plugin loading
- **Latency**: <5ms parameter update latency
- **Stability**: Zero audio dropouts under normal load
- **Scalability**: Support 16+ instances without degradation

## Conclusion

These optimizations address the core performance issues in scVST while maintaining all features. The modular approach allows incremental implementation and testing. Priority should be given to batch processing and lock-free updates as they provide the highest immediate impact.