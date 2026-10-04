//
//  scEQEditor.h
//  ofxOceanodeSuperCollider
//
//  5-band EQ curve editor shared by scGraphicEQ and scGrainBox (header-only,
//  so no build-project change is needed). Extracted from scGraphicEQ, whose
//  display it reproduces exactly with its default colours:
//    • log-frequency grid (20 Hz .. 20 kHz) and dB grid, labelled
//    • optional FFT bar overlay (magnitudes supplied by the caller)
//    • band frequency markers, 0 dB line, combined response curve computed
//      from RBJ cookbook biquads (band 1 low shelf, 2-4 peaks, 5 high shelf),
//      matching the graphiceqN SynthDef (BLowShelf / BPeakEQ / BHiShelf)
//  Optional interaction (scGrainBox; scGraphicEQ leaves it off): a handle per
//  band — drag to set frequency (x) and gain (y), mouse wheel for Q / slope,
//  double-click resets the gain to 0 dB, right-click opens bandMenu(band)
//  (if set) in a popup. draw() returns true when it changed a band.
//

#pragma once

#include "ofMain.h"
#include "ofxOceanodeShared.h"
#include "imgui.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <functional>
#include <string>

class scEQEditor {
public:
    static constexpr int   NUM_BANDS       = 5;
    static constexpr int   NUM_FREQ_POINTS = 256;
    static constexpr float FREQ_MIN        = 20.0f;
    static constexpr float FREQ_MAX        = 20000.0f;
    static constexpr float FFT_FREQ_MIN    = 20.0f;
    static constexpr float FFT_FREQ_MAX    = 22050.0f;
    static constexpr float FFT_DB_FLOOR    = -80.0f;

    // shape: slope for the shelves (bands 0 and 4), Q for the peaks (1..3)
    struct Band { float freqHz; float gainDb; float shape; };
    static bool isShelf(int b) { return b == 0 || b == NUM_BANDS - 1; }

    struct Colors {
        ImU32 bg, border, gridLine, gridText, dbGridLine, dbGridText, zeroGrid, zeroLine, curve, fft;
        ImU32 bandMarker[NUM_BANDS];
        ImU32 handle, handleActive;
    };
    // scGraphicEQ's colours
    static Colors defaultColors() {
        Colors c;
        c.bg         = IM_COL32(12, 14, 20, 255);
        c.border     = IM_COL32(70, 70, 90, 255);
        c.gridLine   = IM_COL32(40, 40, 55, 200);
        c.gridText   = IM_COL32(100, 100, 120, 220);
        c.dbGridLine = IM_COL32(40, 40, 55, 160);
        c.dbGridText = IM_COL32(90, 90, 110, 200);
        c.zeroGrid   = IM_COL32(200, 200, 200, 160);
        c.zeroLine   = IM_COL32(220, 220, 220, 180);
        c.curve      = IM_COL32(80, 210, 255, 255);
        c.fft        = IM_COL32(60, 160, 80, 65);
        c.bandMarker[0] = IM_COL32(100, 200, 255, 140);
        c.bandMarker[1] = IM_COL32(100, 255, 150, 140);
        c.bandMarker[2] = IM_COL32(255, 220, 80,  140);
        c.bandMarker[3] = IM_COL32(255, 140, 80,  140);
        c.bandMarker[4] = IM_COL32(210, 100, 255, 140);
        c.handle       = IM_COL32(220, 220, 220, 200);
        c.handleActive = IM_COL32(255, 255, 255, 255);
        return c;
    }

    std::array<Band, NUM_BANDS> bands {{
        {80.0f, 0.0f, 1.0f}, {250.0f, 0.0f, 1.0f}, {1000.0f, 0.0f, 1.0f},
        {4000.0f, 0.0f, 1.0f}, {12000.0f, 0.0f, 1.0f}
    }};
    float dbRange = 48.0f;   // display range: -dbRange .. +dbRange
    // Interaction limits (scGrainBox)
    float gainLimit = 48.0f, qMin = 0.1f, qMax = 20.0f, slopeMin = 0.1f, slopeMax = 4.0f;

    std::array<float, NUM_FREQ_POINTS> curveDb {};

    // Interactive only: contents of the right-click popup of a band handle
    std::function<void(int band)> bandMenu;

