#pragma once

#include <qpdf/QPDFPageObjectHelper.hh>

#include "ctx.hh"

namespace pdfa {
bool actionAllowed(Ctx& ctx, QPDFObjectHandle action);
bool rasterFlattenPage(Ctx& ctx, QPDFPageObjectHelper& ph, int pageIndex);
QPDFObjectHandle buildIccStream(Ctx& ctx, const unsigned char* data, unsigned int len, int n);
void sanitizeAdditionalActions(Ctx& ctx, QPDFObjectHandle dict, const std::string& where,
                               bool aaAllowed);

void passStructure(Ctx& ctx);
void passPages(Ctx& ctx);
void passCompleteResources(Ctx& ctx);
void passColor(Ctx& ctx);
void passPrint(Ctx& ctx);
void passFonts(Ctx& ctx);
void passGlyphClean(Ctx& ctx);
void passMetadata(Ctx& ctx);
void passLimits(Ctx& ctx);
void passTagging(Ctx& ctx);
}
