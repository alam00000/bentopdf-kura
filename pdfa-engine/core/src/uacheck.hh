#pragma once

#include <qpdf/QPDF.hh>

#include <set>
#include <string>
#include <vector>

namespace pdfa {
struct UaFinding {
  std::string checkpoint;
  std::string detail;
  std::set<int> pages;
};

std::vector<UaFinding> uaMachineCheck(QPDF& pdf);
std::vector<UaFinding> uaCheck(const unsigned char* data, size_t size, const std::string& password);
}
