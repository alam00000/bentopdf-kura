#include <regex>
#include <string>
#include <vector>

#include "pdfa/einvoice.hh"

namespace pdfa {
InvoiceCheck checkInvoice(const unsigned char* data, std::size_t size,
                          const std::string& password) {
  InvoiceCheck out;
  InvoiceRead r = readInvoice(data, size, password);
  if (!r.ok) {
    out.errorCode = "PARSE_ERROR";
    out.error = r.error;
    return out;
  }
  out.ok = true;
  if (r.xml.empty()) {
    out.error = "no e-invoice attachment";
    return out;
  }
  out.einvoice = true;
  out.xml = r.xml;
  out.attachment = r.filename;
  InvoiceProfile want = detectInvoice(r.xml, "", "");
  out.standard = want.standard;
  out.profile = want.profile;
  out.documentType = want.documentType;
  if (!want.detected) out.problems.push_back("payload declares no recognised guideline URN");
  auto present = [&](const std::string& local) {
    return std::regex_search(r.xml, std::regex("<([A-Za-z_][\\w.-]*:)?" + local + "[\\s/>]"));
  };
  std::vector<std::string> required;
  if (want.rootName == "Invoice" || want.rootName == "CreditNote") {
    required = {"ID", "IssueDate", want.rootName + "TypeCode"};
  } else if (want.rootKnown) {
    required = {"ID", "TypeCode", "IssueDateTime"};
  }
  for (const std::string& el : required) {
    if (!present(el)) out.problems.push_back("payload has no " + el + " element, which every " + want.standard + " document needs");
  }
  int xmls = 0;
  for (const std::string& a : r.attachments) {
    if (a.size() >= 4 && (a.compare(a.size() - 4, 4, ".xml") == 0 || a.compare(a.size() - 4, 4, ".XML") == 0)) ++xmls;
  }
  if (xmls > 1) out.problems.push_back(std::to_string(xmls) + " XML files are attached; an e-invoice carries exactly one");
  if (want.profile == "MINIMUM" || want.profile == "BASIC WL") {
    out.warnings.push_back(want.standard + " " + want.profile +
                           " carries only part of the invoice in its structured data, so the "
                           "German mandate does not accept this profile as an e-invoice; a "
                           "compliant one needs BASIC, EN 16931 or EXTENDED");
  }
  if (r.filename != want.filename) {
    out.problems.push_back("attachment is named \"" + r.filename + "\" but " + want.standard +
                           " " + want.profile + " requires \"" + want.filename + "\"");
  }
  bool relKnown = r.relationship == "/Data" || r.relationship == "/Source" || r.relationship == "/Alternative";
  if (!relKnown) {
    out.problems.push_back("AFRelationship is " + (r.relationship.empty() ? std::string("missing") : r.relationship) +
                           "; the invoice attachment must use Data, Source or Alternative");
  } else if (r.relationship != want.relationship) {
    bool headerOnly = want.profile == "MINIMUM" || want.profile == "BASIC WL";
    std::string msg = "AFRelationship is " + r.relationship + " but " + want.profile +
                      " normally uses " + want.relationship;
    if (headerOnly) {
      out.warnings.push_back(msg +
                             " (Factur-X 6.2.2 ties this to whether the page carries more "
                             "invoice data than the XML, which a reader cannot verify)");
    } else {
      out.problems.push_back(msg);
    }
  }
  if (!r.hasAf) out.problems.push_back("catalog has no /AF array");
  std::string xmpName = xmpValue(r.xmp, "DocumentFileName");
  std::string xmpConf = xmpValue(r.xmp, "ConformanceLevel");
  std::string xmpType = xmpValue(r.xmp, "DocumentType");
  if (xmpName.empty() && xmpConf.empty()) {
    out.problems.push_back("XMP carries no e-invoice extension schema");
  } else {
    if (xmpName != r.filename) {
      out.problems.push_back("XMP DocumentFileName \"" + xmpName +
                             "\" does not match the attachment \"" + r.filename + "\"");
    }
    if (!xmpConf.empty() && xmpConf != want.profile) {
      out.problems.push_back("XMP ConformanceLevel \"" + xmpConf +
                             "\" but the payload declares " + want.profile);
    }
    if (!xmpType.empty() && xmpType != want.documentType) {
      out.problems.push_back("XMP DocumentType \"" + xmpType + "\" but the payload is a " +
                             want.documentType);
    }
  }
  out.consistent = out.problems.empty();
  return out;
}
}  // namespace pdfa
