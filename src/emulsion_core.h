// Emulsion - film emulation engine (host independent).
// Copyright (c) 2026. Released under the BSD-3-Clause licence (see LICENSE).
#pragma once

#include <cstddef>
#include <cstdint>

namespace emu {

// Input profiles (order matches the plugin's Input menu).
enum InputProfile {
    kInputDaVinciIntermediate = 0,
    kInputRec709 = 1,
    kInputSLog3SGamut3Cine,
    kInputSLog3SGamut3,
    kInputFLog2,
    kInputFLog2C,
    kInputFLog,
    kInputAppleLog,
    kInputVLog,
    kInputCanonLog3,
    kInputLogC3,
    kInputLogC4,
    kInputBMDFilmGen5,
    kInputNLog,
    kInputDLog,
};

int inputCount();
const char* inputName(int index);
int lookCount();
const char* lookName(int index);
bool lookIsMonochrome(int index);

struct Settings {
    int input = kInputRec709;
    double exposure = 0.0;      // stops
    int look = 0;
    double strength = 1.0;      // 0..1 look blend
    double contrast = 1.0;      // 0.5..1.5, scales stops around mid grey
    double saturation = 1.0;    // 0..2

    double halationAmount = 0.25;
    double halationSize = 0.35;      // 0..1
    double halationThreshold = 2.5;  // stops above mid grey
    double halationWarmth = 0.35;    // 0 red .. 1 orange

    double bloomAmount = 0.15;
    double bloomSize = 0.5;          // 0..1
    double bloomThreshold = 2.0;     // stops above mid grey

    double grainAmount = 0.25;
    double grainSize = 0.35;         // 0..1
    double grainSoftness = 0.5;      // 0..1
    double grainColor = 0.25;        // 0 mono .. 1 colour
    bool grainAnimate = true;
    int grainSeed = 0;

    double frame = 0.0;              // host time, drives animated grain
    int threads = 0;                 // 0 = hardware concurrency
};

// A view on interleaved float pixels. rowStride is in floats and may be negative.
struct ImageRef {
    float* data = nullptr;   // pixel (0, 0)
    int width = 0;
    int height = 0;
    std::ptrdiff_t rowStride = 0;
    int channels = 4;        // 3 (RGB) or 4 (RGBA)
};

// Process src into dst (same width/height). src and dst may alias (in place).
// Coordinates (originX, originY) of pixel (0,0) in the full frame keep grain stable across tiles.
void process(const ImageRef& src, const ImageRef& dst, const Settings& s, int originX = 0, int originY = 0);

// Single pixel through decode -> render -> look -> saturation (no halation, bloom or grain).
// Used by tests to compare against the Python reference.
void processPixelNoEffects(const float in[3], float out[3], const Settings& s);

// Decode one input code value to scene-linear (camera gamut), for tests.
float decodeValue(int profile, float code);

}  // namespace emu
