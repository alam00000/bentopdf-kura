#pragma once

#include "ctx.hh"

namespace pdfa {
void convertColorsX1a(Ctx& ctx);
bool convertImageToProcess(Ctx& ctx, QPDFObjectHandle image);
}
