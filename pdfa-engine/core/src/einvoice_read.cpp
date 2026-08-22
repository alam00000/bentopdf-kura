#include <qpdf/QPDF.hh>
#include "limits.hh"
#include <qpdf/QPDFObjectHandle.hh>

#include <set>

#include "einvoice.hh"
#include "pdfa/pdfa.hh"

namespace pdfa {
namespace {
std::string nameStr(QPDFObjectHandle o) {
  if (o.isString()) return o.getUTF8Value();
  if (o.isName()) {
    std::string n = o.getName();
    return n.empty() || n[0] != '/' ? n : n.substr(1);
  }
  return std::string();
}

void walkNameTree(QPDFObjectHandle node, std::set<QPDFObjGen>& seen,
                  std::vector<std::pair<std::string, QPDFObjectHandle>>& out, int depth) {
  if (depth > kMaxObjectWalk || !node.isDictionary()) return;
  if (node.isIndirect() && !seen.insert(node.getObjGen()).second) return;
  QPDFObjectHandle names = node.getKey("/Names");
  if (names.isArray()) {
    for (int i = 0; i + 1 < names.getArrayNItems(); i += 2) {
      out.emplace_back(nameStr(names.getArrayItem(i)), names.getArrayItem(i + 1));
    }
  }
  QPDFObjectHandle kids = node.getKey("/Kids");
  if (kids.isArray()) {
    for (int i = 0; i < kids.getArrayNItems(); ++i) {
      walkNameTree(kids.getArrayItem(i), seen, out, depth + 1);
    }
  }
}

std::string streamOf(QPDFObjectHandle fs) {
  QPDFObjectHandle ef = fs.getKey("/EF");
  if (!ef.isDictionary()) return std::string();
  for (const char* k : {"/UF", "/F"}) {
    QPDFObjectHandle s = ef.getKey(k);
    if (s.isStream()) {
      try {
        std::shared_ptr<Buffer> b = s.getStreamData(qpdf_dl_all);
        return std::string(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
      } catch (...) {
      }
    }
  }
  return std::string();
}
}

InvoiceRead readInvoice(const unsigned char* data, std::size_t size,
                        const std::string& password) {
  InvoiceRead out;
  try {
    QPDF pdf;
    pdf.setSuppressWarnings(true);
    pdf.setAttemptRecovery(true);
    pdf.processMemoryFile("input", reinterpret_cast<const char*>(data), size, password.c_str());
    QPDFObjectHandle root = pdf.getRoot();
    QPDFObjectHandle names = root.getKey("/Names");
    QPDFObjectHandle tree =
        names.isDictionary() ? names.getKey("/EmbeddedFiles") : QPDFObjectHandle::newNull();
    std::vector<std::pair<std::string, QPDFObjectHandle>> files;
    std::set<QPDFObjGen> seen;
    walkNameTree(tree, seen, files, 0);

    static const char* kNames[] = {"factur-x.xml", "zugferd-invoice.xml", "ZUGFeRD-invoice.xml",
                                   "xrechnung.xml", "order-x.xml"};
    for (const auto& f : files) {
      out.attachments.push_back(f.first);
      for (const char* want : kNames) {
        if (f.first == want && out.xml.empty()) {
          out.xml = streamOf(f.second);
          out.filename = f.first;
          out.relationship = nameStr(f.second.getKey("/AFRelationship"));
          if (!out.relationship.empty()) out.relationship = "/" + out.relationship;
        }
      }
    }
    out.hasAf = root.getKey("/AF").isArray();
    QPDFObjectHandle meta = root.getKey("/Metadata");
    if (meta.isStream()) {
      try {
        std::shared_ptr<Buffer> b = meta.getStreamData(qpdf_dl_all);
        out.xmp.assign(reinterpret_cast<const char*>(b->getBuffer()), b->getSize());
      } catch (...) {
      }
    }
    out.ok = true;
  } catch (const std::exception& e) {
    out.error = e.what();
  }
  return out;
}

std::string xmpValue(const std::string& xmp, const std::string& local) {
  for (const char* px : {"fx", "zf"}) {
    std::string open = std::string("<") + px + ":" + local + ">";
    size_t a = xmp.find(open);
    if (a == std::string::npos) continue;
    size_t b = xmp.find('<', a + open.size());
    if (b == std::string::npos) continue;
    return xmp.substr(a + open.size(), b - a - open.size());
  }
  std::string attr = local + "=\"";
  size_t a = xmp.find(attr);
  if (a != std::string::npos) {
    size_t s = a + attr.size();
    size_t e = xmp.find('"', s);
    if (e != std::string::npos) return xmp.substr(s, e - s);
  }
  return std::string();
}
}
