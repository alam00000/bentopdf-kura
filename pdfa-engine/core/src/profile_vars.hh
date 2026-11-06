#pragma once

#include <map>
#include <string>
#include <vector>

#include "profile_types.hh"

class QPDF;

namespace pdfa {
struct ProfileVariable {
  std::string name;
  std::string label;
  std::string type;
  std::string defaultValue;
  std::string script;
  std::vector<std::string> choices;
};

using VarMap = std::map<std::string, std::string>;

std::vector<ProfileVariable> listProfileVariables(const std::string& profileJson);
std::vector<ProfileVariable> listProfileVariables(const Json& profile);
VarMap profileVariableDefaults(const std::string& profileJson, const VarMap& overrides);
bool scriptEngineAvailable();
std::string docFactsJson(QPDF& pdf, const std::string& name = std::string());
bool evalScript(const std::string& script, const std::string& docJson, VarMap& vars, std::string& result,
                std::string& err, const std::string& resultsJson = "[]");
VarMap resolveVariables(const std::vector<ProfileVariable>& defs, const std::string& docJson,
                        const VarMap& overrides, std::vector<std::string>& notes,
                        const std::string& resultsJson = "[]");
std::string substituteVariables(const std::string& text, const VarMap& values);
void substituteVariables(Json& profile, const VarMap& values);
std::string jsonText(const Json& j);
}
