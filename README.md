# Emulsion — film emulation for DaVinci Resolve

An OpenFX plugin for DaVinci Resolve (free or Studio) in the spirit of Dehancer:
camera log in, film out. One node gives you:

- **Camera input**: 15 input profiles (Sony S-Log3, Fujifilm F-Log / F-Log2 / F-Log2 C,
  Apple Log, Panasonic V-Log, Canon Log 3, ARRI LogC3 / LogC4, Blackmagic Gen 5,
  Nikon N-Log, DJI D-Log, DaVinci Intermediate, Rec.709)
- **Film looks**: Cinematic Neg (the Fujifilm Classic Negative recipe), Tungsten 500 Print,
  Portrait 400, Slide 100, Sunny 200 Faded, Mono 400, and Neutral
- **Halation**: red-orange glow around bright highlights
- **Bloom**: soft glow that lifts the brightest areas
- **Grain**: animated film grain that scales with resolution
- Exposure, look strength, contrast and saturation controls

The looks are original film-inspired designs, not copies of any manufacturer's film stocks
or of Dehancer's profiles. Emulsion is not affiliated with Dehancer, Fujifilm or Blackmagic Design.

---

## Install (macOS, Apple Silicon or Intel)

You need macOS 11 or later and DaVinci Resolve 18 or later (free or Studio).

1. **Unzip** this folder somewhere, for example in Downloads.
2. **Open Terminal** (Applications > Utilities > Terminal) and type `cd ` (with a space),
   drag the unzipped `Emulsion` folder onto the Terminal window, and press Return.
3. **Build** (about a minute):

   ```
   bash build_mac.sh
   ```

   If it says the Command Line Tools are missing, run `xcode-select --install`,
   wait for that to finish, then run `bash build_mac.sh` again.
4. **Install** (asks for your Mac login password):

   ```
   bash install_mac.sh
   ```

5. **Quit and reopen DaVinci Resolve.**

The plugin is built on your own Mac, so macOS doesn't flag it as downloaded software.

To remove it later: `bash uninstall_mac.sh`

---

## Use it in Resolve

1. Go to the **Color** page and open **Effects** (top right). Under **OpenFX**, find
   **Emulsion > Emulsion Film**.
2. Drag it onto a **node at the end of your node tree**. Do exposure and white-balance fixes
   on nodes *before* it.
3. Set **Input** to your camera's log format, for example *Sony S-Log3 / S-Gamut3.Cine*.
   - Normal (non-log) footage or phone video: choose **Rec.709 (Gamma 2.4)**.
   - Already converting with a Color Space Transform to DaVinci Wide Gamut / Intermediate?
     Choose **DaVinci Intermediate / DWG**.
4. Pick a **Film Look**, then set **Exposure** so skin and mid-tones sit where you want them.
5. Adjust **Halation**, **Bloom** and **Grain** to taste.

**Project settings:** use the default *DaVinci YRGB* colour science with timeline colour space
*Rec.709 Gamma 2.4*. Emulsion outputs a finished Rec.709 image, so don't add a LUT or
another output transform after it.

**Playback speed:** Emulsion renders on the CPU. On Apple Silicon expect smooth-ish HD and
slower UHD playback. For real-time playback use Render Cache (Playback > Render Cache > Smart)
or proxies, or set Grain Amount to 0 while you grade and turn it back on before export.

---

## Controls

| Group | Control | What it does |
|---|---|---|
| Input | Input | The footage's camera log format (or Rec.709 for normal footage) |
| | Exposure | Exposure in stops, applied before the film |
| Film | Film Look | The film stock look |
| | Look Strength | 0–100 % blend of the look over the neutral film print |
| | Contrast | Film contrast around mid grey (1.0 = normal) |
| | Saturation | Colour saturation after the look (1.0 = normal) |
| Halation | Amount | Strength of the red-orange halo around highlights |
| | Size | How far the halo spreads |
| | Threshold | How bright (stops above mid grey) a highlight must be to halate |
| | Warmth | 0 = deep red, 1 = orange |
| Bloom | Amount / Size / Threshold | Soft glow around the brightest areas |
| Grain | Amount | Grain strength |
| | Size | Grain size (consistent across HD, UHD and proxies) |
| | Softness | 0 = crisp, 1 = soft |
| | Colour | 0 = monochrome grain, 1 = colour dye grain |
| | Animate / Seed | New grain each frame, or a still pattern; Seed changes the pattern |

## The looks

| Look | Character |
|---|---|
| Cinematic Neg | The Fujifilm Classic Negative recipe: muted teal-cyan skies, cream highlights, olive greens, dense reds |
| Tungsten 500 Print | Cinema print: deep blacks, teal shadows, warm amber highlights, rich skin |
| Portrait 400 | Soft, low contrast, warm skin, pastel greens and skies |
| Slide 100 | Vivid and contrasty: saturated colour, deep blue skies, clean whites |
| Sunny 200 Faded | Warm consumer negative: faded blacks, golden mids, punchy yellows and reds |
| Mono 400 | Contrasty black and white (grain turns monochrome automatically) |
| Neutral (no look) | Just the neutral film print render, a clean base for your own grade |

---

## Troubleshooting

- **Not in the OpenFX list:** make sure `bash install_mac.sh` finished, then fully quit and reopen Resolve.
- **Image looks flat and grey:** Input is set to Rec.709 on log footage. Pick your camera's log format.
- **Image looks too contrasty or clipped:** Input is set to a log format on normal footage. Choose Rec.709.
- **Updated the plugin but Resolve shows old controls:** quit Resolve, delete Resolve's OFX plugin
  cache file (`OFXPluginCacheV2.xml` in Resolve's support folder), and reopen.

## For developers

- `src/emulsion_core.cpp`: the film engine (portable C++17, no dependencies)
- `src/EmulsionPlugin.cpp`: the OpenFX wrapper (OpenFX C++ Support library)
- `src/generated/emulsion_data.h`: look LUTs, print curve and camera matrices, generated by
  `tools/gen_cpp_data.py` (the looks are defined in `tools/looks.py`)
- `tests/test_core.cpp`: checks every camera curve against colour-science, the full colour
  pipeline against the Python reference, and edge cases.
  Build with `g++ -std=c++17 -O2 -pthread -Isrc src/emulsion_core.cpp tests/test_core.cpp -o test_core`
- `Makefile.linux`: Linux build of the plugin

Licence: BSD-3-Clause (see `LICENSE`). Third-party notices are in `THIRD_PARTY_NOTICES.md`.
