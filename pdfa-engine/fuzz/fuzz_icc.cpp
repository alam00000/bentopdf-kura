#include <cstddef>
#include <cstdint>
#include <string>

#include "fuzz_inputs.hh"
#include "pdfa/pdfa.hh"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 1 || size > (1u << 22)) return 0;
  pdfa::Options opt;
  opt.level = (data[0] & 1) ? pdfa::Level::X1A : pdfa::Level::A2B;
  opt.destProfile.assign(reinterpret_cast<const char*>(data + 1), size - 1);
  const std::string& pdf = fuzzseed::minimalPdf();
  pdfa::convert(reinterpret_cast<const unsigned char*>(pdf.data()), pdf.size(), opt);
  return 0;
}
