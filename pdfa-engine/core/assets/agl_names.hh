#pragma once

#include <cstdint>
#include <string>

namespace pdfa {
uint32_t aglNameToUnicode(const std::string& name);
std::string unicodeToAglName(uint32_t uni);
}
