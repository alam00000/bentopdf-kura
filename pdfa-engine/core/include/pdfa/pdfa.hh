#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace pdfa {
constexpr const char* kEngineVersion = "1.1.0";
constexpr const char* kEngineName = "BentoPDF Kura Engine";

enum class Level { A1B, A1A, A2B, A2U, A2A, A3B, A3U, A3A, A4, A4F, A4E,
                   X1A, X3, X4, X6, E1, VT1, VT3 };

enum class Family { PDFA, PDFX, PDFE, PDFVT };

struct Options {
  Level level = Level::A2B;
  bool allowVisualRisk = false;
  bool ua = false;
  std::string docLang;
  std::string nowOverride;
  std::string password;
  std::string outputConditionIdentifier;
  std::string outputConditionInfo;
  std::string outputConditionRegistry;
  std::string destProfile;
  std::string vtRecords;
  std::function<bool(int pageIndex, double dpi, int& width, int& height, std::string& rgb)>
      rasterizePage;
  double rasterDpi = 300.0;
  std::string attachXml;
  std::string attachXmlName;
  std::string facturxProfile;
};

struct Issue {
  std::string code;
  std::string detail;
  bool fixed = false;
};

struct Result {
  bool ok = false;
  std::vector<unsigned char> pdf;
  std::vector<Issue> issues;
  std::string error;
  std::string errorCode;
  std::string suggestedLevel;
};

Result convert(const unsigned char* data, std::size_t size, const Options& options);

bool levelFromString(const std::string& s, Level& out);
std::string levelToString(Level level);
int levelPart(Level level);
char levelConformance(Level level);
Family levelFamily(Level level);
}
