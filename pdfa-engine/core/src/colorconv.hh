#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ctx.hh"

namespace pdfa {
struct ColorValue {
  std::string family;
  std::vector<std::string> colorants;
  std::vector<double> values;
  QPDFObjectHandle space;
};
using ColorRemap = std::function<bool(const ColorValue& in, unsigned objectClass, ColorValue& out)>;

int remapContentColors(Ctx& ctx, const ColorRemap& fn);
bool colorValueToLab(const ColorValue& c, double lab[3]);
bool colorValueToProcess(const ColorValue& c, int n, std::vector<double>& out);
double deltaE2000(const double* a, const double* b);
void passColorConvert(Ctx& ctx);
QPDFObjectHandle sampledColorFunction(QPDF& pdf, int inputs, const std::vector<double>& domain, int outputs,
                                      const std::function<bool(const double*, double*)>& fn);
void prepareColorJob(Options& opt, std::vector<Issue>& notes);
bool resolveIccByName(const Options& opt, const std::string& name, std::string& bytes);
std::string iccDescription(const std::string& bytes);
}
