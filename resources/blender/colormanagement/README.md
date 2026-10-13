# Blender viewport display transforms

`agx_srgb_default_97.bin` is a 97×97×97 RGB float lookup table baked from
Blender 5.2.2's `config.ocio` with the OpenColorIO processor
`Linear Rec.709 → sRGB / AgX`. Its input grid uses a `log2(1 + 64x)` shaper
over scene-linear RGB values from 0 to 16. The expanded range keeps common
exposure-driven HDR values inside the transform domain; the larger 97³ table
preserves near-black sampling resolution while keeping the payload near 11 MB.
Values above 16 still clamp at the upper LUT boundary. The table stores sRGB
display values in little-endian RGB float order, with red varying fastest.
The viewport applies the same shaper before trilinear sampling on CPU and GPU.

`khronos_pbr_neutral_srgb_65.bin` is baked from Blender 5.2.2's
`Khronos PBR Neutral` view on the sRGB display using the same LUT layout.
`filmic_srgb_65.bin` is baked from Blender 5.2.2's `Filmic` view on sRGB.
`Standard` remains an analytic sRGB transfer curve in the renderer.

Regenerate and verify the expanded AgX table with the pinned local Blender
5.2.2 installation (run from the repository root):

```sh
blender --background --python-expr "import sys; sys.path.insert(0, 'resources/blender/colormanagement'); import generate_agx_lut; sys.argv = ['generate_agx_lut.py', '/snap/blender/7803/5.2/datafiles/colormanagement/config.ocio', 'resources/blender/colormanagement/agx_srgb_default_97.bin']; generate_agx_lut.main()"
blender --background --python tests/verify_agx_lut.py
```

The verifier samples the table trilinearly and compares scene-linear values
including values above 2 against Blender's bundled OCIO AgX processor. It
requires a maximum absolute RGB error of 0.001 for its listed samples.

The generator also accepts `--display` and `--view`; for example:

```sh
python generate_agx_lut.py /path/to/blender/datafiles/colormanagement/config.ocio khronos_pbr_neutral_srgb_65.bin --display sRGB --view "Khronos PBR Neutral"
python generate_agx_lut.py /path/to/blender/datafiles/colormanagement/config.ocio filmic_srgb_65.bin --display sRGB --view Filmic
```

The transform follows Blender's OpenColorIO configuration, based on AgX by
Troy Sobotka and further developed by Zijun Eary Zhou, Mark Faderbauer, and
Sakari Kapanen. The OpenColorIO configuration's BSD license notice is in
`LICENSE.txt`.
