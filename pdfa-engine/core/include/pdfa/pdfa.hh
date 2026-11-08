#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pdfa {
constexpr const char* kEngineVersion = "1.4.0";
constexpr const char* kEngineName = "BentoPDF Kura Engine";

enum class Level { A1B, A1A, A2B, A2U, A2A, A3B, A3U, A3A, A4, A4F, A4E,
                   X1A, X3, X4, X6, E1, VT1, VT3,
                   X4P, X5G, X5N, X5PG, X6N, X6P, VT2 };

enum class Family { PDFA, PDFX, PDFE, PDFVT };

struct ColorRule {
  enum Objects : unsigned {
    kText = 1, kVector = 2, kShading = 4, kImageLossy = 8, kImageLossless = 16, kAllObjects = 31
  };
  enum Sources : unsigned {
    kUncRgb = 1, kCalRgb = 2, kUncCmyk = 4, kCalCmyk = 8, kUncGray = 16, kCalGray = 32,
    kLab = 64, kSpot = 128, kRegistration = 256, kAnySource = 511
  };
  enum Action { kConvert, kNone, kDecalibrate, kConvertAlternate, kDecalibrateConvert, kTag, kInkLimit };
  unsigned objects = kAllObjects;
  unsigned sources = kAnySource;
  Action action = kConvert;
  int intent = -1;
};

struct ColorJob {
  enum Embed { kEmbedNone, kEmbedOutputIntent, kEmbedSource };
  bool enabled = false;
  std::string destName;
  std::string destProfile;
  std::string deviceLink;
  std::string rgbName = "sRGB IEC61966-2.1";
  std::string cmykName;
  std::string grayName = "Dot Gain 15%";
  Embed embed = kEmbedOutputIntent;
  bool blackPointCompensation = true;
  bool preserveBlack = true;
  bool softMasks = true;
  bool processSeparationsAsCmyk = false;
  bool grayToBlackOnly = false;
  bool rgbGrayToBlackOnly = false;
  double rgbGrayTolerance[3] = {0, 0, 0};
  bool blendSpaceToDest = true;
  double inkLimit = 0;
  std::vector<std::pair<unsigned, std::vector<double>>> toneCurves;
  bool quick = false;
  double quickBlackGeneration = 1.0;
  double quickUnderColorRemoval = 1.0;
  std::vector<ColorRule> rules;
};

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
  std::function<bool(const std::string& pdf, double dpi, int& width, int& height, std::string& rgb)>
      renderPdf;
  double rasterDpi = 300.0;
  double imageMaxPpi = 0.0;
  double imageDownsampleAbovePpi = 0.0;
  std::string imageDownsampleKind;
  double imageUpsampleBelowPpi = 0.0;
  double imageUpsampleToPpi = 0.0;
  std::string attachXml;
  std::string attachXmlName;
  std::string facturxProfile;
  bool verifyOnly = false;
  bool analyze = false;
  bool outlineFonts = false;
  bool linearize = false;
  std::string preflightProfile;
  std::string profilePath;
  std::string inputPath;
  std::vector<std::pair<std::string, std::map<std::string, std::string>>> profileFixOps;
  std::map<std::size_t, std::vector<std::string>> profileFixRules;
  std::string embedSource;
  std::string embedSourceName;
  std::string embedSourceMime;
  std::string fontFolder;
  std::vector<std::pair<std::string, std::string>> fontSubstitutions;
  std::function<bool(const std::string& wanted, std::string& psName, std::string& bytes)>
      loadFont;
  std::function<bool(const std::string& script, bool bold, std::string& bytes)> loadCjkFont;
  std::function<bool(const std::string& name, std::string& bytes)> loadWorldFont;
  std::string defaultRgbProfile;
  std::string defaultCmykProfile;
  std::string defaultGrayProfile;
  ColorJob colorJob;
  std::string pageOutputIntents;
  bool dualPrint = false;
  bool visibleLayersOnly = false;
  int hitsPerPage = 0;
  int hitsPerDoc = 0;
  std::string externalProfileUrl;
  std::function<bool(const std::string& name, std::string& bytes)> loadIcc;
  bool rasterizeAllPages = false;
  bool rasterKeepText = true;
  std::set<int> rasterPages;
  std::map<std::string, std::string> profileVars;
  std::string pageRange;
  bool levelFromProfile = false;
  bool noStandard = false;
  std::string pdfVersion;
  bool reconvert = false;
  double fallbackDpi = 0.0;
  bool fallbackProblemPagesOnly = false;
  bool quickCheck = false;
  std::string bookmarks;
  std::function<void(const std::string&)> trace;
  std::function<bool(const std::string& htmlOrZip, const std::string& headScript, std::string& pdf)> renderHtml;
  std::string inputName;
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
  std::string level;
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
bool loadWorldFontFromFolder(const std::string& folder, const std::string& name, std::string& bytes);
bool loadCjkFontFromFolder(const std::string& folder, const std::string& script, bool bold,
                           std::string& bytes);

bool loadIccFromFolders(const std::vector<std::string>& folders, const std::string& name,
                        std::string& bytes);

bool configureColorConversion(Options& opt, const std::string& target, const std::string& targetIcc,
                              int intent, bool keepSpots, unsigned objects);

bool levelFromString(const std::string& s, Level& out);
std::string levelToString(Level level);
int levelPart(Level level);
char levelConformance(Level level);
Family levelFamily(Level level);
bool levelVerifyOnly(Level level);
}
