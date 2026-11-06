#pragma once

#include <qpdf/QPDFObjGen.hh>
#include <qpdf/QPDFObjectHandle.hh>

#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "util.hh"

namespace pdfa {
struct Mat {
  double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};

inline Mat mul(const Mat& m, const Mat& n) {
  Mat r;
  r.a = m.a * n.a + m.b * n.c;
  r.b = m.a * n.b + m.b * n.d;
  r.c = m.c * n.a + m.d * n.c;
  r.d = m.c * n.b + m.d * n.d;
  r.e = m.e * n.a + m.f * n.c + n.e;
  r.f = m.e * n.b + m.f * n.d + n.f;
  return r;
}

inline double matScale(const Mat& m) {
  return (std::hypot(m.a, m.b) + std::hypot(m.c, m.d)) / 2.0;
}

struct ColorInfo {
  std::string cls = "gray";
  std::string spot;
  std::string altName;
  std::vector<std::string> colorants;
  int declaredComps = 1;
  bool indexed = false;
  std::vector<double> altValues;
  QPDFObjectHandle csObj;
  bool hasDefault = false;
};

struct GsExtra {
  double flatness = 0;
  double alphaFill = 1.0;
  double alphaStroke = 1.0;
  std::string blendMode = "Normal";
  bool hasSMask = false;
  bool smaskExplicitNone = false;
  bool smaskIsLuminosity = false;
  std::string smaskGroupCS;
  bool hasTR2 = false;
  bool tr2IsDefault = false;
  bool hasBPC = false;
  bool hasHalftoneOrigin = false;
  bool inTransGroup = false;
  int lineCap = 0;
  int lineJoin = 0;
  double miterLimit = 10;
  std::vector<double> dash;
  double dashPhase = 0;
  bool strokeAdjust = false;
  bool hasBG = false, hasBG2 = false, bg2Default = false;
  bool hasUCR = false, hasUCR2 = false, ucr2Default = false;
  bool hasTR = false, trIdentity = false, tr2Identity = false;
  bool ais = false, tk = true, riExplicit = false, hasHTP = false, transKeys = false;
  std::string intent;
  bool negFontSize = false;
  std::string bpcValue;
  bool smaskHasBC = false, smaskHasTR = false, smaskTRIdentity = false, smaskIsAlpha = false;
  QPDFObjectHandle ht;
  int groupDepth = 0;
  std::string groupCSOuter, groupCSInner, groupBaseCS;
  bool groupIsolated = false, groupKnockout = false, groupHasCS = false, groupCSDevInd = false;
  std::vector<std::string> blendChain;
  double charSpacing = 0, wordSpacing = 0, hScale = 100, leading = 0, rise = 0;
  int qDepth = 0;
  int bxDepth = 0;
  int formDepth = 0;
  bool inForm = false, inPattern = false, inType3 = false;
  bool actualText = false;
  bool resInherited = false;
  QPDFObjectHandle groupCSObj;
  QPDFObjectHandle formDict;
  double fontSize = 0;
  Mat ctm;
  Mat tm;
  std::vector<int> zp;
};

struct Box {
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool valid = false;
};

struct PaintEvent {
  double width = 0;
  std::vector<double> comps;
  int page = 0;
  bool stroke = false;
  bool fillOp = false;
  bool shade = false;
  bool noPaint = false;
  int pathNodes = 0;
  ColorInfo color;
  bool overprint = false;
  int opm = 0;
  bool transparency = false;
  GsExtra x;
  Box bbox;
  Box clip;
  bool visible = true;
  bool covers = false;
  bool clippedPart = false;
  bool clippedFull = false;
  bool layerHidden = false;
  int subpaths = 0, closedSubpaths = 0, curves = 0, lines = 0, rects = 0;
  int maxSubPts = 0, minSubPts = 0;
  double length = 0;
  int shadingType = 0;
  QPDFObjectHandle shading;
  std::vector<size_t> src;
  std::vector<double> geo;
};

struct TextEvent {
  double sizePt = 0;
  int renderMode = 0;
  std::vector<double> comps;
  int page = 0;
  ColorInfo color;
  bool overprint = false;
  int opm = 0;
  bool transparency = false;
  GsExtra x;
  Box bbox;
  Box clip;
  bool visible = true;
  bool covers = false;
  bool clippedPart = false;
  bool clippedFull = false;
  std::string fontName;
  std::string bytes;
  QPDFObjGen fontOg;
  bool glyphUndefined = false;
  bool glyphWhitespace = false;
  bool glyphHasContour = true;
  bool mappedToUnicode = true;
  bool layerHidden = false;
  std::vector<size_t> src;
};

struct ImageEvent {
  double ppi = 0;
  int bpc = 8;
  int width = 0;
  int height = 0;
  bool mask = false;
  bool hasSMask = false;
  bool interpolate = false;
  bool inlineImg = false;
  size_t inlineSize = 0;
  std::string inlineSamples;
  GsExtra x;
  bool overprint = false;
  int opm = 0;
  bool transparency = false;
  QPDFObjectHandle obj;
  bool layerHidden = false;
  std::set<std::string> filters;
  std::vector<double> comps;
  ColorInfo color;
  int page = 0;
  Box bbox;
  Box clip;
  bool visible = true;
  bool covers = false;
  bool clippedPart = false;
  bool clippedFull = false;
  std::vector<size_t> src;
};

struct FontFacts {
  std::string baseFont;
  std::string subtype;
  bool embedded = false;
  bool symbolic = false;
  bool hasFlags = false;
  bool type3 = false;
  bool cid = false;
  bool cid0 = false;
  bool openType = false;
  bool trueType = false;
  bool hasCIDToGIDMap = true;
  bool hasToUnicode = false;
  bool hasEncodingDict = false;
  std::string encodingName;
  bool subsetName = false;
  std::string fontProgram;
  std::vector<double> widths;
  int firstChar = 0;
  double ascentEm = 0.80;
  double descentEm = -0.20;
  bool hasVMetrics = false;
  bool ftValid = true;
  bool ftLoaded = false;
  int fsType = 0;
  int cmapCount = 0;
  bool anyUndefinedGlyph = false;
  bool anyWidthMismatch = false;
  bool allUsedMapped = true;
  QPDFObjGen og;
  QPDFObjectHandle dict;
};

struct AnnotFacts {
  std::string subtype;
  bool hasCA = false;
  double ca = 1.0;
  bool printFlag = false;
  bool knownType = true;
  Box rect;
  int page = 0;
  QPDFObjectHandle obj;
};

struct PageFacts {
  bool hasMediaBox = false;
  bool hasCropBox = false;
  bool cropEqualsMedia = true;
  bool scaled = false;
  bool rotated = false;
  bool empty = true;
  int imageCount = 0;
  int page = 0;
  bool hasTransGroup = false;
  bool hasTransObj = false;
  bool contentCompressed = true;
  bool hasPageOI = false;
  bool hasProcStepsLayer = false;
  double wPt = 0, hPt = 0;
  double inkCoverage = -1;
  Box media, trim, bleed, art, crop;
  std::set<std::string> plates;
  QPDFObjectHandle obj;
  bool layerHidden = false;
};

struct ShapeDef {
  std::string box, check;
  double offset = 0;
};

struct Events {
  std::set<QPDFObjGen> formPath;
  int formScans = 0;
  std::vector<PaintEvent> paints;
  std::vector<TextEvent> texts;
  std::vector<ImageEvent> images;
  std::vector<PageFacts> pages;
  std::vector<FontFacts> fonts;
  std::vector<AnnotFacts> annots;
  std::set<std::string> baseFonts;
  std::set<std::string> annotTypes;
  std::set<std::string> contentOps;
  mutable std::map<int, std::string> pageText;
  mutable std::map<QPDFObjGen, std::map<uint32_t, std::u32string>> toUnicode;
  mutable std::map<QPDFObjGen, std::map<std::u32string, int>> toUnicodeDests;
  std::set<std::string> syntaxIssues;
  bool syntaxScanned = false;
  size_t maxContentString = 0;
  int maxFormNest = 0;
  std::set<QPDFObjGen> bdcFiles;
  int bdcDirectFiles = 0;
  struct ObjStats {
    bool done = false;
    bool anyNegInt = false, anyPosInt = false, anyNegReal = false, anyPosReal = false;
    double negInt = 0, posInt = 0, negReal = 0, posReal = 0, negRealNearZero = 0, posRealNearZero = 0;
    int maxArr = 0, maxDict = 0, maxName = 0;
    size_t maxStr = 0, maxStream = 0;
    std::set<int> nameLens;
    std::set<std::string> filters;
    bool cryptFilter = false;
    std::set<std::string> cryptNames;
    bool refMissing = false, dlBad = false, compressedNonTag = false;
  };
  mutable ObjStats objStats;
  std::set<std::string> spotPlates;
  std::map<std::string, std::set<std::string>> spotAlternates;
  std::vector<std::string> cxfLevels;
  struct BarcodeFact {
    int page = 0;
    std::string format, text;
    Box box;
    int rotation = 0;
    double moduleWidthPt = 0, quietZonePt = 0, contrast = 0;
    double barReductionPt = 0, barReductionPct = 0;
    bool barReductionKnown = false;
    bool linear = false;
  };
  std::vector<BarcodeFact> barcodes;
  bool barcodesScanned = false;
  QPDFObjectHandle root;
  QPDFObjectHandle trailer;
  std::string header;
  double filesize = 0;
  int pagesWithMediaBox = 0;
  int pageCount = 0;
  bool hasOutputIntent = false;
  int outputIntentCount = 0;
  std::string iccColorSpace;
  std::string iccProfileId;
  int iccVersionMajor = 0;
  std::string pdfVersion;
  std::string infoCreator, infoProducer, infoTrapped;
  bool infoHasPdfxFields = false;
  bool dataAfterEof = false;
  bool requirementsPdf20 = false;
  bool hasDPartRoot = false;
  bool hasOCProperties = false;
  bool ocHasConfigs = false;
  bool hasSigFields = false;
  bool docHasProcSteps = false;
  bool hasStructTree = false;
  bool hasParentTree = false;
  bool hasTransferCurve = false;
  bool hasHalftoneDict = false;
  bool hasPostScriptXObject = false;
  bool tpBlend = false;
  bool tpSMaskImg = false;
  bool tpSMaskGs = false;
  bool tpAlphaFill = false;
  bool tpAlphaStroke = false;
  bool hasTransparencyAnywhere = false;
  bool encrypted = false;
  bool damaged = false;
  int qpdfWarnings = 0;
  mutable std::vector<std::string> qpdfWarningText;
  std::string xmpRaw;
  std::set<int> pageParseFailed;
  bool trackSrc = false;
  std::vector<size_t> srcPath;
  std::set<QPDFObjGen> foreignForms;
  std::function<bool(int, double, int&, int&, std::string&)> raster;
  mutable std::shared_ptr<void> spatialCache;
  std::map<std::string, ShapeDef> shapes;
  const struct PfProfile* prof = nullptr;
  std::string inputPath;
  const unsigned char* inputData = nullptr;
  std::size_t inputSize = 0;
  mutable std::shared_ptr<void> extCache;
};

struct Gs {
  Mat ctm;
  double lineWidth = 1.0;
  int renderMode = 0;
  double fontSize = 0;
  double tmScale = 1.0;
  std::vector<double> fill{0};
  std::vector<double> stroke{0};
  ColorInfo fillColor;
  ColorInfo strokeColor;
  bool overprintFill = false;
  bool overprintStroke = false;
  int opm = 0;
  bool transparency = false;
  bool smaskActive = false;
  GsExtra x;
  std::string fontName;
  QPDFObjGen fontOg;
  Box clip;
  bool ocHidden = false;
  std::string fillPattern, strokePattern;
};

void enterGroup(GsExtra& x, QPDFObjectHandle grp, QPDFObjectHandle res);
ColorInfo classifyColor(QPDFObjectHandle cs, QPDFObjectHandle res, int depth = 0);
void scanEvents(QPDFObjectHandle contents, QPDFObjectHandle res, const Gs& initial,
                int page, int depth, Visited& seen, Events& ev);

}
