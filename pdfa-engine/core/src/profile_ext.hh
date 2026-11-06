#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "profile_eval.hh"

namespace pdfa {

bool xmpWellFormed(const std::string& packet);
bool xmlRootOf(const std::string& xml, std::string& ns, std::string& local);
double barWidthReduction(const std::vector<int>& runs, bool firstDark, double& modulePx);

std::vector<std::pair<QPDFObjectHandle, std::string>> metadataPackets(const Events& ev, const std::string& scope);
void registerXmpChecks(CheckTable& t);

std::vector<std::string> fontProgramProblems(const FontFacts& f, const Events& ev, bool strict);

struct DocumentPart {
  QPDFObjectHandle dict;
  int depth = 0;
  int parent = -1;
  int start = -1, end = -1;
  bool leaf = false;
};
const std::vector<DocumentPart>& documentParts(const Events& ev);
std::shared_ptr<void>& extTaggingSlot(const Events& ev);

struct MarkedState {
  std::vector<std::string> layers;
  std::vector<QPDFObjectHandle> ocConds;
  bool artifact = false;
  std::string artifactType, artifactSubtype;
  bool tagged = false;
  bool artifactInsideTagged = false;
  bool taggedInsideArtifact = false;
  long long mcid = -1;
};
const MarkedState* markedStateOf(const Subject& s, const Events& ev, bool& known);
std::vector<QPDFObjectHandle> ocgsOf(QPDFObjectHandle cond);
bool objectOcConds(const Subject& s, const Events& ev, std::vector<QPDFObjectHandle>& out);
const std::map<int, std::map<long long, int>>& pageMarkedContentIds(const Events& ev);
const std::map<int, std::set<long long>>& pageTextMarkedContentIds(const Events& ev);
const std::set<int>& pagesWithUntaggedText(const Events& ev);
void registerStructureChecks(CheckTable& t);
}