    static float pitchToHz(float midi) { return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f); }

    // ── Biquad coefficients (Robert Bristow-Johnson Audio EQ Cookbook) ──────
    struct BiquadCoeffs { float b0, b1, b2, a1, a2; };

    // Shelf slope S that keeps the RBJ shelf defined: alpha needs
    // (A + 1/A) * (1/S - 1) + 2 >= 0, otherwise sqrt(< 0) = NaN (in SC's
    // BLowShelf / BHiShelf too: the filter output becomes NaN, which then
    // silences everything after it until the synth is replaced). Steep
    // slopes are allowed only as far as the gain permits.
    static float safeShelfSlope(float slope, float gainDb) {
        const float A = std::pow(10.0f, gainDb / 40.0f);
        const float k = 1.0f - 1.9f / (A + 1.0f / A);   // 1/S must stay above k
        float s = std::max(0.01f, slope);
        if(k > 0.0f) s = std::min(s, 1.0f / k);
        return s;
    }

    static BiquadCoeffs computeLowShelf(float freqHz, float gainDb, float slope, float sr) {
        slope = safeShelfSlope(slope, gainDb);
        float freq = ofClamp(freqHz, 10.0f, sr * 0.499f);
        float A    = std::pow(10.0f, gainDb / 40.0f);
        float w0   = 2.0f * (float)M_PI * freq / sr;
        float cosW = std::cos(w0);
        float sinW = std::sin(w0);
        float alpha = sinW / 2.0f * std::sqrt((A + 1.0f/A) * (1.0f/slope - 1.0f) + 2.0f);
        float sqA  = std::sqrt(A);

        float b0 =  A * ((A+1) - (A-1)*cosW + 2.0f*sqA*alpha);
        float b1 = 2.0f*A * ((A-1) - (A+1)*cosW);
        float b2 =  A * ((A+1) - (A-1)*cosW - 2.0f*sqA*alpha);
        float a0 =       (A+1) + (A-1)*cosW + 2.0f*sqA*alpha;
        float a1 = -2.0f * ((A-1) + (A+1)*cosW);
        float a2 =       (A+1) + (A-1)*cosW - 2.0f*sqA*alpha;

        return { b0/a0, b1/a0, b2/a0, a1/a0, a2/a0 };
    }

    static BiquadCoeffs computeHighShelf(float freqHz, float gainDb, float slope, float sr) {
        slope = safeShelfSlope(slope, gainDb);
        float freq = ofClamp(freqHz, 10.0f, sr * 0.499f);
        float A    = std::pow(10.0f, gainDb / 40.0f);
        float w0   = 2.0f * (float)M_PI * freq / sr;
        float cosW = std::cos(w0);
        float sinW = std::sin(w0);
        float alpha = sinW / 2.0f * std::sqrt((A + 1.0f/A) * (1.0f/slope - 1.0f) + 2.0f);
        float sqA  = std::sqrt(A);

        float b0 =  A * ((A+1) + (A-1)*cosW + 2.0f*sqA*alpha);
        float b1 = -2.0f*A * ((A-1) + (A+1)*cosW);
        float b2 =  A * ((A+1) + (A-1)*cosW - 2.0f*sqA*alpha);
        float a0 =       (A+1) - (A-1)*cosW + 2.0f*sqA*alpha;
        float a1 =  2.0f * ((A-1) - (A+1)*cosW);
        float a2 =       (A+1) - (A-1)*cosW - 2.0f*sqA*alpha;

        return { b0/a0, b1/a0, b2/a0, a1/a0, a2/a0 };
    }

    static BiquadCoeffs computePeakEQ(float freqHz, float gainDb, float qFactor, float sr) {
        float freq = ofClamp(freqHz, 10.0f, sr * 0.499f);
        float q    = std::max(qFactor, 0.01f);
        float A    = std::pow(10.0f, gainDb / 40.0f);
        float w0   = 2.0f * (float)M_PI * freq / sr;
        float cosW = std::cos(w0);
        float sinW = std::sin(w0);
        float alpha = sinW / (2.0f * q);

        float b0 =  1.0f + alpha * A;
        float b1 = -2.0f * cosW;
        float b2 =  1.0f - alpha * A;
        float a0 =  1.0f + alpha / A;
        float a1 = -2.0f * cosW;
        float a2 =  1.0f - alpha / A;

        return { b0/a0, b1/a0, b2/a0, a1/a0, a2/a0 };
    }

    static float computeMagnitudeDb(const BiquadCoeffs& c, float freqHz, float sr) {
        float freq = ofClamp(freqHz, 10.0f, sr * 0.499f);
        float w    = 2.0f * (float)M_PI * freq / sr;
        float cw   = std::cos(w);
        float c2w  = std::cos(2.0f * w);

        float numRe = c.b0 + c.b1*cw + c.b2*c2w;
        float numIm = -(c.b1*std::sin(w) + c.b2*std::sin(2.0f*w));
        float denRe = 1.0f + c.a1*cw + c.a2*c2w;
        float denIm = -(c.a1*std::sin(w) + c.a2*std::sin(2.0f*w));

        float numSq = numRe*numRe + numIm*numIm;
        float denSq = denRe*denRe + denIm*denIm;

        if(denSq < 1e-12f) return 0.0f;
        float mag = std::sqrt(numSq / denSq);
        return 20.0f * std::log10(std::max(mag, 1e-12f));
    }

    // Combined response of the 5 bands, clamped to the display range +/- 3 dB
    void recompute(float sr) {
        BiquadCoeffs c[NUM_BANDS];
        for(int b = 0; b < NUM_BANDS; b++) {
            const Band& bd = bands[b];
            if(b == 0)                  c[b] = computeLowShelf (bd.freqHz, bd.gainDb, bd.shape, sr);
            else if(b == NUM_BANDS - 1) c[b] = computeHighShelf(bd.freqHz, bd.gainDb, bd.shape, sr);
            else                        c[b] = computePeakEQ   (bd.freqHz, bd.gainDb, bd.shape, sr);
        }
        float logMin = std::log10(FREQ_MIN);
        float logMax = std::log10(FREQ_MAX);
        for(int i = 0; i < NUM_FREQ_POINTS; i++) {
            float t    = (float)i / (float)(NUM_FREQ_POINTS - 1);
            float freq = std::pow(10.0f, logMin + t * (logMax - logMin));
            float db = 0.0f;
            for(int b = 0; b < NUM_BANDS; b++) db += computeMagnitudeDb(c[b], freq, sr);
            curveDb[i] = ofClamp(db, -dbRange - 3.0f, dbRange + 3.0f);
        }
    }

    static float logFreqToX(float freq, float xStart, float width) {
        float logMin = std::log10(FREQ_MIN);
        float logMax = std::log10(FREQ_MAX);
        float t = (std::log10(ofClamp(freq, FREQ_MIN, FREQ_MAX)) - logMin) / (logMax - logMin);
        return xStart + t * width;
    }
    static float xToLogFreq(float x, float xStart, float width) {
        float logMin = std::log10(FREQ_MIN);
        float logMax = std::log10(FREQ_MAX);
        float t = ofClamp((x - xStart) / std::max(1.0f, width), 0.0f, 1.0f);
        return std::pow(10.0f, logMin + t * (logMax - logMin));
    }
    float dbToY(float db, float yStart, float height) const {
        float t = (dbRange - db) / (2.0f * dbRange);
        return yStart + ofClamp(t, 0.0f, 1.0f) * height;
    }
    float yToDb(float y, float yStart, float height) const {
        float t = ofClamp((y - yStart) / std::max(1.0f, height), 0.0f, 1.0f);
        return dbRange - t * 2.0f * dbRange;
    }

    // Draws at the cursor a W x H display (plus the 2*zoom padding and 4*zoom
    // spacer scGraphicEQ always had). fftMags: numBins magnitudes on the
    // FFT_FREQ_MIN..MAX log scale, or nullptr for no overlay. Returns true
    // when an interactive edit changed `bands` (call recompute() after).
    bool draw(float W, float H, const Colors& col, bool interactive = false,
              const float* fftMags = nullptr, int numBins = 0, const char* id = "##eqEditor") {
        float zoom = ofxOceanodeShared::getZoomLevel();

        ImDrawList* dl     = ImGui::GetWindowDrawList();
        ImVec2      cursor = ImGui::GetCursorScreenPos();

        const float pad = 2.0f * zoom;
        const float xS  = cursor.x + pad;
        const float yS  = cursor.y + pad;
        const float xE  = xS + W;
        const float yE  = yS + H;

        bool changed = false;
        if(interactive) {
            // Hit area first, so the handles below know hover / drag state
            ImGui::SetCursorScreenPos(ImVec2(xS, yS));
            ImGui::InvisibleButton(id, ImVec2(W, H), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            changed = handleInteraction(xS, yS, W, H, zoom);
            popupId = std::string(id) + "_bandMenu";
            if(bandMenu && hoverBand >= 0 && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                menuBand = hoverBand;
                ImGui::OpenPopup(popupId.c_str());
            }
            ImGui::SetCursorScreenPos(cursor);
        }

        // Background
        dl->AddRectFilled(ImVec2(xS, yS), ImVec2(xE, yE), col.bg);
        dl->AddRect(ImVec2(xS, yS), ImVec2(xE, yE), col.border);

        // ── Frequency grid (vertical) ─────────────────────────────────────
        static const float gridFreqs[]  = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
        static const char* gridLabels[] = { "20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k" };
        for(int g = 0; g < 10; g++) {
            float gx = logFreqToX(gridFreqs[g], xS, W);
            dl->AddLine(ImVec2(gx, yS), ImVec2(gx, yE - 14.0f * zoom), col.gridLine);
            dl->AddText(ImVec2(gx + 2.0f * zoom, yE - 14.0f * zoom), col.gridText, gridLabels[g]);
        }

        // ── dB grid (horizontal): 9 lines, step dbRange / 4 ───────────────
        for(int k = -4; k <= 4; k++) {
            float db    = dbRange * (float)k / 4.0f;
            float gy    = dbToY(db, yS, H);
            ImU32 color = (k == 0) ? col.zeroGrid : col.dbGridLine;
            dl->AddLine(ImVec2(xS, gy), ImVec2(xE, gy), color);
            if(k != -4 && k != 4) {
                char label[8]; std::snprintf(label, sizeof(label), "%+.0f", db);
                dl->AddText(ImVec2(xS + 2.0f * zoom, gy - 11.0f * zoom), col.dbGridText, label);
            }
        }

        // ── Optional FFT spectrum overlay ─────────────────────────────────
        if(fftMags && numBins > 0) {
            const float fftLogRatio = std::log(FFT_FREQ_MAX / FFT_FREQ_MIN);
            const float eqLogMin    = std::log10(FREQ_MIN);
            const float eqLogMax    = std::log10(FREQ_MAX);

            for(int b = 0; b < numBins; b++) {
                float fc1 = FFT_FREQ_MIN * std::exp(fftLogRatio * (float)b       / (float)numBins);
                float fc2 = FFT_FREQ_MIN * std::exp(fftLogRatio * (float)(b + 1) / (float)numBins);

                if(fc2 <= FREQ_MIN) continue;
                if(fc1 >= FREQ_MAX) break;
                fc1 = std::max(fc1, FREQ_MIN);
                fc2 = std::min(fc2, FREQ_MAX);

                float x1 = xS + W * ofClamp((std::log10(fc1) - eqLogMin) / (eqLogMax - eqLogMin), 0.0f, 1.0f);
                float x2 = xS + W * ofClamp((std::log10(fc2) - eqLogMin) / (eqLogMax - eqLogMin), 0.0f, 1.0f);
                if(x2 <= x1) continue;

                float mag   = fftMags[b];
                float magDb = 20.0f * std::log10(std::max(mag, 1e-12f));
                float barH  = H * ofClamp((magDb - FFT_DB_FLOOR) / (-FFT_DB_FLOOR), 0.0f, 1.0f);
                float barTop = yE - barH;

                dl->AddRectFilled(ImVec2(x1, barTop), ImVec2(x2, yE), col.fft);
            }
        }

        // ── Band frequency markers ────────────────────────────────────────
        for(int b = 0; b < NUM_BANDS; b++) {
            float bx = logFreqToX(bands[b].freqHz, xS, W);
            dl->AddLine(ImVec2(bx, yS), ImVec2(bx, yE - 14.0f * zoom), col.bandMarker[b]);
        }

        // ── 0 dB reference line — drawn BEFORE curve so curve sits on top ─
        {
            float y0 = dbToY(0.0f, yS, H);
            dl->AddLine(ImVec2(xS, y0), ImVec2(xE, y0), col.zeroLine, 1.0f);
        }

        // ── EQ curve ──────────────────────────────────────────────────────
        {
            float logMin = std::log10(FREQ_MIN);
            float logMax = std::log10(FREQ_MAX);
            for(int i = 0; i < NUM_FREQ_POINTS - 1; i++) {
                float t1 = (float)i       / (float)(NUM_FREQ_POINTS - 1);
                float t2 = (float)(i + 1) / (float)(NUM_FREQ_POINTS - 1);
                float freq1 = std::pow(10.0f, logMin + t1 * (logMax - logMin));
                float freq2 = std::pow(10.0f, logMin + t2 * (logMax - logMin));

                float x1 = logFreqToX(freq1, xS, W);
                float y1 = dbToY(curveDb[i],   yS, H);
                float x2 = logFreqToX(freq2, xS, W);
                float y2 = dbToY(curveDb[i+1], yS, H);

                dl->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), col.curve, 2.0f);
            }
        }

        // ── Handles (interactive only) ────────────────────────────────────
        if(interactive) {
            for(int b = 0; b < NUM_BANDS; b++) {
                ImVec2 p(logFreqToX(bands[b].freqHz, xS, W), dbToY(bands[b].gainDb, yS, H));
                const bool hot = (b == dragBand) || (b == hoverBand);
                dl->AddCircleFilled(p, (hot ? 6.0f : 5.0f) * zoom, hot ? col.handleActive : col.handle);
                dl->AddCircle(p, (hot ? 6.0f : 5.0f) * zoom, col.bandMarker[b], 16, 1.5f * zoom);
                char num[4]; std::snprintf(num, sizeof(num), "%d", b + 1);
                dl->AddText(ImVec2(p.x + 7.0f * zoom, p.y - 16.0f * zoom), col.gridText, num);
            }
            if((hoverBand >= 0 || dragBand >= 0) && !ImGui::IsPopupOpen(popupId.c_str())) {
                const Band& bd = bands[dragBand >= 0 ? dragBand : hoverBand];
                ImGui::SetTooltip("%s  %.0f Hz  %+.1f dB  %s %.2f\n(drag: freq / gain, Shift+drag: fine, wheel: %s, double-click: 0 dB)",
                                  bandName(dragBand >= 0 ? dragBand : hoverBand), bd.freqHz, bd.gainDb,
                                  isShelf(dragBand >= 0 ? dragBand : hoverBand) ? "slope" : "Q", bd.shape,
                                  isShelf(dragBand >= 0 ? dragBand : hoverBand) ? "slope" : "Q");
            }
        }

        if(interactive && bandMenu && ImGui::BeginPopup(popupId.c_str())) {
            bandMenu(std::max(0, std::min(NUM_BANDS - 1, menuBand)));
            ImGui::EndPopup();
        }

        ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + H + 2.0f * pad));
        ImGui::Dummy(ImVec2(W, 4.0f * zoom));
        return changed;
    }

    static const char* bandName(int b) {
        static const char* names[NUM_BANDS] = {"Low shelf", "Low mid", "Mid", "High mid", "High shelf"};
        return names[std::max(0, std::min(NUM_BANDS - 1, b))];
    }

