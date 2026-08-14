#pragma once

#include <string>
#include <vector>

#include "ctx.hh"
#include "pdfa/pdfa.hh"

namespace pdfa {
void addSignaturePlaceholder(Ctx& ctx);

bool applySignature(const Options& opt, std::vector<unsigned char>& pdf, std::string& err);
}
