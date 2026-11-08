# Third-Party Notices

This project (Apache-2.0) depends on, or incorporates data derived from, the
following third-party projects. See `docs/references.md` for where each one
is used.

## LVGL

- Project: https://github.com/lvgl/lvgl
- Version: 9.5.0 (fetched via CMake FetchContent, not vendored/forked)
- License: MIT
- Adapted code: `components/wisp_port_esp/lv_blend/lv_blend_mask_rgb565.h`
  re-implements the mixing formula of `lv_color_16_16_mix()` and the
  masked solid fill of `lv_draw_sw_blend_color_to_rgb565_swapped()`
  (see `docs/references.md`)
- Derived data: `components/wisp_ui/src/pinyin_dict.cpp` is LVGL's default
  pinyin dictionary (`lv_ime_pinyin_def_dict` in
  `src/widgets/ime/lv_ime_pinyin.c`) filtered to the built-in CJK font's
  characters by `tools/gen_pinyin_dict.py` (see `docs/references.md`)

Copied verbatim from `build/sim/_deps/lvgl-src/LICENCE.txt` (fetched source
tree; not committed to this repo):

```
MIT licence
Copyright (c) 2025 LVGL Kft

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

### Libraries bundled inside LVGL

LVGL's source tree carries third-party decoders under their own licenses;
three of them are compiled here:

- **LodePNG** (`src/libs/lodepng/`, version 20230410): PNG files of
  `ui.image` (`LV_USE_LODEPNG` in `components/wisp_ui/config/lv_conf.h`),
  in the firmware and the simulator. zlib License, copied verbatim from
  the header of `src/libs/lodepng/lodepng.c`:

```
Copyright (c) 2005-2023 Lode Vandevenne

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
    claim that you wrote the original software. If you use this software
    in a product, an acknowledgment in the product documentation would be
    appreciated but is not required.

    2. Altered source versions must be plainly marked as such, and must not be
    misrepresented as being the original software.

    3. This notice may not be removed or altered from any source
    distribution.
```

- **TJpgDec** R0.03 (`src/libs/tjpgd/`): the simulator's JPEG decoder
  only (`components/wisp_port_sim/ui/image_decoders.cpp`; LV_USE_TJPGD is
  set by `cmake/wisp_host_deps.cmake` and stays off in the firmware).
  ChaN's license, copied verbatim from the header of
  `src/libs/tjpgd/tjpgd.c`:

```
/ TJpgDec - Tiny JPEG Decompressor R0.03                      (C)ChaN, 2021
/-----------------------------------------------------------------------------/
/ The TJpgDec is a generic JPEG decompressor module for tiny embedded systems.
/ This is a free software that opened for education, research and commercial
/  developments under license policy of following terms.
/
/  Copyright (C) 2021, ChaN, all right reserved.
/
/ * The TJpgDec module is a free software and there is NO WARRANTY.
/ * No restriction on use. You can use, modify and redistribute it for
/   personal, non-profit or commercial products UNDER YOUR RESPONSIBILITY.
/ * Redistributions of source code must retain the above copyright notice.
```

- **stb_truetype** (`src/libs/tiny_ttf/stb_truetype_htcw.h`): the TrueType
  rasteriser behind LVGL's Tiny TTF (`LV_USE_TINY_TTF` in
  `components/wisp_ui/config/lv_conf.h`), which `wisp::ui::font()` uses to
  render the vector font (vector-font spec 4.1-4.2), in the firmware and
  the simulator. Dual-licensed MIT / public domain, copied verbatim from
  the end of the header:

```
------------------------------------------------------------------------------
This software is available under 2 licenses -- choose whichever you prefer.
------------------------------------------------------------------------------
ALTERNATIVE A - MIT License
Copyright (c) 2017 Sean Barrett
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
------------------------------------------------------------------------------
ALTERNATIVE B - Public Domain (www.unlicense.org)
This is free and unencumbered software released into the public domain.
Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
In jurisdictions that recognize copyright laws, the author or authors of this
software dedicate any and all copyright interest in the software to the public
domain. We make this dedication for the benefit of the public at large and to
the detriment of our heirs and successors. We intend this dedication to be an
overt act of relinquishment in perpetuity of all present and future rights to
this software under copyright law.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
------------------------------------------------------------------------------
```

## Lua

- Project: https://www.lua.org/
- Version: 5.5.1 (`https://www.lua.org/ftp/lua-5.5.1.tar.gz`, SHA-256
  `1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce`),
  vendored verbatim into `third_party/lua/lua-5.5.1/` by
  `tools/vendor_lua.sh`; not modified or forked
- License: MIT

Copied verbatim from the copyright notice at the end of
`third_party/lua/lua-5.5.1/src/lua.h` (comment markers removed):

