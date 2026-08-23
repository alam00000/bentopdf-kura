#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

namespace pdfa {
struct PfAtom {
  std::string token;
  std::string op;
  std::vector<std::string> vals;
};

struct PfCondition {
  std::vector<PfAtom> atoms;
};

struct PfRule {
  std::string id;
  std::string name;
  int severity = 1;
  int logic = 0;
  int scope = 0;
  std::vector<std::string> condIds;
};

struct PfBuiltin {
  std::string name;
  int severity = 1;
  std::map<std::string, double> params;
};

struct PfProfile {
  std::string name;
  std::vector<PfBuiltin> builtins;
  std::map<std::string, PfCondition> conds;
  std::vector<PfRule> rules;
};

struct PfFix {
  std::string op;
  std::vector<std::string> params;
};

std::vector<PfFix> collectFixes(const std::string& text);

bool parseKuraJson(const std::string& text, PfProfile& out);
bool parsePreflightXml(const std::string& text, PfProfile& out);

}
