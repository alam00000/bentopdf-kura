# Third-party notices

Kura bundles or links the following components, each under its own license.
Every one is compatible with the AGPL for the combined work and permits
commercial redistribution.

| Component | License | How it is used |
|---|---|---|
| qpdf 12.3.2 | Apache-2.0 | PDF object model, reading and writing; linked statically |
| FreeType | FTL (BSD-style with attribution) | font parsing, glyph metrics and outlines |
| OpenJPEG 2.5 | BSD-2-Clause | JPEG 2000 decoding |
| libjpeg-turbo | IJG / BSD-3-Clause | JPEG decoding and encoding |
| zlib | zlib | Flate streams |
| Little CMS 2.16 | MIT | colour transforms and the bundled ICC profiles |
| libpng | PNG Reference Library License | required by FreeType for colour bitmap fonts |
| PDFium (chromium/7961) | BSD-3-Clause | page rasterization, built from source with V8, XFA and Skia disabled; its own notices ship under `licenses/pdfium/` in every release archive |
| OpenSSL | Apache-2.0 | PKCS#7 signing in the native CLI only |
| Liberation Fonts 2.1.5 | SIL Open Font License 1.1 | substitute fonts compiled into `pdfa-engine/core/assets/fonts_data.cpp` |
| DejaVu Sans | Bitstream Vera / DejaVu license | substitute font for Symbol and ZapfDingbats, same file |
| Noto Sans CJK (SC, TC, JP, KR) | SIL Open Font License 1.1 | substitute fonts for Chinese, Japanese and Korean, shipped as files in `pdfa-engine/core/fonts-cjk/` with `OFL.txt`; not compiled in |
| HarfBuzz 14.2.1 | MIT ("Old MIT") | text shaping for substitute fonts in complex scripts; compiled from `pdfa-engine/third_party/harfbuzz/` |
| Noto fonts (world scripts) | SIL Open Font License 1.1 | substitute fonts for Arabic, Indic, Southeast Asian and other scripts, shipped as files in `pdfa-engine/core/fonts-world/` with `OFL.txt`; not compiled in |
| Adobe Glyph List | Adobe AGL license (BSD-like) | glyph name to Unicode mapping, `pdfa-engine/core/assets/agl_names.cpp` |
| Adobe CMap resources (cmap-resources) | BSD-3-Clause, Copyright 1990-2023 Adobe | predefined CJK CMaps (code to CID) and CID to Unicode tables, compiled into `pdfa-engine/core/assets/cjk_cmaps.cpp` from `pdfa-engine/third_party/cmap-resources/`; license text below and in `CMAP-RESOURCES-LICENSE.md` |
| DM Sans | SIL Open Font License 1.1 | the demo site's typeface, `site/fonts/` |
| Phosphor Icons | MIT | the demo site's icons, `site/icons/` |

Colour profiles: the CMYK profile compiled into the engine
(`pdfa-engine/core/assets/cmyk_icc.cpp`) is Adobe's Coated GRACoL 2006, and the
sRGB profile (`pdfa-engine/core/assets/srgb_icc.cpp`) is Hewlett-Packard's
sRGB IEC61966-2.1. Adobe's profiles are distributed under Adobe's Color Profile
Bundling License; by using Kura you agree to the end-user terms in
`pdfa-engine/third_party/icc/LICENSES/ICC-PROFILE-TERMS.md`. The other profiles
in `pdfa-engine/third_party/icc/` and their owners' terms are listed in that
folder's README. Adobe and Adobe RGB (1998) are trademarks of Adobe Systems
Incorporated. The profiles in `pdfa-engine/core/icc/` were made by Kura from
published formulas and carry no rights; see that folder's README.

The engine sources under `pdfa-engine/` and everything else in this repository
are licensed under the GNU Affero General Public License v3.0; see
[LICENSE](LICENSE). A commercial license is available for uses the AGPL does not
permit; see [docs/licensing.md](docs/licensing.md).

The full text of every license above ships in `licenses/` inside each release
archive. The SIL Open Font License is also at <https://openfontlicense.org/>
and the DejaVu license at <https://dejavu-fonts.github.io/License.html>.

## Adobe CMap resources

Copyright 1990-2023 Adobe. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

Neither the name of Adobe nor the names of its contributors may be
used to endorse or promote products derived from this software without
specific prior written permission.

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
