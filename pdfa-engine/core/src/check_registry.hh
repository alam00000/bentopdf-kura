#pragma once

#include <string>
#include <vector>

#include "profile_types.hh"

namespace pdfa {
struct Events;
struct PaintEvent;
struct TextEvent;
struct ImageEvent;
struct PageFacts;
struct AnnotFacts;
struct FontFacts;

enum class Domain { kNone, kPaint, kText, kImage, kDoc, kPage, kAny, kAnnot, kFont };

struct ParamDef {
  const char* name;
  const char* type;
  const char* defaultValue;
  bool hasDefault;
  bool required;
  const char* enums;
};

struct CheckDef {
  const char* id;
  const char* group;
  const char* title;
  const char* value;
  Domain domain;
  const char* operators;
  const ParamDef* params;
  int paramCount;
};

struct FixDef {
  const char* id;
  const char* group;
  const char* title;
  const ParamDef* params;
  int paramCount;
};

struct OutputCodeDef {
  const char* code;
  const char* meaning;
};

int checkIndex(const std::string& id);
int fixIndex(const std::string& id);
const CheckDef& checkDef(int index);
const ParamDef* findParam(const ParamDef* params, int count, const std::string& name);

double lengthToPt(const std::string& text, const std::string& defaultUnit, bool* ok = nullptr);
double parseQuantity(const std::string& text, const std::string& type, const std::string& unit, bool* ok = nullptr);

std::string checkAtom(PfAtom& a, const std::string& where);
std::string checkFix(PfFix& f, const std::string& where);

const std::string& param(const PfAtom& a, const std::string& name);
double paramNum(const PfAtom& a, const std::string& name);
bool paramBool(const PfAtom& a, const std::string& name);
std::vector<std::string> paramList(const PfAtom& a, const std::string& name);
const std::string& fixParam(const PfFix& f, const std::string& name);
double fixParamNum(const PfFix& f, const std::string& name);
bool fixParamBool(const PfFix& f, const std::string& name);
std::vector<std::string> fixParamList(const PfFix& f, const std::string& name);

struct Value {
  enum Kind { kNone, kBool, kNum, kText, kList, kSize };
  Kind kind = kNone;
  bool b = false;
  double num = 0;
  double h = 0;
  std::string text;
  std::vector<std::string> list;

  Value() = default;
  Value(bool v) : kind(kBool), b(v) {}
  Value(int v) : kind(kNum), num(v) {}
  Value(unsigned v) : kind(kNum), num(v) {}
  Value(float v) : kind(kNum), num(v) {}
  Value(long v) : kind(kNum), num(static_cast<double>(v)) {}
  Value(long long v) : kind(kNum), num(static_cast<double>(v)) {}
  Value(unsigned long v) : kind(kNum), num(static_cast<double>(v)) {}
  Value(unsigned long long v) : kind(kNum), num(static_cast<double>(v)) {}
  Value(double v) : kind(kNum), num(v) {}
  Value(const char* v) : kind(kText), text(v ? v : "") {}
  Value(std::string v) : kind(kText), text(std::move(v)) {}
  Value(std::vector<std::string> v) : kind(kList), list(std::move(v)) {}
  static Value size(double widthPt, double heightPt) {
    Value v;
    v.kind = kSize;
    v.num = widthPt;
    v.h = heightPt;
    return v;
  }
};

int compare(const Value& v, const PfAtom& a);

struct Subject {
  Domain kind = Domain::kNone;
  int page = 0;
  const PaintEvent* paint = nullptr;
  const TextEvent* text = nullptr;
  const ImageEvent* image = nullptr;
  const PageFacts* pageFacts = nullptr;
  const AnnotFacts* annot = nullptr;
  const FontFacts* font = nullptr;
};

Subject subjectFor(Domain kind, const void* object, int page, const Events& ev);

using CheckFn = int (*)(const PfAtom& a, const Subject& s, const Events& ev, bool& supported);

class CheckTable {
 public:
  void add(const char* id, CheckFn fn);
  CheckFn find(int check) const;

 private:
  std::vector<CheckFn> fns_;
};

void registerColorChecks(CheckTable& t);
void registerObjectChecks(CheckTable& t);
void registerExtChecks(CheckTable& t);
void registerSpatialChecks(CheckTable& t);

const CheckTable& checkTable();
Domain domainOf(const PfAtom& a);
int evalCheck(const PfAtom& a, const Subject& s, const Events& ev, bool& supported);

}