private:
    int dragBand = -1, hoverBand = -1, menuBand = 0;
    std::string popupId;

    // On the InvisibleButton just submitted
    bool handleInteraction(float xS, float yS, float W, float H, float zoom) {
        bool changed = false;
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool hovered = ImGui::IsItemHovered();
        hoverBand = -1;
        if(hovered && dragBand < 0) {
            float best = (10.0f * zoom) * (10.0f * zoom);
            for(int b = 0; b < NUM_BANDS; b++) {
                float dx = mouse.x - logFreqToX(bands[b].freqHz, xS, W);
                float dy = mouse.y - dbToY(bands[b].gainDb, yS, H);
                if(dx * dx + dy * dy < best) { best = dx * dx + dy * dy; hoverBand = b; }
            }
        }
        if(ImGui::IsItemActivated() && hoverBand >= 0) dragBand = hoverBand;
        if(!ImGui::IsMouseDown(0)) dragBand = -1;

        if(hoverBand >= 0 && ImGui::IsMouseDoubleClicked(0)) {
            bands[hoverBand].gainDb = 0.0f;
            changed = true;
        }
        if(dragBand >= 0 && ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 0.0f)) {
            Band& bd = bands[dragBand];
            // Relative drag (Shift: 10x slower): pressing / releasing Shift
            // never makes the handle jump
            const ImVec2 d     = ImGui::GetIO().MouseDelta;
            const float  speed = ImGui::GetIO().KeyShift ? 0.1f : 1.0f;
            const float  x = logFreqToX(bd.freqHz, xS, W) + d.x * speed;
            const float  y = dbToY(bd.gainDb, yS, H) + d.y * speed;
            bd.freqHz = ofClamp(xToLogFreq(x, xS, W), FREQ_MIN, FREQ_MAX);
            bd.gainDb = ofClamp(yToDb(y, yS, H), -gainLimit, gainLimit);
            changed = true;
        }
        const int wheelBand = dragBand >= 0 ? dragBand : hoverBand;
        const float wheel = ImGui::GetIO().MouseWheel;
        if(hovered && wheelBand >= 0 && wheel != 0.0f) {
            Band& bd = bands[wheelBand];
            const float f = std::pow(1.1f, wheel);
            bd.shape = isShelf(wheelBand) ? ofClamp(bd.shape * f, slopeMin, slopeMax)
                                          : ofClamp(bd.shape * f, qMin, qMax);
            changed = true;
        }
        return changed;
    }
};
