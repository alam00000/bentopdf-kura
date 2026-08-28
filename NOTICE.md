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
| Adobe Glyph List | Adobe AGL license (BSD-like) | glyph name to Unicode mapping, `pdfa-engine/core/assets/agl_names.cpp` |
| DM Sans | SIL Open Font License 1.1 | the demo site's typeface, `site/fonts/` |
| Phosphor Icons | MIT | the demo site's icons, `site/icons/` |

The sRGB and CMYK ICC profiles in `pdfa-engine/core/assets/` were generated
in-house with Little CMS and contain no third-party profile data.

The engine sources under `pdfa-engine/` and everything else in this repository
are licensed under the GNU Affero General Public License v3.0; see
[LICENSE](LICENSE). A commercial license is available for uses the AGPL does not
permit; see [docs/licensing.md](docs/licensing.md).

The full text of every license above ships in `licenses/` inside each release
archive. The SIL Open Font License is also at <https://openfontlicense.org/>
and the DejaVu license at <https://dejavu-fonts.github.io/License.html>.
