// Emulsion - film emulation OFX plugin for DaVinci Resolve (and other OFX hosts).
// Copyright (c) 2026. Released under the BSD-3-Clause licence (see LICENSE).
//
// Built on the OpenFX C++ Support library (BSD-3-Clause, OpenFX contributors).

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"

#include "emulsion_core.h"

#define kPluginName "Emulsion Film"
#define kPluginGrouping "Emulsion"
#define kPluginDescription                                                                                   \
    "Film emulation: camera log input, film looks (Cinematic Neg and original film-inspired stocks), "     \
    "halation, bloom and animated grain. Put it on a node, set Input to your camera's log format (or "     \
    "Rec.709 for normal footage), then pick a Film Look."
#define kPluginIdentifier "com.emulsionfilm.Emulsion"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0

namespace {

const char* const kParamInput = "input";
const char* const kParamExposure = "exposure";
const char* const kParamLook = "look";
const char* const kParamStrength = "strength";
const char* const kParamContrast = "contrast";
const char* const kParamSaturation = "saturation";
const char* const kParamHalAmount = "halationAmount";
const char* const kParamHalSize = "halationSize";
const char* const kParamHalThreshold = "halationThreshold";
const char* const kParamHalWarmth = "halationWarmth";
const char* const kParamBloomAmount = "bloomAmount";
const char* const kParamBloomSize = "bloomSize";
const char* const kParamBloomThreshold = "bloomThreshold";
const char* const kParamGrainAmount = "grainAmount";
const char* const kParamGrainSize = "grainSize";
const char* const kParamGrainSoftness = "grainSoftness";
const char* const kParamGrainColor = "grainColor";
const char* const kParamGrainAnimate = "grainAnimate";
const char* const kParamGrainSeed = "grainSeed";

class EmulsionPlugin : public OFX::ImageEffect {
public:
    explicit EmulsionPlugin(OfxImageEffectHandle handle) : OFX::ImageEffect(handle) {
        dstClip_ = fetchClip(kOfxImageEffectOutputClipName);
        srcClip_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
        input_ = fetchChoiceParam(kParamInput);
        exposure_ = fetchDoubleParam(kParamExposure);
        look_ = fetchChoiceParam(kParamLook);
        strength_ = fetchDoubleParam(kParamStrength);
        contrast_ = fetchDoubleParam(kParamContrast);
        saturation_ = fetchDoubleParam(kParamSaturation);
        halAmount_ = fetchDoubleParam(kParamHalAmount);
        halSize_ = fetchDoubleParam(kParamHalSize);
        halThreshold_ = fetchDoubleParam(kParamHalThreshold);
        halWarmth_ = fetchDoubleParam(kParamHalWarmth);
        bloomAmount_ = fetchDoubleParam(kParamBloomAmount);
        bloomSize_ = fetchDoubleParam(kParamBloomSize);
        bloomThreshold_ = fetchDoubleParam(kParamBloomThreshold);
        grainAmount_ = fetchDoubleParam(kParamGrainAmount);
        grainSize_ = fetchDoubleParam(kParamGrainSize);
        grainSoftness_ = fetchDoubleParam(kParamGrainSoftness);
        grainColor_ = fetchDoubleParam(kParamGrainColor);
        grainAnimate_ = fetchBooleanParam(kParamGrainAnimate);
        grainSeed_ = fetchIntParam(kParamGrainSeed);
    }

