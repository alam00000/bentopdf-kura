#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace pdfa {
constexpr const char* kEngineVersion = "1.2.1";
constexpr const char* kEngineName = "BentoPDF Kura Engine";

enum class Level { A1B, A1A, A2B, A2U, A2A, A3B, A3U, A3A, A4, A4F, A4E,
                   X1A, X3, X4, X6, E1, VT1, VT3,
                   X4P, X5G, X5N, X5PG, X6N, X6P, VT2 };

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
  bool analyze = false;
  bool outlineFonts = false;
  bool linearize = false;
  std::string preflightProfile;
  std::vector<std::pair<std::string, std::vector<std::string>>> profileFixOps;
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
  int severity = 0;        // 1 info, 2 warning, 3 error; 0 not yet classified
  std::vector<int> pages{};  // 1-based page numbers the finding refers to; empty = whole document
};

struct Result {
  bool ok = false;
  std::vector<unsigned char> pdf;
  std::vector<Issue> issues;
  std::vector<Issue> analysis;
  std::string error;
  std::string errorCode;
  std::string suggestedLevel;
  bool compliant = false;
};

Result convert(const unsigned char* data, std::size_t size, const Options& options);

bool verifyPassword(const unsigned char* data, std::size_t size, const std::string& password);

bool issueIsNormalization(const std::string& code);

// Default severity for an issue code: normalizations and analysis facts are
// info, repairs with a visual or semantic risk are warnings, deviations are errors.
int issueSeverity(const std::string& code);

// Look up a font file (ttf/ttc/otf) whose file name matches `wanted`, ignoring
// case, spaces, dashes and underscores; fills the PostScript-style name and bytes.
bool loadFontFromFolder(const std::string& folder, const std::string& wanted,
                        std::string& psName, std::string& bytes);

bool levelFromString(const std::string& s, Level& out);
std::string levelToString(Level level);
int levelPart(Level level);
char levelConformance(Level level);
Family levelFamily(Level level);
bool levelVerifyOnly(Level level);
}
