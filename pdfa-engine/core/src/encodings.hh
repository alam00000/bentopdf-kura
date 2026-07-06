#pragma once

#include <cstdint>
#include <string>

namespace pdfa {
uint16_t winAnsiToUnicode(int code);
uint16_t macRomanToUnicode(int code);
uint16_t standardToUnicode(int code);
uint16_t symbolToUnicode(int code);
uint16_t zapfDingbatsToUnicode(int code);
const char* symbolCodeToName(int code);
std::string zapfCodeToName(int code);
}
