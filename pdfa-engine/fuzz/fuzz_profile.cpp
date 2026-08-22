#include <cstddef>
#include <cstdint>
#include <string>

#include "fuzz_inputs.hh"
#include "pdfa/pdfa.hh"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > (1u << 20)) return 0;
  pdfa::Options opt;
  opt.level = pdfa::Level::A2B;
  opt.preflightProfile.assign(reinterpret_cast<const char*>(data), size);
  const std::string& pdf = fuzzseed::minimalPdf();
  pdfa::convert(reinterpret_cast<const unsigned char*>(pdf.data()), pdf.size(), opt);
  return 0;
}
