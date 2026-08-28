#!/usr/bin/env python3
import pathlib
import sys

BUILDCONFIG_FROM = '''} else if (target_os == "emscripten") {
  # Because it's too hard to remove all targets from //BUILD.gn that do not work
  # with it.
  assert(
      false,
      "emscripten is not a supported target_os. It is available only as secondary toolchain.")
}'''

BUILDCONFIG_TO = '''} else if (target_os == "emscripten") {
  _default_toolchain = "//build/toolchain/wasm:wasm"
}'''

RENDERDEVICE_WIN_FROM = '''#if BUILDFLAG(IS_WIN)
class CFX_PSFontTracker;
#endif'''

RENDERDEVICE_WIN_TO = '''#if BUILDFLAG(IS_WIN)
#include <windows.h>
class CFX_PSFontTracker;
#endif'''

FXGE_FROM = "if (is_linux || is_chromeos) {"
FXGE_TO = "if (is_linux || is_chromeos || is_wasm) {"

SKIA_FROM = "if (defined(checkout_skia) && checkout_skia && !is_android) {"
SKIA_TO = "if (defined(checkout_skia) && checkout_skia && !is_android && !is_wasm) {"

WASM_FLAGS = ("-D_POSIX_C_SOURCE=200112 -fno-stack-protector -fno-exceptions "
              "-Wno-unknown-warning-option -sSUPPORT_LONGJMP=wasm")

TOOLCHAIN_FROM = """  nm = cc
  ld = cxx
"""

TOOLCHAIN_TO = f"""  nm = cc
  ld = cxx

  extra_cflags = "{WASM_FLAGS}"
  extra_cxxflags = "{WASM_FLAGS}"
"""

def patch(path, old, new, label):
    text = path.read_text()
    if new in text:
        print(f"  {label}: already applied")
        return
    if old not in text:
        print(f"  {label}: FAILED, anchor not found", file=sys.stderr)
        sys.exit(3)
    path.write_text(text.replace(old, new, 1))
    print(f"  {label}: applied")

GEN_INCLUDE_FROM = '#include "core/fpdfapi/page/cpdf_textobject.h"'
GEN_INCLUDE_TO = ('#include "core/fpdfapi/page/cpdf_color.h"\n'
                  '#include "core/fpdfapi/page/cpdf_pattern.h"\n'
                  '#include "core/fpdfapi/page/cpdf_shadingobject.h"\n'
                  '#include "core/fpdfapi/page/cpdf_shadingpattern.h"\n'
                  '#include "core/fpdfapi/page/cpdf_textobject.h"')

GEN_INLINE_FROM = """  RetainPtr<CPDF_Image> pImage = pImageObj->GetImage();
  if (pImage->IsInline()) {
    return;
  }

  RetainPtr<const CPDF_Stream> pStream = pImage->GetStream();"""
GEN_INLINE_TO = """  RetainPtr<CPDF_Image> pImage = pImageObj->GetImage();
  // Inline images are serialized through the XObject-conversion path below;
  // returning early here silently dropped them on regeneration.

  RetainPtr<const CPDF_Stream> pStream = pImage->GetStream();"""

GEN_PATTERN_FROM = """  *buf << "q ";
  if (WriteColorToStream(*buf, pPageObj->color_state().GetFillColor())) {
    *buf << " rg ";
  }
  if (WriteColorToStream(*buf, pPageObj->color_state().GetStrokeColor())) {
    *buf << " RG ";
  }"""
GEN_PATTERN_TO = """  *buf << "q ";
  const CPDF_Color* ec_fill = pPageObj->color_state().GetFillColor();
  if (ec_fill && ec_fill->IsPattern()) {
    RetainPtr<CPDF_Pattern> ec_pat = ec_fill->GetPattern();
    if (ec_pat && ec_pat->ec_pattern_obj()) {
      ByteString ec_name = RealizeResource(ec_pat->ec_pattern_obj().Get(), "Pattern");
      *buf << "/Pattern cs /" << PDF_NameEncode(ec_name) << " scn ";
    }
  } else if (WriteColorToStream(*buf, ec_fill)) {
    *buf << " rg ";
  }
  const CPDF_Color* ec_stroke = pPageObj->color_state().GetStrokeColor();
  if (ec_stroke && ec_stroke->IsPattern()) {
    RetainPtr<CPDF_Pattern> ec_spat = ec_stroke->GetPattern();
    if (ec_spat && ec_spat->ec_pattern_obj()) {
      ByteString ec_sname = RealizeResource(ec_spat->ec_pattern_obj().Get(), "Pattern");
      *buf << "/Pattern CS /" << PDF_NameEncode(ec_sname) << " SCN ";
    }
  } else if (WriteColorToStream(*buf, ec_stroke)) {
    *buf << " RG ";
  }"""

GEN_DISPATCH_FROM = """  } else if (CPDF_TextObject* pTextObj = pPageObj->AsText()) {
    ProcessText(buf, pTextObj);
  }
  pPageObj->SetDirty(false);"""
