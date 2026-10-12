# Third-party notices

## OpenFX (third_party/openfx)
OpenFX API headers and the OpenFX C++ Support library, BSD-3-Clause.
Copyright OpenFX and contributors to the OpenFX project.
https://github.com/AcademySoftwareFoundation/openfx
See `third_party/openfx/LICENSE.md`.

## Camera log formulas
The camera log curves and gamut matrices follow the camera makers' published specifications,
as implemented in colour-science 0.4.7 (BSD-3-Clause, https://www.colour-science.org).
The plugin contains its own C++ implementation; colour-science was used to generate and verify
the constants (`tools/gen_cpp_data.py`, `tests/ref/`).

## Test images (not included in the plugin)
Preview images were rendered from the OpenEXR sample images
(https://github.com/AcademySoftwareFoundation/openexr-images).
