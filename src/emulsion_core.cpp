// Emulsion - film emulation engine (host independent).
// Copyright (c) 2026. Released under the BSD-3-Clause licence (see LICENSE).
#include "emulsion_core.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "generated/emulsion_data.h"

namespace emu {

static_assert(data::kInputCount == 15, "input profile table out of sync");

namespace {

constexpr float kMidGrey = 0.18f;
const float kY2020[3] = {0.2627f, 0.6780f, 0.0593f};
const float kY709[3] = {0.2126f, 0.7152f, 0.0722f};

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float max3(const float v[3]) { return std::max(v[0], std::max(v[1], v[2])); }
inline float min3(const float v[3]) { return std::min(v[0], std::min(v[1], v[2])); }
inline float dot3(const float w[3], const float v[3]) { return w[0] * v[0] + w[1] * v[1] + w[2] * v[2]; }
inline void mat3(const float* m, const float in[3], float out[3]) {
    const float r = in[0], g = in[1], b = in[2];
    out[0] = m[0] * r + m[1] * g + m[2] * b;
    out[1] = m[3] * r + m[4] * g + m[5] * b;
    out[2] = m[6] * r + m[7] * g + m[8] * b;
}
inline float sanitize(float v) {
    if (std::isnan(v)) return 0.0f;
    return clampf(v, -65504.0f, 65504.0f);  // also maps +/-inf to the largest half-float value
}
inline uint32_t floatBits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
inline float bitsFloat(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// ------------------------------------------------------------------ camera log decoding (exact)
// Formulas follow the camera makers' published curves as implemented in colour-science 0.4.7,
// applied to Resolve's normalised (0..1) signal.
inline float decodeDaVinciIntermediate(float v) {
    const float A = 0.0075f, B = 7.0f, C = 0.07329248f, M = 10.44426855f, LOG_CUT = 0.02740668f;
    return v <= LOG_CUT ? v / M : std::exp2(v / C - B) - A;
}
inline float decodeSLog3(float y) {
    return y >= 171.2102946929f / 1023.0f
               ? std::pow(10.0f, (y * 1023.0f - 420.0f) / 261.5f) * (0.18f + 0.01f) - 0.01f
               : (y * 1023.0f - 95.0f) * 0.01125f / (171.2102946929f - 95.0f);
}
inline float decodeFLogFamily(float v, float a, float b, float c, float d, float e, float f, float cut2) {
    return v < cut2 ? (v - f) / e : std::pow(10.0f, (v - d) / c) / a - b / a;
}
inline float decodeFLog(float v) {
    return decodeFLogFamily(v, 0.555556f, 0.009468f, 0.344676f, 0.790453f, 8.735631f, 0.092864f, 0.100537775223865f);
}
inline float decodeFLog2(float v) {
    return decodeFLogFamily(v, 5.555556f, 0.064829f, 0.245281f, 0.384316f, 8.799461f, 0.092864f, 0.100686685370811f);
}
inline float decodeAppleLog(float P) {
    const float R0 = -0.05641088f, Rt = 0.01f, sigma = 47.28711236f, beta = 0.00964052f, gamma = 0.08550479f,
                delta = 0.69336945f;
    const float Pt = sigma * (Rt - R0) * (Rt - R0);
    if (P >= Pt) return std::exp2((P - delta) / gamma) - beta;
    if (P >= 0.0f) return std::sqrt(P / sigma) + R0;
    return R0;
}
inline float decodeVLog(float v) {
    const float cut2 = 0.181f, b = 0.00873f, c = 0.241514f, d = 0.598206f;
    return v < cut2 ? (v - 0.125f) / 5.6f : std::pow(10.0f, (v - d) / c) - b;
}
inline float decodeCanonLog3(float x) {  // v1.2, reflection output
    float y;
    if (x < 0.097465473f)
        y = -(std::pow(10.0f, (0.12783901f - x) / 0.36726845f) - 1.0f) / 14.98325f;
    else if (x <= 0.15277891f)
        y = (x - 0.12512219f) / 1.9754798f;
    else
        y = (std::pow(10.0f, (x - 0.12240537f) / 0.36726845f) - 1.0f) / 14.98325f;
    return y * 0.9f;
}
inline float decodeLogC3(float t) {  // ALEXA LogC3, EI 800, SUP 3.x, scene exposure factor
    const float cut = 0.010591f, a = 5.555556f, b = 0.052272f, c = 0.24719f, d = 0.385537f, e = 5.367655f,
                f = 0.092809f;
    return t > e * cut + f ? (std::pow(10.0f, (t - d) / c) - b) / a : (t - f) / e;
}
inline float decodeLogC4(float E) {
    const float a = 2231.8263090676883f, b = 0.9071358748778103f, c = 0.09286412512218964f,
                s = 0.1135972086105891f, t = -0.01805699611991131f;
    return E >= 0.0f ? (std::exp2(14.0f * (E - c) / b + 6.0f) - 64.0f) / a : E * s + t;
}
inline float decodeBMDGen5(float y) {
    const float A = 0.08692876065491224f, B = 0.005494072432257808f, C = 0.5300133392291939f,
                D = 8.283605932402494f, E = 0.09246575342465753f, LIN_CUT = 0.005f;
    const float LOG_CUT = D * LIN_CUT + E;
    return y < LOG_CUT ? (y - E) / D : std::exp((y - C) / A) - B;
}
inline float decodeNLog(float x) {
    const float cut2 = 0.4418377321603128f, a = 0.635386119257087f, b = 0.0075f, c = 0.1466275659824047f,
                d = 0.6050830889540567f;
    if (x < cut2) {
        const float t = x / a;
        return t * t * t - b;
    }
    return std::exp((x - d) / c);
}
inline float decodeDLog(float y) {
    return y <= 0.14f ? (y - 0.0929f) / 6.025f : (std::pow(10.0f, 3.89616f * y - 2.27752f) - 0.0108f) / 0.9892f;
}
inline float decodeRec709(float v) {  // BT.1886 gamma 2.4, scaled so code 0.42 lands near 0.18
    return std::copysign(std::pow(std::fabs(v), 2.4f), v) * data::kDisplayScale;
}

float decodeExact(int profile, float v) {
    switch (profile) {
        case kInputDaVinciIntermediate: return decodeDaVinciIntermediate(v);
        case kInputRec709: return decodeRec709(v);
        case kInputSLog3SGamut3Cine:
        case kInputSLog3SGamut3: return decodeSLog3(v);
        case kInputFLog2:
        case kInputFLog2C: return decodeFLog2(v);
        case kInputFLog: return decodeFLog(v);
        case kInputAppleLog: return decodeAppleLog(v);
        case kInputVLog: return decodeVLog(v);
        case kInputCanonLog3: return decodeCanonLog3(v);
        case kInputLogC3: return decodeLogC3(v);
        case kInputLogC4: return decodeLogC4(v);
        case kInputBMDFilmGen5: return decodeBMDGen5(v);
        case kInputNLog: return decodeNLog(v);
        case kInputDLog: return decodeDLog(v);
        default: return v;
    }
}

// Decode tables: linear interpolation over [-0.125, 1.25]; exact formula outside.
struct DecodeTables {
    static constexpr float kLo = -0.125f, kHi = 1.25f;
    static constexpr int kN = 16384;
    float scale = float(kN - 1) / (kHi - kLo);
    std::vector<float> t[data::kInputCount];
    DecodeTables() {
        for (int p = 0; p < data::kInputCount; ++p) {
            t[p].resize(kN);
            for (int i = 0; i < kN; ++i) {
                const double x = double(kLo) + (double(kHi) - double(kLo)) * double(i) / double(kN - 1);
                t[p][size_t(i)] = sanitize(decodeExact(p, float(x)));
            }
        }
    }
};
const DecodeTables& decodeTables() {
    static const DecodeTables tables;  // thread-safe one-time init
    return tables;
}
inline float decodeFast(const float* table, float scale, int profile, float x) {
    if (!(x >= DecodeTables::kLo && x <= DecodeTables::kHi)) return sanitize(decodeExact(profile, x));
    const float tt = (x - DecodeTables::kLo) * scale;
    const int i = std::min(int(tt), DecodeTables::kN - 2);
    const float f = tt - float(i);
    return table[i] + (table[i + 1] - table[i]) * f;
}

// ------------------------------------------------------------------ log-indexed lookup tables
// f(x) sampled on the float bit pattern: 256 samples per octave, linear within each sample.
class LogLut {
public:
    template <class F>
    void build(int e0, int e1, F f) {
        minX_ = std::ldexp(1.0f, e0);
        maxX_ = std::ldexp(1.0f, e1);
        minBits_ = floatBits(minX_);
        const int n = (e1 - e0) * 256 + 2;
        v_.resize(size_t(n));
        for (int i = 0; i < n; ++i) v_[size_t(i)] = f(bitsFloat(minBits_ + (uint32_t(i) << 15)));
        atMin_ = v_.front();
        atMax_ = f(maxX_);
    }
    float operator()(float x) const {
        if (!(x > minX_)) return x > 0.0f ? atMin_ * (x / minX_) : 0.0f;  // linear ramp to 0 below range
        if (x >= maxX_) return atMax_;
        const uint32_t rel = floatBits(x) - minBits_;
        const uint32_t i = rel >> 15;
        const float fr = float(rel & 0x7FFFu) * (1.0f / 32768.0f);
        return v_[i] + (v_[i + 1] - v_[i]) * fr;
    }

private:
    float minX_ = 0, maxX_ = 0, atMin_ = 0, atMax_ = 0;
    uint32_t minBits_ = 0;
    std::vector<float> v_;
};

inline float curveLookup(float stops) {
    const float lo = data::kCurveStopMin, hi = data::kCurveStopMax;
    if (std::isnan(stops)) stops = lo;
    const float t = (clampf(stops, lo, hi) - lo) / (hi - lo) * float(data::kCurveSize - 1);
    const int i = std::min(int(t), data::kCurveSize - 2);
    const float f = t - float(i);
    return data::kCurve[i] * (1.0f - f) + data::kCurve[i + 1] * f;
}

// Shared encode table: display linear -> BT.1886 code value.
const LogLut& encodeLut() {
    static const LogLut lut = [] {
        LogLut l;
        l.build(-24, 0, [](float d) { return std::pow(d, 1.0f / 2.4f); });
        return l;
    }();
    return lut;
}
inline float encode24(const LogLut& enc, float d) { return d >= 1.0f ? 1.0f : enc(d); }

// Per-render tone table: scene linear (per channel) -> display linear through the print curve.
struct ToneTable {
    LogLut lut;
    void build(float contrast) {
        lut.build(-30, 17, [contrast](float x) {
            const float stops = std::log2(x / kMidGrey) * contrast;
            return std::pow(curveLookup(stops), 2.4f);
        });
    }
};

// Scene-linear Rec.2020 -> Rec.709 code values (gamma 2.4) through the neutral print curve.
inline void renderScene(const ToneTable& tone, const LogLut& enc, const float lin[3], float code[3]) {
    float disp[3];
    for (int c = 0; c < 3; ++c) disp[c] = lin[c] > 0.0f ? tone.lut(lin[c]) : 0.0f;
    float d709[3];
    mat3(data::kRec2020ToRec709, disp, d709);
    // desaturate toward luminance instead of clipping channels
    const float mn = min3(d709);
    if (mn < 0.0f) {
        const float Y = dot3(kY709, d709);
        if (Y <= 0.0f) {
            d709[0] = d709[1] = d709[2] = 0.0f;
        } else {
            const float t = Y / std::max(Y - mn, 1e-12f);
            for (int c = 0; c < 3; ++c) d709[c] = std::max(0.0f, Y + t * (d709[c] - Y));
        }
    }
    const float mx = max3(d709);
    if (mx > 1.0f) {
        const float Y = clampf(dot3(kY709, d709), 0.0f, 1.0f);
        const float t = (1.0f - Y) / std::max(mx - Y, 1e-12f);
        for (int c = 0; c < 3; ++c) d709[c] = Y + t * (d709[c] - Y);
    }
    for (int c = 0; c < 3; ++c) code[c] = encode24(enc, d709[c] > 0.0f ? d709[c] : 0.0f);
}

// Display-referred (Rec.709 input) path back to code values.
inline void renderDisplay(const LogLut& enc, const float lin[3], float code[3]) {
    float d709[3];
    mat3(data::kRec2020ToRec709, lin, d709);
    const float inv = 1.0f / data::kDisplayScale;
    for (int c = 0; c < 3; ++c) code[c] = encode24(enc, d709[c] > 0.0f ? d709[c] * inv : 0.0f);
}

// Pull a colour with a negative channel toward its luminance until the smallest channel is zero.
inline void desatToNonNegative(float rgb[3], const float w[3]) {
    const float mn = min3(rgb);
    if (mn < 0.0f) {
        const float Y = dot3(w, rgb);
        if (Y <= 0.0f) {
            rgb[0] = rgb[1] = rgb[2] = 0.0f;
            return;
        }
        const float t = Y / std::max(Y - mn, 1e-12f);
        for (int c = 0; c < 3; ++c) rgb[c] = Y + t * (rgb[c] - Y);
    }
    for (int c = 0; c < 3; ++c) rgb[c] = std::max(rgb[c], 0.0f);
}

// ------------------------------------------------------------------ look LUTs
struct LookEntry {
    std::string name;
    int size = 0;
    std::vector<float> data;  // [b][g][r][3]
};

// User looks folder: .cube files here are appended to the Film Look menu at plugin load.
std::filesystem::path userLookDir() {
    const char* home = std::getenv("HOME");
    if (!home) return {};
#ifdef __APPLE__
    return std::filesystem::path(home) / "Library/Application Support/Emulsion/Looks";
#else
    return std::filesystem::path(home) / ".local/share/emulsion/looks";
#endif
}

struct LookTables {
    std::vector<LookEntry> looks;
    LookTables() {
        const size_t n = size_t(data::kLutSize) * data::kLutSize * data::kLutSize * 3;
        for (int k = 0; k < data::kLookCount; ++k) {
            LookEntry e{data::kLookNames[k], data::kLutSize, std::vector<float>(n)};
            for (size_t i = 0; i < n; ++i) e.data[i] = float(data::kLookLuts[k][i]) * (1.0f / 65535.0f);
            looks.push_back(std::move(e));
        }
        std::error_code ec;
        std::vector<std::filesystem::path> files;
        for (const auto& f : std::filesystem::directory_iterator(userLookDir(), ec))
            if (f.path().extension() == ".cube") files.push_back(f.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            LookEntry e{f.stem().string()};
            if (loadCube(f.string().c_str(), e.size, e.data)) looks.push_back(std::move(e));
        }
    }
};
const std::vector<LookEntry>& looks() {
    static const LookTables tables;  // thread-safe one-time init
    return tables.looks;
}
const LookEntry& lookTable(int index) {
    const auto& l = looks();
    return l[std::max(0, std::min(index, int(l.size()) - 1))];
}

// Tetrahedral interpolation in a [b][g][r][3] cube.
inline void applyLut(const float* lut, int N, const float in[3], float out[3]) {
    const float r = clampf(in[0], 0.0f, 1.0f) * float(N - 1);
    const float g = clampf(in[1], 0.0f, 1.0f) * float(N - 1);
    const float b = clampf(in[2], 0.0f, 1.0f) * float(N - 1);
    const int r0 = std::min(int(r), N - 2), g0 = std::min(int(g), N - 2), b0 = std::min(int(b), N - 2);
    const float fr = r - float(r0), fg = g - float(g0), fb = b - float(b0);
    const float* base = lut + ((size_t(b0) * N + size_t(g0)) * N + size_t(r0)) * 3;
    const size_t sR = 3, sG = size_t(N) * 3, sB = size_t(N) * N * 3;
    auto V = [&](int dr, int dg, int db) { return base + size_t(dr) * sR + size_t(dg) * sG + size_t(db) * sB; };
    const float* c000 = V(0, 0, 0);
    const float* c111 = V(1, 1, 1);
    const float *cA, *cB;
    float t1, t2, t3;
    if (fr >= fg) {
        if (fg >= fb) { cA = V(1, 0, 0); cB = V(1, 1, 0); t1 = fr; t2 = fg; t3 = fb; }
        else if (fr >= fb) { cA = V(1, 0, 0); cB = V(1, 0, 1); t1 = fr; t2 = fb; t3 = fg; }
        else { cA = V(0, 0, 1); cB = V(1, 0, 1); t1 = fb; t2 = fr; t3 = fg; }
    } else {
        if (fr >= fb) { cA = V(0, 1, 0); cB = V(1, 1, 0); t1 = fg; t2 = fr; t3 = fb; }
        else if (fg >= fb) { cA = V(0, 1, 0); cB = V(0, 1, 1); t1 = fg; t2 = fb; t3 = fr; }
        else { cA = V(0, 0, 1); cB = V(0, 1, 1); t1 = fb; t2 = fg; t3 = fr; }
    }
    for (int c = 0; c < 3; ++c)
        out[c] = (1.0f - t1) * c000[c] + (t1 - t2) * cA[c] + (t2 - t3) * cB[c] + t3 * c111[c];
}

// ------------------------------------------------------------------ threading
template <class F>
void parallelFor(int n, int threads, F&& fn) {
    if (n <= 0) return;
    if (threads <= 1 || n < 32) {
        fn(0, n);
        return;
    }
    const int T = std::min(threads, n);
    const int chunk = (n + T - 1) / T;
    std::vector<std::thread> pool;
    pool.reserve(size_t(T));
    for (int t = 1; t < T; ++t) {
        const int a = t * chunk, b = std::min(n, a + chunk);
        if (a >= b) break;
        pool.emplace_back([&fn, a, b] { fn(a, b); });
    }
    fn(0, std::min(n, chunk));
    for (auto& th : pool) th.join();
}

int resolveThreads(int requested) {
    if (requested > 0) return requested;
    const unsigned hc = std::thread::hardware_concurrency();
    return hc == 0 ? 4 : int(std::min(hc, 32u));
}

// ------------------------------------------------------------------ glow (halation / bloom)
struct Plane {
    int w = 0, h = 0, ch = 1, factor = 1;
    std::vector<float> px;
    float* row(int y) { return px.data() + size_t(y) * size_t(w) * size_t(ch); }
    const float* row(int y) const { return px.data() + size_t(y) * size_t(w) * size_t(ch); }
};

inline float softThreshold(float x, float T) {
    const float k = 0.5f * T;
    if (x <= T - k) return 0.0f;
    if (x < T + k) {
        const float d = x - T + k;
        return d * d / (4.0f * k);
    }
    return x - T;
}

// Box blur sizes approximating a Gaussian with n passes (Kutskir).
void gaussBoxes(float sigma, int n, int* radii) {
    const float wIdeal = std::sqrt(12.0f * sigma * sigma / float(n) + 1.0f);
    int wl = int(std::floor(wIdeal));
    if (wl % 2 == 0) wl--;
    const int wu = wl + 2;
    const float mIdeal = (12.0f * sigma * sigma - float(n * wl * wl) - 4.0f * float(n * wl) - 3.0f * float(n)) /
                         (-4.0f * float(wl) - 4.0f);
    const int m = int(std::lround(mIdeal));
    for (int i = 0; i < n; ++i) radii[i] = std::max(0, ((i < m ? wl : wu) - 1) / 2);
}

void boxBlurH(const Plane& in, Plane& out, int r, int threads) {
    const int w = in.w, ch = in.ch;
    parallelFor(in.h, threads, [&](int y0, int y1) {
        const double inv = 1.0 / double(2 * r + 1);
        for (int y = y0; y < y1; ++y) {
            const float* s = in.row(y);
            float* d = out.row(y);
            for (int c = 0; c < ch; ++c) {
                double sum = 0.0;
                for (int k = -r; k <= r; ++k) sum += s[size_t(std::max(0, std::min(w - 1, k))) * ch + c];
                for (int x = 0; x < w; ++x) {
                    d[size_t(x) * ch + c] = float(sum * inv);
                    const int addX = std::min(w - 1, x + r + 1);
                    const int subX = std::max(0, x - r);
                    sum += double(s[size_t(addX) * ch + c]) - double(s[size_t(subX) * ch + c]);
                }
            }
        }
    });
}

void boxBlurV(const Plane& in, Plane& out, int r, int threads) {
    const int w = in.w, h = in.h, ch = in.ch;
    parallelFor(w, threads, [&](int x0, int x1) {
        std::vector<double> sum(size_t(x1 - x0) * size_t(ch), 0.0);
        const double inv = 1.0 / double(2 * r + 1);
        for (int k = -r; k <= r; ++k) {
            const float* s = in.row(std::max(0, std::min(h - 1, k)));
            for (int x = x0; x < x1; ++x)
                for (int c = 0; c < ch; ++c) sum[size_t(x - x0) * ch + c] += s[size_t(x) * ch + c];
        }
        for (int y = 0; y < h; ++y) {
            float* d = out.row(y);
            const float* sAdd = in.row(std::min(h - 1, y + r + 1));
            const float* sSub = in.row(std::max(0, y - r));
            for (int x = x0; x < x1; ++x)
                for (int c = 0; c < ch; ++c) {
                    double& acc = sum[size_t(x - x0) * ch + c];
                    d[size_t(x) * ch + c] = float(acc * inv);
                    acc += double(sAdd[size_t(x) * ch + c]) - double(sSub[size_t(x) * ch + c]);
                }
        }
    });
}

void gaussianBlur(Plane& p, float sigma, int threads) {
    if (sigma < 0.5f || p.w < 2 || p.h < 2) return;
    int radii[3];
    gaussBoxes(sigma, 3, radii);
    Plane tmp = p;
    for (int i = 0; i < 3; ++i) {
        if (radii[i] <= 0) continue;
        boxBlurH(p, tmp, radii[i], threads);
        boxBlurV(tmp, p, radii[i], threads);
    }
}

// 2x box downsample (channels kept).
Plane halve(const Plane& in, int threads) {
    Plane o;
    o.ch = in.ch;
    o.factor = in.factor * 2;
    o.w = (in.w + 1) / 2;
    o.h = (in.h + 1) / 2;
    o.px.assign(size_t(o.w) * size_t(o.h) * size_t(o.ch), 0.0f);
    parallelFor(o.h, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* a = in.row(std::min(2 * y, in.h - 1));
            const float* b = in.row(std::min(2 * y + 1, in.h - 1));
            float* d = o.row(y);
            for (int x = 0; x < o.w; ++x) {
                const int xa = std::min(2 * x, in.w - 1), xb = std::min(2 * x + 1, in.w - 1);
                for (int c = 0; c < o.ch; ++c)
                    d[size_t(x) * o.ch + c] = 0.25f * (a[size_t(xa) * in.ch + c] + a[size_t(xb) * in.ch + c] +
                                                       b[size_t(xa) * in.ch + c] + b[size_t(xb) * in.ch + c]);
            }
        }
    });
    return o;
}

Plane extractChannels(const Plane& in, int c0, int count) {
    Plane o;
    o.ch = count;
    o.factor = in.factor;
    o.w = in.w;
    o.h = in.h;
    o.px.resize(size_t(o.w) * size_t(o.h) * size_t(count));
    const size_t n = size_t(o.w) * size_t(o.h);
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < count; ++c) o.px[i * count + c] = in.px[i * in.ch + c0 + c];
    return o;
}

