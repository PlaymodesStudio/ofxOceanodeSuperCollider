# scVST.cpp Compilation Fix

## Issue Fixed
The lambda function on line 3371 had undefined variable references causing compilation errors.

## Errors Resolved
- `Use of undeclared identifier 'paramIndex'` (multiple occurrences)
- `Variable 'e' cannot be implicitly captured in a lambda`

## Solution Applied
Changed the lambda capture to properly capture the `paramIndex` variable:

```cpp
// Before (line 3371):
listeners.push(newParam->newListener([this, paramIndex](vector<float> &values) -> void {

// After:
int capturedParamIndex = paramIndex;  // Capture paramIndex properly
listeners.push(newParam->newListener([this, capturedParamIndex](vector<float> &values) -> void {
```

All references to `paramIndex` within the lambda were updated to use `capturedParamIndex`.

## Status
✅ Compilation errors fixed
✅ Lambda capture corrected
✅ Variable scope issues resolved

The file should now compile successfully within the openFrameworks build environment.