    void render(const OFX::RenderArguments& args) override {
        std::unique_ptr<OFX::Image> dst(dstClip_->fetchImage(args.time));
        if (!dst.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
        if (dst->getPixelDepth() != OFX::eBitDepthFloat) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        const OFX::PixelComponentEnum dstComp = dst->getPixelComponents();
        if (dstComp != OFX::ePixelComponentRGBA && dstComp != OFX::ePixelComponentRGB)
            OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        const int dstCh = dstComp == OFX::ePixelComponentRGBA ? 4 : 3;

        std::unique_ptr<OFX::Image> src(srcClip_ && srcClip_->isConnected() ? srcClip_->fetchImage(args.time) : nullptr);
        int srcCh = 4;
        if (src.get()) {
            if (src->getPixelDepth() != OFX::eBitDepthFloat) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
            const OFX::PixelComponentEnum sc = src->getPixelComponents();
            if (sc != OFX::ePixelComponentRGBA && sc != OFX::ePixelComponentRGB)
                OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
            srcCh = sc == OFX::ePixelComponentRGBA ? 4 : 3;
        }

        // Render window, clipped to the destination bounds.
        const OfxRectI db = dst->getBounds();
        OfxRectI rw = args.renderWindow;
        rw.x1 = std::max(rw.x1, db.x1);
        rw.y1 = std::max(rw.y1, db.y1);
        rw.x2 = std::min(rw.x2, db.x2);
        rw.y2 = std::min(rw.y2, db.y2);
        const int W = rw.x2 - rw.x1, H = rw.y2 - rw.y1;
        if (W <= 0 || H <= 0) return;

        emu::ImageRef dref;
        dref.data = static_cast<float*>(dst->getPixelAddress(rw.x1, rw.y1));
        dref.width = W;
        dref.height = H;
        dref.rowStride = std::ptrdiff_t(dst->getRowBytes()) / std::ptrdiff_t(sizeof(float));
        dref.channels = dstCh;
        if (!dref.data) OFX::throwSuiteStatusException(kOfxStatFailed);

        // Source view: direct when it covers the window, otherwise a zero-padded copy.
        std::vector<float> padded;
        emu::ImageRef sref;
        sref.width = W;
        sref.height = H;
        sref.channels = srcCh;
        bool direct = false;
        if (src.get()) {
            const OfxRectI sb = src->getBounds();
            direct = sb.x1 <= rw.x1 && sb.y1 <= rw.y1 && sb.x2 >= rw.x2 && sb.y2 >= rw.y2;
            if (direct) {
                sref.data = static_cast<float*>(src->getPixelAddress(rw.x1, rw.y1));
                sref.rowStride = std::ptrdiff_t(src->getRowBytes()) / std::ptrdiff_t(sizeof(float));
                direct = sref.data != nullptr;
            }
        }
        if (!direct) {
            padded.assign(size_t(W) * size_t(H) * size_t(srcCh), 0.0f);
            if (srcCh == 4)
                for (size_t i = 3; i < padded.size(); i += 4) padded[i] = 1.0f;
            if (src.get()) {
                const OfxRectI sb = src->getBounds();
                const int x1 = std::max(rw.x1, sb.x1), x2 = std::min(rw.x2, sb.x2);
                const int y1 = std::max(rw.y1, sb.y1), y2 = std::min(rw.y2, sb.y2);
                for (int y = y1; y < y2 && x1 < x2; ++y) {
                    const float* sp = static_cast<const float*>(src->getPixelAddress(x1, y));
                    if (!sp) continue;
                    float* pp = padded.data() + (size_t(y - rw.y1) * W + size_t(x1 - rw.x1)) * srcCh;
                    std::memcpy(pp, sp, size_t(x2 - x1) * size_t(srcCh) * sizeof(float));
                }
            }
            sref.data = padded.data();
            sref.rowStride = std::ptrdiff_t(W) * srcCh;
        }

        emu::Settings s;
        const double t = args.time;
        int iv = 0;
        input_->getValueAtTime(t, iv);
        s.input = iv;
        s.exposure = exposure_->getValueAtTime(t);
        look_->getValueAtTime(t, iv);
        s.look = iv;
        s.strength = strength_->getValueAtTime(t) / 100.0;
        s.contrast = contrast_->getValueAtTime(t);
        s.saturation = saturation_->getValueAtTime(t);
        s.halationAmount = halAmount_->getValueAtTime(t);
        s.halationSize = halSize_->getValueAtTime(t);
        s.halationThreshold = halThreshold_->getValueAtTime(t);
        s.halationWarmth = halWarmth_->getValueAtTime(t);
        s.bloomAmount = bloomAmount_->getValueAtTime(t);
        s.bloomSize = bloomSize_->getValueAtTime(t);
        s.bloomThreshold = bloomThreshold_->getValueAtTime(t);
        s.grainAmount = grainAmount_->getValueAtTime(t);
        s.grainSize = grainSize_->getValueAtTime(t);
        s.grainSoftness = grainSoftness_->getValueAtTime(t);
        s.grainColor = grainColor_->getValueAtTime(t);
        bool anim = true;
        grainAnimate_->getValueAtTime(t, anim);
        s.grainAnimate = anim;
        grainSeed_->getValueAtTime(t, iv);
        s.grainSeed = iv;
        s.frame = t;

        // Pixel (0,0) of the window sits at (rw.x1, rw.y1) in the frame, which keeps grain stable.
        emu::process(sref, dref, s, rw.x1, rw.y1);
    }

private:
    OFX::Clip* dstClip_ = nullptr;
    OFX::Clip* srcClip_ = nullptr;
    OFX::ChoiceParam* input_ = nullptr;
    OFX::DoubleParam* exposure_ = nullptr;
    OFX::ChoiceParam* look_ = nullptr;
    OFX::DoubleParam* strength_ = nullptr;
    OFX::DoubleParam* contrast_ = nullptr;
    OFX::DoubleParam* saturation_ = nullptr;
    OFX::DoubleParam* halAmount_ = nullptr;
    OFX::DoubleParam* halSize_ = nullptr;
    OFX::DoubleParam* halThreshold_ = nullptr;
    OFX::DoubleParam* halWarmth_ = nullptr;
    OFX::DoubleParam* bloomAmount_ = nullptr;
    OFX::DoubleParam* bloomSize_ = nullptr;
    OFX::DoubleParam* bloomThreshold_ = nullptr;
    OFX::DoubleParam* grainAmount_ = nullptr;
    OFX::DoubleParam* grainSize_ = nullptr;
    OFX::DoubleParam* grainSoftness_ = nullptr;
    OFX::DoubleParam* grainColor_ = nullptr;
    OFX::BooleanParam* grainAnimate_ = nullptr;
    OFX::IntParam* grainSeed_ = nullptr;
};

OFX::GroupParamDescriptor* defineGroup(OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, const char* name,
                                       const char* label, bool open) {
    OFX::GroupParamDescriptor* g = desc.defineGroupParam(name);
    g->setLabels(label, label, label);
    g->setOpen(open);
    if (page) page->addChild(*g);
    return g;
}

OFX::DoubleParamDescriptor* defineDouble(OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                         OFX::GroupParamDescriptor* group, const char* name, const char* label,
                                         const char* hint, double def, double mn, double mx, double dmn, double dmx,
                                         double inc) {
    OFX::DoubleParamDescriptor* p = desc.defineDoubleParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    p->setDefault(def);
    p->setRange(mn, mx);
    p->setDisplayRange(dmn, dmx);
    p->setIncrement(inc);
    p->setDigits(2);
    p->setAnimates(true);
    if (group) p->setParent(*group);
    if (page) page->addChild(*p);
    return p;
}

}  // namespace

using namespace OFX;

mDeclarePluginFactory(EmulsionPluginFactory, {}, {});

void EmulsionPluginFactory::describe(OFX::ImageEffectDescriptor& desc) {
    desc.setLabels(kPluginName, kPluginName, kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedBitDepth(eBitDepthFloat);

    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(true);
    desc.setSupportsTiles(false);  // halation and bloom need the whole frame
    desc.setTemporalClipAccess(false);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setRenderThreadSafety(eRenderFullySafe);
}

void EmulsionPluginFactory::describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum /*context*/) {
    ClipDescriptor* srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->addSupportedComponent(ePixelComponentRGB);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(false);
    srcClip->setIsMask(false);

    ClipDescriptor* dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->addSupportedComponent(ePixelComponentRGB);
    dstClip->setSupportsTiles(false);

    PageParamDescriptor* page = desc.definePageParam("Controls");

    // Input
    GroupParamDescriptor* gIn = defineGroup(desc, page, "groupInput", "Input", true);
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam(kParamInput);
        p->setLabels("Input", "Input", "Input");
        p->setHint("What the footage on this node is: your camera's log format, DaVinci Intermediate (after a "
                   "Color Space Transform to DaVinci Wide Gamut / Intermediate), or Rec.709 for normal footage.");
        for (int i = 0; i < emu::inputCount(); ++i) p->appendOption(emu::inputName(i));
        p->setDefault(emu::kInputRec709);
        p->setAnimates(false);
        p->setParent(*gIn);
        page->addChild(*p);
    }
    defineDouble(desc, page, gIn, kParamExposure, "Exposure", "Exposure in stops, applied before the film.", 0.0, -6.0,
                 6.0, -3.0, 3.0, 0.1);