```
Copyright (C) 1994-2026 Lua.org, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## Noto Sans CJK SC (via `lv_font_cjk_16.c` and `noto_sans_cjk_sc_subset.ttf`)

- `components/wisp_ui/fonts/lv_font_cjk_16.c` embeds glyph bitmap data
  (16 px, 4 bpp; GB2312, Japanese kana and symbols, JIS X 0208 level-1
  kanji) rendered from Noto Sans CJK SC Regular by `tools/gen_cjk_font.py`
  with lv_font_conv 1.5.3.
- `components/wisp_ui/fonts/noto_sans_cjk_sc_subset.ttf` is a second
  rendering of the same source font: a TrueType-outline (`glyf`) subset
  covering the 16 px font's character set plus Latin-1 and common
  punctuation (vector-font spec 3.1), generated by `tools/gen_vector_font.py`.
  fontTools subsets the OTF, converts its CFF outlines to `glyf` (quadratic
  curves via `Cu2QuPen`), and strips hinting and most `name` entries; the
  result is packed with a 32-byte header into the `fonts` flash partition's
  image by `tools/pack_font_image.py` (vector-font spec 3.2). It is what
  `ui::font(px)` renders through LVGL's Tiny TTF for text 20 px and larger
  (vector-font spec 4.1).
- Source font: `Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf` at
  release tag `Sans2.004` of https://github.com/notofonts/noto-cjk
  (SHA-256 `2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b`),
  fetched directly from upstream -- the same file `gen_cjk_font.py` uses.
- License: SIL Open Font License, Version 1.1. The bitmap font's upstream
  `LICENSE` file at that tag declares no Reserved Font Name; the converted
  font is named `lv_font_cjk_16`, not after the source font. The subset TTF
  is checked against the same tag's `LICENSE` file (SHA-256
  `6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2`), which
  likewise declares no copyright line and no Reserved Font Name -- the
  copyright notice lives in the font's own `name` table (ID 0) instead, not
  in `LICENSE`. Subsetting and the CFF-to-TrueType conversion both make the
  subset TTF a Modified Version under the OFL, so it keeps the upstream
  `name` table entries (ID 0, 13, 14) and stays named "Noto Sans CJK SC",
  same as the source: with no Reserved Font Name declared there is nothing
  to rename away from.

The source font's own copyright notice (OpenType `name` table, ID 0) and
license description (IDs 13 and 14), verbatim, followed by the license text
from the upstream `LICENSE` file:

```
© 2014-2021 Adobe (http://www.adobe.com/).

This Font Software is licensed under the SIL Open Font License, Version 1.1.
This Font Software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the SIL Open Font
License for the specific language, permissions and limitations governing your
use of this Font Software.
http://scripts.sil.org/OFL

This Font Software is licensed under the SIL Open Font License,
Version 1.1.

This license is copied below, and is also available with a FAQ at:
http://scripts.sil.org/OFL

-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font
creation efforts of academic and linguistic communities, and to
provide a free and open framework in which fonts may be shared and
improved in partnership with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded,
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply to
any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software
components as distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to,
deleting, or substituting -- in part or in whole -- any of the
components of the Original Version, by changing formats or by porting
the Font Software to a new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed,
modify, redistribute, and sell modified and unmodified copies of the
Font Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components, in
Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the
corresponding Copyright Holder. This restriction only applies to the
primary font name as presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created using
the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```

## Font Awesome Free 5 (via `lv_font_wisp_icons_*.c`)

- `components/wisp_ui/fonts/lv_font_wisp_icons_{24,32,48}.c` are icon-only
  bitmap fonts (24/32/48 px, 4 bpp) holding exactly the `LV_SYMBOL_*` code
  points, generated by `tools/gen_icon_font.py` with lv_font_conv 1.5.3
  from LVGL 9.5.0's `scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff`
  (fetched as part of the LVGL source tree the build already pulls in, not
  downloaded separately). `ui::font(px)` falls back to one of these for
  icons, since the vector font has no `LV_SYMBOL_*` glyphs (vector-font
  spec 4.2, 4.6) -- the same icon set LVGL's own Montserrat 24/32/48 fonts
  used to carry, now generated here instead because those fonts were
  removed.
- License: SIL Open Font License, Version 1.1, copyright "Copyright (c)
  2022 Fonticons, Inc. (https://fontawesome.com) with Reserved Font Name:
  'Font Awesome'." The three generated fonts are Modified Versions and do
  not use the Reserved Font Name; they are named `lv_font_wisp_icons_24`,
  `lv_font_wisp_icons_32` and `lv_font_wisp_icons_48`.

Copied verbatim from LVGL 9.5.0's
`scripts/built_in_font/font_license/FontAwesome5/LICENSE.txt`:

```
Fonticons, Inc. (https://fontawesome.com)

--------------------------------------------------------------------------------

Font Awesome Free License

Font Awesome Free is free, open source, and GPL friendly. You can use it for
commercial projects, open source projects, or really almost whatever you want.
Full Font Awesome Free license: https://fontawesome.com/license/free.

--------------------------------------------------------------------------------

# Icons: CC BY 4.0 License (https://creativecommons.org/licenses/by/4.0/)

The Font Awesome Free download is licensed under a Creative Commons
Attribution 4.0 International License and applies to all icons packaged
as SVG and JS file types.

--------------------------------------------------------------------------------

# Fonts: SIL OFL 1.1 License

In the Font Awesome Free download, the SIL OFL license applies to all icons
packaged as web and desktop font files.

Copyright (c) 2022 Fonticons, Inc. (https://fontawesome.com)
with Reserved Font Name: "Font Awesome".

This Font Software is licensed under the SIL Open Font License, Version 1.1.
This license is copied below, and is also available with a FAQ at:
http://scripts.sil.org/OFL

SIL OPEN FONT LICENSE
Version 1.1 - 26 February 2007

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded,
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting — in part or in whole — any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.

--------------------------------------------------------------------------------

# Code: MIT License (https://opensource.org/licenses/MIT)

In the Font Awesome Free download, the MIT license applies to all non-font and
non-icon files.

Copyright 2022 Fonticons, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in the
Software without restriction, including without limitation the rights to use, copy,
modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
and to permit persons to whom the Software is furnished to do so, subject to the
following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

--------------------------------------------------------------------------------

# Attribution

Attribution is required by MIT, SIL OFL, and CC BY licenses. Downloaded Font
Awesome Free files already contain embedded comments with sufficient
attribution, so you shouldn't need to do anything additional when using these
files normally.

We've kept attribution comments terse, so we ask that you do not actively work
to remove them from files, especially code. They're a great way for folks to
learn about Font Awesome.

--------------------------------------------------------------------------------

# Brand Icons

All brand icons are trademarks of their respective owners. The use of these
trademarks does not indicate endorsement of the trademark holder by Font
Awesome, nor vice versa. **Please do not use brand logos for any purpose except
to represent the company, product, or service to which they refer.**
```

## fontTools (build-time only, not shipped in the firmware)

- Project: https://github.com/fonttools/fonttools
- Version: 4.55.3 exactly (installed in a venv outside this repo, e.g.
  `python3 -m venv <dir> && <dir>/bin/pip install fonttools==4.55.3`; not
  vendored, not a build or runtime dependency of any shipped artifact)
- Used by: `tools/gen_vector_font.py` only, to subset the vector font and
  convert its CFF outlines to TrueType `glyf` outlines (vector-font spec
  3.1). The conversion step (`_cff_to_truetype()`) is adapted from
  fontTools' own `Snippets/otf2ttf.py` (same license); the file header
  notes this (`docs/references.md`).
- License: MIT

Copied verbatim from the `LICENSE` file of the fonttools 4.55.3 release
(PyPI sdist/wheel):

```
MIT License

Copyright (c) 2017 Just van Rossum

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## ESP-Claw (board definition for `boards/waveshare_esp32_s3_touch_amoled_2_16/`)

- Project: https://github.com/espressif/esp-claw (commit
  `36b377f0a46688916e4f6458a87dd92bee42aa92`)
- Files adapted: `application/edge_agent/boards/waveshare/waveshare_ESP32_S3_Touch_AMOLED_2_16/`
  `board_info.yaml`, `board_peripherals.yaml`, `board_devices.yaml`,
  `setup_device.c` (modifications listed in `docs/references.md`; each
  file's header notes its origin)
- License: Apache License 2.0 (same as this project; full text in
  `LICENSE`)
- Copyright: 2026 Espressif Systems (Shanghai) CO LTD

## Waveshare ESP32-S3-Touch-AMOLED-1.75C BSP (init table in `boards/waveshare_esp32_s3_touch_amoled_1_75c/setup_device.c`)

- Project: `waveshare/esp32_s3_touch_amoled_1_75c` 3.0.0 on the ESP
  Component Registry, from https://github.com/waveshareteam/Waveshare-ESP32-components
  (commit `010c5fb39ea7f0e996efd1f8e162cc70e569e57f`, path
  `bsp/esp32_s3_touch_amoled_1_75c`)
- Code adapted: the CO5300 init command table `lcd_init_cmds` of
  `esp32_s3_touch_amoled_1_75c.c`, copied verbatim, plus the panel column
  offset and touch orientation from the same file (details in
  `docs/references.md`). The board YAML in the same directory is derived
  from the 2.16's ESP-Claw files (see the ESP-Claw section above).
- License: Apache License 2.0 (same as this project; full text in
  `LICENSE`). The BSP ships the license text without a copyright line;
  the copyright holder is Waveshare.

## esp_lvgl_port (ESP32-S3 SIMD blend routines in `components/wisp_port_esp/lv_blend/esp32s3/`)

- Project: https://github.com/espressif/esp-bsp, `components/esp_lvgl_port`
  version 2.9.0 (commit `f0ef9497efce684997ce391edd19733483e250a5`)
- Files: `src/lvgl9/simd/lv_color_blend_to_rgb565_esp32s3.S`,
  `lv_rgb565_blend_normal_to_rgb565_esp32s3.S` and `lv_macro_memcpy.S`
  (copied unchanged apart from a provenance comment), and
  `lv_blend_esp32s3.h`, adapted from `include/esp_lvgl_port_lv_blend.h`
  (modifications listed in `docs/references.md`; each file's header notes
  its origin)
- License: Apache License 2.0 (same as this project; full text in
  `LICENSE`)
- Copyright: 2024-2025 Espressif Systems (Shanghai) CO LTD

## ESP-IDF

- Project: https://github.com/espressif/esp-idf
- Version: v6.0.3 (official toolchain/SDK, installed outside this repo
  per `docs/architecture.md`'s version lock, e.g. to `~/esp/esp-idf-v6.0.3/`;
  invoked via `idf.py`; not vendored or forked)
- License: Apache License 2.0 (full text in `LICENSE`, same as this
  project)
- Copyright: 2015-2023 Espressif Systems, per ESP-IDF's own
  `docs/en/COPYRIGHT.rst`

This project is a thin application on top of the ESP-IDF framework, which
itself bundles a number of third-party components under their own
licenses -- authoritative per ESP-IDF's own `docs/en/COPYRIGHT.rst`
(narrative summary) and `tools/ci/check_copyright_config.yaml` (the
per-file license each ESP-IDF source file actually declares), neither of
which this project vendors or reproduces here. The table below lists
every one of those bundled third-party components that is actually
linked into this firmware's build: counted by whether at least one of
their object files appears in `firmware/build/wisp.map`'s "Archive
member included" section, which lists every archive member the linker
actually pulled in before dead-code elimination -- not every component
ESP-IDF compiles, since many go unreferenced at that stage and are
dropped. Where the linker's own garbage collection (`--gc-sections`)
later discards most of a pulled-in archive member, that is noted
per-component below; the "linked" determination itself is always made
against "Archive member included", not against the smaller amount of
code that survives gc:

| Component | License | Notes |
|---|---|---|
| FreeRTOS kernel (`components/freertos`) | MIT | Copyright (C) 2017 Amazon.com, Inc. or its affiliates |
| Picolibc (`components/esp_libc`) | BSD-style | This firmware's `sdkconfig` selects `CONFIG_LIBC_PICOLIBC` (not Newlib; confirmed via `sdkconfig`'s `CONFIG_LIBC_PICOLIBC=y` / `# CONFIG_LIBC_NEWLIB is not set`, and the linked `libc.a` resolving to the toolchain's `picolibc/xtensa-esp-elf/lib/esp32s3/no-rtti/libc.a`) |
| TLSF allocator (`components/heap/tlsf`) | BSD-3-Clause | `libheap.a(tlsf.c.obj)` -- the block allocator backing `heap_caps`/`multi_heap`; Copyright (C) 2006-2016 Matthew Conte (full text below) |
| FatFs (`components/fatfs`) | BSD-style (ChaN license) | `libfatfs.a(ff.c.obj, ffunicode.c.obj, ffsystem.c.obj, diskio.c.obj, diskio_sdmmc.c.obj, vfs_fat.c.obj, vfs_fat_sdmmc.c.obj)` -- backs the SD card FAT filesystem (`storage.sd`); Copyright (C) 2022, ChaN, all right reserved (full text below) |
| Xtensa/Cadence HAL (`components/xtensa`) | MIT | `libxt_hal.a(windowspill_asm.o, int_asm--set_intclear.o, interrupts--intlevel.o, state_asm--save_extra_nw.o, state_asm--restore_extra_nw.o)` -- the vendor-supplied low-level Xtensa HAL (register window spill/fill, interrupt level primitives), distinct from ESP-IDF's own Apache-2.0 glue code in the same component (`libxtensa.a`); Copyright (C) 2003, 2006, 2010 Tensilica Inc, per `check_copyright_config.yaml`'s `xtensa` section and `docs/en/COPYRIGHT.rst` lines 26/88 (full text below) |
| Mbed TLS / TF-PSA-Crypto (`components/mbedtls`) | Apache License 2.0 | By the "Archive member included" count, `libtfpsacrypto.a` alone contributes 137 archive members (aes/gcm/rsa/ecdsa/psa_crypto/...) and `libmbedtls.a`'s own `esp-idf/mbedtls/mbedtls/library/libmbedtls.a(esp_psa_crypto_init.c.obj)` and `esp-idf/mbedtls/libmbedtls.a(esp_mem.c.obj)` are also pulled in. Most of that is discarded by the linker's `--gc-sections`; what actually survives into `wisp.bin` is a small amount of PSA core code (key slot management, the init/free paths, and the ESP RSA-DS opaque-key driver) from `psa_crypto.c`, `psa_crypto_slot_management.c`, and `psa_crypto_driver_esp_rsa_ds.c`, plus the allocator/mutex shim in `esp_mem.c.obj`/`esp_psa_crypto_init.c.obj` -- since networking landed, `libmbedtls.a`/`libmbedx509.a`/`libtfpsacrypto.a` also carry the TLS client and X.509 verification that `wisp::port::esp::EspHttpClient` (`components/wisp_port_esp/net/esp_http_client.cpp`, via esp-tls) uses for the model connectivity test, and the crypto that wpa_supplicant uses for WPA2/WPA3; Copyright (C) 2006-2018 ARM Limited |
| lwIP (`components/lwip`) | BSD-3-Clause | `liblwip.a` (84 archive members: TCP/IP stack, sockets, DHCP client/server, DNS) -- the network stack under Wi-Fi, both HTTP servers, the captive DNS responder and mDNS; Copyright (C) 2001, 2002 Swedish Institute of Computer Science (full text below) |
| wpa_supplicant (`components/wpa_supplicant`) | BSD-3-Clause | `libwpa_supplicant.a` (137 archive members: WPA2/WPA3 station and soft-AP key management, SAE, crypto glue over Mbed TLS) -- `CONFIG_FAST_PBKDF2` is off in this build, so the CC0 "Fast PBKDF2" code ESP-IDF's `COPYRIGHT.rst` also lists is not compiled in; Copyright (c) 2002-2022, Jouni Malinen and contributors (full text below) |
| FreeBSD net80211 (inside `components/esp_wifi`'s binary `libnet80211.a`) | BSD | Per ESP-IDF's `docs/en/COPYRIGHT.rst`, the closed-source Wi-Fi library contains code derived from FreeBSD net80211, Copyright (C) 2004-2008 Sam Leffler, Errno Consulting; ESP-IDF ships no separate license file for it. The Wi-Fi/PHY binary libraries themselves (`libnet80211.a`, `libpp.a`, `libphy.a`, `libcore.a`, `libmesh.a`, ...) are Espressif's own, under Apache-2.0 (`components/esp_wifi/lib/LICENSE`, `components/esp_phy/lib/LICENSE`) |
| HTTP Parser (`components/http_parser`) | MIT | `libhttp_parser.a(http_parser.c.obj)` -- request/response parsing inside esp_http_server and esp_http_client; based on NGINX's parser, Copyright Igor Sysoev, additional changes Copyright Joyent, Inc. and other Node contributors (full text below) |

ESP-IDF's Bluetooth stacks, OpenThread and the other bundled
components not listed above have no object files in this build's
"Archive member included" section, so they are not listed here.

lwIP: copied verbatim from `components/lwip/lwip/COPYING`
(`~/esp/esp-idf-v6.0.3`):

```
Copyright (c) 2001, 2002 Swedish Institute of Computer Science.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. The name of the author may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
OF SUCH DAMAGE.
```

wpa_supplicant: the copyright line from `components/wpa_supplicant/COPYING`
followed by the license terms that file points to, copied verbatim from
`components/wpa_supplicant/README` (`~/esp/esp-idf-v6.0.3`):

```
wpa_supplicant and hostapd
--------------------------

Copyright (c) 2002-2022, Jouni Malinen <j@w1.fi> and contributors
All Rights Reserved.

This software may be distributed, used, and modified under the terms of
BSD license:

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

3. Neither the name(s) of the above-listed copyright holder(s) nor the
   names of its contributors may be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

HTTP Parser: copied verbatim from `components/http_parser/LICENSE.txt`
(`~/esp/esp-idf-v6.0.3`):

```
/* Based on src/http/ngx_http_parse.c from NGINX copyright Igor Sysoev
 *
 * Additional changes are licensed under the same terms as NGINX and
 * copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
```

Copied verbatim from `components/heap/tlsf/include/tlsf.h`'s SPDX header
(`~/esp/esp-idf-v6.0.3`; Copyright (C) 2006-2016 Matthew Conte,
SPDX-License-Identifier: BSD-3-Clause) plus the standard BSD-3-Clause
template text -- ESP-IDF does not vendor a separate `LICENSE` file for
this component, only the SPDX header and the narrative entry in
`docs/en/COPYRIGHT.rst`:

```
Copyright (c) 2006-2016, Matthew Conte
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

    * Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the
distribution.
    * Neither the name of the copyright holder nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

Copied verbatim from `components/fatfs/src/ff.h`'s header comment
(`~/esp/esp-idf-v6.0.3`; same text ESP-IDF's own
`docs/en/COPYRIGHT.rst` line 46 points at):

```
Copyright (C) 2022, ChaN, all right reserved.

FatFs module is an open source software. Redistribution and use of FatFs in
source and binary forms, with or without modification, are permitted provided
that the following condition is met:

1. Redistributions of source code must retain the above copyright notice,
   this condition and the following disclaimer.

This software is provided by the copyright holder and contributors "AS IS"
and any warranties related to this software are DISCLAIMED.
The copyright owner or contributors be NOT LIABLE for any damages caused
by use of this software.
```

Copied verbatim from `docs/en/COPYRIGHT.rst`'s "Xtensa libhal MIT
License" section (`~/esp/esp-idf-v6.0.3`):

```
Copyright (C) 2003, 2006, 2010 Tensilica Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## ESP-IDF components used by the firmware

Fetched by the ESP-IDF component manager at build time (direct
dependencies pinned in `firmware/main/idf_component.yml`; every entry
below, including the transitive ones pulled in by esp_board_manager and
by the bmgr-generated `gen_bmgr_codes` component, is locked in
`firmware/dependencies.lock`, which is the authority for this list; not
vendored or forked). License checked against each component's own
license file under `firmware/managed_components/`:

| Component | Version | Pulled in by | License |
|---|---|---|---|
| `lvgl/lvgl` | 9.5.0 | `firmware/main` (see the LVGL section above) | MIT |
| `bblanchon/arduinojson` | 7.4.3 | `firmware/main` (see the ArduinoJson section below) | MIT |
| `espressif/esp_board_manager` | 0.7.5 | `firmware/main` | Espressif Modified MIT License (`LicenseRef-Espressif-Modified-MIT`, below) |
| `espressif/esp_boards` | 0.7.2 | esp_board_manager | Espressif Modified MIT License (same text, copyright 2026) |
| `espressif/esp_codec_dev` | 2.0.0-beta5 | `firmware/main` (direct dependency -- esp_board_manager only pulls this in conditionally on a Kconfig option that doesn't exist yet on a from-scratch configure, the same bootstrap issue esp_lcd_touch/esp_lcd_co5300 below also avoid by depending on it directly; used directly by `wisp::port::esp::EspAudioOut`/`EspAudioIn`, `components/wisp_port_esp/dev/esp_audio.cpp`) | Espressif Modified MIT License (same text, copyright 2023-2026) |
| `espressif/esp_lcd_co5300` | 2.2.0 | `firmware/main`, gen_bmgr_codes | Apache-2.0 |
| `espressif/esp_lcd_touch` | 1.2.1 | `firmware/main`, gen_bmgr_codes | Apache-2.0 |
| `waveshare/esp_lcd_touch_cst9217` | 2.0.0 | `firmware/main`, gen_bmgr_codes | Apache-2.0 |
| `espressif/esp_lcd_panel_io_additions` | 1.0.1~1 | esp_board_manager, gen_bmgr_codes | Apache-2.0 |
| `espressif/esp_io_expander` | 1.2.1 | esp_lcd_panel_io_additions | Apache-2.0 |
| `espressif/usb` | 1.6.0 | esp_board_manager (via usb_host_uac) | Apache-2.0 |
| `espressif/usb_host_uac` | 1.5.0 | esp_board_manager | Apache-2.0 |
| `espressif/cmake_utilities` | 0.5.3 | esp_board_manager (build-time CMake helpers only) | Apache-2.0 |
| `espressif/mdns` | 1.14.0 | `firmware/main` (direct dependency; the `wisp.local` responder started by `wisp::port::esp::EspWifi`, `components/wisp_port_esp/net/esp_wifi.cpp`) | Apache-2.0 |
| `espressif/esp_new_jpeg` | 1.0.2 | `firmware/main` (direct dependency; baseline JPEG for `ui.image`, decoded by `wisp::port::esp::install_image_decoders()`, `components/wisp_port_esp/ui/esp_jpeg_decoder.cpp`; a prebuilt library) | ESPRESSIF MIT License (below) |
| `joltwallet/littlefs` | 1.23.1 | `firmware/main` (direct dependency, backs `wisp::port::esp::EspInternalStorage`, `components/wisp_port_esp/dev/esp_storage.cpp`) | MIT (ESP-IDF wrapper) + BSD-3-Clause (vendored littlefs core) -- both texts below |

The Apache-2.0 components are under the same license as this project
(full text in `LICENSE`).

esp_board_manager's (and esp_boards'/esp_codec_dev's) license restricts use to Espressif products, which
matches its role here (it is only linked into the ESP32 firmware, never
into the portable layers or the simulator). Copied verbatim from
`firmware/managed_components/espressif__esp_board_manager/LICENSE`:

```
Espressif Modified MIT License

 Copyright (c) 2025 Espressif Systems (Shanghai) CO., LTD

 Permission is hereby granted for use EXCLUSIVELY with Espressif Systems products.
 This includes the right to use, copy, modify, merge, publish, distribute, and sublicense
 the Software, subject to the following conditions:

 1. This Software MUST BE USED IN CONJUNCTION WITH ESPRESSIF SYSTEMS PRODUCTS.
 2. The above copyright notice and this permission notice shall be included in all copies
    or substantial portions of the Software.
 3. Redistribution of the Software in source or binary form FOR USE WITH NON-ESPRESSIF PRODUCTS
    is strictly prohibited.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
 FOR ANY CLAIM, DAMAGES, OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 DEALINGS IN THE SOFTWARE.

 SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
```

esp_new_jpeg's license permits use on Espressif products, which matches
its role here (linked into the ESP32 firmware only). Copied verbatim from
`firmware/managed_components/espressif__esp_new_jpeg/LICENSE`:

```
 ESPRESSIF MIT License

 Copyright (c) 2024 <ESPRESSIF SYSTEMS (SHANGHAI) CO.，LTD>

 Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 it is free of charge, to any person obtaining a copy of this software and associated
 documentation files (the "Software"), to deal in the Software without restriction, including
 without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all copies or
 substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## MyOS (drivers for AXP2101, PCF85063, QMI8658)

- Project: MyOS (not published; same author as this project)
- Files ported: `src/hal/hal_power.{hpp,cpp}`, `src/hal/hal_rtc.{hpp,cpp}`,
  `src/hal/hal_imu.{hpp,cpp}` -> `drivers/axp2101/`, `drivers/pcf85063/`,
  `drivers/qmi8658/` (register sequences and conversion formulas; see
  `docs/references.md` for exactly what was dropped/added)
- License: MIT
- Copyright: 2026 ShouChen

```
MIT License

Copyright (c) 2026 ShouChen

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## ArduinoJson

- Project: https://github.com/bblanchon/ArduinoJson
- Version: 7.4.3 (host: CMake FetchContent, `cmake/wisp_host_deps.cmake`;
  firmware: `bblanchon/arduinojson` from the ESP component registry; not
  vendored or forked)
- License: MIT

Copied verbatim from
`firmware/managed_components/bblanchon__arduinojson/LICENSE.txt`:

```
The MIT License (MIT)
---------------------

Copyright © 2014-2026, Benoit BLANCHON

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## GoogleTest

- Project: https://github.com/google/googletest
- Version: 1.17.0 (fetched via CMake FetchContent,
  `cmake/wisp_host_deps.cmake`; not vendored/forked)
- Used by: `tests/` only (host-side unit tests, `tests/CMakeLists.txt`'s
  `wisp_test()`). Never linked into the firmware or any portable
  component -- a test-only dependency.
- License: BSD-3-Clause

Copied verbatim from
`build/sim/_deps/googletest-src/LICENSE` (fetched source tree; not
committed to this repo):

```
Copyright 2008, Google Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

    * Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the
distribution.
    * Neither the name of Google Inc. nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## SDL2

- Project: https://www.libsdl.org/
- Version: whatever the host's SDL2 development package provides --
  `simulator/CMakeLists.txt` locates it with `find_package(SDL2
  REQUIRED)` against the system installation; there is no FetchContent
  pin because it is a build-time dependency of the simulator executable
  only, never redistributed or vendored, and not linked into the
  firmware.
- Used by: `simulator/` (window/event loop) and
  `components/wisp_port_sim/` (the simulated `Display`/`Touch`/`AudioOut`/
  `AudioIn` devices) -- see AGENTS.md's layering rule: only these two may
  `#include SDL*`.
- License: zlib

Copied verbatim from this machine's installed SDL2-compatible package
(`/usr/share/licenses/sdl2-compat/LICENSE.txt`; the canonical SDL2
`COPYING.txt` carries the same zlib text):

```
Copyright (C) 2026 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required. 
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## libcurl

- Project: https://curl.se/libcurl/
- Version: whatever the host's libcurl development package provides (7.80
  or newer) -- `cmake/wisp_host_deps.cmake` locates it with
  `find_package(CURL 7.80 REQUIRED)` against the system installation; it
  is a build-time dependency of the simulator and the host tests only,
  never redistributed or vendored, and not linked into the firmware.
- Used by: `components/wisp_port_sim/net/sim_http_client.cpp` (the
  simulator's `net::HttpClient`).
- License: curl (MIT/X derivative)

Copied verbatim from this machine's installed package
(`/usr/share/licenses/curl/COPYING`):

```
COPYRIGHT AND PERMISSION NOTICE

Copyright (c) 1996 - 2026, Daniel Stenberg, <daniel@haxx.se>, and many
contributors, see the THANKS file.

All rights reserved.

Permission to use, copy, modify, and distribute this software for any purpose
with or without fee is hereby granted, provided that the above copyright
notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS. IN
NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
OR OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of a copyright holder shall not
be used in advertising or otherwise to promote the sale, use or other dealings
in this Software without prior written authorization of the copyright holder.
```

## zlib

- Project: https://zlib.net/
- Version: whatever the host's zlib development package provides --
  `cmake/wisp_host_deps.cmake` locates it with `find_package(ZLIB
  REQUIRED)` against the system installation; it is a build-time
  dependency of the simulator and the host tests only, never
  redistributed or vendored, and not linked into the firmware.
- Used by: `components/wisp_port_sim/net/sim_web_pack.cpp` (gzip-compressing
  the admin web files of `--web-dir` into an in-memory asset pack).
- License: zlib

Copied verbatim from this machine's installed package
(`/usr/share/licenses/zlib/LICENSE`):

```
Copyright notice:

 (C) 1995-2026 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu
```

## littlefs (via joltwallet/littlefs, ESP-IDF wrapper)

- Project: https://github.com/joltwallet/esp_littlefs (ESP-IDF VFS wrapper
  around https://github.com/littlefs-project/littlefs, the vendored core)
- Version: 1.23.1 (ESP component registry, `firmware/main/idf_component.yml`
  pins the exact version; `firmware/dependencies.lock` is the authority;
  not vendored or forked in this repo)
- Used by: `wisp::port::esp::EspInternalStorage`
  (`components/wisp_port_esp/dev/esp_storage.cpp`) for
  `storage.int` via `esp_vfs_littlefs_register()`/`esp_littlefs_info()`
- License: MIT (wrapper) + BSD-3-Clause (vendored littlefs core)

Copied verbatim from
`firmware/managed_components/joltwallet__littlefs/LICENSE`:

```
Copyright 2020 Brian Pugh

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
Copyright (c) 2022, The littlefs authors.
Copyright (c) 2017, Arm Limited. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

-  Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
-  Redistributions in binary form must reproduce the above copyright notice, this
   list of conditions and the following disclaimer in the documentation and/or
   other materials provided with the distribution.
-  Neither the name of ARM nor the names of its contributors may be used to
   endorse or promote products derived from this software without specific prior
   written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## htm and Preact (via `web/vendor/htm-preact-3.1.1.mjs`)

- Projects: https://github.com/developit/htm (htm) and
  https://github.com/preactjs/preact (Preact)
- Version: htm 3.1.1; file `preact/standalone.module.js` from the npm
  tarball https://registry.npmjs.org/htm/-/htm-3.1.1.tgz (integrity
  `sha512-983Vyg8NwUE7JkZ6NmOqpCZ+sh1bKv2iYTlUkzlWmA5JD2acKoxd4KVxbMmxX/85mtfdnDmTFoNKcg5DGAvxNQ==`),
  vendored unmodified below an added header comment. The file bundles a
  minified Preact 10.x runtime; htm's build declares `preact ^10.2.0`, so
  the exact 10.x release is not recorded upstream.
- Used by: the browser admin UI under `web/` (served from the device's
  asset pack; never linked into the firmware's C/C++ code).
- License: htm is Apache License 2.0 (same as this project; full text in
  `LICENSE`), Copyright 2018 Google Inc. The bundled Preact runtime is MIT.

Preact's license, copied verbatim from `LICENSE` in the npm package
preact@10.6.6 (the copyright line is the same across the 10.x line):

```
The MIT License (MIT)

Copyright (c) 2015-present Jason Miller

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Howard Hinnant's civil date algorithms

- Source: "chrono-Compatible Low-Level Date Algorithms",
  http://howardhinnant.github.io/date_algorithms.html
- Used by: `components/wisp_core/src/local_time.cpp`
  (`days_from_civil()`, `civil_from_days()`), reimplemented from the
  published algorithms; the file header names the source.
- License: the page places the algorithms in the public domain. No notice
  is required; this entry only records the origin.
