#ifndef scMorphOscScope_h
#define scMorphOscScope_h

#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeShared.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <vector>

class scMorphOscScope : public ofxOceanodeNodeModel {
public:
    scMorphOscScope() : ofxOceanodeNodeModel("Morph Osc Scope") {}

    void setup() override {
        description = "Single-cycle MorphOSC waveform preview with the same shaping controls as the synthdef.";

        addParameter(shape.set("Shape", {0.5f}, {0.0f}, {1.0f}));
        addParameter(skew.set("Skew", {0.5f}, {0.0f}, {1.0f}));
        addParameter(pw.set("Pw", {1.0f}, {0.01f}, {1.0f}));
        addParameter(powParam.set("Pow", {1.0f}, {0.1f}, {4.0f}));
        addParameter(bipow.set("BiPow", {0.0f}, {-0.99f}, {0.99f}));
        addParameter(quant.set("Quant", {0}, {0}, {64}));
        addParameter(levels.set("Levels", {1.0f}, {0.0f}, {1.0f}));

        addInspectorParameter(widgetWidth.set("Widget Width", 240.0f, 100.0f, 1000.0f));
        addInspectorParameter(widgetHeight.set("Widget Height", 130.0f, 60.0f, 500.0f));
        addInspectorParameter(samples.set("Samples", 256, 32, 2048));
        addInspectorParameter(lineColor.set("Color",
                                            ofColor(120, 230, 190, 255),
                                            ofColor(0, 0, 0, 0),
                                            ofColor(255, 255, 255, 255)));

        addCustomRegion(region.set("Waveform", [this]() {
            drawWaveform();
        }), [this]() {
            drawWaveform();
        });

        auto recompute = [this]() { rebuildWaveform(); };
        listeners.push(shape.newListener([recompute](vector<float>&) { recompute(); }));
        listeners.push(skew.newListener([recompute](vector<float>&) { recompute(); }));
        listeners.push(pw.newListener([recompute](vector<float>&) { recompute(); }));
        listeners.push(powParam.newListener([recompute](vector<float>&) { recompute(); }));
        listeners.push(bipow.newListener([recompute](vector<float>&) { recompute(); }));
        listeners.push(quant.newListener([recompute](vector<int>&) { recompute(); }));
        listeners.push(levels.newListener([recompute](vector<float>&) { recompute(); }));
        listeners.push(samples.newListener([recompute](int&) { recompute(); }));

        rebuildWaveform();
    }

private:
    static constexpr float kPi = 3.14159265358979323846f;
    static constexpr float kPad = 4.0f;

    ofParameter<vector<float>> shape;
    ofParameter<vector<float>> skew;
    ofParameter<vector<float>> pw;
    ofParameter<vector<float>> powParam;
    ofParameter<vector<float>> bipow;
    ofParameter<vector<int>> quant;
    ofParameter<vector<float>> levels;

    ofParameter<float> widgetWidth;
    ofParameter<float> widgetHeight;
    ofParameter<int> samples;
    ofParameter<ofColor> lineColor;

    customGuiRegion region;
    ofEventListeners listeners;
    vector<float> waveform;

    static float getValue(const vector<float>& values, float fallback) {
        return values.empty() ? fallback : values.front();
    }

    static int getValue(const vector<int>& values, int fallback) {
        return values.empty() ? fallback : values.front();
    }

    static float clamp01(float value) {
        return std::clamp(value, 0.0f, 1.0f);
    }

    static float applyCustomPow(float signal, float curve) {
        curve = std::clamp(curve, -0.9999f, 0.9999f);
        const float k1 = 2.0f * curve;
        const float k2 = k1 / (1.0f - curve);
        const float k3 = (k2 * std::abs(signal)) + 1.0f;
        return signal * (k2 + 1.0f) / std::max(k3, 1.0e-6f);
    }

    static float computeTriangle(float phase, float skewValue) {
        skewValue = std::clamp(skewValue, 0.00001f, 0.99999f);
        if (phase <= skewValue) return phase / skewValue;
        return 1.0f - ((phase - skewValue) / (1.0f - skewValue));
    }

    static float computeSine(float phase) {
        return (1.0f - std::cos(phase * kPi)) * 0.5f;
    }

    static float computeTrapezoid(float phase, float shapeValue) {
        shapeValue = clamp01(shapeValue);
        if (shapeValue >= 1.0f - 1.0e-6f) return phase > 0.0f ? 1.0f : 0.0f;
        const float steepness = 1.0f / std::max(1.0f - shapeValue, 1.0e-6f);
        return clamp01(phase * steepness);
    }

