#include <cstddef>
#include <cstdint>
#include <string>

#include "fuzz_inputs.hh"
#include "pdfa/pdfa.hh"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 2 || size > (1u << 16)) return 0;
  size_t split = data[0] % size;
  pdfa::Options opt;
  opt.level = pdfa::Level::A2B;
  opt.password.assign(reinterpret_cast<const char*>(data + 1), split);
  std::string pdf(reinterpret_cast<const char*>(data + 1 + split), size - 1 - split);
  if (pdf.empty()) pdf = fuzzseed::minimalPdf();
  pdfa::convert(reinterpret_cast<const unsigned char*>(pdf.data()), pdf.size(), opt);
  return 0;
}