inline void sampleBilinear(const Plane& p, float fx, float fy, float* out) {
    // fx, fy in full-resolution pixel coordinates
    const float x = (fx + 0.5f) / float(p.factor) - 0.5f;
    const float y = (fy + 0.5f) / float(p.factor) - 0.5f;
    const float cx = clampf(x, 0.0f, float(p.w - 1));
    const float cy = clampf(y, 0.0f, float(p.h - 1));
    const int x0 = std::min(int(cx), p.w - 1), y0 = std::min(int(cy), p.h - 1);
    const int x1 = std::min(x0 + 1, p.w - 1), y1 = std::min(y0 + 1, p.h - 1);
    const float ax = cx - float(x0), ay = cy - float(y0);
    const float* r0 = p.row(y0);
    const float* r1 = p.row(y1);
    for (int c = 0; c < p.ch; ++c) {
        const float top = r0[size_t(x0) * p.ch + c] * (1.0f - ax) + r0[size_t(x1) * p.ch + c] * ax;
        const float bot = r1[size_t(x0) * p.ch + c] * (1.0f - ax) + r1[size_t(x1) * p.ch + c] * ax;
        out[c] = top * (1.0f - ay) + bot * ay;
    }
}

inline int powerOfTwoFactor(float sigmaFull) {
    // keep >= ~3 px of blur at the reduced resolution
    int f = 1;
    while (f < 32 && sigmaFull / float(f * 2) >= 3.0f) f *= 2;
    return f;
}

