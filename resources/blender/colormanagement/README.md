# Blender AgX viewport transform

`agx_srgb_default_65.bin` is a 65×65×65 RGB float lookup table baked from
Blender 5.2.2's `config.ocio` with the OpenColorIO processor
`Linear Rec.709 → sRGB / AgX`. It covers scene-linear RGB values from 0 to 2
and stores sRGB display values in little-endian RGB float order, with red
varying fastest. The viewport samples it trilinearly on both CPU and GPU.

Regenerate it with Blender's bundled PyOpenColorIO module:

```sh
python generate_agx_lut.py /path/to/blender/datafiles/colormanagement/config.ocio agx_srgb_default_65.bin
```

The transform follows Blender's OpenColorIO configuration, based on AgX by
Troy Sobotka and further developed by Zijun Eary Zhou, Mark Faderbauer, and
Sakari Kapanen. The OpenColorIO configuration's BSD license notice is in
`LICENSE.txt`.
