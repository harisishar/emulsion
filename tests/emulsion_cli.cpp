// Direct engine render (no OFX), same parameter names/semantics as the plugin.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../src/emulsion_core.h"
int main(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: in.f32 W H out.f32 [--time T] [k=v...]\n"); return 2; }
    const int w = std::atoi(argv[2]), h = std::atoi(argv[3]);
    std::vector<float> src(size_t(w) * h * 4), dst(src.size());
    FILE* f = std::fopen(argv[1], "rb");
    if (!f || std::fread(src.data(), 4, src.size(), f) != src.size()) return 3;
    std::fclose(f);
    emu::Settings s;
    s.strength = 1.0;
    for (int i = 5; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--time" && i + 1 < argc) { s.frame = std::atof(argv[++i]); continue; }
        const size_t eq = a.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = a.substr(0, eq);
        const double v = std::atof(a.substr(eq + 1).c_str());
        if (k == "input") s.input = int(v);
        else if (k == "exposure") s.exposure = v;
        else if (k == "look") s.look = int(v);
        else if (k == "strength") s.strength = v / 100.0;
        else if (k == "contrast") s.contrast = v;
        else if (k == "saturation") s.saturation = v;
        else if (k == "halationAmount") s.halationAmount = v;
        else if (k == "halationSize") s.halationSize = v;
        else if (k == "halationThreshold") s.halationThreshold = v;
        else if (k == "halationWarmth") s.halationWarmth = v;
        else if (k == "bloomAmount") s.bloomAmount = v;
        else if (k == "bloomSize") s.bloomSize = v;
        else if (k == "bloomThreshold") s.bloomThreshold = v;
        else if (k == "grainAmount") s.grainAmount = v;
        else if (k == "grainSize") s.grainSize = v;
        else if (k == "grainSoftness") s.grainSoftness = v;
        else if (k == "grainColor") s.grainColor = v;
        else if (k == "grainAnimate") s.grainAnimate = v != 0;
        else if (k == "grainSeed") s.grainSeed = int(v);
        else { std::fprintf(stderr, "unknown param %s\n", k.c_str()); return 4; }
    }
    emu::process({src.data(), w, h, w * 4, 4}, {dst.data(), w, h, w * 4, 4}, s);
    f = std::fopen(argv[4], "wb");
    std::fwrite(dst.data(), 4, dst.size(), f);
    std::fclose(f);
    return 0;
}
