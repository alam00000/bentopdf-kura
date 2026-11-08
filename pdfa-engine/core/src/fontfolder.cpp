#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "pdfa/pdfa.hh"

namespace pdfa {
namespace {
std::string fontKey(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  s.erase(std::remove_if(s.begin(), s.end(),
                         [](unsigned char c) { return c == ' ' || c == '-' || c == '_'; }),
          s.end());
  return s;
}
}  // namespace

bool loadFontFromFolder(const std::string& folder, const std::string& wanted,
                        std::string& psName, std::string& bytes) {
  std::error_code ec;
  const std::string want = fontKey(wanted);
  if (want.empty()) return false;
  for (const auto& e : std::filesystem::recursive_directory_iterator(
           folder, std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (ec) break;
    if (!e.is_regular_file(ec)) continue;
    std::string ext = e.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".ttf" && ext != ".ttc" && ext != ".otf") continue;
    if (fontKey(e.path().stem().string()) != want) continue;
    std::ifstream f(e.path(), std::ios::binary);
    if (!f) continue;
    bytes.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    psName = e.path().stem().string();
    return !bytes.empty();
  }
  return false;
}

bool loadCjkFontFromFolder(const std::string& folder, const std::string& script, bool bold,
                           std::string& bytes) {
  std::ifstream in(std::filesystem::path(folder) / ("NotoSans" + script + (bold ? "-Bold.ttf" : "-Regular.ttf")),
                   std::ios::binary);
  if (!in) return false;
  bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return bytes.size() > 64;
}
}  // namespace pdfa
