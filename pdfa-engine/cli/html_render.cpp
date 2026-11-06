#include "tools.hh"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace kuratools {
namespace {
namespace fs = std::filesystem;

std::string findChrome() {
  std::vector<std::string> paths{"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
                                 "/Applications/Chromium.app/Contents/MacOS/Chromium",
                                 "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge"};
#ifdef _WIN32
  for (const char* env : {"ProgramW6432", "ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"})
    if (const char* d = std::getenv(env))
      for (const char* rel : {"Google\\Chrome\\Application\\chrome.exe", "Microsoft\\Edge\\Application\\msedge.exe",
                              "Chromium\\Application\\chrome.exe"})
        paths.push_back((fs::path(d) / rel).string());
#endif
  return findProgram("KURA_CHROME", {"google-chrome", "google-chrome-stable", "chromium", "chromium-browser", "chrome", "msedge"}, paths);
}

std::string fileUrl(const fs::path& p) {
  std::string s = fs::absolute(p).generic_string(), o = s.size() && s[0] == '/' ? "file://" : "file:///";
  for (unsigned char c : s) {
    if (std::isalnum(c) || std::string("/-._~:").find(char(c)) != std::string::npos) o += char(c);
    else {
      static const char hex[] = "0123456789ABCDEF";
      o += '%';
      o += hex[c >> 4];
      o += hex[c & 15];
    }
  }
  return o;
}
}

bool renderHtmlFile(const std::string& htmlPath, const std::string& outPdf, const std::string& workDir) {
  std::string ch = findChrome();
  if (ch.empty()) return false;
  std::vector<std::string> a{ch, "--headless", "--disable-gpu", "--no-first-run", "--no-default-browser-check", "--disable-extensions",
                             "--disable-sync", "--hide-scrollbars", "--run-all-compositor-stages-before-draw",
                             "--virtual-time-budget=5000", "--user-data-dir=" + (fs::path(workDir) / "chrome").string(),
                             "--no-pdf-header-footer", "--print-to-pdf-no-header", "--print-to-pdf=" + outPdf,
                             "--proxy-server=http://127.0.0.1:9", "--proxy-bypass-list=<-loopback>",
                             "--host-resolver-rules=MAP * ~NOTFOUND"};
#ifndef _WIN32
  if (geteuid() == 0) a.push_back("--no-sandbox");
#endif
  a.push_back(fileUrl(htmlPath));
  runProcess(a, 300);
  std::error_code ec;
  return fs::is_regular_file(outPdf, ec) && fs::file_size(outPdf, ec) > 0;
}
}
