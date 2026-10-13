# stb

`stb_image.h` — a single-header image decoder.

- Version: 2.27
- License: public domain / MIT (see the header of `stb_image.h`)
- Source: https://github.com/nothings/stb

It is used by the `resources` module to decode PNG/JPEG/BMP/TGA/GIF files into
CPU pixel data. The GPU upload is a separate step (`Texture2DLoader`), so the
decoded image can be shared between CPU and GPU consumers of the same file.

Do not edit the header. To update it, replace the file and record the new version
here and in `../README.md`.
