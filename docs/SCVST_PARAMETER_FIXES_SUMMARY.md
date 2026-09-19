# scVST Parameter Management Fixes - Summary

## Date: February 11, 2026
## Author: Debug Mode Assistant

## Issues Addressed

### 1. Parameters Cannot Be Removed After Preset Reload
**Problem**: After loading a preset, parameters that were saved could not be removed using either individual removal buttons or "Remove All".

**Root Cause**: The `removeParameterFromGUI()` function was not temporarily disabling the preset loading flags, which prevented parameter removal during or after preset loading.

**Fix Applied**: 
- Modified [`removeParameterFromGUI()`](/Users/santiagovilanova/Documents/OF/of_playmodes/openFrameworks/addons/ofxOceanodeSuperCollider/src/scVST.cpp:1596) to temporarily disable `isPresetLoading` and `oceanodePresetLoading` flags during removal
- Restore the flags after removal completes

### 2. Parameter Duplication with Suffixes
**Problem**: When loading presets, parameters were duplicated with numeric suffixes (e.g., "cutoff" and "cutoff_1").

**Root Cause**: The [`addParameterToGUI()`](/Users/santiagovilanova/Documents/OF/of_playmodes/openFrameworks/addons/ofxOceanodeSuperCollider/src/scVST.cpp:1306) function was creating new parameters without properly checking if they already existed in the parameter group.

**Fix Applied**:
- Enhanced parameter existence checking in `addParameterToGUI()` to search for existing parameters by name
- Added logic to detect parameters with suffixes that might have been previously added
- Skip parameter creation if a matching parameter already exists

### 3. Parameters Not Cleared Between Presets
**Problem**: When loading a new preset with an sc vst node with the same name, previous parameters were not removed.

**Root Cause**: The [`presetWillBeLoaded()`](/Users/santiagovilanova/Documents/OF/of_playmodes/openFrameworks/addons/ofxOceanodeSuperCollider/src/scVST.cpp:4249) function was not clearing existing dynamic parameters before loading new ones.

**Fix Applied**:
- Modified `presetWillBeLoaded()` to call `removeAllDynamicParametersForce()` before loading new preset
- This ensures a clean slate for each preset load

## Key Code Changes

### 1. presetWillBeLoaded() - Clear parameters before loading
```cpp
void scVST::presetWillBeLoaded(){
    isPresetLoading = true;
    oceanodePresetLoading = true;
    
    // CRITICAL FIX: Clear all dynamic parameters before loading preset
    ofLogNotice("scVST::presetWillBeLoaded") << "🧹 Clearing all dynamic parameters before preset load";
    removeAllDynamicParametersForce();
}
```

### 2. addParameterToGUI() - Prevent duplication
```cpp
// Check if parameter already exists in ANY form
if(dynamicParameters.count(paramIndex) > 0 || dynamicVectorParameters.count(paramIndex) > 0) {
    // Just update value, don't create new parameter
    return;
}

// Also check parameter group for existing names
for(int i = 0; i < getParameterGroup().size(); i++) {
    string existingName = getParameterGroup().get(i).getName();
    if(existingName == paramName || existingName.find(paramName + "_") == 0) {
        // Parameter already exists, skip creation
        return;
    }
}
```

### 3. removeParameterFromGUI() - Force removal during preset loading
```cpp
void scVST::removeParameterFromGUI(int paramIndex) {
    // Force removal even during preset loading
    bool wasPresetLoading = isPresetLoading;
    bool wasOceanodePresetLoading = oceanodePresetLoading;
    isPresetLoading = false;
    oceanodePresetLoading = false;
    
    // ... perform removal ...
    
    // Restore flags
    isPresetLoading = wasPresetLoading;
    oceanodePresetLoading = wasOceanodePresetLoading;
}
```

### 4. Enhanced Logging
Added comprehensive logging throughout the parameter management functions:
- Parameter counts before and after operations
- Clear indication of which functions are being called
- Success/failure messages for parameter operations

## Testing Recommendations

### Test Scenario 1: Parameter Removal After Preset Reload
1. Load a preset with VST parameters
2. Try to remove individual parameters using the remove button
3. Try to remove all parameters using "Remove All"
4. **Expected**: All removal operations should work correctly

### Test Scenario 2: No Parameter Duplication
1. Load a preset with VST parameters
2. Save the preset
3. Load the same preset again
4. **Expected**: No duplicate parameters with suffixes should appear

### Test Scenario 3: Clean Parameter State Between Presets
1. Load preset A with VST parameters
2. Load preset B with different VST parameters but same node name
3. **Expected**: Only preset B's parameters should be visible, preset A's should be cleared

## Additional Improvements

### Robust Parameter Removal
- The removal functions now try multiple strategies to find and remove parameters
- Handles parameters with various naming patterns and suffixes
- Continues operation even if individual removals fail

### State Management
- Proper tracking of `oceanodePresetLoading` flag alongside `isPresetLoading`
- Temporary flag disabling during critical operations
- Proper restoration of flags after operations complete

## Files Modified
- `/Users/santiagovilanova/Documents/OF/of_playmodes/openFrameworks/addons/ofxOceanodeSuperCollider/src/scVST.cpp`

## Compatibility Notes
- All fixes maintain compatibility with the Oceanode framework
- No changes to the base Oceanode parameter management system
- All existing functionality is preserved

## Future Considerations
1. Consider implementing a parameter versioning system to track parameter changes
2. Add parameter validation to ensure consistency between GUI and VST state
3. Implement parameter state caching for faster preset switching
4. Consider adding a parameter lock mechanism for critical parameters

## Debug Output
The enhanced logging will provide detailed information about:
- Parameter addition/removal operations
- Parameter counts at various stages
- Success/failure of individual operations
- State of preset loading flags

Monitor the console output for messages prefixed with:
- `scVST::presetWillBeLoaded`
- `scVST::addParameterToGUI`
- `scVST::removeParameterFromGUI`
- `scVST::removeAllDynamicParameters`
- `scVST::removeAllDynamicParametersForce`