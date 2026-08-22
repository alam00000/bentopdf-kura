#include <cstddef>
#include <cstdint>
#include <string>

#include "fuzz_inputs.hh"
#include "pdfa/pdfa.hh"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 1 || size > (1u << 20)) return 0;
  pdfa::Options opt;
  opt.level = pdfa::Level::A3B;
  opt.attachXml.assign(reinterpret_cast<const char*>(data + 1), size - 1);
  opt.attachXmlName = "invoice.xml";
  switch (data[0] % 3) {
    case 0: opt.facturxProfile = "EN 16931"; break;
    case 1: opt.facturxProfile = "BASIC"; break;
    default: break;
  }
  const std::string& pdf = fuzzseed::minimalPdf();
  pdfa::convert(reinterpret_cast<const unsigned char*>(pdf.data()), pdf.size(), opt);
  return 0;
}
