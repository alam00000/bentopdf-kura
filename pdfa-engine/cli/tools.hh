#pragma once

#include <string>
#include <vector>

namespace kuratools {
constexpr int kUnknownTool = -1;
constexpr int kSpawnFail = -1, kTimedOut = -2;
int runTool(int argc, char** argv);
int runProcess(const std::vector<std::string>& argv, int timeoutSec, std::string* out = nullptr);
std::string findProgram(const char* env, std::vector<std::string> names, const std::vector<std::string>& paths);
int toolOutput(const std::string& name, const std::vector<std::string>& args);
bool renderHtmlFile(const std::string& htmlPath, const std::string& outPdf, const std::string& workDir);
int toolReport(const std::string& name, const std::vector<std::string>& args);
bool writeReport(const std::string& path, const std::string& resultJson, const std::string& dictPath,
                 const std::string& lang, std::string& err);
}