    // Film
    GroupParamDescriptor* gFilm = defineGroup(desc, page, "groupFilm", "Film", true);
    {
        ChoiceParamDescriptor* p = desc.defineChoiceParam(kParamLook);
        p->setLabels("Film Look", "Film Look", "Film Look");
        p->setHint("Film look. Cinematic Neg is the Classic Negative recipe; the others are original film-inspired "
                   "looks. Neutral gives the plain film print render.");
        for (int i = 0; i < emu::lookCount(); ++i) p->appendOption(emu::lookName(i));
        p->setDefault(0);
        p->setAnimates(false);
        p->setParent(*gFilm);
        page->addChild(*p);
    }
    defineDouble(desc, page, gFilm, kParamStrength, "Look Strength", "How much of the film look to apply (%).", 100.0,
                 0.0, 100.0, 0.0, 100.0, 1.0);
    defineDouble(desc, page, gFilm, kParamContrast, "Contrast", "Film contrast around mid grey.", 1.0, 0.5, 1.5, 0.5,
                 1.5, 0.01);
    defineDouble(desc, page, gFilm, kParamSaturation, "Saturation", "Colour saturation after the look.", 1.0, 0.0, 2.0,
                 0.0, 2.0, 0.01);

    // Halation
    GroupParamDescriptor* gHal = defineGroup(desc, page, "groupHalation", "Halation", true);
    defineDouble(desc, page, gHal, kParamHalAmount, "Amount", "Red-orange glow around bright highlights.", 0.25, 0.0,
                 2.0, 0.0, 1.0, 0.01);
    defineDouble(desc, page, gHal, kParamHalSize, "Size", "How far the halation spreads.", 0.35, 0.0, 1.0, 0.0, 1.0,
                 0.01);
    defineDouble(desc, page, gHal, kParamHalThreshold, "Threshold",
                 "Stops above mid grey where highlights start to halate.", 2.5, 0.0, 8.0, 0.0, 6.0, 0.1);
    defineDouble(desc, page, gHal, kParamHalWarmth, "Warmth", "0 = deep red, 1 = orange.", 0.35, 0.0, 1.0, 0.0, 1.0,
                 0.01);

