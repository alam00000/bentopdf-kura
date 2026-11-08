#pragma once

#include <qpdf/QPDFObjectHandle.hh>

#include <cstdint>
#include <map>
#include <string>

#include "ctx.hh"

namespace pdfa {
bool embedWorldSubstitute(Ctx& ctx, QPDFObjectHandle font);
std::map<uint32_t, std::string> worldToUnicodeMap(QPDFObjectHandle tu);
std::string worldToUnicodeCMap(const std::map<uint32_t, std::string>& entries, bool twoByte);
bool loadWorldFontFromFolder(const std::string& folder, const std::string& name, std::string& bytes);
}
