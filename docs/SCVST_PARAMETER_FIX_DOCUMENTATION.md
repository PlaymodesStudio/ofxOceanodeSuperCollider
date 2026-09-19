# scVST Parameter Management System - Complete Overhaul Documentation

## Overview
This document describes the comprehensive fixes applied to the scVST node's parameter management system to resolve critical issues with parameter persistence, removal, and VST linkage.

## Problems Addressed

### 1. Parameter Removal Broken
**Issue**: Neither "Remove All" nor individual parameter removal worked for parameters added in previous sessions.
**Root Cause**: The removal functions were searching for parameters by their current display names, which could have been modified or have suffixes added during GUI creation.

### 2. Parameter-VST Linking Fails After Reload
**Issue**: Parameters linked correctly in the current session but failed to maintain their link to VST after reloading the preset.
**Root Cause**: The parameter index mapping was not properly preserved and restored during preset save/load cycles.

### 3. Parameter Renaming Lost
**Issue**: Custom parameter names were not preserved when reloading presets.
**Root Cause**: Only the display name was saved, not the original VST parameter name, and the name restoration logic was incomplete.

### 4. External Modulation Connections Lost
**Issue**: Wire connections from external sources to parameters were not restored after preset reload.
**Root Cause**: Connection metadata was not tracked or saved with the parameter information.

### 5. Parameters Not Cleared on New Preset
**Issue**: When loading a new preset with a VST node of the same name, old parameters persisted instead of being cleared.
**Root Cause**: The preset loading logic didn't force-clear existing parameters when loading a different VST configuration.

## Implementation Details

### Enhanced VSTParameterInfo Structure
```cpp
struct VSTParameterInfo {
    int index;
    std::string displayName;      // User-editable name
    std::string originalName;     // Original VST parameter name
    float value;
    bool isConnected;             // Track if parameter has external connections
    bool isPersistent;            // Track if parameter should persist across preset loads
};
```

### New Helper Functions

#### removeAllDynamicParametersForce()
Forces removal of all dynamic parameters regardless of preset loading state. This ensures complete cleanup even during preset transitions.

#### clearParameterMaps()
Clears all parameter tracking maps to ensure no stale data persists between preset loads.

#### validateParameterConsistency()
Validates and cleans up orphaned parameters in the GUI that don't have corresponding entries in the tracking maps.

### Improved Parameter Removal Logic

The `removeParameterFromGUI()` function now:
1. Searches for parameters using multiple possible names (original, with suffixes, generic names)
2. Iterates through all GUI parameters to find matches by prefix
3. Properly removes all associated inspector parameters (name editors, removal buttons)
4. Cleans up all tracking maps

### Enhanced Preset Save/Load

#### presetSave()
- Saves complete parameter metadata including:
  - Display name and original name
  - Connection status
  - Persistence flags
  - Vector/scalar values
  - External connection indicators

#### loadBeforeConnections()
- Force-clears all existing parameters before loading new ones
- Restores complete parameter metadata
- Properly creates GUI parameters with preserved names and values
- Maintains parameter indices for VST linkage

#### syncGUIParametersToVST()
- Updates parameter info map to ensure VST linkage
- Includes recovery mechanism to recreate missing parameters
- Properly sends values to all VST instances
- Maintains feedback suppression to prevent loops

## Key Improvements

### 1. Robust Parameter Search
Parameters are now found using multiple search strategies:
- Exact name match
- Name with numeric suffixes (_1, _2, etc.)
- Generic "Param" + index fallback
- Prefix-based search for modified names

### 2. Complete Metadata Persistence
All parameter properties are now saved and restored:
- User-defined display names
- Original VST parameter names
- Connection status
- Value arrays (for multi-instance support)

### 3. Force Cleanup on Preset Load
The system now ensures complete parameter cleanup when loading new presets:
- Temporarily disables preset loading flag for force removal
- Clears all parameter maps
- Validates consistency after loading

### 4. Connection-Aware Parameter Management
Parameters now track their connection status:
- Saves whether parameters have external modulation
- Preserves wire connections across preset loads
- Maintains proper signal flow after restoration

## Testing Scenarios

To verify the fixes work correctly, test these scenarios:

### 1. Parameter Removal Test
1. Add several parameters to a VST
2. Save the preset
3. Reload the preset
4. Try removing individual parameters - should work
5. Try "Remove All" - should clear all parameters

### 2. Parameter Persistence Test
1. Add parameters and rename them
2. Connect external modulation to some parameters
3. Save the preset
4. Reload the preset
5. Verify all names are preserved
6. Verify connections are maintained
7. Verify parameters still control the VST

### 3. Plugin Switch Test
1. Load VST plugin A with parameters
2. Save preset
3. Load VST plugin B with different parameters
4. Load original preset
5. Verify plugin A loads with correct parameters
6. Verify no leftover parameters from plugin B

### 4. Multi-Instance Test
1. Create multi-channel VST setup
2. Add vector parameters
3. Set different values per instance
4. Save and reload preset
5. Verify all instance values are preserved

## Performance Considerations

The fixes maintain performance through:
- Efficient parameter search algorithms
- Proper use of maps for O(1) lookups
- Minimal GUI updates during batch operations
- Thread-safe feedback suppression

## Future Improvements

Potential areas for further enhancement:
1. Parameter grouping and categories
2. Parameter automation recording
3. MIDI learn functionality
4. Parameter morphing/interpolation
5. Undo/redo for parameter changes

## Migration Notes

Existing presets will still load but may not have full metadata. To get full benefits:
1. Load existing presets
2. Re-save them with the new system
3. This will capture all metadata for future loads

## Conclusion

The parameter management system has been completely overhauled to provide reliable, persistent, and user-friendly parameter handling. All critical issues have been addressed through a combination of enhanced data structures, improved search algorithms, and comprehensive metadata tracking.