struct GlowPlanes {
    Plane halation;  // 1 channel
    Plane bloom;     // 3 channels
};

// One pass over the working image collects every highlight source at the finest needed scale;
// coarser scales are made by 2x halving, then each is blurred.
GlowPlanes buildGlow(const ImageRef& work, bool doHal, float halSigma, float halThr, bool doBloom, float bloomSigma,
                     float bloomThr, int threads) {
    GlowPlanes g;
    const int fTight = doHal ? powerOfTwoFactor(halSigma) : 64;
    const int fWide = doHal ? powerOfTwoFactor(halSigma * 3.0f) : 64;
    const int fBloom = doBloom ? powerOfTwoFactor(bloomSigma) : 64;
    const int f0 = std::min(fTight, std::min(fWide, fBloom));

    Plane base;
    base.ch = 4;  // halation source, bloom R G B
    base.factor = f0;
    base.w = (work.width + f0 - 1) / f0;
    base.h = (work.height + f0 - 1) / f0;
    base.px.assign(size_t(base.w) * size_t(base.h) * 4, 0.0f);
    parallelFor(base.h, threads, [&](int ly0, int ly1) {
        std::vector<double> acc(size_t(base.w) * 4);
        for (int ly = ly0; ly < ly1; ++ly) {
            std::fill(acc.begin(), acc.end(), 0.0);
            const int yA = ly * f0, yB = std::min(work.height, yA + f0);
            for (int y = yA; y < yB; ++y) {
                const float* row = work.data + std::ptrdiff_t(y) * work.rowStride;
                for (int x = 0; x < work.width; ++x) {
                    const float* px = row + size_t(x) * work.channels;
                    const float lum = std::min(std::max(0.0f, dot3(kY2020, px)), 65000.0f);
                    double* a = &acc[size_t(x / f0) * 4];
                    // Highlight excess above threshold, saturating at the threshold level: like film,
                    // a light 12 stops over doesn't throw a halo 64x stronger than one 6 stops over.
                    if (doHal) {
                        const float e = softThreshold(lum, halThr);
                        const float cap = 2.5f * halThr;  // lets small point lights still halate
                        a[0] += e * cap / (e + cap);
                    }
                    if (doBloom) {
                        float e = softThreshold(lum, bloomThr);
                        e = e * bloomThr / (e + bloomThr);
                        if (e > 0.0f) {
                            const float k = e / std::max(lum, 1e-6f);
                            a[1] += std::max(0.0f, px[0]) * k;
                            a[2] += std::max(0.0f, px[1]) * k;
                            a[3] += std::max(0.0f, px[2]) * k;
                        }
                    }
                }
            }
            float* out = base.row(ly);
            const double norm = 1.0 / double(f0 * f0);
            for (size_t i = 0; i < acc.size(); ++i) out[i] = float(acc[i] * norm);
        }
    });

    auto atFactor = [&](int f, int c0, int n) {
        Plane p = extractChannels(base, c0, n);
        while (p.factor < f && p.w > 1 && p.h > 1) p = halve(p, threads);
        return p;
    };
    if (doHal) {
        Plane tight = atFactor(fTight, 0, 1);
        Plane wide = atFactor(fWide, 0, 1);
        gaussianBlur(tight, halSigma / float(tight.factor), threads);
        gaussianBlur(wide, halSigma * 3.0f / float(wide.factor), threads);
        // merge the wide scale into the tight plane: 0.6 tight + 0.4 wide
        parallelFor(tight.h, threads, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                float* r = tight.row(y);
                for (int x = 0; x < tight.w; ++x) {
                    float w = 0.0f;
                    const float fx = (float(x) + 0.5f) * float(tight.factor) - 0.5f;
                    const float fy = (float(y) + 0.5f) * float(tight.factor) - 0.5f;
                    sampleBilinear(wide, fx, fy, &w);
                    r[x] = 0.6f * r[x] + 0.4f * w;
                }
            }
        });
        g.halation = std::move(tight);
    }
    if (doBloom) {
        g.bloom = atFactor(fBloom, 1, 3);
        gaussianBlur(g.bloom, bloomSigma / float(g.bloom.factor), threads);
    }
    return g;
}

