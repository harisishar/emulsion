// Tests for the Emulsion engine against the Python / colour-science reference.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>
#include <string>
#include <vector>

#include "../src/emulsion_core.h"

static int g_fail = 0;
#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            std::printf("FAIL: " __VA_ARGS__);   \
            std::printf("\n");                   \
            ++g_fail;                            \
        }                                        \
    } while (0)

static std::string refDir() {
    const char* d = std::getenv("EMU_REF");
    return d ? d : "tests/ref";
}

static void testDecode() {
    std::ifstream f(refDir() + "/decode_vectors.txt");
    CHECK(f.good(), "missing decode_vectors.txt");
    int prof;
    double code, expect;
    int n = 0;
    double worst = 0;
    while (f >> prof >> code >> expect) {
        const double got = emu::decodeValue(prof, float(code));
        const double err = std::fabs(got - expect) / std::max(1.0, std::fabs(expect));
        worst = std::max(worst, err);
        if (err > 2e-4) {
            std::printf("  decode profile %d (%s) code %.6f: got %.8g expected %.8g\n", prof, emu::inputName(prof), code, got, expect);
            ++g_fail;
        }
        ++n;
    }
    std::printf("decode: %d vectors, worst relative error %.2e\n", n, worst);
}

static void testPixels() {
    std::ifstream f(refDir() + "/pixel_vectors.txt");
    CHECK(f.good(), "missing pixel_vectors.txt");
    std::string line;
    int n = 0, bad = 0;
    double worst = 0, sum = 0;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        double v[12];
        for (double& x : v) ss >> x;
        emu::Settings s;
        s.input = int(v[0]);
        s.look = int(v[1]);
        s.exposure = v[2];
        s.contrast = v[3];
        s.saturation = v[4];
        s.strength = v[5];
        const float in[3] = {float(v[6]), float(v[7]), float(v[8])};
        float out[3];
        emu::processPixelNoEffects(in, out, s);
        for (int c = 0; c < 3; ++c) {
            const double e = std::fabs(out[c] - v[9 + c]);
            worst = std::max(worst, e);
            sum += e;
            if (e > 2e-3) {
                if (bad < 10)
                    std::printf("  pixel input=%d look=%d exp=%g con=%g sat=%g str=%g in=(%g %g %g) ch%d got %.5f exp %.5f\n",
                                s.input, s.look, s.exposure, s.contrast, s.saturation, s.strength, in[0], in[1], in[2], c,
                                out[c], v[9 + c]);
                ++bad;
            }
        }
        ++n;
    }
    std::printf("pixels: %d vectors, mean abs err %.2e, worst %.2e, >2e-3: %d\n", n, sum / (3.0 * n), worst, bad);
    if (bad) ++g_fail;
}

static std::vector<float> randomImage(int w, int h, int ch, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    std::vector<float> img(size_t(w) * h * ch);
    for (auto& v : img) v = U(rng);
    if (ch == 4)
        for (size_t i = 3; i < img.size(); i += 4) img[i] = 0.5f;
    return img;
}

static void testImageNoEffectsMatchesPixel() {
    const int w = 37, h = 23;
    auto src = randomImage(w, h, 4, 3);
    std::vector<float> dst(src.size());
    emu::Settings s;
    s.input = emu::kInputSLog3SGamut3Cine;
    s.look = 1;
    s.halationAmount = s.bloomAmount = s.grainAmount = 0;
    emu::ImageRef a{src.data(), w, h, w * 4, 4}, b{dst.data(), w, h, w * 4, 4};
    emu::process(a, b, s);
    double worst = 0;
    for (int i = 0; i < w * h; ++i) {
        float out[3];
        emu::processPixelNoEffects(&src[size_t(i) * 4], out, s);
        for (int c = 0; c < 3; ++c) worst = std::max(worst, double(std::fabs(out[c] - dst[size_t(i) * 4 + c])));
        CHECK(dst[size_t(i) * 4 + 3] == 0.5f, "alpha not preserved");
    }
    std::printf("image vs pixel path (effects off): worst %.2e\n", worst);
    CHECK(worst < 1e-6, "image path differs from pixel path");
}