GEN_DISPATCH_TO = """  } else if (CPDF_TextObject* pTextObj = pPageObj->AsText()) {
    ProcessText(buf, pTextObj);
  } else if (CPDF_ShadingObject* pShadingObj = pPageObj->AsShading()) {
    ProcessShading(buf, pShadingObj);
  }
  pPageObj->SetDirty(false);"""

GEN_METHOD_FROM = """void CPDF_PageContentGenerator::ProcessForm(fxcrt::ostringstream* buf,"""
GEN_METHOD_TO = """void CPDF_PageContentGenerator::ProcessShading(
    fxcrt::ostringstream* buf,
    CPDF_ShadingObject* pShadingObj) {
  const CPDF_ShadingPattern* pattern = pShadingObj->pattern();
  if (!pattern) {
    return;
  }
  RetainPtr<const CPDF_Object> shading_obj = pattern->GetShadingObject();
  if (!shading_obj) {
    return;
  }
  ProcessGraphics(buf, pShadingObj);
  const CFX_Matrix& matrix = pShadingObj->matrix();
  if (!matrix.IsIdentity()) {
    WriteMatrix(*buf, matrix) << " cm ";
  }
  ByteString name = RealizeResource(shading_obj.Get(), "Shading");
  *buf << "BX /" << PDF_NameEncode(name) << " sh EX";
  EndProcessGraphics(*buf);
}

void CPDF_PageContentGenerator::ProcessForm(fxcrt::ostringstream* buf,"""

GEN_H_FWD_FROM = "class CPDF_TextObject;"
GEN_H_FWD_TO = "class CPDF_ShadingObject;\nclass CPDF_TextObject;"
GEN_H_DECL_FROM = "  void ProcessForm(fxcrt::ostringstream* buf, CPDF_FormObject* pFormObj);"
GEN_H_DECL_TO = ("  void ProcessForm(fxcrt::ostringstream* buf, CPDF_FormObject* pFormObj);\n"
                 "  void ProcessShading(fxcrt::ostringstream* buf,\n"
                 "                      CPDF_ShadingObject* pShadingObj);")

PATTERN_H_FROM = "  const CFX_Matrix& pattern_to_form() const { return pattern_to_form_; }"
PATTERN_H_TO = ("  const CFX_Matrix& pattern_to_form() const { return pattern_to_form_; }\n"
                "  // Exposed for content regeneration (pattern fill re-emit).\n"
                "  RetainPtr<CPDF_Object> ec_pattern_obj() const { return pattern_obj_; }")

def patch_generator(src):
    print("patching content generator (shading round-trip)")
    gen = src / "core" / "fpdfapi" / "edit" / "cpdf_pagecontentgenerator.cpp"
    hdr = src / "core" / "fpdfapi" / "edit" / "cpdf_pagecontentgenerator.h"
    patch(gen, GEN_INCLUDE_FROM, GEN_INCLUDE_TO, "generator: includes")
    patch(gen, GEN_DISPATCH_FROM, GEN_DISPATCH_TO, "generator: shading dispatch")
    patch(gen, GEN_METHOD_FROM, GEN_METHOD_TO, "generator: ProcessShading method")
    patch(gen, GEN_INLINE_FROM, GEN_INLINE_TO, "generator: inline-image keep")
    patch(gen, GEN_PATTERN_FROM, GEN_PATTERN_TO, "generator: pattern fill/stroke")
    patch(hdr, GEN_H_FWD_FROM, GEN_H_FWD_TO, "generator hdr: forward decl")
    patch(hdr, GEN_H_DECL_FROM, GEN_H_DECL_TO, "generator hdr: ProcessShading decl")
    patch(src / "core" / "fpdfapi" / "page" / "cpdf_pattern.h",
          PATTERN_H_FROM, PATTERN_H_TO, "pattern hdr: public pattern_obj accessor")

def main():
    src = pathlib.Path(sys.argv[1])
    target = sys.argv[2]
    patch_generator(src)
    if target.startswith("win"):
        patch(src / "core" / "fxge" / "cfx_renderdevice.h",
              RENDERDEVICE_WIN_FROM, RENDERDEVICE_WIN_TO, "renderdevice: windows.h for HDC")
    if not target.startswith("wasm"):
        print("no wasm patches needed for", target)
        return 0
    print("patching tree for wasm")
    patch(src / "build" / "config" / "BUILDCONFIG.gn",
          BUILDCONFIG_FROM, BUILDCONFIG_TO, "BUILDCONFIG default toolchain")
    patch(src / "core" / "fxge" / "BUILD.gn",
          FXGE_FROM, FXGE_TO, "fxge font backend")
    patch(src / "BUILD.gn", SKIA_FROM, SKIA_TO, "skia gn_check guard")
    patch(src / "build" / "toolchain" / "wasm" / "BUILD.gn",
          TOOLCHAIN_FROM, TOOLCHAIN_TO, "wasm toolchain posix flags")
    return 0

if __name__ == "__main__":
    sys.exit(main())
