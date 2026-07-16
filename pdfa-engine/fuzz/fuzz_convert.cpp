

#include <cstddef>
#include <cstdint>

#include "pdfa/pdfa.hh"

namespace {
const pdfa::Level kLevels[] = {
    pdfa::Level::A1B, pdfa::Level::A1A, pdfa::Level::A2B, pdfa::Level::A2U,
    pdfa::Level::A2A, pdfa::Level::A3B, pdfa::Level::A3U, pdfa::Level::A3A,
    pdfa::Level::A4,  pdfa::Level::A4F, pdfa::Level::A4E, pdfa::Level::X1A,
    pdfa::Level::X3,  pdfa::Level::X4,  pdfa::Level::X6,  pdfa::Level::E1,
    pdfa::Level::VT1, pdfa::Level::VT3};
constexpr int kNumLevels = sizeof(kLevels) / sizeof(kLevels[0]);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 1) return 0;
  uint8_t sel = data[0];
  pdfa::Options opt;
  opt.level = kLevels[sel % kNumLevels];
  opt.ua = (sel & 0x40) != 0;
  opt.allowVisualRisk = (sel & 0x80) != 0;

  pdfa::convert(data + 1, size - 1, opt);
  return 0;
}