static void testInPlaceAndStride() {
    const int w = 64, h = 48;
    auto src = randomImage(w, h, 4, 5);
    emu::Settings s;
    s.input = emu::kInputFLog2;
    s.look = 0;
    s.halationAmount = 0.6;
    s.bloomAmount = 0.4;
    s.grainAmount = 0.3;
    s.frame = 12;
    // reference: separate buffers, top-down
    std::vector<float> ref(src.size());
    emu::process({src.data(), w, h, w * 4, 4}, {ref.data(), w, h, w * 4, 4}, s);
    // in place
    std::vector<float> ip = src;
    emu::process({ip.data(), w, h, w * 4, 4}, {ip.data(), w, h, w * 4, 4}, s);
    double d1 = 0;
    for (size_t i = 0; i < ref.size(); ++i) d1 = std::max(d1, double(std::fabs(ref[i] - ip[i])));
    CHECK(d1 == 0.0, "in-place result differs (%g)", d1);
    // bottom-up (negative stride): same pixels, rows reversed in memory
    std::vector<float> flipped(src.size()), outFlip(src.size());
    for (int y = 0; y < h; ++y)
        std::copy(src.begin() + size_t(y) * w * 4, src.begin() + size_t(y + 1) * w * 4, flipped.begin() + size_t(h - 1 - y) * w * 4);
    emu::ImageRef fs{flipped.data() + size_t(h - 1) * w * 4, w, h, -std::ptrdiff_t(w * 4), 4};
    emu::ImageRef fd{outFlip.data() + size_t(h - 1) * w * 4, w, h, -std::ptrdiff_t(w * 4), 4};
    emu::process(fs, fd, s);
    double d2 = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w * 4; ++x)
            d2 = std::max(d2, double(std::fabs(ref[size_t(y) * w * 4 + x] - outFlip[size_t(h - 1 - y) * w * 4 + x])));
    CHECK(d2 == 0.0, "negative stride result differs (%g)", d2);
    std::printf("in-place diff %g, negative-stride diff %g\n", d1, d2);
}

static void testEdgeCases() {
    emu::Settings s;
    s.input = emu::kInputLogC4;
    s.halationAmount = 1.0;
    s.bloomAmount = 1.0;
    s.grainAmount = 1.0;
    const int sizes[][2] = {{1, 1}, {2, 1}, {1, 2}, {3, 7}, {401, 3}, {5, 333}};
    for (auto& sz : sizes) {
        auto src = randomImage(sz[0], sz[1], 4, 11);
        std::vector<float> dst(src.size());
        emu::process({src.data(), sz[0], sz[1], sz[0] * 4, 4}, {dst.data(), sz[0], sz[1], sz[0] * 4, 4}, s);
        bool finite = true;
        for (float v : dst) finite &= std::isfinite(v);
        CHECK(finite, "non-finite output at %dx%d", sz[0], sz[1]);
    }
    // NaN / Inf / huge inputs
    std::vector<float> src = {NAN, INFINITY, -INFINITY, 1.0f, 1e30f, -1e30f, 0.5f, 1.0f, 2.0f, -2.0f, 100.0f, 1.0f};
    std::vector<float> dst(src.size());
    for (int in = 0; in < emu::inputCount(); ++in) {
        s.input = in;
        emu::process({src.data(), 3, 1, 12, 4}, {dst.data(), 3, 1, 12, 4}, s);
        for (int i = 0; i < 12; ++i) {
            if (i % 4 == 3) continue;
            CHECK(std::isfinite(dst[i]) && dst[i] >= 0.0f && dst[i] <= 1.0f, "bad output %g for input profile %d", dst[i], in);
        }
    }
    // RGB (3 channel) buffers
    auto src3 = randomImage(16, 9, 3, 2);
    std::vector<float> dst3(src3.size());
    emu::process({src3.data(), 16, 9, 48, 3}, {dst3.data(), 16, 9, 48, 3}, s);
    std::printf("edge cases done\n");
}

