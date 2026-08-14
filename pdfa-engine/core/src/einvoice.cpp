#include "einvoice.hh"

#include <algorithm>
#include <cctype>

namespace pdfa {
namespace {
std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return std::string();
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

bool contains(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

std::string elementText(const std::string& xml, const char* local, size_t from, size_t to) {
  size_t p = from;
  while (p < to) {
    size_t open = xml.find(local, p);
    if (open == std::string::npos || open >= to) break;
    size_t gt = xml.find('>', open);
    if (gt == std::string::npos || gt >= to) break;
    size_t lt = xml.find('<', gt);
    if (lt == std::string::npos) break;
    std::string v = trim(xml.substr(gt + 1, lt - gt - 1));
    if (!v.empty()) return v;
    p = lt + 1;
  }
  return std::string();
}

std::string rootLocalName(const std::string& xml) {
  size_t p = 0;
  while (p < xml.size()) {
    size_t lt = xml.find('<', p);
    if (lt == std::string::npos) return std::string();
    if (lt + 1 < xml.size() && (xml[lt + 1] == '?' || xml[lt + 1] == '!')) {
      size_t gt = xml.find('>', lt);
      if (gt == std::string::npos) return std::string();
      p = gt + 1;
      continue;
    }
    size_t e = xml.find_first_of(" \t\r\n/>", lt + 1);
    if (e == std::string::npos) return std::string();
    std::string name = xml.substr(lt + 1, e - lt - 1);
    size_t colon = name.find(':');
    return colon == std::string::npos ? name : name.substr(colon + 1);
  }
  return std::string();
}

std::string guidelineId(const std::string& xml) {
  size_t g = xml.find("GuidelineSpecifiedDocumentContextParameter");
  if (g != std::string::npos) {
    size_t end = xml.find("/GuidelineSpecifiedDocumentContextParameter", g);
    if (end == std::string::npos) end = std::min(xml.size(), g + 4096);
    size_t p = g;
    while (p < end) {
      size_t open = xml.find("ID", p);
      if (open == std::string::npos || open >= end) break;
      size_t gt = xml.find('>', open);
      if (gt == std::string::npos || gt >= end) break;
      size_t lt = xml.find('<', gt);
      if (lt == std::string::npos) break;
      std::string v = trim(xml.substr(gt + 1, lt - gt - 1));
      if (v.compare(0, 4, "urn:") == 0 || v.compare(0, 4, "urn.") == 0) return v;
      p = lt + 1;
    }
  }
  size_t c = xml.find("CustomizationID");
  if (c != std::string::npos) {
    std::string v = elementText(xml, "CustomizationID", c, xml.size());
    if (!v.empty()) return v;
  }
  return std::string();
}

std::string orderDocumentType(const std::string& xml) {
  size_t d = xml.find("ExchangedDocument");
  size_t from = d == std::string::npos ? 0 : d;
  size_t to = std::min(xml.size(), from + 4096);
  std::string code = elementText(xml, "TypeCode", from, to);
  if (code == "230") return "ORDER_CHANGE";
  if (code == "231") return "ORDER_RESPONSE";
  return "ORDER";
}
}

InvoiceProfile detectInvoice(const std::string& xml, const std::string& profileOverride,
                             const std::string& nameOverride) {
  InvoiceProfile inv;
  inv.profile = "EN 16931";
  inv.filename = "factur-x.xml";
  inv.prefix = "fx";
  inv.nsUri = "urn:factur-x:pdfa:CrossIndustryDocument:invoice:1p0#";
  inv.version = "1.0";
  inv.standard = "Factur-X";
  inv.documentType = "INVOICE";
  inv.schemaName = "Factur-X PDFA Extension Schema";

  inv.rootName = rootLocalName(xml);
  inv.rootKnown = inv.rootName == "CrossIndustryInvoice" ||
                  inv.rootName == "CrossIndustryDocument" ||
                  inv.rootName == "SCRDMCCBDACIOMessageStructure" ||
                  inv.rootName == "Invoice" || inv.rootName == "CreditNote";

  std::string urn = guidelineId(xml);
  inv.guidelineId = urn;

  if (!urn.empty()) {
    inv.detected = true;
    if (contains(urn, "urn:order-x.eu")) {
      inv.standard = "Order-X";
      inv.filename = "order-x.xml";
      inv.nsUri = "urn:factur-x:pdfa:CrossIndustryDocument:1p0#";
      inv.schemaName = "Factur-X PDFA Extension Schema";
      inv.documentType = orderDocumentType(xml);
      if (contains(urn, ":basic")) inv.profile = "BASIC";
      else if (contains(urn, ":extended")) inv.profile = "EXTENDED";
      else inv.profile = "COMFORT";
    } else if (contains(urn, "urn:ferd:")) {
      inv.standard = "ZUGFeRD 1.0";
      inv.filename = "ZUGFeRD-invoice.xml";
      inv.prefix = "zf";
      inv.nsUri = "urn:ferd:pdfa:CrossIndustryDocument:invoice:1p0#";
      inv.schemaName = "ZUGFeRD PDFA Extension Schema";
      if (contains(urn, ":basic")) inv.profile = "BASIC";
      else if (contains(urn, ":extended")) inv.profile = "EXTENDED";
      else inv.profile = "COMFORT";
    } else if (contains(urn, "urn:zugferd.de:2p0")) {
      inv.standard = "ZUGFeRD 2.0";
      inv.filename = "zugferd-invoice.xml";
      inv.prefix = "zf";
      inv.nsUri = "urn:zugferd:pdfa:CrossIndustryDocument:invoice:1p0#";
      inv.schemaName = "ZUGFeRD PDFA Extension Schema";
      inv.version = "2p0";
      if (contains(urn, ":minimum")) inv.profile = "MINIMUM";
      else if (contains(urn, ":basic")) inv.profile = "BASIC";
      else if (contains(urn, ":extended")) inv.profile = "EXTENDED";
      else inv.profile = "EN 16931";
    } else if (contains(urn, "xrechnung")) {
      inv.standard = "XRechnung";
      inv.profile = "XRECHNUNG";
      inv.filename = "xrechnung.xml";
    } else if (contains(urn, "ereporting")) {
      inv.profile = "MINIMUM";
    } else if (contains(urn, "urn:factur-x.eu:1p0:minimum")) {
      inv.profile = "MINIMUM";
    } else if (contains(urn, "urn:factur-x.eu:1p0:basicwl")) {
      inv.profile = "BASIC WL";
    } else if (contains(urn, "urn:factur-x.eu:1p0:basic")) {
      inv.profile = "BASIC";
    } else if (contains(urn, "urn:factur-x.eu:1p0:extended")) {
      inv.profile = "EXTENDED";
    } else if (contains(urn, "urn:cen.eu:en16931:2017")) {
      inv.profile = "EN 16931";
    } else {
      inv.detected = false;
    }
  }

  if (!profileOverride.empty()) {
    inv.profile = profileOverride;
    inv.detected = true;
  }
  if (!nameOverride.empty()) inv.filename = nameOverride;

  static const char* kLevels[] = {"MINIMUM", "BASIC WL", "BASIC", "EN 16931",
                                  "EXTENDED", "XRECHNUNG", "COMFORT"};
  inv.profileValid = false;
  for (const char* l : kLevels) {
    if (inv.profile == l) {
      inv.profileValid = true;
      break;
    }
  }

  inv.relationship =
      (inv.profile == "MINIMUM" || inv.profile == "BASIC WL") ? "/Data" : "/Alternative";
  return inv;
}
}