// ------------------------------------------------------------------ grain
inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
inline float lattice(int ix, int iy, uint32_t seed) {
    const uint32_t h = hash32(uint32_t(ix) * 0x8da6b343U + uint32_t(iy) * 0xd8163841U + seed);
    return float(h >> 8) * (1.0f / 8388608.0f) - 1.0f;  // [-1, 1)
}
inline float valueNoise(float x, float y, uint32_t seed) {
    const float flx = std::floor(x), fly = std::floor(y);
    const int ix = int(flx), iy = int(fly);
    const float fx = x - flx, fy = y - fly;
    const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
    const float a = lattice(ix, iy, seed), b = lattice(ix + 1, iy, seed);
    const float c = lattice(ix, iy + 1, seed), d = lattice(ix + 1, iy + 1, seed);
    const float top = a + (b - a) * sx, bot = c + (d - c) * sx;
    return top + (bot - top) * sy;
}

// Precomputed lattice of value-noise values covering the frame (one per octave/channel).
struct NoiseGrid {
    int ix0 = 0, iy0 = 0, gw = 0, gh = 0;
    float k = 1.0f, ox = 0.0f, oy = 0.0f;
    std::vector<float> v;
    void build(int x0, int y0, int W, int H, float invCell, float offX, float offY, uint32_t seed, int threads) {
        k = invCell;
        ox = offX;
        oy = offY;
        ix0 = int(std::floor((float(x0) + 0.5f) * k + ox));
        iy0 = int(std::floor((float(y0) + 0.5f) * k + oy));
        const int ix1 = int(std::floor((float(x0 + W) - 0.5f) * k + ox)) + 1;
        const int iy1 = int(std::floor((float(y0 + H) - 0.5f) * k + oy)) + 1;
        gw = ix1 - ix0 + 1;
        gh = iy1 - iy0 + 1;
        v.resize(size_t(gw) * size_t(gh));
        parallelFor(gh, threads, [&](int a, int b) {
            for (int j = a; j < b; ++j)
                for (int i = 0; i < gw; ++i) v[size_t(j) * gw + i] = lattice(ix0 + i, iy0 + j, seed);
        });
    }
    inline float sample(int px, int py) const {
        const float x = (float(px) + 0.5f) * k + ox, y = (float(py) + 0.5f) * k + oy;
        const float flx = std::floor(x), fly = std::floor(y);
        const int i = std::min(std::max(int(flx) - ix0, 0), gw - 2);
        const int j = std::min(std::max(int(fly) - iy0, 0), gh - 2);
        const float fx = x - flx, fy = y - fly;
        const float sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
        const float* r0 = v.data() + size_t(j) * gw + i;
        const float* r1 = r0 + gw;
        const float top = r0[0] + (r0[1] - r0[0]) * sx, bot = r1[0] + (r1[1] - r1[0]) * sx;
        return top + (bot - top) * sy;
    }
};

