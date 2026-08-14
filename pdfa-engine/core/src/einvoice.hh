#pragma once

#include <string>
#include <vector>

namespace pdfa {
struct InvoiceProfile {
  std::string standard;
  std::string profile;
  std::string filename;
  std::string prefix;
  std::string nsUri;
  std::string version;
  std::string relationship;
  std::string guidelineId;
  std::string documentType;
  std::string schemaName;
  std::string rootName;
  bool detected = false;
  bool profileValid = false;
  bool rootKnown = false;
};

struct InvoiceRead {
  bool ok = false;
  std::string error;
  std::string xml;
  std::string filename;
  std::string relationship;
  std::string xmp;
  std::vector<std::string> attachments;
  bool hasAf = false;
};

InvoiceProfile detectInvoice(const std::string& xml, const std::string& profileOverride,
                             const std::string& nameOverride);

InvoiceRead readInvoice(const unsigned char* data, std::size_t size,
                        const std::string& password);

std::string xmpValue(const std::string& xmp, const std::string& local);
}
