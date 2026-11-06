#pragma once

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pdfa {
struct Json {
  enum Type { kNull, kBool, kNum, kStr, kArr, kObj } type = kNull;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;

  const Json* get(const std::string& key) const;
  std::string text() const;
};

bool parseJson(const std::string& text, Json& out, std::string& error);

struct PfAtom {
  std::string token;
  std::string op;
  std::vector<std::string> vals;
  std::map<std::string, std::string> params;
  std::vector<std::string> refs;
  double tol = 0;
  bool negate = false;
  bool icase = false;
  std::string countOp;
  std::vector<double> nums;
  int check = -1;
};

struct PfCondition {
  std::vector<PfAtom> atoms;
};

struct PfRule {
  std::string id;
  std::string name;
  std::string description;
  int severity = 1;
  int logic = 0;
  int scope = 0;
  bool silent = false;
  std::string reportDocHits;
  std::string reportPageHits;
  long long maxHitsPerPage = 0;
  std::vector<std::string> condIds;
};

struct PfFix {
  std::string id;
  std::map<std::string, std::string> args;
  std::vector<std::string> onlyWhere;
  int fix = -1;
};

struct PfShape {
  std::string box, check;
  double offset = 0;
};

struct PfProfile {
  std::string name;
  std::string description;
  std::string error;
  std::map<std::string, PfCondition> conds;
  std::vector<PfRule> rules;
  std::map<std::string, PfRule> ruleIndex;
  std::vector<PfFix> fixes;
  std::map<std::string, PfShape> shapes;
};


bool parseKuraJson(const std::string& text, PfProfile& out);

}
