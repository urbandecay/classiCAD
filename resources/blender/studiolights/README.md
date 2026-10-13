# Blender Workbench lighting assets

The `.sl` Studio presets are copied from Blender 5.2.2's
`release/datafiles/studiolights/studio` directory. All 27 bundled MatCaps from
Blender 5.2.2's `release/datafiles/studiolights/matcap` directory are included.
Their multipart EXR layers are converted into separate 8-bit sRGB diffuse and
specular PNGs. The RGB-only `check_normal+y` preset is stored as the diffuse
layer with a black specular layer. Blender marks those MatCaps as CC0 or public
domain in `matcap/LICENSE.txt`.

Each MatCap also has a `.qmc` payload containing Blender 5.2.2's source EXR
diffuse and specular RGB channels as little-endian half-floats. Its 12-byte
header is `CADMC01\0`, followed by little-endian 16-bit width and height. The
remaining bytes use Qt `qCompress` framing: a big-endian 32-bit uncompressed
byte count and zlib data. Decompressed pixels are top-down RGB half-floats,
with the complete diffuse layer followed by the complete specular layer. The
GPU upload flips each layer vertically for GL; the QPainter sampler uses the
top-down data directly. `check_normal+y` is the one unlayered RGB source; its
specular layer is zero-filled. This preserves the source EXR values where
8-bit PNG quantization caused a visible MatCap mismatch.

Regenerate and validate all 27 payloads with Blender 5.2.2's bundled Python
and OpenImageIO (the default path is the installed snap location):

```sh
/snap/blender/7803/5.2/python/bin/python3.13 \
    resources/blender/studiolights/matcap/generate_qmc.py
```

Use `--blender-data PATH` and `--output PATH` to select another Blender 5.2.2
data directory or an output directory for review. The script verifies the
expected preset set, 512x512 dimensions, Qt-compatible compression, and all
27 output names.

Source: https://github.com/blender/blender/tree/v5.2.2/release/datafiles/studiolights

The app decodes the PNG layers back to linear values and combines them with the
same diffuse-plus-specular expression and normal-to-UV mapping used by
Workbench. The original EXR images are not required at runtime.
