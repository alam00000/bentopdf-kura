#pragma once

#include <qpdf/QPDF.hh>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace pdfa {
struct UaFinding {
  std::string checkpoint;
  std::string detail;
  std::set<int> pages;
};

struct NestingTable {
  std::map<std::string, std::set<std::string>> forbiddenParents;
  std::map<std::string, std::map<std::string, bool>> kidLimits;
  std::set<std::string> noContent;
};

const NestingTable& nestingTable();
std::vector<UaFinding> uaMachineCheck(QPDF& pdf, int part = 1);
std::vector<UaFinding> uaCheck(const unsigned char* data, size_t size, const std::string& password, int part = 1);
}
