# Blender Workbench lighting assets

The `.sl` Studio presets are copied from Blender 5.2.2's
`release/datafiles/studiolights/studio` directory. The included MatCaps are
converted from Blender's bundled multipart EXR files into separate 8-bit sRGB
diffuse and specular PNG layers. Blender marks those MatCaps as CC0 or public
domain in `matcap/LICENSE.txt`.

Source: https://github.com/blender/blender/tree/v5.2.2/release/datafiles/studiolights

The app decodes the PNG layers back to linear values and combines them with the
same diffuse-plus-specular expression and normal-to-UV mapping used by
Workbench. The original EXR images are not required at runtime.