struct GrainParams {
    float amp = 0.0f, invCell = 1.0f, wFine = 0.0f, wPix = 0.0f, norm = 1.0f, normColor = 1.0f;
    float chroma = 0.0f, chromaNorm = 1.0f;
    uint32_t seed = 0;
    NoiseGrid coarse;      // main luminance octave
    NoiseGrid colour[3];   // one octave per channel
    float weight[1025];    // grain visibility vs luma
};

// Luminance grain: two octaves plus a little per-pixel noise (less of both when soft).
inline float grainMono(const GrainParams& g, int px, int py) {
    float n = 0.75f * g.coarse.sample(px, py);
    if (g.wFine > 0.0f) {
        const float x = (float(px) + 0.5f) * g.invCell * 2.13f + 17.13f, y = (float(py) + 0.5f) * g.invCell * 2.13f + 3.71f;
        n += g.wFine * valueNoise(x, y, g.seed ^ 0x5bd1e995U);
    }
    if (g.wPix > 0.0f) {
        const uint32_t h = hash32(uint32_t(px) * 0x27d4eb2dU + uint32_t(py) * 0x165667b1U + (g.seed ^ 0x9e3779b9U));
        n += g.wPix * (float(h >> 8) * (1.0f / 8388608.0f) - 1.0f);
    }
    return n * g.norm;
}

