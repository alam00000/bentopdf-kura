#pragma once

#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <map>
#include <string>
#include <vector>

#include "ctx.hh"
#include "profile_events.hh"

namespace pdfa {
int layerObjectsByRules(Ctx& ctx, std::vector<QPDFPageObjectHelper>& pages, const std::vector<std::string>& ruleIds,
                        QPDFObjectHandle ocg, int& unmovable);
bool objectBoxesByRules(Ctx& ctx, const std::vector<std::string>& ruleIds, std::map<int, std::vector<Box>>& out);
}
