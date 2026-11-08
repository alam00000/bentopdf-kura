#include "uacheck.hh"

#include <sstream>

namespace pdfa {
namespace {
const char* const kForbiddenParents[] = {
    "Annot:Form L LI Ruby StructTreeRoot TBody TFoot THead TOC TOCI TR Table Warichu",
    "Art:Art BibEntry Code Em Form Formula Index L LI Lbl P Quote RB RP RT Reference Ruby Span Strong StructTreeRoot Sub TBody TFoot THead TOC TOCI TR Table Title WP WT Warichu",
    "Artifact:Ruby StructTreeRoot Warichu",
    "Aside:Aside BibEntry BlockQuote Code Em Form H Hn Index L LI Lbl P Quote RB RP RT Reference Ruby Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOC TOCI TR Table WP WT Warichu",
    "BibEntry:Art Aside BibEntry BlockQuote Document DocumentFragment FENote Index L LI RB RP RT Ruby Sect StructTreeRoot TBody TFoot THead TOC TOCI TR Table WP WT Warichu",
    "BlockQuote:BibEntry Code Em Form H Hn Index L LI Lbl P Quote RB RP RT Reference Ruby Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOC TOCI TR Table Title WP WT Warichu",
    "Caption:BibEntry Caption Code Document DocumentFragment Em H Hn LI Lbl P Quote RB RP RT Reference Ruby Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOCI TR WP WT Warichu",
    "Code:BibEntry Code Index L LI RB RP RT Reference Ruby StructTreeRoot TBody TFoot THead TOC TOCI TR Table WP WT Warichu",
    "Div:Em H Hn L Lbl P Quote RB RP RT Reference Ruby Span Strong StructTreeRoot Sub TBody TFoot THead TOC TR Table WP WT Warichu",
    "Document:Annot Art BibEntry Caption Code Em FENote Figure Form Formula H Hn Index L LBody LI Lbl Link Note P Quote RB RP RT Reference Ruby Sect Span Strong Sub TBody TD TFoot TH THead TOC TOCI TR Table Title WP WT Warichu",
    "DocumentFragment:BibEntry Em Figure Form Formula H Hn Index L LBody LI Lbl P Quote RB RP RT Reference Ruby Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOC TOCI TR Table Title WP WT Warichu",
    "Em:Art Aside BlockQuote Document DocumentFragment Form Index L LI Ruby Sect StructTreeRoot TBody TFoot THead TOC TOCI TR Table Warichu",
    "FENote:L LI RB RP RT Ruby StructTreeRoot TBody TFoot THead TOC TOCI TR Table WP WT Warichu",
    "Figure:Code L LI RB RP RT Ruby StructTreeRoot TBody TFoot THead TOC TOCI TR Table WP WT Warichu",
    "Form:BibEntry Code Form Index L LI Reference Ruby StructTreeRoot TBody TFoot THead TOC TOCI TR Table Warichu",
    "Formula:BibEntry Code L LI RB RP RT Reference Ruby StructTreeRoot TBody TFoot THead TOC TOCI TR Table WP WT Warichu",
    "H:BibEntry Code Em FENote Form H Hn L LI Lbl Note P Quote RB RP RT Reference Ruby Span Strong StructTreeRoot Sub TBody TFoot THead TOC TOCI TR Table Title WP WT Warichu",
    "Hn:BibEntry Code Em FENote Form H Hn L LI Lbl Note P Quote RB RP RT Reference Ruby Span Strong StructTreeRoot Sub TBody TFoot THead TOC TOCI TR Table Title WP WT Warichu",
    "Index:Annot BibEntry Code Em FENote Form H Hn Index L LI Lbl Link P Quote RB RP RT Reference Ruby Span Strong StructTreeRoot Sub TBody TFoot THead TOC TOCI TR Table Title WP WT Warichu",
    "L:BibEntry Code Em H Hn LI Lbl Quote RB RP RT Reference Ruby Span Strong StructTreeRoot TBody TFoot THead TOC TOCI TR Table WP WT Warichu",
    "LBody:Annot Art Aside BibEntry BlockQuote Caption Code Document DocumentFragment Em FENote Figure Form Formula H Hn Index L LBody Lbl Link Note P Quote RB RP RT Reference Ruby Sect Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOC TOCI TR Table Title WP WT Warichu",
    "LI:Annot Art Aside BibEntry BlockQuote Caption Code Document DocumentFragment Em FENote Figure Form Formula H Hn Index LBody LI Lbl Link Note P Quote RB RP RT Reference Ruby Sect Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOC TOCI TR Table Title WP WT Warichu",
    "Lbl:Code Document DocumentFragment Index L LBody Lbl RB RP RT Ruby StructTreeRoot TBody TFoot THead TOC TR Table WP WT Warichu",
    "Link:Form Index L LI Link Ruby StructTreeRoot TBody TFoot THead TOC TOCI TR Table Warichu",
    "NonStruct:StructTreeRoot",
    "Note:L LI RB RP RT Ruby StructTreeRoot TBody TFoot THead TOC TOCI TR Table WP WT Warichu",
    "P:Code Em Form H Hn L LI Lbl P Quote RB RP RT Reference Ruby Span Strong StructTreeRoot Sub TBody TFoot THead TOC TR Table WP WT Warichu",
    "Part:Em H Hn L LI Lbl P Quote RB RP RT Reference Ruby Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOC TOCI TR Table WP WT Warichu",
    "Private:Annot StructTreeRoot",
    "Quote:Art Aside BibEntry BlockQuote Code Document DocumentFragment Form Index L LI Reference Ruby Sect StructTreeRoot TBody TFoot THead TOC TOCI TR Table Warichu",
    "RB:Annot Art Aside BibEntry BlockQuote Caption Code Document DocumentFragment Em FENote Figure Form Formula H Hn Index L LBody LI Lbl Link Note P Quote RB RP RT Reference Sect Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOC TOCI TR Table Title WP WT Warichu",
    "RP:Annot Art Aside BibEntry BlockQuote Caption Code Document DocumentFragment Em FENote Figure Form Formula H Hn Index L LBody LI Lbl Link Note P Quote RB RP RT Reference Sect Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOC TOCI TR Table Title WP WT Warichu",
    "RT:Annot Art Aside BibEntry BlockQuote Caption Code Document DocumentFragment Em FENote Figure Form Formula H Hn Index L LBody LI Lbl Link Note P Quote RB RP RT Reference Sect Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOC TOCI TR Table Title WP WT Warichu",
    "Reference:Art Document DocumentFragment L LI Reference Ruby Sect StructTreeRoot TBody TFoot THead TOC TR Table Warichu",
    "Ruby:Art Aside BibEntry BlockQuote Code Document DocumentFragment Form Index L LI RB RP RT Reference Ruby Sect StructTreeRoot TBody TFoot THead TOC TOCI TR Table WP WT Warichu",
    "Sect:BibEntry Code Em Form Formula L LI Lbl P Quote RB RP RT Reference Ruby Span Strong StructTreeRoot Sub TBody TFoot THead TOC TOCI TR Table Title WP WT Warichu",
    "Span:Art Aside BlockQuote Document DocumentFragment Form Index L LI Ruby Sect StructTreeRoot TBody TFoot THead TOC TOCI TR Table Warichu",
    "Strong:Art Aside BlockQuote Document DocumentFragment Form Index L LI Ruby Sect StructTreeRoot TBody TFoot THead TOC TOCI TR Table Warichu",
    "Sub:Art Aside BibEntry BlockQuote Document DocumentFragment Form Index L LI Reference Ruby Sect StructTreeRoot Sub TBody TD TFoot TH THead TOC TOCI TR Table Title Warichu",
    "TBody:Annot Art Aside BibEntry BlockQuote Caption Code Document DocumentFragment Em FENote Figure Form Formula H Hn Index L LBody LI Lbl Link Note P Quote RB RP RT Reference Ruby Sect Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOC TOCI TR Title WP WT Warichu",
    "TD:Annot Art Aside BibEntry BlockQuote Caption Code Document DocumentFragment Em FENote Figure Form Formula H Hn Index L LBody LI Lbl Link Note P Quote RB RP RT Reference Ruby Sect Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOC TOCI Table Title WP WT Warichu",
    "TFoot:Annot Art Aside BibEntry BlockQuote Caption Code Document DocumentFragment Em FENote Figure Form Formula H Hn Index L LBody LI Lbl Link Note P Quote RB RP RT Reference Ruby Sect Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOC TOCI TR Title WP WT Warichu",
    "TH:Annot Art Aside BibEntry BlockQuote Caption Code Document DocumentFragment Em FENote Figure Form Formula H Hn Index L LBody LI Lbl Link Note P Quote RB RP RT Reference Ruby Sect Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOC TOCI Table Title WP WT Warichu",
    "THead:Annot Art Aside BibEntry BlockQuote Caption Code Document DocumentFragment Em FENote Figure Form Formula H Hn Index L LBody LI Lbl Link Note P Quote RB RP RT Reference Ruby Sect Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOC TOCI TR Title WP WT Warichu",
    "TOC:Annot BibEntry Caption Code Em FENote Figure Form Formula H Hn Index L LBody LI Lbl Link Note P Quote RB RP RT Reference Ruby Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TR Table Title WP WT Warichu",
    "TOCI:Annot Art Aside BibEntry BlockQuote Caption Code Document DocumentFragment Em FENote Figure Form Formula H Hn Index L LBody LI Lbl Link Note P Quote RB RP RT Reference Ruby Sect Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOCI TR Table Title WP WT Warichu",
    "TR:Annot Art Aside BibEntry BlockQuote Caption Code Document DocumentFragment Em FENote Figure Form Formula H Hn Index L LBody LI Lbl Link Note P Quote RB RP RT Reference Ruby Sect Span Strong StructTreeRoot Sub TD TH TOC TOCI TR Title WP WT Warichu",
    "Table:BibEntry Code Em H Hn L LI Lbl Quote RB RP RT Reference Ruby Span Strong StructTreeRoot Sub TBody TFoot THead TOC TOCI TR Table WP WT Warichu",
    "Title:Aside BibEntry BlockQuote Caption Code Em FENote Figure Form Formula H Hn Index L LBody LI Lbl Note P Quote RB RP RT Reference Ruby Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOC TOCI TR Table Title WP WT Warichu",
    "WP:Annot Art Aside BibEntry BlockQuote Caption Code Document DocumentFragment Em FENote Figure Form Formula H Hn Index L LBody LI Lbl Link Note P Quote RB RP RT Reference Ruby Sect Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOC TOCI TR Table Title WP WT",
    "WT:Annot Art Aside BibEntry BlockQuote Caption Code Document DocumentFragment Em FENote Figure Form Formula H Hn Index L LBody LI Lbl Link Note P Quote RB RP RT Reference Ruby Sect Span Strong StructTreeRoot Sub TBody TD TFoot TH THead TOC TOCI TR Table Title WP WT",
    "Warichu:Art Aside BibEntry BlockQuote Code Document DocumentFragment Form Index L LI RB RP RT Reference Ruby Sect StructTreeRoot TBody TFoot THead TOC TOCI TR Table WP WT Warichu",
};

const char* const kKidLimits[] = {
    "Art:H?",
    "Artifact:Caption? H?",
    "Aside:Caption? H?",
    "BlockQuote:Caption? H?",
    "Caption:H?",
    "Document:H?",
    "DocumentFragment:H?",
    "Figure:Caption? H?",
    "Form:Caption?",
    "Formula:Caption? H?",
    "H:Art? Sect?",
    "Hn:Art? Sect?",
    "L:Caption?",
    "LBody:Caption? H?",
    "Sect:H?",
    "StructTreeRoot:Document!",
    "TD:H?",
    "TH:H?",
    "TOC:Caption?",
    "Table:Caption? TFoot? THead?",
    "Title:Caption?",
};

const char* const kNoContent = "Art Document DocumentFragment Index L StructTreeRoot Sect TBody TFoot THead TOC TOCI TR Table";

std::set<std::string> words(const std::string& s) {
  std::set<std::string> out;
  std::istringstream in(s);
  for (std::string w; in >> w;) out.insert("/" + w);
  return out;
}
}

const NestingTable& nestingTable() {
  static const NestingTable t = [] {
    NestingTable n;
    for (const char* row : kForbiddenParents) {
      std::string r(row);
      size_t colon = r.find(':');
      n.forbiddenParents["/" + r.substr(0, colon)] = words(r.substr(colon + 1));
    }
    for (const char* row : kKidLimits) {
      std::string r(row);
      size_t colon = r.find(':');
      for (const std::string& w : words(r.substr(colon + 1))) {
        n.kidLimits["/" + r.substr(0, colon)][w.substr(0, w.size() - 1)] = w.back() == '!';
      }
    }
    n.noContent = words(kNoContent);
    return n;
  }();
  return t;
}
}