inline float grainWeightExact(float Y) {  // most visible in the midtones, a little in shadows/highlights
    const float b = clampf(4.0f * Y * (1.0f - Y), 0.0f, 1.0f);
    return 0.3f + 0.7f * std::pow(b, 0.75f);
}
inline float grainWeight(const GrainParams& g, float Y) {
    const float t = clampf(Y, 0.0f, 1.0f) * 1024.0f;
    const int i = std::min(int(t), 1023);
    const float f = t - float(i);
    return g.weight[i] + (g.weight[i + 1] - g.weight[i]) * f;
}

// ------------------------------------------------------------------ per-pixel stages
struct Prepared {
    int input = 0;
    bool display = false;
    const float* inMatrix = nullptr;
    const float* decodeTable = nullptr;
    float decodeScale = 1.0f;
    float exposureGain = 1.0f;
    float contrast = 1.0f;
    const float* lut = nullptr;
    int lutN = 0;
    float strength = 1.0f;
    float saturation = 1.0f;
    std::shared_ptr<ToneTable> tone;
    const LogLut* enc = nullptr;
};

Prepared prepare(const Settings& s) {
    Prepared p;
    p.input = std::max(0, std::min(s.input, data::kInputCount - 1));
    p.display = p.input == kInputRec709;
    p.inMatrix = data::kInputToRec2020[p.input];
    const DecodeTables& dt = decodeTables();
    p.decodeTable = dt.t[p.input].data();
    p.decodeScale = dt.scale;
    p.exposureGain = float(std::exp2(clampf(float(s.exposure), -10.0f, 10.0f)));
    p.contrast = clampf(float(s.contrast), 0.2f, 3.0f);
    const LookEntry& look = lookTable(s.look);
    p.lut = look.data.data();
    p.lutN = look.size;
    p.strength = clampf(float(s.strength), 0.0f, 1.0f);
    p.saturation = clampf(float(s.saturation), 0.0f, 3.0f);
    p.enc = &encodeLut();
    if (!p.display) {
        p.tone = std::make_shared<ToneTable>();
        p.tone->build(p.contrast);
    }
    return p;
}

