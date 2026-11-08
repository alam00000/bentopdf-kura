#pragma once

#include <qpdf/QPDFPageObjectHelper.hh>

#include <map>
#include <set>

#include "ctx.hh"

namespace pdfa {
bool actionAllowed(Ctx& ctx, QPDFObjectHandle action);
bool nextChainAllowed(Ctx& ctx, QPDFObjectHandle action, int depth);
bool rasterFlattenPage(Ctx& ctx, QPDFPageObjectHelper& ph, int pageIndex,
                       bool preserveText = false, double dpi = 0);
void completeResources(Ctx& ctx);
std::string invisibleTextLayer(QPDFPageObjectHelper& ph, int& runs);
QPDFObjectHandle buildIccStream(Ctx& ctx, const unsigned char* data, unsigned int len, int n);
bool aaForbidden(Ctx& ctx, QPDFObjectHandle aa, const std::string& where);
void sanitizeAdditionalActions(Ctx& ctx, QPDFObjectHandle dict, const std::string& where,
                               bool aaAllowed);

void passAnalyze(Ctx& ctx);
void passOutlineFonts(Ctx& ctx);
void passProfile(Ctx& ctx, const unsigned char* inputData, std::size_t inputSize);
void applyProfileFixes(Options& opt, std::vector<Issue>& notes);
void passProfileFixups(Ctx& ctx);
bool isContentFix(const std::string& id);
void passStructure(Ctx& ctx);
void passPages(Ctx& ctx);
void passImageResolution(Ctx& ctx);
void passCompleteResources(Ctx& ctx);
void passColor(Ctx& ctx);
void passDowngradeIcc(Ctx& ctx);
void passPrint(Ctx& ctx);
void passFonts(Ctx& ctx);
void passGlyphClean(Ctx& ctx);
void passMetadata(Ctx& ctx);
void passLimits(Ctx& ctx);
void checkFileSyntax(Ctx& ctx, const unsigned char* data, std::size_t size, bool encrypted);
void passTagging(Ctx& ctx);
void passSemanticRepair(Ctx& ctx, bool mergeHeadings = true);
void passBookmarks(Ctx& ctx);
bool pageHasTransparency(Ctx& ctx, QPDFPageObjectHelper& ph);
void flattenOptionalContent(Ctx& ctx, std::vector<QPDFPageObjectHelper>& pages, bool keepHidden,
                            long long& droppedMarks, long long& droppedObjects);
bool vectorFlattenPage(Ctx& ctx, QPDFPageObjectHelper& ph, int pageIndex, int* regionsOut = nullptr,
                       double dpi = 0);
bool resourcesHaveTransparency(QPDFObjectHandle res);
bool resourcesOverprint(QPDFObjectHandle res, std::set<QPDFObjGen>& seen, int depth = 0);
bool resourcesHaveTransparency(QPDFObjectHandle res, std::map<QPDFObjGen, bool>& memo, int depth = 0);
void neutraliseTransparency(Ctx& ctx, QPDFObjectHandle res);
bool simpleFontAdvances(QPDFObjectHandle font, std::vector<double>& w);
Result convertOnce(const unsigned char* data, std::size_t size, const Options& options);
}