    // Bloom
    GroupParamDescriptor* gBloom = defineGroup(desc, page, "groupBloom", "Bloom", false);
    defineDouble(desc, page, gBloom, kParamBloomAmount, "Amount", "Soft glow that lifts bright highlights.", 0.15, 0.0,
                 2.0, 0.0, 1.0, 0.01);
    defineDouble(desc, page, gBloom, kParamBloomSize, "Size", "How wide the bloom spreads.", 0.5, 0.0, 1.0, 0.0, 1.0,
                 0.01);
    defineDouble(desc, page, gBloom, kParamBloomThreshold, "Threshold", "Stops above mid grey where bloom starts.", 2.0,
                 0.0, 8.0, 0.0, 6.0, 0.1);

    // Grain
    GroupParamDescriptor* gGrain = defineGroup(desc, page, "groupGrain", "Grain", true);
    defineDouble(desc, page, gGrain, kParamGrainAmount, "Amount", "Film grain strength.", 0.25, 0.0, 2.0, 0.0, 1.0,
                 0.01);
    defineDouble(desc, page, gGrain, kParamGrainSize, "Size", "Grain size (scales with resolution).", 0.35, 0.0, 1.0,
                 0.0, 1.0, 0.01);
    defineDouble(desc, page, gGrain, kParamGrainSoftness, "Softness", "0 = crisp, 1 = soft.", 0.5, 0.0, 1.0, 0.0, 1.0,
                 0.01);
    defineDouble(desc, page, gGrain, kParamGrainColor, "Colour", "0 = monochrome grain, 1 = colour dye grain.", 0.25,
                 0.0, 1.0, 0.0, 1.0, 0.01);
    {
        BooleanParamDescriptor* p = desc.defineBooleanParam(kParamGrainAnimate);
        p->setLabels("Animate", "Animate", "Animate");
        p->setHint("New grain every frame. Turn off for a still grain pattern.");
        p->setDefault(true);
        p->setAnimates(false);
        p->setParent(*gGrain);
        page->addChild(*p);
    }
    {
        IntParamDescriptor* p = desc.defineIntParam(kParamGrainSeed);
        p->setLabels("Seed", "Seed", "Seed");
        p->setHint("Change for a different grain pattern.");
        p->setDefault(0);
        p->setRange(0, 9999);
        p->setDisplayRange(0, 100);
        p->setAnimates(false);
        p->setParent(*gGrain);
        page->addChild(*p);
    }
}

OFX::ImageEffect* EmulsionPluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum /*context*/) {
    return new EmulsionPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray& ids) {
    static EmulsionPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
    ids.push_back(&p);
}
}  // namespace Plugin
}  // namespace OFX