// Stage 1: input code values -> working scene-linear Rec.2020 (exposure, contrast for display path).
inline void stageDecode(const Prepared& p, const float in[3], float lin[3]) {
    float cam[3];
    for (int c = 0; c < 3; ++c) cam[c] = decodeFast(p.decodeTable, p.decodeScale, p.input, sanitize(in[c]));
    mat3(p.inMatrix, cam, lin);
    for (int c = 0; c < 3; ++c) lin[c] = sanitize(lin[c] * p.exposureGain);
    if (p.display) {
        if (p.contrast != 1.0f)
            for (int c = 0; c < 3; ++c)
                if (lin[c] > 0.0f) lin[c] = kMidGrey * std::pow(std::max(lin[c], 1e-12f) / kMidGrey, p.contrast);
    } else {
        desatToNonNegative(lin, kY2020);
    }
}

// Stage 2: working linear -> look -> saturation (code values).
inline void stageRender(const Prepared& p, const float lin[3], float out[3]) {
    float base[3];
    if (p.display)
        renderDisplay(*p.enc, lin, base);
    else
        renderScene(*p.tone, *p.enc, lin, base);
    float looked[3];
    applyLut(p.lut, p.lutN, base, looked);
    float mix[3];
    for (int c = 0; c < 3; ++c) mix[c] = base[c] + p.strength * (looked[c] - base[c]);
    const float Y = dot3(kY709, mix);
    for (int c = 0; c < 3; ++c) out[c] = clampf(Y + p.saturation * (mix[c] - Y), 0.0f, 1.0f);
}

}  // namespace

// ------------------------------------------------------------------ public API
int inputCount() { return data::kInputCount; }
const char* inputName(int i) { return (i >= 0 && i < data::kInputCount) ? data::kInputNames[i] : ""; }
int lookCount() { return int(looks().size()); }
const char* lookName(int i) { return (i >= 0 && i < lookCount()) ? looks()[i].name.c_str() : ""; }
bool lookIsMonochrome(int i) { return i >= 0 && i < lookCount() && looks()[i].name.compare(0, 4, "Mono") == 0; }

bool loadCube(const char* path, int& size, std::vector<float>& out) {
    std::ifstream f(path);
    if (!f) return false;
    size = 0;
    out.clear();
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::string key;
        if (!(ls >> key) || key[0] == '#' || key == "TITLE") continue;
        if (key == "LUT_1D_SIZE") return false;
        if (key == "LUT_3D_SIZE") {
            if (!(ls >> size) || size < 2 || size > 129) return false;
            out.reserve(size_t(size) * size * size * 3);
            continue;
        }
        if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX") {
            const float want = key == "DOMAIN_MIN" ? 0.0f : 1.0f;
            float v[3];
            if (!(ls >> v[0] >> v[1] >> v[2]) || v[0] != want || v[1] != want || v[2] != want) return false;
            continue;
        }
        if (key.find_first_not_of("+-.0123456789eE") != std::string::npos) continue;  // other keywords
        float g, b;
        if (size == 0 || !(ls >> g >> b)) return false;
        out.push_back(std::strtof(key.c_str(), nullptr));
        out.push_back(g);
        out.push_back(b);
    }
    if (size == 0 || out.size() != size_t(size) * size * size * 3) return false;
    for (float& v : out) v = std::isfinite(v) ? clampf(v, 0.0f, 1.0f) : 0.0f;
    return true;
}

float decodeValue(int profile, float code) { return decodeExact(profile, code); }

void processPixelNoEffects(const float in[3], float out[3], const Settings& s) {
    const Prepared p = prepare(s);
    float lin[3];
    stageDecode(p, in, lin);
    stageRender(p, lin, out);
}

