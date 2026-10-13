// A spectral filter drawn by an array of gains: see PV_MaskFilter.cpp.
// PV_MaskFilter(chain, mask, transpose, lowFreq, highFreq, wrap, attack, release, hold)
PV_MaskFilter : PV_ChainUGen {
    *new { arg buffer, mask = #[1], transpose = 0.0, lowFreq = 20.0, highFreq = 20000.0,
               wrap = 1, attack = 0.0, release = 0.0, hold = 0;
        ^this.multiNewList(['control', buffer, transpose, lowFreq, highFreq, wrap, attack, release, hold] ++ mask.asArray)
    }
}