    static float computeMorphWave(float phase, float skewValue, float shapeValue, float pwValue) {
        shapeValue = clamp01(shapeValue);
        pwValue = std::clamp(pwValue, 0.01f, 1.0f);

        float effectivePw = pwValue;
        if (shapeValue > 0.5f) {
            const float shrink = ofMap(shapeValue, 0.5f, 1.0f, 1.0f, 0.5f, true);
            effectivePw *= shrink;
        }

        const float scaledPhase = clamp01(phase / std::max(effectivePw, 1.0e-6f));
        const float triangle = computeTriangle(scaledPhase, skewValue);
        const float sine = computeSine(triangle);
        const float blend = shapeValue * 2.0f;

        if (blend < 1.0f) {
            const float amount = clamp01(blend);
            return ofLerp(sine, triangle, amount);
        }

        const float trapShape = clamp01(blend - 1.0f);
        return computeTrapezoid(triangle, trapShape);
    }

    static float quantizeBipolar(float value, int quantValue) {
        if (quantValue <= 1) return value;
        const float unipolar = (value + 1.0f) * 0.5f;
        const float steps = (float)(quantValue - 1);
        const float quantized = std::round(unipolar * steps) / std::max(steps, 1.0f);
        return (quantized * 2.0f) - 1.0f;
    }

    float computeSample(float phase) const {
        float value = computeMorphWave(
            phase,
            getValue(skew.get(), 0.5f),
            getValue(shape.get(), 0.5f),
            getValue(pw.get(), 1.0f)
        );

        value = std::pow(clamp01(value), getValue(powParam.get(), 1.0f));
        value = (value * 2.0f) - 1.0f;
        value = applyCustomPow(value, getValue(bipow.get(), 0.0f));
        value = quantizeBipolar(value, getValue(quant.get(), 0));
        value *= getValue(levels.get(), 1.0f);
        return std::clamp(value, -1.0f, 1.0f);
    }

    void rebuildWaveform() {
        const int numSamples = std::max(samples.get(), 2);
        waveform.assign(numSamples, 0.0f);
        for (int i = 0; i < numSamples; i++) {
            const float phase = (float)i / (float)(numSamples - 1);
            waveform[i] = computeSample(phase);
        }
    }

    void drawWaveform() {
        float zoom = ofxOceanodeShared::getZoomLevel();
        const auto& ctx = ofxOceanodeShared::getCustomRegionRenderContext();

        const float pad = kPad * zoom;
        const float width = ctx.active
            ? std::max(1.0f, ctx.width - pad * 2.0f)
            : widgetWidth.get() * zoom;
        const float height = ctx.active
            ? std::max(1.0f, ctx.height - pad * 2.0f)
            : widgetHeight.get() * zoom;

        ImVec2 start = ImGui::GetCursorScreenPos();
        ImVec2 min(start.x + pad, start.y + pad);
        ImVec2 max(min.x + width, min.y + height);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        ImGui::InvisibleButton("##MorphOscScope", ImVec2(width + pad * 2.0f, height + pad * 2.0f));

        drawList->AddRectFilled(min, max, IM_COL32(10, 16, 22, 255), 6.0f);
        drawList->AddRect(min, max, IM_COL32(58, 72, 84, 255), 6.0f);

        const float midY = min.y + height * 0.5f;
        const float q1Y = min.y + height * 0.25f;
        const float q3Y = min.y + height * 0.75f;

        drawList->AddLine(ImVec2(min.x, midY), ImVec2(max.x, midY), IM_COL32(120, 140, 155, 120), 1.0f);
        drawList->AddLine(ImVec2(min.x, q1Y), ImVec2(max.x, q1Y), IM_COL32(60, 78, 92, 90), 1.0f);
        drawList->AddLine(ImVec2(min.x, q3Y), ImVec2(max.x, q3Y), IM_COL32(60, 78, 92, 90), 1.0f);
        drawList->AddLine(ImVec2(min.x + width * 0.25f, min.y), ImVec2(min.x + width * 0.25f, max.y), IM_COL32(60, 78, 92, 60), 1.0f);
        drawList->AddLine(ImVec2(min.x + width * 0.5f, min.y), ImVec2(min.x + width * 0.5f, max.y), IM_COL32(78, 100, 118, 80), 1.0f);
        drawList->AddLine(ImVec2(min.x + width * 0.75f, min.y), ImVec2(min.x + width * 0.75f, max.y), IM_COL32(60, 78, 92, 60), 1.0f);

        if (waveform.size() >= 2) {
            vector<ImVec2> points(waveform.size());
            for (size_t i = 0; i < waveform.size(); i++) {
                const float x = min.x + ((float)i / (float)(waveform.size() - 1)) * width;
                const float y = min.y + (1.0f - ((waveform[i] + 1.0f) * 0.5f)) * height;
                points[i] = ImVec2(x, y);
            }

            const ofColor& color = lineColor.get();
            drawList->AddPolyline(points.data(),
                                  (int)points.size(),
                                  IM_COL32(color.r, color.g, color.b, color.a),
                                  false,
                                  2.0f * zoom);
        }

        ImGui::Dummy(ImVec2(width + pad * 2.0f, ctx.active ? height + pad * 2.0f : 4.0f * zoom));
    }
};

#endif /* scMorphOscScope_h */