void process(const ImageRef& src, const ImageRef& dst, const Settings& s, int originX, int originY) {
    if (!src.data || !dst.data || dst.width <= 0 || dst.height <= 0) return;
    const int W = dst.width, H = dst.height;
    const int threads = resolveThreads(s.threads);
    const Prepared p = prepare(s);
    const bool hasSrcAlpha = src.channels >= 4;
    const bool hasDstAlpha = dst.channels >= 4;

    // Stage 1: decode into dst (used as the working buffer).
    parallelFor(H, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* srow = (y < src.height) ? src.data + std::ptrdiff_t(y) * src.rowStride : nullptr;
            float* drow = dst.data + std::ptrdiff_t(y) * dst.rowStride;
            for (int x = 0; x < W; ++x) {
                float in[3] = {0.0f, 0.0f, 0.0f};
                float alpha = 1.0f;
                if (srow && x < src.width) {
                    const float* sp = srow + size_t(x) * src.channels;
                    in[0] = sp[0];
                    in[1] = sp[1];
                    in[2] = sp[2];
                    if (hasSrcAlpha) alpha = sp[3];
                }
                float lin[3];
                stageDecode(p, in, lin);
                float* dp = drow + size_t(x) * dst.channels;
                dp[0] = lin[0];
                dp[1] = lin[1];
                dp[2] = lin[2];
                if (hasDstAlpha) dp[3] = alpha;
            }
        }
    });

    // Glow planes from the working (scene-linear) image.
    const float refDim = float(std::min(W, H));
    const bool doHal = s.halationAmount > 1e-4;
    const bool doBloom = s.bloomAmount > 1e-4;
    GlowPlanes glow;
    float halTint[3] = {0, 0, 0};
    float halAmt = 0.0f, bloomAmt = 0.0f;
    if (doHal || doBloom) {
        const float halSigma = std::max(0.75f, (0.002f + 0.016f * clampf(float(s.halationSize), 0.0f, 1.0f)) * refDim);
        const float halThr = kMidGrey * std::exp2(clampf(float(s.halationThreshold), -2.0f, 10.0f));
        const float bloomSigma = std::max(1.0f, (0.01f + 0.07f * clampf(float(s.bloomSize), 0.0f, 1.0f)) * refDim);
        const float bloomThr = kMidGrey * std::exp2(clampf(float(s.bloomThreshold), -2.0f, 10.0f));
        glow = buildGlow(dst, doHal, halSigma, halThr, doBloom, bloomSigma, bloomThr, threads);
        if (doHal) {
            const float w = clampf(float(s.halationWarmth), 0.0f, 1.0f);
            const float tint709[3] = {1.0f, 0.12f + 0.33f * w, 0.03f + 0.07f * w};
            mat3(data::kInputToRec2020[kInputRec709], tint709, halTint);
            halAmt = float(s.halationAmount) * 1.5f;
        }
        if (doBloom) bloomAmt = float(s.bloomAmount) * 0.8f;
    }

    // Grain set-up.
    GrainParams g;
    const bool mono = lookIsMonochrome(s.look) && p.strength > 0.999f;
    const bool doGrain = s.grainAmount > 1e-4;
    if (doGrain) {
        const float cell = (0.6f + 2.2f * clampf(float(s.grainSize), 0.0f, 1.0f)) * refDim / 1080.0f;
        g.invCell = 1.0f / std::max(cell, 0.25f);
        const float soft = clampf(float(s.grainSoftness), 0.0f, 1.0f);
        g.wFine = (1.0f - soft) * 0.55f;
        g.wPix = (1.0f - soft) * (1.0f - soft) * 0.35f;
        // variance: value noise ~0.184 * sum(w^2) + uniform pixel noise / 3
        const float var = 0.184f * (0.75f * 0.75f + g.wFine * g.wFine) + g.wPix * g.wPix / 3.0f;
        g.norm = 1.0f / std::sqrt(var);
        g.normColor = 1.0f / std::sqrt(0.184f);
        g.chroma = mono ? 0.0f : clampf(float(s.grainColor), 0.0f, 1.0f);
        g.chromaNorm = 1.0f / std::sqrt((1.0f - g.chroma) * (1.0f - g.chroma) + g.chroma * g.chroma);
        g.amp = float(s.grainAmount) * 0.075f;
        const uint32_t frame = s.grainAnimate ? uint32_t(int64_t(std::floor(s.frame + 0.5))) : 0u;
        g.seed = hash32(frame * 0x9E3779B1U ^ hash32(uint32_t(s.grainSeed) * 0x85EBCA6BU + 0x632BE5ABU));
        for (int i = 0; i <= 1024; ++i) g.weight[i] = grainWeightExact(float(i) / 1024.0f);
        g.coarse.build(originX, originY, W, H, g.invCell, 0.0f, 0.0f, g.seed, threads);
        if (g.chroma > 0.0f) {
            const uint32_t salts[3] = {0x1b873593U, 0xcc9e2d51U, 0xe6546b64U};
            for (int c = 0; c < 3; ++c)
                g.colour[c].build(originX, originY, W, H, g.invCell * 0.8f, 5.17f, 11.3f, g.seed ^ salts[c], threads);
        }
    }

    // Stage 2: glow, render, look, grain.
    parallelFor(H, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* drow = dst.data + std::ptrdiff_t(y) * dst.rowStride;
            for (int x = 0; x < W; ++x) {
                float* dp = drow + size_t(x) * dst.channels;
                float lin[3] = {dp[0], dp[1], dp[2]};
                if (doHal) {
                    float h = 0.0f;
                    sampleBilinear(glow.halation, float(x), float(y), &h);
                    h *= halAmt;
                    for (int c = 0; c < 3; ++c) lin[c] += h * halTint[c];
                }
                if (doBloom) {
                    float bl[3];
                    sampleBilinear(glow.bloom, float(x), float(y), bl);
                    for (int c = 0; c < 3; ++c) lin[c] += bloomAmt * bl[c];
                }
                if (doHal || doBloom)
                    for (int c = 0; c < 3; ++c) lin[c] = sanitize(lin[c]);
                float out[3];
                stageRender(p, lin, out);
                if (doGrain) {
                    const int gx = x + originX, gy = y + originY;
                    const float Y = dot3(kY709, out);
                    const float amp = g.amp * grainWeight(g, Y);
                    const float m = grainMono(g, gx, gy);
                    if (g.chroma <= 0.0f) {
                        for (int c = 0; c < 3; ++c) out[c] += amp * m;
                    } else {
                        for (int c = 0; c < 3; ++c) {
                            const float n = ((1.0f - g.chroma) * m + g.chroma * g.colour[c].sample(gx, gy) * g.normColor) * g.chromaNorm;
                            out[c] += amp * n;
                        }
                    }
                }
                for (int c = 0; c < 3; ++c) dp[c] = clampf(out[c], 0.0f, 1.0f);
            }
        }
    });
}

}  // namespace emu
