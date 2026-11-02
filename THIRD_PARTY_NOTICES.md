# Third-Party Notices

This project (Apache-2.0) depends on, or incorporates data derived from, the
following third-party projects. See `docs/references.md` for where each one
is used.

## LVGL

- Project: https://github.com/lvgl/lvgl
- Version: 9.5.0 (fetched via CMake FetchContent, not vendored/forked)
- License: MIT

Copied verbatim from `build/sim/_deps/lvgl-src/LICENCE.txt` (fetched source
tree; not committed to this repo):

```
MIT licence
Copyright (c) 2025 LVGL Kft

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## Noto Sans CJK SC (via `lv_font_cjk_16.c`)

- `components/wisp_ui/fonts/lv_font_cjk_16.c` embeds glyph bitmap data for a
  GB2312 character set generated from the Noto Sans CJK SC typeface,
  ported unmodified from the MyOS project's
  `src/config/lv_font_cjk_16.c` (itself generated with LVGL's own font
  conversion tool).
- Typeface project: https://github.com/notofonts/noto-cjk
- License: SIL Open Font License, Version 1.1

We have not independently verified every detail of the upstream copyright
notice (the font was ported into this repo via the MyOS project rather than
fetched directly from notofonts/noto-cjk); the line below is as given to us
and should be checked against the upstream OFL.txt before a release build:

```
Copyright 2014-2021 Adobe (http://www.adobe.com/), with Reserved Font Name
'Source'.

This Font Software is licensed under the SIL Open Font License, Version 1.1.
This license is copied below, and is also available with a FAQ at:
https://scripts.sil.org/OFL

-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

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
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical writer or
other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining a
copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components, in
Original or Modified Versions, may be sold by itself.

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
Software shall not be used to promote, endorse or advertise any Modified
Version, except to acknowledge the contribution(s) of the Copyright
Holder(s) and the Author(s) or with their explicit written permission.

5) The Font Software, modified or unmodified, in part or in whole, must be
distributed entirely under this license, and must not be distributed
under any other license. The requirement for fonts to remain under this
license does not apply to any document created using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are not
met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```

## ESP-Claw (board definition for `boards/waveshare_amoled_216/`)

- Project: https://github.com/espressif/esp-claw (commit
  `36b377f0a46688916e4f6458a87dd92bee42aa92`)
- Files adapted: `application/edge_agent/boards/waveshare/waveshare_ESP32_S3_Touch_AMOLED_2_16/`
  `board_info.yaml`, `board_peripherals.yaml`, `board_devices.yaml`,
  `setup_device.c` (modifications listed in `docs/references.md`; each
  file's header notes its origin)
- License: Apache License 2.0 (same as this project; full text in
  `LICENSE`)
- Copyright: 2026 Espressif Systems (Shanghai) CO LTD

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
only the ones actually linked into this firmware's build (checked
against `firmware/build/wisp.map`'s "Archive member included" section,
which lists every object file the linker actually pulled in -- not every
component ESP-IDF compiles, since many go unreferenced and are dropped):

| Component | License | Notes |
|---|---|---|
| FreeRTOS kernel (`components/freertos`) | MIT | Copyright (C) 2017 Amazon.com, Inc. or its affiliates |
| Picolibc (`components/esp_libc`) | BSD-style | This firmware's `sdkconfig` selects `CONFIG_LIBC_PICOLIBC` (not Newlib) |
| Mbed TLS (`components/mbedtls`) | Apache License 2.0 | Only a small allocator/PSA-crypto-init shim is actually linked in this build (sub-project 1 has no networking/TLS usage yet); Copyright (C) 2006-2018 ARM Limited |

ESP-IDF also bundles lwIP, wpa_supplicant, and various other
networking/Bluetooth components under their own licenses, but none of
their object files appear in this build's link -- sub-project 1 has no
networking yet (see `AGENTS.md`'s roadmap) -- so they are not listed
here; this note should be revisited once a later sub-project adds
networking and actually links one of them in.

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
| `espressif/esp_codec_dev` | 2.0.0-beta5 | `firmware/main` (direct dependency, Task 18 -- esp_board_manager only pulls this in conditionally on a Kconfig option that doesn't exist yet on a from-scratch configure, the same bootstrap issue esp_lcd_touch/esp_lcd_co5300 below already avoid this way; used directly by `wisp::port::esp::EspAudioOut`/`EspAudioIn`, `components/wisp_port_esp/dev/esp_audio.cpp`) | Espressif Modified MIT License (same text, copyright 2023-2026) |
| `espressif/esp_lcd_co5300` | 2.2.0 | `firmware/main`, gen_bmgr_codes | Apache-2.0 |
| `espressif/esp_lcd_touch` | 1.2.1 | `firmware/main`, gen_bmgr_codes | Apache-2.0 |
| `waveshare/esp_lcd_touch_cst9217` | 2.0.0 | `firmware/main`, gen_bmgr_codes | Apache-2.0 |
| `espressif/esp_lcd_panel_io_additions` | 1.0.1~1 | esp_board_manager, gen_bmgr_codes | Apache-2.0 |
| `espressif/esp_io_expander` | 1.2.1 | esp_lcd_panel_io_additions | Apache-2.0 |
| `espressif/usb` | 1.6.0 | esp_board_manager (via usb_host_uac) | Apache-2.0 |
| `espressif/usb_host_uac` | 1.5.0 | esp_board_manager | Apache-2.0 |
| `espressif/cmake_utilities` | 0.5.3 | esp_board_manager (build-time CMake helpers only) | Apache-2.0 |
| `joltwallet/littlefs` | 1.23.1 | `firmware/main` (Task 18, backs `wisp::port::esp::EspInternalStorage`, `components/wisp_port_esp/dev/esp_storage.cpp`) | MIT (ESP-IDF wrapper) + BSD-3-Clause (vendored littlefs core) -- both texts below |

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

## MyOS (drivers for AXP2101, PCF85063, QMI8658)

- Project: the user's own `~/Projects/MyOS` (not published; same author as
  this project)
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

## littlefs (via joltwallet/littlefs, ESP-IDF wrapper)

- Project: https://github.com/joltwallet/esp_littlefs (ESP-IDF VFS wrapper
  around https://github.com/littlefs-project/littlefs, the vendored core)
- Version: 1.23.1 (ESP component registry, `firmware/main/idf_component.yml`
  pins the exact version; `firmware/dependencies.lock` is the authority;
  not vendored or forked in this repo)
- Used by: `wisp::port::esp::EspInternalStorage`
  (`components/wisp_port_esp/dev/esp_storage.cpp`, Task 18) for
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