static void testGrain() {
    const int w = 512, h = 512;
    std::vector<float> src(size_t(w) * h * 4, 0.45f), dst(src.size()), dst2(src.size());
    emu::Settings s;
    s.input = emu::kInputRec709;
    s.look = 6;  // neutral
    s.halationAmount = s.bloomAmount = 0;
    s.grainAmount = 1.0;
    s.grainColor = 0.0;
    s.frame = 3;
    emu::process({src.data(), w, h, w * 4, 4}, {dst.data(), w, h, w * 4, 4}, s);
    float base[3];
    emu::processPixelNoEffects(&src[0], base, s);
    double mean = 0, var = 0;
    for (int i = 0; i < w * h; ++i) mean += dst[size_t(i) * 4] - base[0];
    mean /= w * h;
    for (int i = 0; i < w * h; ++i) {
        const double d = dst[size_t(i) * 4] - base[0] - mean;
        var += d * d;
    }
    const double sd = std::sqrt(var / (w * h));
    std::printf("grain at amount 1: mean %.4f sd %.4f (target sd ~%.4f)\n", mean, sd, 0.075 * 0.3 + 0.075 * 0.7 * std::pow(4 * base[0] * (1 - base[0]), 0.75));
    CHECK(std::fabs(mean) < 0.004, "grain mean not ~0");
    CHECK(sd > 0.04 && sd < 0.1, "grain sd out of range");
    // mono grain: channels identical
    for (int i = 0; i < 1000; ++i) {
        const float d0 = dst[size_t(i) * 4] - base[0], d1 = dst[size_t(i) * 4 + 1] - base[1], d2 = dst[size_t(i) * 4 + 2] - base[2];
        if (std::fabs(d0 - d1) > 1e-5f || std::fabs(d1 - d2) > 1e-5f) { CHECK(false, "mono grain differs between channels"); break; }
    }
    // animated: different frame -> different pattern; same frame -> identical
    s.frame = 4;
    emu::process({src.data(), w, h, w * 4, 4}, {dst2.data(), w, h, w * 4, 4}, s);
    int same = 0;
    for (int i = 0; i < w * h; ++i) same += dst2[size_t(i) * 4] == dst[size_t(i) * 4];
    CHECK(same < w * h / 50, "grain did not change between frames");
    s.grainAnimate = false;
    s.frame = 3;
    emu::process({src.data(), w, h, w * 4, 4}, {dst.data(), w, h, w * 4, 4}, s);
    s.frame = 99;
    emu::process({src.data(), w, h, w * 4, 4}, {dst2.data(), w, h, w * 4, 4}, s);
    CHECK(dst == dst2, "static grain changed between frames");
}

static void testSpeed() {
    for (auto sz : {std::pair<int, int>{1920, 1080}, std::pair<int, int>{3840, 2160}}) {
        auto src = randomImage(sz.first, sz.second, 4, 9);
        std::vector<float> dst(src.size());
        emu::Settings s;
        s.input = emu::kInputSLog3SGamut3Cine;
        auto t0 = std::chrono::steady_clock::now();
        emu::process({src.data(), sz.first, sz.second, sz.first * 4, 4}, {dst.data(), sz.first, sz.second, sz.first * 4, 4}, s);
        auto t1 = std::chrono::steady_clock::now();
        std::printf("speed %dx%d (all effects on): %.1f ms, threads=%u\n", sz.first, sz.second,
                    std::chrono::duration<double, std::milli>(t1 - t0).count(), std::thread::hardware_concurrency());
    }
}

int main() {
    testDecode();
    testPixels();
    testImageNoEffectsMatchesPixel();
    testInPlaceAndStride();
    testEdgeCases();
    testGrain();
    testSpeed();
    std::printf(g_fail ? "\n%d FAILURES\n" : "\nALL TESTS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
