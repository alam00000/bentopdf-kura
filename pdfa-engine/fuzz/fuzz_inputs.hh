#pragma once

#include <cstddef>
#include <string>

namespace fuzzseed {

inline const std::string& minimalPdf() {
  static const std::string pdf = [] {
    std::string objs[5] = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /TrimBox [0 0 612 792] "
        "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        "",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"};
    std::string content = "BT /F1 12 Tf 40 700 Td (fuzz seed) Tj ET";
    objs[3] = "<< /Length " + std::to_string(content.size()) + " >>stream\n" + content +
              "\nendstream";
    std::string out = "%PDF-1.7\n";
    size_t offs[5];
    for (int i = 0; i < 5; ++i) {
      offs[i] = out.size();
      out += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    size_t xref = out.size();
    out += "xref\n0 6\n0000000000 65535 f \n";
    for (int i = 0; i < 5; ++i) {
      std::string n = std::to_string(offs[i]);
      out += std::string(10 - n.size(), '0') + n + " 00000 n \n";
    }
    out += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) +
           "\n%%EOF\n";
    return out;
  }();
  return pdf;
}

}
