#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
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
  double imageMaxPpi = 0.0;
  std::string attachXml;
  std::string attachXmlName;
  std::string facturxProfile;
  bool verifyOnly = false;
  std::string embedSource;
  std::string embedSourceName;
  std::string embedSourceMime;
  std::string fontFolder;
  std::vector<std::pair<std::string, std::string>> fontSubstitutions;
  std::function<bool(const std::string& wanted, std::string& psName, std::string& bytes)>
      loadFont;
  std::string defaultRgbProfile;
  std::string defaultCmykProfile;
  std::string defaultGrayProfile;
  bool rasterizeAllPages = false;
  std::function<bool(const std::string& signedData, std::string& pkcs7Der)> signDocument;
  std::string signName;
  std::string signReason;
  std::string signLocation;
  std::string signContactInfo;
  std::size_t signReserveBytes = 16384;

  struct OcrWord {
    std::string text;
    double x = 0, y = 0, width = 0, height = 0;
  };
  std::function<bool(int pageIndex, double dpi, int imgWidth, int imgHeight,
                     const std::string& rgb, std::vector<OcrWord>& words)>
      ocrPage;
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
  bool compliant = false;
};

Result convert(const unsigned char* data, std::size_t size, const Options& options);

bool issueIsNormalization(const std::string& code);

bool levelFromString(const std::string& s, Level& out);
std::string levelToString(Level level);
int levelPart(Level level);
char levelConformance(Level level);
Family levelFamily(Level level);
}
