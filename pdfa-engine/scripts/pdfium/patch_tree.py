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

FXGE_FROM = "if (is_linux || is_chromeos) {"
FXGE_TO = "if (is_linux || is_chromeos || is_wasm) {"

SKIA_FROM = "if (defined(checkout_skia) && checkout_skia && !is_android) {"
SKIA_TO = "if (defined(checkout_skia) && checkout_skia && !is_android && !is_wasm) {"

WASM_FLAGS = ("-D_POSIX_C_SOURCE=200112 -fno-stack-protector "
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

def main():
    src = pathlib.Path(sys.argv[1])
    target = sys.argv[2]
    if not target.startswith("wasm"):
        print("no patches needed for", target)
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
