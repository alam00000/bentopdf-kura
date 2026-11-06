#include "tools.hh"

#include <qpdf/JSON.hh>
#include <qpdf/Pl_Buffer.hh>
#include <qpdf/Pl_Flate.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFCryptoProvider.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/QUtil.hh>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "pdfa/pdfa.hh"
#include "../core/src/einvoice.hh"

#ifdef KURA_WITH_PDFIUM
#include "kura/raster.hh"
#endif

namespace {
constexpr const char* kCertKey = "/BentoPDFKura";

struct Item {
  std::string code, detail;
  bool fixed = false;
  int severity = 0;
  std::vector<int> pages;
};

struct Result {
  std::string file, output, level, engine, errorCode, error, mode, jobId;
  bool ok = false, compliant = false, hasCompliant = false;
  long findings = -1;
  std::vector<Item> issues, analysis;
  bool hasAnalysis = false;
};

struct Col {
  std::string key;
  double w = 1;
};

struct Section {
  Section(std::string i = "", std::string t = "") : id(std::move(i)), title(std::move(t)) {}
  std::string id, title;
  int status = 0;
  std::string statusText;
  std::vector<std::pair<std::string, std::string>> kv;
  std::vector<Col> cols;
  std::vector<std::vector<std::string>> rows;
  std::string empty;
};

const char* const kLangs[] = {"en", "de", "fr", "es", "nl", "it"};

struct TrRow {
  const char* key;
  const char* v[6];
};

const TrRow kStrings[] = {
    {"title", {"Preflight report", "Preflight-Bericht", "Rapport de contrôle en amont", "Informe de verificación previa", "Preflightrapport", "Rapporto di verifica preliminare"}},
    {"summary", {"Summary", "Zusammenfassung", "Résumé", "Resumen", "Samenvatting", "Riepilogo"}},
    {"file", {"File", "Datei", "Fichier", "Archivo", "Bestand", "File"}},
    {"output", {"Output file", "Ausgabedatei", "Fichier de sortie", "Archivo de salida", "Uitvoerbestand", "File di output"}},
    {"level", {"Standard", "Standard", "Norme", "Norma", "Norm", "Standard"}},
    {"engine", {"Engine", "Engine", "Moteur", "Motor", "Engine", "Motore"}},
    {"date", {"Date", "Datum", "Date", "Fecha", "Datum", "Data"}},
    {"job", {"Job ID", "Auftrags-ID", "ID de tâche", "ID de trabajo", "Taak-ID", "ID lavoro"}},
    {"result", {"Result", "Ergebnis", "Résultat", "Resultado", "Resultaat", "Risultato"}},
    {"pass", {"Compliant", "Konform", "Conforme", "Conforme", "Conform", "Conforme"}},
    {"fail", {"Not compliant", "Nicht konform", "Non conforme", "No conforme", "Niet conform", "Non conforme"}},
    {"done", {"Processed successfully", "Erfolgreich verarbeitet", "Traité avec succès", "Procesado correctamente", "Succesvol verwerkt", "Elaborato correttamente"}},
    {"failed", {"Processing failed", "Verarbeitung fehlgeschlagen", "Échec du traitement", "Error de procesamiento", "Verwerking mislukt", "Elaborazione non riuscita"}},
    {"findings", {"Findings", "Befunde", "Constats", "Hallazgos", "Bevindingen", "Rilievi"}},
    {"error", {"Error", "Fehler", "Erreur", "Error", "Fout", "Errore"}},
    {"issues", {"Issues", "Probleme", "Problèmes", "Problemas", "Problemen", "Problemi"}},
    {"analysis", {"Analysis", "Analyse", "Analyse", "Análisis", "Analyse", "Analisi"}},
    {"code", {"Code", "Code", "Code", "Código", "Code", "Codice"}},
    {"detail", {"Description", "Beschreibung", "Description", "Descripción", "Beschrijving", "Descrizione"}},
    {"severity", {"Severity", "Schweregrad", "Gravité", "Gravedad", "Ernst", "Gravità"}},
    {"pages", {"Pages", "Seiten", "Pages", "Páginas", "Pagina's", "Pagine"}},
    {"fixed", {"Fixed", "Behoben", "Corrigé", "Corregido", "Hersteld", "Corretto"}},
    {"yes", {"Yes", "Ja", "Oui", "Sí", "Ja", "Sì"}},
    {"no", {"No", "Nein", "Non", "No", "Nee", "No"}},
    {"sev3", {"Error", "Fehler", "Erreur", "Error", "Fout", "Errore"}},
    {"sev2", {"Warning", "Warnung", "Avertissement", "Advertencia", "Waarschuwing", "Avviso"}},
    {"sev1", {"Info", "Info", "Info", "Info", "Info", "Info"}},
    {"none", {"No issues found", "Keine Probleme gefunden", "Aucun problème trouvé", "No se encontraron problemas", "Geen problemen gevonden", "Nessun problema rilevato"}},
    {"document", {"Document", "Dokument", "Document", "Documento", "Document", "Documento"}},
    {"inventory", {"Inventory", "Inventar", "Inventaire", "Inventario", "Inventaris", "Inventario"}},
    {"pagecount", {"Page count", "Seitenanzahl", "Nombre de pages", "Número de páginas", "Aantal pagina's", "Numero di pagine"}},
    {"fonts", {"Fonts", "Schriften", "Polices", "Fuentes", "Lettertypen", "Font"}},
    {"images", {"Images", "Bilder", "Images", "Imágenes", "Afbeeldingen", "Immagini"}},
    {"colorspaces", {"Colour spaces", "Farbräume", "Espaces colorimétriques", "Espacios de color", "Kleurruimten", "Spazi colore"}},
    {"spots", {"Spot colours", "Sonderfarben", "Couleurs d'accompagnement", "Colores directos", "Steunkleuren", "Colori spot"}},
    {"icc", {"ICC profiles", "ICC-Profile", "Profils ICC", "Perfiles ICC", "ICC-profielen", "Profili ICC"}},
    {"annotations", {"Annotations", "Anmerkungen", "Annotations", "Anotaciones", "Annotaties", "Annotazioni"}},
    {"layers", {"Layers", "Ebenen", "Calques", "Capas", "Lagen", "Livelli"}},
    {"name", {"Name", "Name", "Nom", "Nombre", "Naam", "Nome"}},
    {"type", {"Type", "Typ", "Type", "Tipo", "Type", "Tipo"}},
    {"embedded", {"Embedded", "Eingebettet", "Incorporée", "Incrustada", "Ingesloten", "Incorporato"}},
    {"size", {"Size (px)", "Größe (px)", "Taille (px)", "Tamaño (px)", "Grootte (px)", "Dimensioni (px)"}},
    {"ppi", {"Resolution (ppi)", "Auflösung (ppi)", "Résolution (ppp)", "Resolución (ppp)", "Resolutie (ppi)", "Risoluzione (ppi)"}},
    {"colorspace", {"Colour space", "Farbraum", "Espace colorimétrique", "Espacio de color", "Kleurruimte", "Spazio colore"}},
    {"bpc", {"Bits", "Bits", "Bits", "Bits", "Bits", "Bit"}},
    {"filter", {"Compression", "Komprimierung", "Compression", "Compresión", "Compressie", "Compressione"}},
    {"count", {"Count", "Anzahl", "Nombre", "Cantidad", "Aantal", "Numero"}},
    {"components", {"Components", "Komponenten", "Composantes", "Componentes", "Componenten", "Componenti"}},
    {"version", {"Version", "Version", "Version", "Versión", "Versie", "Versione"}},
    {"outputintent", {"Output intent", "Output Intent", "Mode de sortie", "Intención de salida", "Uitvoerintentie", "Intento di output"}},
    {"invoice", {"E-invoice", "E-Rechnung", "Facture électronique", "Factura electrónica", "E-factuur", "Fattura elettronica"}},
    {"format", {"Format", "Format", "Format", "Formato", "Formaat", "Formato"}},
    {"attachment", {"Attachment", "Anhang", "Pièce jointe", "Adjunto", "Bijlage", "Allegato"}},
    {"number", {"Invoice number", "Rechnungsnummer", "Numéro de facture", "Número de factura", "Factuurnummer", "Numero fattura"}},
    {"doctype", {"Document type", "Dokumentart", "Type de document", "Tipo de documento", "Documenttype", "Tipo documento"}},
    {"issued", {"Issue date", "Rechnungsdatum", "Date d'émission", "Fecha de emisión", "Factuurdatum", "Data di emissione"}},
    {"due", {"Due date", "Fälligkeitsdatum", "Date d'échéance", "Fecha de vencimiento", "Vervaldatum", "Data di scadenza"}},
    {"currency", {"Currency", "Währung", "Devise", "Moneda", "Valuta", "Valuta"}},
    {"seller", {"Seller", "Verkäufer", "Vendeur", "Vendedor", "Verkoper", "Venditore"}},
    {"buyer", {"Buyer", "Käufer", "Acheteur", "Comprador", "Koper", "Acquirente"}},
    {"address", {"Address", "Anschrift", "Adresse", "Dirección", "Adres", "Indirizzo"}},
    {"vat", {"VAT ID", "USt-IdNr.", "N° TVA", "NIF-IVA", "Btw-nummer", "Partita IVA"}},
    {"lines", {"Line items", "Positionen", "Lignes de facture", "Líneas", "Factuurregels", "Righe"}},
    {"id", {"No.", "Nr.", "N°", "N.º", "Nr.", "N."}},
    {"qty", {"Quantity", "Menge", "Quantité", "Cantidad", "Aantal", "Quantità"}},
    {"price", {"Unit price", "Einzelpreis", "Prix unitaire", "Precio unitario", "Stukprijs", "Prezzo unitario"}},
    {"amount", {"Amount", "Betrag", "Montant", "Importe", "Bedrag", "Importo"}},
    {"taxes", {"Tax breakdown", "Steueraufschlüsselung", "Ventilation de la TVA", "Desglose de impuestos", "Btw-specificatie", "Riepilogo IVA"}},
    {"category", {"Category", "Kategorie", "Catégorie", "Categoría", "Categorie", "Categoria"}},
    {"rate", {"Rate (%)", "Satz (%)", "Taux (%)", "Tipo (%)", "Tarief (%)", "Aliquota (%)"}},
    {"basis", {"Taxable amount", "Bemessungsgrundlage", "Base imposable", "Base imponible", "Belastbaar bedrag", "Imponibile"}},
    {"tax", {"Tax", "Steuer", "Taxe", "Impuesto", "Belasting", "Imposta"}},
    {"totals", {"Totals", "Summen", "Totaux", "Totales", "Totalen", "Totali"}},
    {"net", {"Net total", "Nettosumme", "Total HT", "Total neto", "Totaal excl. btw", "Totale imponibile"}},
    {"taxtotal", {"Tax total", "Steuerbetrag", "Total TVA", "Total impuestos", "Totaal btw", "Totale imposta"}},
    {"grand", {"Grand total", "Gesamtbetrag", "Total TTC", "Total", "Totaal incl. btw", "Totale documento"}},
    {"payable", {"Amount due", "Zahlbetrag", "Montant à payer", "Importe a pagar", "Te betalen", "Importo dovuto"}},
    {"page", {"Page", "Seite", "Page", "Página", "Pagina", "Pagina"}},
    {"generated", {"Generated by", "Erstellt mit", "Généré par", "Generado por", "Gemaakt met", "Generato da"}},
};

struct Strings {
  std::map<std::string, std::string> ui, custom;
  std::string lang = "en";
  std::string tr(const std::string& k) const {
    auto it = ui.find(k);
    return it == ui.end() ? k : it->second;
  }
  std::string text(const Item& it) const {
    auto c = custom.find(it.code);
    if (c != custom.end()) return c->second;
    c = custom.find(it.detail);
    return c != custom.end() ? c->second : it.detail;
  }
};

bool readAll(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

bool writeAll(const std::string& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(out);
}

bool loadStrings(const std::string& lang, const std::string& dictPath, Strings& s, std::string& err) {
  std::string l = lang.substr(0, 2);
  std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return std::tolower(c); });
  int idx = 0;
  for (int i = 0; i < 6; ++i)
    if (l == kLangs[i]) idx = i;
  s.lang = kLangs[idx];
  for (const TrRow& r : kStrings) s.ui[r.key] = r.v[idx];
  if (dictPath.empty()) return true;
  std::string raw;
  if (!readAll(dictPath, raw)) {
    err = "cannot read dictionary " + dictPath;
    return false;
  }
  try {
    JSON j = JSON::parse(raw);
    auto take = [&](JSON obj, bool codesOnly) {
      obj.forEachDictItem([&](const std::string& k, JSON v) {
        std::string val;
        if (!v.getString(val)) return;
        if (!codesOnly && s.ui.count(k)) s.ui[k] = val;
        else s.custom[k] = val;
      });
    };
    if (!j.isDictionary()) throw std::runtime_error("not an object");
    take(j, false);
    JSON byLang = j.getDictItem(s.lang);
    if (byLang.isDictionary()) take(byLang, false);
    JSON codes = j.getDictItem("codes");
    if (codes.isDictionary()) take(codes, true);
  } catch (std::exception& e) {
    err = "bad dictionary " + dictPath + ": " + e.what();
    return false;
  }
  return true;
}

std::string jstr(JSON j, const char* k) {
  std::string v;
  j.getDictItem(k).getString(v);
  return v;
}

std::vector<Item> parseItems(JSON arr) {
  std::vector<Item> out;
  arr.forEachArrayItem([&](JSON v) {
    Item it;
    it.code = jstr(v, "code");
    it.detail = jstr(v, "detail");
    v.getDictItem("fixed").getBool(it.fixed);
    std::string n;
    if (v.getDictItem("severity").getNumber(n)) it.severity = std::atoi(n.c_str());
    v.getDictItem("pages").forEachArrayItem([&](JSON p) {
      std::string pn;
      if (p.getNumber(pn)) it.pages.push_back(std::atoi(pn.c_str()));
    });
    out.push_back(it);
  });
  return out;
}

bool parseResult(const std::string& json, Result& r, std::string& err) {
  try {
    JSON j = JSON::parse(json);
    r.file = jstr(j, "file");
    r.output = jstr(j, "output");
    r.level = jstr(j, "level");
    r.engine = jstr(j, "engine");
    r.errorCode = jstr(j, "errorCode");
    r.error = jstr(j, "error");
    r.mode = jstr(j, "mode");
    r.jobId = jstr(j, "jobId");
    j.getDictItem("ok").getBool(r.ok);
    r.hasCompliant = j.getDictItem("compliant").getBool(r.compliant);
    std::string n;
    if (j.getDictItem("findings").getNumber(n)) r.findings = std::atol(n.c_str());
    r.issues = parseItems(j.getDictItem("issues"));
    r.hasAnalysis = j.getDictItem("analysis").isArray();
    r.analysis = parseItems(j.getDictItem("analysis"));
  } catch (std::exception& e) {
    err = std::string("bad result JSON: ") + e.what();
    return false;
  }
  return true;
}

std::string pageList(const std::set<int>& pages) {
  std::string out;
  for (auto it = pages.begin(); it != pages.end();) {
    int a = *it, b = a;
    while (++it != pages.end() && *it == b + 1) b = *it;
    if (!out.empty()) out += ", ";
    out += a == 0 ? "-" : (a == b ? std::to_string(a) : std::to_string(a) + "-" + std::to_string(b));
  }
  return out;
}

std::string fmt(double v, int prec = 0) {
  char b[32];
  std::snprintf(b, sizeof b, "%.*f", prec, v);
  return b;
}

struct Inventory {
  struct Font {
    std::string name, type;
    bool embedded = false;
    std::set<int> pages;
  };
  struct Image {
    std::string name, cs, filter;
    int w = 0, h = 0, bpc = 0;
    double ppi = 0;
    std::set<int> pages;
  };
  struct Icc {
    std::string desc, cs, version;
    int n = 0;
    bool outputIntent = false;
    std::set<int> pages;
  };
  int pageCount = 0;
  std::map<std::string, Font> fonts;
  std::map<std::string, Image> images;
  std::map<std::string, std::set<int>> colorSpaces, spots;
  std::map<std::string, Icc> icc;
  std::map<std::string, std::pair<int, std::set<int>>> annots;
  std::vector<std::string> layers;
};

std::string bufStr(const std::shared_ptr<Buffer>& b) {
  return b ? std::string(reinterpret_cast<const char*>(b->getBuffer()), b->getSize()) : std::string();
}

std::string objKey(QPDFObjectHandle o) {
  return o.isIndirect() ? o.getObjGen().unparse(',') : o.unparse();
}

std::string iccDesc(const std::string& d) {
  auto u32 = [&](size_t o) -> size_t {
    if (o + 4 > d.size()) return 0;
    return (size_t(uint8_t(d[o])) << 24) | (size_t(uint8_t(d[o + 1])) << 16) |
           (size_t(uint8_t(d[o + 2])) << 8) | uint8_t(d[o + 3]);
  };
  size_t n = u32(128);
  for (size_t i = 0; i < n && i < 200; ++i) {
    size_t e = 132 + i * 12;
    if (d.compare(e, 4, "desc") != 0) continue;
    size_t off = u32(e + 4), len = u32(e + 8);
    if (off + len > d.size() || len < 12) return "";
    if (d.compare(off, 4, "desc") == 0) {
      size_t cnt = u32(off + 8);
      std::string s = d.substr(off + 12, std::min(cnt, len - 12));
      return s.substr(0, s.find('\0'));
    }
    if (d.compare(off, 4, "mluc") == 0 && u32(off + 8) > 0) {
      size_t sl = u32(off + 20), so = u32(off + 24);
      std::string s;
      for (size_t k = off + so; k + 1 < off + so + sl && k + 1 < d.size(); k += 2) {
        unsigned c = (uint8_t(d[k]) << 8) | uint8_t(d[k + 1]);
        if (c < 0x80) s += char(c);
        else if (c < 0x800) s += char(0xC0 | (c >> 6)), s += char(0x80 | (c & 0x3F));
        else s += char(0xE0 | (c >> 12)), s += char(0x80 | ((c >> 6) & 0x3F)), s += char(0x80 | (c & 0x3F));
      }
      return s;
    }
  }
  return "";
}

class InvWalker : public QPDFObjectHandle::ParserCallbacks {
 public:
  InvWalker(Inventory& inv, int page) : inv_(inv), page_(page) {}

  void noteIcc(QPDFObjectHandle s, bool oi) {
    if (!s.isStream()) return;
    auto& e = inv_.icc[objKey(s)];
    e.outputIntent = e.outputIntent || oi;
    if (page_ > 0) e.pages.insert(page_);
    if (!e.desc.empty() || !e.cs.empty()) return;
    std::string d;
    try {
      d = bufStr(s.getStreamData(qpdf_dl_all));
    } catch (std::exception&) {
    }
    e.desc = iccDesc(d);
    if (d.size() >= 20) {
      e.cs = d.substr(16, 4);
      while (!e.cs.empty() && e.cs.back() == ' ') e.cs.pop_back();
      e.version = std::to_string(uint8_t(d[8])) + "." + std::to_string(uint8_t(d[9]) >> 4);
    }
    QPDFObjectHandle n = s.getDict().getKey("/N");
    if (n.isInteger()) e.n = int(n.getIntValue());
  }

  std::string csName(QPDFObjectHandle cs, QPDFObjectHandle res, int depth = 0) {
    if (depth > 4) return "?";
    if (cs.isName()) {
      std::string n = cs.getName();
      if (n == "/Pattern") return "Pattern";
      if (n.rfind("/Device", 0) == 0 || n == "/CalRGB" || n == "/Lab") return n.substr(1);
      if (res.isDictionary()) {
        QPDFObjectHandle d = res.getKey("/ColorSpace");
        if (d.isDictionary() && d.hasKey(n)) return csName(d.getKey(n), res, depth + 1);
      }
      return n.substr(1);
    }
    if (!cs.isArray() || cs.getArrayNItems() < 1 || !cs.getArrayItem(0).isName()) return "?";
    std::string fam = cs.getArrayItem(0).getName().substr(1);
    if (fam == "ICCBased") {
      noteIcc(cs.getArrayItem(1), false);
      QPDFObjectHandle n = cs.getArrayItem(1).isStream() ? cs.getArrayItem(1).getDict().getKey("/N") : QPDFObjectHandle();
      std::string desc = inv_.icc[objKey(cs.getArrayItem(1))].desc;
      return "ICCBased" + (n.isInteger() ? " N=" + std::to_string(n.getIntValue()) : std::string()) +
             (desc.empty() ? "" : " (" + desc + ")");
    }
    if (fam == "Separation" && cs.getArrayNItems() > 1 && cs.getArrayItem(1).isName()) {
      std::string n = cs.getArrayItem(1).getName().substr(1);
      if (n != "All" && n != "None" && page_ > 0) inv_.spots[n].insert(page_);
      csName(cs.getArrayItem(2), res, depth + 1);
      return "Separation (" + n + ")";
    }
    if (fam == "DeviceN" && cs.getArrayNItems() > 1 && cs.getArrayItem(1).isArray()) {
      std::string names;
      for (auto& c : cs.getArrayItem(1).aitems()) {
        if (!c.isName()) continue;
        std::string n = c.getName().substr(1);
        names += (names.empty() ? "" : ", ") + n;
        if (n != "All" && n != "None" && n != "Cyan" && n != "Magenta" && n != "Yellow" && n != "Black" && page_ > 0)
          inv_.spots[n].insert(page_);
      }
      csName(cs.getArrayItem(2), res, depth + 1);
      return "DeviceN (" + names + ")";
    }
    if (fam == "Indexed" && cs.getArrayNItems() > 1) return "Indexed (" + csName(cs.getArrayItem(1), res, depth + 1) + ")";
    return fam;
  }

  void noteCs(QPDFObjectHandle cs, QPDFObjectHandle res) {
    std::string n = csName(cs, res);
    if (n != "?") inv_.colorSpaces[n].insert(page_);
  }

  void walkResources(QPDFObjectHandle res) {
    if (!res.isDictionary()) return;
    QPDFObjectHandle fonts = res.getKey("/Font");
    if (fonts.isDictionary()) {
      for (auto& [k, f] : fonts.ditems()) {
        if (!f.isDictionary()) continue;
        auto& e = inv_.fonts[objKey(f)];
        e.pages.insert(page_);
        if (!e.name.empty()) continue;
        QPDFObjectHandle bf = f.getKey("/BaseFont");
        e.name = bf.isName() ? bf.getName().substr(1) : k.substr(1);
        QPDFObjectHandle st = f.getKey("/Subtype");
        e.type = st.isName() ? st.getName().substr(1) : "?";
        QPDFObjectHandle fd = f.getKey("/FontDescriptor");
        QPDFObjectHandle df = f.getKey("/DescendantFonts");
        if (df.isArray() && df.getArrayNItems() > 0) fd = df.getArrayItem(0).getKey("/FontDescriptor");
        e.embedded = e.type == "Type3" || (fd.isDictionary() && (fd.hasKey("/FontFile") || fd.hasKey("/FontFile2") || fd.hasKey("/FontFile3")));
      }
    }
    QPDFObjectHandle css = res.getKey("/ColorSpace");
    if (css.isDictionary())
      for (auto& [k, c] : css.ditems()) noteCs(c, res);
    QPDFObjectHandle sh = res.getKey("/Shading");
    if (sh.isDictionary())
      for (auto& [k, s] : sh.ditems()) {
        QPDFObjectHandle d = s.isStream() ? s.getDict() : s;
        if (d.isDictionary()) noteCs(d.getKey("/ColorSpace"), res);
      }
  }

  void run(QPDFObjectHandle content, QPDFObjectHandle res, const std::vector<double>& m, int depth) {
    if (depth > 8) return;
    auto saveRes = res_;
    auto saveCtm = ctm_;
    auto saveStack = stack_;
    res_ = res;
    ctm_ = m;
    stack_.clear();
    walkResources(res);
    try {
      if (content.isStream()) content.parseAsContents(this);
      else if (page_ > 0 && depth == 0) QPDFPageObjectHelper(content).parseContents(this);
    } catch (std::exception&) {
    }
    res_ = saveRes;
    ctm_ = saveCtm;
    stack_ = saveStack;

  }

  void handleObject(QPDFObjectHandle o) override {
    if (!o.isOperator()) {
      ops_.push_back(o);
      return;
    }
    std::string op = o.getOperatorValue();
    if (op == "q") stack_.push_back(ctm_);
    else if (op == "Q" && !stack_.empty()) ctm_ = stack_.back(), stack_.pop_back();
    else if (op == "cm" && ops_.size() >= 6) {
      double a[6];
      for (int i = 0; i < 6; ++i) a[i] = ops_[ops_.size() - 6 + i].getNumericValue();
      const auto& c = ctm_;
      ctm_ = {a[0] * c[0] + a[1] * c[2], a[0] * c[1] + a[1] * c[3], a[2] * c[0] + a[3] * c[2],
              a[2] * c[1] + a[3] * c[3], a[4] * c[0] + a[5] * c[2] + c[4], a[4] * c[1] + a[5] * c[3] + c[5]};
    } else if (op == "rg" || op == "RG") inv_.colorSpaces["DeviceRGB"].insert(page_);
    else if (op == "k" || op == "K") inv_.colorSpaces["DeviceCMYK"].insert(page_);
    else if (op == "g" || op == "G") inv_.colorSpaces["DeviceGray"].insert(page_);
    else if ((op == "cs" || op == "CS") && !ops_.empty()) noteCs(ops_.back(), res_);
    else if (op == "Do" && !ops_.empty() && ops_.back().isName() && res_.isDictionary()) doXObject(ops_.back().getName());
    ops_.clear();
  }

  void handleEOF() override {}

 private:
  void doXObject(const std::string& name) {
    QPDFObjectHandle xo = res_.getKey("/XObject");
    if (!xo.isDictionary()) return;
    QPDFObjectHandle x = xo.getKey(name);
    if (!x.isStream()) return;
    QPDFObjectHandle d = x.getDict();
    QPDFObjectHandle st = d.getKey("/Subtype");
    if (st.isNameAndEquals("/Form")) {
      std::vector<double> m = ctm_;
      QPDFObjectHandle fm = d.getKey("/Matrix");
      if (fm.isArray() && fm.getArrayNItems() == 6) {
        double a[6];
        for (int i = 0; i < 6; ++i) a[i] = fm.getArrayItem(i).isNumber() ? fm.getArrayItem(i).getNumericValue() : 0;
        const auto& c = ctm_;
        m = {a[0] * c[0] + a[1] * c[2], a[0] * c[1] + a[1] * c[3], a[2] * c[0] + a[3] * c[2],
             a[2] * c[1] + a[3] * c[3], a[4] * c[0] + a[5] * c[2] + c[4], a[4] * c[1] + a[5] * c[3] + c[5]};
      }
      QPDFObjectHandle fr = d.getKey("/Resources");
      if (formDepth_ < 8 && !inForm_.count(objKey(x))) {
        inForm_.insert(objKey(x));
        ++formDepth_;
        run(x, fr.isDictionary() ? fr : res_, m, formDepth_);
        --formDepth_;
        inForm_.erase(objKey(x));
      }
      return;
    }
    if (!st.isNameAndEquals("/Image")) return;
    auto& e = inv_.images[objKey(x)];
    e.pages.insert(page_);
    if (e.w == 0) {
      e.name = name.substr(1);
      auto num = [&](const char* k) { QPDFObjectHandle v = d.getKey(k); return v.isInteger() ? int(v.getIntValue()) : 0; };
      e.w = num("/Width");
      e.h = num("/Height");
      e.bpc = num("/BitsPerComponent");
      e.cs = d.getKey("/ImageMask").isBool() && d.getKey("/ImageMask").getBoolValue() ? "ImageMask" : csName(d.getKey("/ColorSpace"), res_);
      if (e.cs == "?") e.cs = d.getKey("/Filter").isNameAndEquals("/JPXDecode") ? "JPX" : "-";
      else inv_.colorSpaces[e.cs].insert(page_);
      QPDFObjectHandle f = d.getKey("/Filter");
      if (f.isArray() && f.getArrayNItems() > 0) f = f.getArrayItem(f.getArrayNItems() - 1);
      e.filter = f.isName() ? f.getName().substr(1) : "-";
    }
    double wpt = std::hypot(ctm_[0], ctm_[1]), hpt = std::hypot(ctm_[2], ctm_[3]);
    if (wpt > 0 && hpt > 0 && e.w > 0) {
      double ppi = std::min(e.w * 72.0 / wpt, e.h * 72.0 / hpt);
      if (e.ppi == 0 || ppi < e.ppi) e.ppi = ppi;
    }
  }

  Inventory& inv_;
  int page_;
  int formDepth_ = 0;
  QPDFObjectHandle res_;
  std::vector<double> ctm_{1, 0, 0, 1, 0, 0};
  std::vector<std::vector<double>> stack_;
  std::vector<QPDFObjectHandle> ops_;
  std::set<std::string> inForm_;
};

bool buildInventory(QPDF& pdf, Inventory& inv) {
  const auto& pages = pdf.getAllPages();
  inv.pageCount = int(pages.size());
  QPDFObjectHandle root = pdf.getRoot();
  for (size_t i = 0; i < pages.size(); ++i) {
    int pn = int(i) + 1;
    QPDFPageObjectHelper ph(pages[i]);
    InvWalker w(inv, pn);
    w.run(pages[i], ph.getAttribute("/Resources", false), {1, 0, 0, 1, 0, 0}, 0);
    QPDFObjectHandle ann = pages[i].getKey("/Annots");
    if (ann.isArray())
      for (auto& a : ann.aitems()) {
        if (!a.isDictionary()) continue;
        QPDFObjectHandle st = a.getKey("/Subtype");
        auto& e = inv.annots[st.isName() ? st.getName().substr(1) : "?"];
        ++e.first;
        e.second.insert(pn);
      }
    QPDFObjectHandle oi = pages[i].getKey("/OutputIntents");
    if (oi.isArray())
      for (auto& o : oi.aitems())
        if (o.isDictionary()) w.noteIcc(o.getKey("/DestOutputProfile"), true);
  }
  InvWalker w(inv, 0);
  QPDFObjectHandle oi = root.getKey("/OutputIntents");
  if (oi.isArray())
    for (auto& o : oi.aitems())
      if (o.isDictionary()) w.noteIcc(o.getKey("/DestOutputProfile"), true);
  QPDFObjectHandle ocgs = root.getKey("/OCProperties").isDictionary() ? root.getKey("/OCProperties").getKey("/OCGs") : QPDFObjectHandle();
  if (ocgs.isArray())
    for (auto& g : ocgs.aitems())
      if (g.isDictionary()) inv.layers.push_back(g.getKey("/Name").isString() ? g.getKey("/Name").getUTF8Value() : "?");
  return true;
}

std::vector<Section> inventorySections(const Inventory& inv, const Strings& s) {
  std::vector<Section> out;
  Section sum{"inventory", s.tr("inventory")};
  sum.kv = {{"pagecount", std::to_string(inv.pageCount)}, {"fonts", std::to_string(inv.fonts.size())},
            {"images", std::to_string(inv.images.size())}, {"colorspaces", std::to_string(inv.colorSpaces.size())},
            {"spots", std::to_string(inv.spots.size())}, {"icc", std::to_string(inv.icc.size())},
            {"annotations", std::to_string(inv.annots.size())}, {"layers", std::to_string(inv.layers.size())}};
  out.push_back(sum);
  Section f{"fonts", s.tr("fonts")};
  f.cols = {{"name", 3}, {"type", 1.2}, {"embedded", 1}, {"pages", 1.2}};
  for (auto& [k, e] : inv.fonts) f.rows.push_back({e.name, e.type, s.tr(e.embedded ? "yes" : "no"), pageList(e.pages)});
  out.push_back(f);
  Section im{"images", s.tr("images")};
  im.cols = {{"name", 1}, {"size", 1.2}, {"bpc", 0.6}, {"colorspace", 2.4}, {"ppi", 1}, {"filter", 1.1}, {"pages", 1}};
  for (auto& [k, e] : inv.images)
    im.rows.push_back({e.name, std::to_string(e.w) + " x " + std::to_string(e.h), e.bpc ? std::to_string(e.bpc) : "-", e.cs,
                       e.ppi > 0 ? fmt(e.ppi) : "-", e.filter, pageList(e.pages)});
  out.push_back(im);
  Section cs{"colorspaces", s.tr("colorspaces")};
  cs.cols = {{"name", 4}, {"pages", 1}};
  for (auto& [k, p] : inv.colorSpaces) cs.rows.push_back({k, pageList(p)});
  out.push_back(cs);
  Section sp{"spots", s.tr("spots")};
  sp.cols = {{"name", 4}, {"pages", 1}};
  for (auto& [k, p] : inv.spots) sp.rows.push_back({k, pageList(p)});
  out.push_back(sp);
  Section ic{"icc", s.tr("icc")};
  ic.cols = {{"name", 3}, {"colorspace", 1}, {"components", 1}, {"version", 0.8}, {"outputintent", 1}, {"pages", 1}};
  for (auto& [k, e] : inv.icc)
    ic.rows.push_back({e.desc.empty() ? "-" : e.desc, e.cs, std::to_string(e.n), e.version, s.tr(e.outputIntent ? "yes" : "no"),
                       e.pages.empty() ? "-" : pageList(e.pages)});
  out.push_back(ic);
  Section an{"annotations", s.tr("annotations")};
  an.cols = {{"type", 2}, {"count", 1}, {"pages", 2}};
  for (auto& [k, e] : inv.annots) an.rows.push_back({k, std::to_string(e.first), pageList(e.second)});
  out.push_back(an);
  Section ly{"layers", s.tr("layers")};
  ly.cols = {{"name", 1}};
  for (auto& n : inv.layers) ly.rows.push_back({n});
  out.push_back(ly);
  for (auto& sec : out) sec.empty = "-";
  return out;
}

struct XNode {
  std::string name;
  std::string text;
  std::map<std::string, std::string> attrs;
  std::vector<XNode> kids;
};

std::string xmlUnescape(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') {
      out += s[i];
      continue;
    }
    size_t e = s.find(';', i);
    if (e == std::string::npos) {
      out += s[i];
      continue;
    }
    std::string ent = s.substr(i + 1, e - i - 1);
    unsigned cp = 0;
    if (ent == "amp") cp = '&';
    else if (ent == "lt") cp = '<';
    else if (ent == "gt") cp = '>';
    else if (ent == "quot") cp = '"';
    else if (ent == "apos") cp = '\'';
    else if (!ent.empty() && ent[0] == '#') cp = unsigned(std::strtoul(ent.c_str() + (ent.size() > 1 && ent[1] == 'x' ? 2 : 1), nullptr, ent.size() > 1 && ent[1] == 'x' ? 16 : 10));
    if (!cp) {
      out += s[i];
      continue;
    }
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) out += char(0xC0 | (cp >> 6)), out += char(0x80 | (cp & 0x3F));
    else out += char(0xE0 | ((cp >> 12) & 0x0F)), out += char(0x80 | ((cp >> 6) & 0x3F)), out += char(0x80 | (cp & 0x3F));
    i = e;
  }
  return out;
}

bool parseXml(const std::string& x, size_t& i, XNode& node, int depth) {
  if (depth > 64) return false;
  while (i < x.size()) {
    size_t lt = x.find('<', i);
    if (lt == std::string::npos) return depth == 0;
    node.text += xmlUnescape(x.substr(i, lt - i));
    i = lt;
    if (x.compare(i, 4, "<!--") == 0) {
      size_t e = x.find("-->", i);
      i = e == std::string::npos ? x.size() : e + 3;
    } else if (x.compare(i, 9, "<![CDATA[") == 0) {
      size_t e = x.find("]]>", i);
      node.text += x.substr(i + 9, (e == std::string::npos ? x.size() : e) - i - 9);
      i = e == std::string::npos ? x.size() : e + 3;
    } else if (x.compare(i, 2, "<?") == 0 || x.compare(i, 2, "<!") == 0) {
      size_t e = x.find('>', i);
      i = e == std::string::npos ? x.size() : e + 1;
    } else if (x.compare(i, 2, "</") == 0) {
      size_t e = x.find('>', i);
      i = e == std::string::npos ? x.size() : e + 1;
      return true;
    } else {
      size_t e = i + 1;
      char q = 0;
      while (e < x.size() && (q || x[e] != '>')) {
        if (q && x[e] == q) q = 0;
        else if (!q && (x[e] == '"' || x[e] == '\'')) q = x[e];
        ++e;
      }
      if (e >= x.size()) return false;
      std::string tag = x.substr(i + 1, e - i - 1);
      bool selfClose = !tag.empty() && tag.back() == '/';
      if (selfClose) tag.pop_back();
      XNode kid;
      size_t sp = tag.find_first_of(" \t\r\n");
      kid.name = tag.substr(0, sp);
      size_t c = kid.name.find(':');
      if (c != std::string::npos) kid.name = kid.name.substr(c + 1);
      for (size_t p = sp; p != std::string::npos && p < tag.size();) {
        size_t eq = tag.find('=', p);
        if (eq == std::string::npos) break;
        size_t qs = tag.find_first_of("\"'", eq);
        if (qs == std::string::npos) break;
        size_t qe = tag.find(tag[qs], qs + 1);
        if (qe == std::string::npos) break;
        std::string an = tag.substr(p, eq - p);
        an.erase(0, an.find_first_not_of(" \t\r\n"));
        an.erase(an.find_last_not_of(" \t\r\n") + 1);
        kid.attrs[an] = xmlUnescape(tag.substr(qs + 1, qe - qs - 1));
        p = qe + 1;
      }
      i = e + 1;
      if (!selfClose && !parseXml(x, i, kid, depth + 1)) return false;
      node.kids.push_back(std::move(kid));
    }
  }
  return depth == 0;
}

const XNode* xfind(const XNode* n, const std::string& path) {
  if (!n) return nullptr;
  size_t sl = path.find('/');
  std::string head = path.substr(0, sl);
  std::vector<const XNode*> q{n};
  for (size_t k = 0; k < q.size(); ++k)
    for (const XNode& c : q[k]->kids) {
      if (c.name == head) return sl == std::string::npos ? &c : xfind(&c, path.substr(sl + 1));
      q.push_back(&c);
    }
  return nullptr;
}

std::vector<const XNode*> xkids(const XNode* n, const std::string& name) {
  std::vector<const XNode*> out;
  if (n)
    for (const XNode& c : n->kids)
      if (c.name == name) out.push_back(&c);
  return out;
}

std::string xtext(const XNode* n, const std::string& path) {
  const XNode* f = path.empty() ? n : xfind(n, path);
  if (!f) return "";
  std::string t = f->text;
  t.erase(0, t.find_first_not_of(" \t\r\n"));
  t.erase(t.find_last_not_of(" \t\r\n") + 1);
  return t;
}

std::string ciiDate(const std::string& d) {
  return d.size() == 8 && std::all_of(d.begin(), d.end(), ::isdigit) ? d.substr(0, 4) + "-" + d.substr(4, 2) + "-" + d.substr(6, 2) : d;
}

std::string joinNonEmpty(std::initializer_list<std::string> parts, const char* sep = ", ") {
  std::string out;
  for (auto& p : parts)
    if (!p.empty()) out += (out.empty() ? "" : sep) + p;
  return out;
}

bool invoiceSections(const std::string& pdfBytes, const std::string& password, const Strings& s,
                     std::vector<Section>& out) {
  pdfa::InvoiceRead r = pdfa::readInvoice(reinterpret_cast<const unsigned char*>(pdfBytes.data()), pdfBytes.size(), password);
  if (!r.ok || r.xml.empty()) return false;
  XNode doc;
  size_t i = 0;
  if (!parseXml(r.xml, i, doc, 0) || doc.kids.empty()) return false;
  const XNode* root = &doc.kids.front();
  pdfa::InvoiceProfile prof = pdfa::detectInvoice(r.xml, "", "");
  Section h{"invoice", s.tr("invoice")}, lines{"invoice_lines", s.tr("lines")}, tax{"invoice_taxes", s.tr("taxes")},
      tot{"invoice_totals", s.tr("totals")};
  lines.cols = {{"id", 0.6}, {"detail", 3.4}, {"qty", 1.2}, {"price", 1.2}, {"amount", 1.2}};
  tax.cols = {{"category", 1}, {"rate", 1}, {"basis", 1.5}, {"tax", 1.5}};
  auto qty = [](const XNode* q) { return q ? joinNonEmpty({xtext(q, ""), q->attrs.count("unitCode") ? q->attrs.at("unitCode") : ""}, " ") : std::string(); };
  std::string fmtName = joinNonEmpty({prof.standard, prof.profile}, " ");
  h.kv.push_back({"format", fmtName.empty() ? root->name : fmtName});
  h.kv.push_back({"attachment", r.filename});
  std::string party[2][3];
  if (root->name == "CrossIndustryInvoice" || root->name == "CrossIndustryDocument") {
    const XNode* ed = xfind(root, "ExchangedDocument");
    if (!ed) ed = xfind(root, "HeaderExchangedDocument");
    h.kv.push_back({"number", xtext(ed, "ID")});
    h.kv.push_back({"doctype", xtext(ed, "TypeCode")});
    h.kv.push_back({"issued", ciiDate(xtext(ed, "IssueDateTime/DateTimeString"))});
    h.kv.push_back({"due", ciiDate(xtext(root, "SpecifiedTradePaymentTerms/DueDateDateTime/DateTimeString"))});
    h.kv.push_back({"currency", xtext(root, "InvoiceCurrencyCode")});
    const char* names[2] = {"SellerTradeParty", "BuyerTradeParty"};
    for (int p = 0; p < 2; ++p) {
      const XNode* n = xfind(root, names[p]);
      const XNode* a = xfind(n, "PostalTradeAddress");
      party[p][0] = xtext(n, "Name");
      party[p][1] = joinNonEmpty({xtext(a, "LineOne"), xtext(a, "LineTwo"), joinNonEmpty({xtext(a, "PostcodeCode"), xtext(a, "CityName")}, " "), xtext(a, "CountryID")});
      for (const XNode* t : xkids(n, "SpecifiedTaxRegistration"))
        if (const XNode* id = xfind(t, "ID"); id && (!id->attrs.count("schemeID") || id->attrs.at("schemeID") == "VA")) party[p][2] = xtext(t, "ID");
    }
    std::vector<const XNode*> items;
    std::vector<const XNode*> q{root};
    for (size_t k = 0; k < q.size(); ++k)
      for (const XNode& c : q[k]->kids) {
        if (c.name == "IncludedSupplyChainTradeLineItem") items.push_back(&c);
        else q.push_back(&c);
      }
    for (const XNode* li : items)
      lines.rows.push_back({xtext(li, "AssociatedDocumentLineDocument/LineID"), xtext(li, "SpecifiedTradeProduct/Name"),
                            qty(xfind(li, "BilledQuantity")), xtext(li, "NetPriceProductTradePrice/ChargeAmount"),
                            xtext(li, "SpecifiedTradeSettlementLineMonetarySummation/LineTotalAmount")});
    const XNode* st = xfind(root, "ApplicableHeaderTradeSettlement");
    if (!st) st = xfind(root, "ApplicableSupplyChainTradeSettlement");
    for (const XNode* t : xkids(st, "ApplicableTradeTax"))
      tax.rows.push_back({xtext(t, "CategoryCode"), xtext(t, "RateApplicablePercent").empty() ? xtext(t, "ApplicablePercent") : xtext(t, "RateApplicablePercent"),
                          xtext(t, "BasisAmount"), xtext(t, "CalculatedAmount")});
    const XNode* ms = xfind(st, "SpecifiedTradeSettlementHeaderMonetarySummation");
    if (!ms) ms = xfind(st, "SpecifiedTradeSettlementMonetarySummation");
    tot.kv = {{"net", xtext(ms, "TaxBasisTotalAmount")}, {"taxtotal", xtext(ms, "TaxTotalAmount")},
              {"grand", xtext(ms, "GrandTotalAmount")}, {"payable", xtext(ms, "DuePayableAmount")}};
  } else {
    bool credit = root->name == "CreditNote";
    h.kv.push_back({"number", xtext(root, "ID")});
    h.kv.push_back({"doctype", xtext(root, credit ? "CreditNoteTypeCode" : "InvoiceTypeCode")});
    h.kv.push_back({"issued", xtext(root, "IssueDate")});
    h.kv.push_back({"due", xtext(root, "DueDate").empty() ? xtext(root, "PaymentMeans/PaymentDueDate") : xtext(root, "DueDate")});
    h.kv.push_back({"currency", xtext(root, "DocumentCurrencyCode")});
    const char* names[2] = {"AccountingSupplierParty", "AccountingCustomerParty"};
    for (int p = 0; p < 2; ++p) {
      const XNode* n = xfind(root, names[p]);
      const XNode* a = xfind(n, "PostalAddress");
      party[p][0] = xtext(n, "PartyLegalEntity/RegistrationName").empty() ? xtext(n, "PartyName/Name") : xtext(n, "PartyLegalEntity/RegistrationName");
      party[p][1] = joinNonEmpty({xtext(a, "StreetName"), xtext(a, "AdditionalStreetName"), joinNonEmpty({xtext(a, "PostalZone"), xtext(a, "CityName")}, " "), xtext(a, "Country/IdentificationCode")});
      party[p][2] = xtext(n, "PartyTaxScheme/CompanyID");
    }
    for (const XNode* li : xkids(root, credit ? "CreditNoteLine" : "InvoiceLine"))
      lines.rows.push_back({xtext(li, "ID"), xtext(li, "Item/Name"), qty(xfind(li, credit ? "CreditedQuantity" : "InvoicedQuantity")),
                            xtext(li, "Price/PriceAmount"), xtext(li, "LineExtensionAmount")});
    for (const XNode* tt : xkids(root, "TaxTotal"))
      for (const XNode* t : xkids(tt, "TaxSubtotal"))
        tax.rows.push_back({xtext(t, "TaxCategory/ID"), xtext(t, "TaxCategory/Percent"), xtext(t, "TaxableAmount"), xtext(t, "TaxAmount")});
    const XNode* ms = xfind(root, "LegalMonetaryTotal");
    std::string taxTotal;
    for (const XNode* tt : xkids(root, "TaxTotal"))
      if (taxTotal.empty()) taxTotal = xtext(tt, "TaxAmount");
    tot.kv = {{"net", xtext(ms, "TaxExclusiveAmount")}, {"taxtotal", taxTotal},
              {"grand", xtext(ms, "TaxInclusiveAmount")}, {"payable", xtext(ms, "PayableAmount")}};
  }
  const char* pk[2] = {"seller", "buyer"};
  for (int p = 0; p < 2; ++p) {
    h.kv.push_back({pk[p], party[p][0]});
    h.kv.push_back({std::string(pk[p]) + ":address", party[p][1]});
    h.kv.push_back({std::string(pk[p]) + ":vat", party[p][2]});
  }
  auto strip = [](Section& sec) {
    sec.kv.erase(std::remove_if(sec.kv.begin(), sec.kv.end(), [](auto& p) { return p.second.empty(); }), sec.kv.end());
  };
  strip(h);
  strip(tot);
  lines.empty = tax.empty = "-";
  out.push_back(h);
  out.push_back(lines);
  out.push_back(tax);
  out.push_back(tot);
  return true;
}

std::string label(const Strings& s, const std::string& key) {
  size_t c = key.find(':');
  return c == std::string::npos ? s.tr(key) : s.tr(key.substr(0, c)) + " - " + s.tr(key.substr(c + 1));
}

std::string nowIso() { return QUtil::qpdf_time_to_iso8601(QUtil::get_current_qpdf_time()); }

std::vector<Section> resultSections(const Result& r, const Strings& s) {
  std::vector<Section> out;
  Section sum{"summary", s.tr("summary")};
  if (!r.ok) sum.status = 2, sum.statusText = s.tr("failed");
  else if (r.hasCompliant) sum.status = r.compliant ? 1 : 2, sum.statusText = s.tr(r.compliant ? "pass" : "fail");
  else sum.status = 1, sum.statusText = s.tr("done");
  sum.kv = {{"result", sum.statusText}, {"file", r.file}, {"output", r.output}, {"level", r.level}, {"engine", r.engine},
            {"date", nowIso()}, {"job", r.jobId}, {"findings", r.findings >= 0 ? std::to_string(r.findings) : ""},
            {"error", r.errorCode.empty() ? r.error : r.errorCode + ": " + r.error}};
  sum.kv.erase(std::remove_if(sum.kv.begin(), sum.kv.end(), [](auto& p) { return p.second.empty() || p.second == ": "; }), sum.kv.end());
  out.push_back(sum);
  auto table = [&](const char* id, const std::vector<Item>& items, bool withFixed) {
    Section t{id, s.tr(id)};
    t.cols = {{"severity", 1.3}, {"code", 2.2}, {"detail", 4.4}, {"pages", 1}};
    if (withFixed) t.cols.push_back({"fixed", 0.8});
    for (const Item& it : items) {
      std::vector<std::string> row{it.severity >= 1 && it.severity <= 3 ? s.tr("sev" + std::to_string(it.severity)) : "-",
                                   it.code, s.text(it),
                                   it.pages.empty() ? s.tr("document") : pageList(std::set<int>(it.pages.begin(), it.pages.end()))};
      if (withFixed) row.push_back(s.tr(it.fixed ? "yes" : "no"));
      t.rows.push_back(row);
    }
    t.empty = s.tr("none");
    out.push_back(t);
  };
  table("issues", r.issues, r.mode != "check");
  if (r.hasAnalysis) table("analysis", r.analysis, false);
  return out;
}

std::string htmlEsc(const std::string& in) {
  std::string o;
  for (char c : in) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

std::string jsonEsc(const std::string& in) { return JSON::makeString(in).unparse(); }

std::string renderHtml(const std::vector<Section>& secs, const Strings& s, const std::string& title, const std::string& engine) {
  std::string h = "<!DOCTYPE html>\n<html lang=\"" + s.lang + "\"><head><meta charset=\"utf-8\"><title>" + htmlEsc(title) +
                  "</title><style>body{font-family:Helvetica,Arial,sans-serif;margin:2em;color:#222}h1{font-size:1.6em}"
                  "h2{font-size:1.2em;margin-top:1.6em;border-bottom:1px solid #ccc}table{border-collapse:collapse;width:100%}"
                  "th,td{border:1px solid #ddd;padding:4px 6px;text-align:left;vertical-align:top;font-size:.9em}th{background:#eee}"
                  ".kv th{width:25%}.status{padding:12px 16px;color:#fff;font-weight:bold;font-size:1.2em;border-radius:4px}"
                  ".ok{background:#2e7d32}.bad{background:#c62828}.foot{margin-top:2em;color:#888;font-size:.8em}</style></head><body>\n<h1>" +
                  htmlEsc(title) + "</h1>\n";
  for (const Section& sec : secs) {
    h += "<section id=\"" + sec.id + "\"><h2>" + htmlEsc(sec.title) + "</h2>\n";
    if (sec.status) h += "<div class=\"status " + std::string(sec.status == 1 ? "ok" : "bad") + "\">" + htmlEsc(sec.statusText) + "</div>\n";
    if (!sec.kv.empty()) {
      h += "<table class=\"kv\">";
      for (auto& [k, v] : sec.kv) h += "<tr><th>" + htmlEsc(label(s, k)) + "</th><td>" + htmlEsc(v) + "</td></tr>";
      h += "</table>\n";
    }
    if (!sec.cols.empty()) {
      if (sec.rows.empty()) h += "<p>" + htmlEsc(sec.empty) + "</p>\n";
      else {
        h += "<table><tr>";
        for (auto& c : sec.cols) h += "<th>" + htmlEsc(s.tr(c.key)) + "</th>";
        h += "</tr>";
        for (auto& row : sec.rows) {
          h += "<tr>";
          for (auto& c : row) h += "<td>" + htmlEsc(c) + "</td>";
          h += "</tr>";
        }
        h += "</table>\n";
      }
    }
    h += "</section>\n";
  }
  return h + "<p class=\"foot\">" + htmlEsc(s.tr("generated") + " " + engine) + "</p>\n</body></html>\n";
}

std::string renderXml(const std::vector<Section>& secs, const Strings& s, const std::string& engine) {
  std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<report xmlns=\"urn:bentopdf:kura:report:1\" lang=\"" + s.lang +
                  "\" engine=\"" + htmlEsc(engine) + "\" created=\"" + nowIso() + "\">\n";
  for (const Section& sec : secs) {
    x += "  <section id=\"" + sec.id + "\" title=\"" + htmlEsc(sec.title) + "\"";
    if (sec.status) x += " status=\"" + std::string(sec.status == 1 ? "pass" : "fail") + "\"";
    x += ">\n";
    for (auto& [k, v] : sec.kv) x += "    <item key=\"" + htmlEsc(k) + "\" label=\"" + htmlEsc(label(s, k)) + "\">" + htmlEsc(v) + "</item>\n";
    if (!sec.cols.empty()) {
      x += "    <table>\n      <columns>";
      for (auto& c : sec.cols) x += "<column key=\"" + c.key + "\" label=\"" + htmlEsc(s.tr(c.key)) + "\"/>";
      x += "</columns>\n";
      for (auto& row : sec.rows) {
        x += "      <row>";
        for (size_t i = 0; i < row.size() && i < sec.cols.size(); ++i) x += "<cell key=\"" + sec.cols[i].key + "\">" + htmlEsc(row[i]) + "</cell>";
        x += "</row>\n";
      }
      x += "    </table>\n";
    }
    x += "  </section>\n";
  }
  return x + "</report>\n";
}

std::string renderJson(const std::vector<Section>& secs, const std::string& resultJson) {
  std::string j = "{";
  if (!resultJson.empty()) j += "\"result\":" + resultJson + ",";
  j += "\"report\":{";
  for (size_t n = 0; n < secs.size(); ++n) {
    const Section& sec = secs[n];
    j += (n ? "," : "") + jsonEsc(sec.id) + ":{\"title\":" + jsonEsc(sec.title);
    if (sec.status) j += ",\"status\":\"" + std::string(sec.status == 1 ? "pass" : "fail") + "\"";
    for (auto& [k, v] : sec.kv) j += "," + jsonEsc(k) + ":" + jsonEsc(v);
    if (!sec.cols.empty()) {
      j += ",\"rows\":[";
      for (size_t r = 0; r < sec.rows.size(); ++r) {
        j += r ? ",{" : "{";
        for (size_t i = 0; i < sec.rows[r].size() && i < sec.cols.size(); ++i)
          j += (i ? "," : "") + jsonEsc(sec.cols[i].key) + ":" + jsonEsc(sec.rows[r][i]);
        j += "}";
      }
      j += "]";
    }
    j += "}";
  }
  return j + "}}\n";
}

std::string renderText(const std::vector<Section>& secs, const Strings& s, const std::string& title) {
  std::string t = title + "\n" + std::string(title.size(), '=') + "\n";
  for (const Section& sec : secs) {
    t += "\n" + sec.title + "\n" + std::string(sec.title.size(), '-') + "\n";
    for (auto& [k, v] : sec.kv) t += label(s, k) + ": " + v + "\n";
    if (!sec.cols.empty() && sec.rows.empty()) t += sec.empty + "\n";
    for (auto& row : sec.rows) {
      std::string line;
      for (size_t i = 0; i < row.size() && i < sec.cols.size(); ++i) line += (i ? " | " : "") + row[i];
      t += line + "\n";
    }
  }
  return t;
}

const short kHelv[95] = {278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556,
                         556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667,
                         611, 778, 722, 278, 500, 667, 556, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667,
                         667, 611, 278, 278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500,
                         222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584};

std::string winAnsi(const std::string& u) {
  static const std::map<unsigned, char> extra = {{0x20AC, '\x80'}, {0x201A, '\x82'}, {0x0192, '\x83'}, {0x201E, '\x84'},
                                                 {0x2026, '\x85'}, {0x2020, '\x86'}, {0x2021, '\x87'}, {0x2030, '\x89'},
                                                 {0x0160, '\x8A'}, {0x2039, '\x8B'}, {0x0152, '\x8C'}, {0x017D, '\x8E'},
                                                 {0x2018, '\x91'}, {0x2019, '\x92'}, {0x201C, '\x93'}, {0x201D, '\x94'},
                                                 {0x2022, '\x95'}, {0x2013, '\x96'}, {0x2014, '\x97'}, {0x2122, '\x99'},
                                                 {0x0161, '\x9A'}, {0x203A, '\x9B'}, {0x0153, '\x9C'}, {0x017E, '\x9E'},
                                                 {0x0178, '\x9F'}};
  std::string o;
  for (size_t i = 0; i < u.size();) {
    unsigned char c = u[i];
    unsigned cp = c;
    int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (n > 1) {
      cp = c & (0x3F >> (n - 1));
      for (int k = 1; k < n && i + k < u.size(); ++k) cp = (cp << 6) | (u[i + k] & 0x3F);
    }
    i += n;
    if (cp < 0x20) o += ' ';
    else if (cp < 0x7F || (cp >= 0xA0 && cp <= 0xFF)) o += char(cp);
    else if (extra.count(cp)) o += extra.at(cp);
    else o += '?';
  }
  return o;
}

class PdfOut {
 public:
  static constexpr double kW = 595, kH = 842, kM = 48;
  std::vector<std::string> pages;
  std::string cur;
  double y = 0;

  PdfOut() { newPage(); }

  void newPage() {
    if (!cur.empty() || !pages.empty()) pages.push_back(cur);
    cur.clear();
    y = kH - kM;
  }
  void need(double h) {
    if (y - h < kM + 20) newPage();
  }
  static double width(const std::string& a, double size, bool bold) {
    double w = 0;
    for (unsigned char c : a) w += (c >= 32 && c < 127) ? kHelv[c - 32] : 556;
    return w * size / 1000.0 * (bold ? 1.08 : 1.0);
  }
  static std::vector<std::string> wrap(const std::string& text, double maxw, double size, bool bold) {
    std::vector<std::string> lines;
    std::string a = winAnsi(text), line;
    std::istringstream ws(a);
    std::string word;
    while (ws >> word) {
      while (width(word, size, bold) > maxw && word.size() > 1) {
        size_t k = 1;
        while (k < word.size() && width(word.substr(0, k + 1), size, bold) <= maxw) ++k;
        if (!line.empty()) lines.push_back(line), line.clear();
        lines.push_back(word.substr(0, k));
        word = word.substr(k);
      }
      std::string cand = line.empty() ? word : line + " " + word;
      if (width(cand, size, bold) > maxw && !line.empty()) lines.push_back(line), line = word;
      else line = cand;
    }
    if (!line.empty() || lines.empty()) lines.push_back(line);
    return lines;
  }
  void text(double x, double yy, double size, bool bold, const std::string& ansi, const char* rgb = "0 0 0") {
    std::string e;
    for (char c : ansi) {
      if (c == '(' || c == ')' || c == '\\') e += '\\';
      e += c;
    }
    cur += std::string(rgb) + " rg BT /" + (bold ? "F2 " : "F1 ") + fmt(size, 1) + " Tf " + fmt(x, 2) + " " + fmt(yy, 2) + " Td (" + e + ") Tj ET\n";
  }
  void rect(double x, double yy, double w, double h, const char* rgb, bool fill) {
    cur += std::string(rgb) + (fill ? " rg " : " RG 0.5 w ") + fmt(x, 2) + " " + fmt(yy, 2) + " " + fmt(w, 2) + " " + fmt(h, 2) + (fill ? " re f\n" : " re S\n");
  }
  void heading(const std::string& t, double size) {
    need(size + 60);
    y -= size + 8;
    text(kM, y, size, true, winAnsi(t));
    y -= 6;
    rect(kM, y, kW - 2 * kM, 0.6, "0.7 0.7 0.7", true);
    y -= 8;
  }
  void table(const std::vector<std::string>& head, const std::vector<std::vector<std::string>>& rows, const std::vector<double>& weights) {
    double total = 0;
    for (double w : weights) total += w;
    std::vector<double> cw;
    for (double w : weights) cw.push_back((kW - 2 * kM) * w / total);
    const double fs = 8.5, lh = 10.5;
    auto drawRow = [&](const std::vector<std::string>& cells, bool hdr) {
      std::vector<std::vector<std::string>> wrapped;
      size_t n = 1;
      for (size_t i = 0; i < cw.size(); ++i) {
        wrapped.push_back(wrap(i < cells.size() ? cells[i] : "", cw[i] - 6, fs, hdr));
        n = std::max(n, wrapped.back().size());
      }
      double h = n * lh + 5;
      if (y - h < kM + 20) {
        newPage();
        if (!hdr) drawHeader(head, cw, fs, lh);
      }
      if (hdr) rect(kM, y - h, kW - 2 * kM, h, "0.9 0.9 0.9", true);
      double x = kM;
      for (size_t i = 0; i < cw.size(); ++i) {
        for (size_t l = 0; l < wrapped[i].size(); ++l) text(x + 3, y - 3 - fs - l * lh, fs, hdr, wrapped[i][l]);
        x += cw[i];
      }
      y -= h;
      rect(kM, y, kW - 2 * kM, 0.4, "0.85 0.85 0.85", true);
    };
    drawRow(head, true);
    for (auto& r : rows) drawRow(r, false);
  }
  void drawHeader(const std::vector<std::string>& head, const std::vector<double>& cw, double fs, double lh) {
    double h = lh + 5;
    rect(kM, y - h, kW - 2 * kM, h, "0.9 0.9 0.9", true);
    double x = kM;
    for (size_t i = 0; i < cw.size() && i < head.size(); ++i) {
      text(x + 3, y - 3 - fs, fs, true, wrap(head[i], cw[i] - 6, fs, true).front());
      x += cw[i];
    }
    y -= h;
  }
  void kv(const std::string& k, const std::string& v) {
    const double fs = 9.5, lh = 12, lw = 150;
    auto lines = wrap(v, kW - 2 * kM - lw, fs, false);
    for (size_t i = 0; i < lines.size(); ++i) {
      need(lh);
      y -= lh;
      if (i == 0) text(kM, y, fs, true, wrap(k, lw - 6, fs, true).front());
      text(kM + lw, y, fs, false, lines[i]);
    }
  }
};

bool renderPdf(const std::vector<Section>& secs, const Strings& s, const std::string& title, const std::string& engine,
               const std::string& path, std::string& err) {
  PdfOut p;
  p.y -= 24;
  p.text(PdfOut::kM, p.y, 20, true, winAnsi(title));
  p.y -= 10;
  for (const Section& sec : secs) {
    p.heading(sec.title, 13);
    if (sec.status) {
      p.need(40);
      p.rect(PdfOut::kM, p.y - 32, PdfOut::kW - 2 * PdfOut::kM, 32, sec.status == 1 ? "0.18 0.49 0.2" : "0.78 0.16 0.16", true);
      p.text(PdfOut::kM + 12, p.y - 21, 14, true, winAnsi(sec.statusText), "1 1 1");
      p.y -= 40;
    }
    for (auto& [k, v] : sec.kv) p.kv(label(s, k), v);
    if (!sec.cols.empty()) {
      if (sec.rows.empty()) {
        p.need(14);
        p.y -= 12;
        p.text(PdfOut::kM, p.y, 9.5, false, winAnsi(sec.empty));
      } else {
        std::vector<std::string> head;
        std::vector<double> w;
        for (auto& c : sec.cols) head.push_back(s.tr(c.key)), w.push_back(c.w);
        p.table(head, sec.rows, w);
      }
    }
    p.y -= 6;
  }
  p.pages.push_back(p.cur);
  try {
    QPDF pdf;
    pdf.emptyPDF();
    auto font = [&](const char* name) {
      return pdf.makeIndirectObject(QPDFObjectHandle::parse(std::string("<< /Type /Font /Subtype /Type1 /BaseFont /") + name + " /Encoding /WinAnsiEncoding >>"));
    };
    QPDFObjectHandle f1 = font("Helvetica"), f2 = font("Helvetica-Bold");
    QPDFPageDocumentHelper dh(pdf);
    std::string foot = winAnsi(s.tr("generated") + " " + engine);
    for (size_t i = 0; i < p.pages.size(); ++i) {
      PdfOut f;
      f.cur.clear();
      f.text(PdfOut::kM, 24, 7.5, false, foot, "0.5 0.5 0.5");
      std::string pn = winAnsi(s.tr("page")) + " " + std::to_string(i + 1) + " / " + std::to_string(p.pages.size());
      f.text(PdfOut::kW - PdfOut::kM - PdfOut::width(pn, 7.5, false), 24, 7.5, false, pn, "0.5 0.5 0.5");
      QPDFObjectHandle page = pdf.makeIndirectObject(QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 595 842] >>"));
      QPDFObjectHandle fonts = QPDFObjectHandle::newDictionary();
      fonts.replaceKey("/F1", f1);
      fonts.replaceKey("/F2", f2);
      QPDFObjectHandle res = QPDFObjectHandle::newDictionary();
      res.replaceKey("/Font", fonts);
      page.replaceKey("/Resources", res);
      page.replaceKey("/Contents", pdf.newStream(p.pages[i] + f.cur));
      dh.addPage(QPDFPageObjectHelper(page), false);
    }
    QPDFObjectHandle info = pdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
    info.replaceKey("/Title", QPDFObjectHandle::newUnicodeString(title));
    info.replaceKey("/Producer", QPDFObjectHandle::newUnicodeString(engine));
    info.replaceKey("/CreationDate", QPDFObjectHandle::newString(QUtil::qpdf_time_to_pdf_time(QUtil::get_current_qpdf_time())));
    pdf.getTrailer().replaceKey("/Info", info);
    QPDFWriter w(pdf, path.c_str());
    w.setCompressStreams(true);
    w.write();
  } catch (std::exception& e) {
    err = e.what();
    return false;
  }
  return true;
}

std::string extOf(const std::string& path) {
  std::string e = std::filesystem::path(path).extension().string();
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
  return e;
}

bool emit(const std::string& path, const std::vector<Section>& secs, const Strings& s, const std::string& title,
          const std::string& resultJson, std::string& err) {
  std::string engine = std::string(pdfa::kEngineName) + " " + pdfa::kEngineVersion;
  std::string e = extOf(path), data;
  if (e == ".pdf") return renderPdf(secs, s, title, engine, path, err);
  if (e == ".html" || e == ".htm") data = renderHtml(secs, s, title, engine);
  else if (e == ".xml") data = renderXml(secs, s, engine);
  else if (e == ".json") data = renderJson(secs, resultJson);
  else if (e == ".txt") data = renderText(secs, s, title);
  else {
    err = "unsupported report format " + e + " (use .html, .xml, .pdf, .json or .txt)";
    return false;
  }
  if (!writeAll(path, data)) {
    err = "cannot write " + path;
    return false;
  }
  return true;
}

bool openPdf(QPDF& pdf, const std::string& bytes, const std::string& password, std::string& err) {
  try {
    pdf.setAttemptRecovery(true);
    pdf.setSuppressWarnings(true);
    pdf.processMemoryFile("input", bytes.data(), bytes.size(), password.empty() ? nullptr : password.c_str());
    return true;
  } catch (std::exception& e) {
    err = e.what();
    return false;
  }
}

std::string ok(const std::string& extra) { return "{\"ok\":true" + extra + "}"; }

int fail(int rc, const std::string& code, const std::string& msg) {
  std::cout << "{\"ok\":false,\"errorCode\":\"" << code << "\",\"error\":" << jsonEsc(msg) << "}" << std::endl;
  return rc;
}

void hashObj(QPDFCryptoImpl& h, QPDFObjectHandle o, std::set<QPDFObjGen>& seen, int depth) {
  auto put = [&](const std::string& s) {
    h.SHA2_update(reinterpret_cast<const unsigned char*>(s.data()), s.size());
  };
  if (depth > 64) return put("D");
  if (o.isIndirect()) {
    if (seen.count(o.getObjGen())) return put("R");
    seen.insert(o.getObjGen());
  }
  if (o.isStream()) {
    put("S<");
    QPDFObjectHandle d = o.getDict();
    for (auto& k : d.getKeys()) {
      if (k == "/Length" || k == "/Filter" || k == "/DecodeParms" || k == "/Parent" || k == "/P") continue;
      put(k);
      hashObj(h, d.getKey(k), seen, depth + 1);
    }
    std::string data;
    try {
      data = bufStr(o.getStreamData(qpdf_dl_generalized));
    } catch (std::exception&) {
      data = bufStr(o.getRawStreamData());
    }
    put(std::to_string(data.size()) + ">");
    put(data);
  } else if (o.isDictionary()) {
    put("<<");
    for (auto& k : o.getKeys()) {
      if (k == "/Parent" || k == "/P") continue;
      put(k);
      hashObj(h, o.getKey(k), seen, depth + 1);
    }
    put(">>");
  } else if (o.isArray()) {
    put("[");
    for (auto& it : o.aitems()) hashObj(h, it, seen, depth + 1);
    put("]");
  } else {
    put(o.unparse() + " ");
  }
}

std::string contentDigest(QPDF& pdf) {
  auto h = QPDFCryptoProvider::getImpl();
  h->SHA2_init(256);
  const auto& pages = pdf.getAllPages();
  std::string pc = "pages:" + std::to_string(pages.size()) + "\n";
  h->SHA2_update(reinterpret_cast<const unsigned char*>(pc.data()), pc.size());
  std::set<QPDFObjGen> seen;
  for (auto& pg : pages) {
    QPDFPageObjectHelper ph(pg);
    Pl_Buffer buf("content");
    try {
      ph.pipeContents(&buf);
    } catch (std::exception&) {
    }
    std::string c = buf.getString();
    std::string hdr = "page:" + std::to_string(c.size()) + "\n";
    h->SHA2_update(reinterpret_cast<const unsigned char*>(hdr.data()), hdr.size());
    h->SHA2_update(reinterpret_cast<const unsigned char*>(c.data()), c.size());
    for (const char* k : {"/MediaBox", "/CropBox", "/Rotate"}) {
      std::string v = k + ph.getAttribute(k, false).unparseResolved();
      h->SHA2_update(reinterpret_cast<const unsigned char*>(v.data()), v.size());
    }
    hashObj(*h, ph.getAttribute("/Resources", false), seen, 0);
  }
  h->SHA2_finalize();
  return QUtil::hex_encode(h->SHA2_digest());
}

bool writePdf(QPDF& pdf, const std::string& path, std::string& err) {
  try {
    QPDFWriter w(pdf, path.c_str());
    w.setNewlineBeforeEndstream(true);
    w.setObjectStreamMode(qpdf_o_preserve);
    w.setPreserveEncryption(false);
    w.write();
    return true;
  } catch (std::exception& e) {
    err = e.what();
    return false;
  }
}

std::string strVal(QPDFObjectHandle d, const char* k) {
  QPDFObjectHandle v = d.getKey(k);
  return v.isString() ? v.getUTF8Value() : v.isName() ? v.getName().substr(1) : "";
}

struct Args {
  std::vector<std::string> pos;
  std::map<std::string, std::string> opt;
  bool ok = true;
};

Args parseArgs(const std::vector<std::string>& args, std::initializer_list<const char*> valued) {
  Args a;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& s = args[i];
    if (s.rfind("--", 0) == 0 && s.size() > 2) {
      bool v = std::find_if(valued.begin(), valued.end(), [&](const char* n) { return s == n; }) != valued.end();
      if (!v || i + 1 >= args.size()) {
        a.ok = false;
        return a;
      }
      a.opt[s] = args[++i];
    } else {
      a.pos.push_back(s);
    }
  }
  return a;
}

#ifdef KURA_WITH_PDFIUM
void putPng(std::string& out, const char* type, const std::string& data) {
  static unsigned table[256];
  if (!table[1])
    for (unsigned n = 0; n < 256; ++n) {
      unsigned c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[n] = c;
    }
  auto be = [&](unsigned v) { out += char(v >> 24), out += char(v >> 16), out += char(v >> 8), out += char(v); };
  be(unsigned(data.size()));
  std::string chunk = std::string(type) + data;
  unsigned crc = 0xFFFFFFFFu;
  for (unsigned char c : chunk) crc = table[(crc ^ c) & 0xFF] ^ (crc >> 8);
  out += chunk;
  be(crc ^ 0xFFFFFFFFu);
}

std::string encodePng(int w, int h, const std::string& rgb) {
  Pl_Buffer buf("png");
  Pl_Flate fl("deflate", &buf, Pl_Flate::a_deflate);
  for (int y = 0; y < h; ++y) {
    unsigned char f = 0;
    fl.write(&f, 1);
    fl.write(reinterpret_cast<const unsigned char*>(rgb.data()) + size_t(y) * w * 3, size_t(w) * 3);
  }
  fl.finish();
  std::string png = "\x89PNG\r\n\x1a\n", ihdr;
  for (unsigned v : {unsigned(w), unsigned(h)}) ihdr += char(v >> 24), ihdr += char(v >> 16), ihdr += char(v >> 8), ihdr += char(v);
  ihdr += std::string("\x08\x02\x00\x00\x00", 5);
  putPng(png, "IHDR", ihdr);
  putPng(png, "IDAT", buf.getString());
  putPng(png, "IEND", "");
  return png;
}
#endif

int toolInventory(const std::vector<std::string>& args) {
  Args a = parseArgs(args, {"--password", "--report-lang", "--report-dict"});
  if (!a.ok || a.pos.empty() || a.pos.size() > 2) return fail(64, "USAGE", "inventory in.pdf [out.json|out.html|out.xml|out.pdf]");
  std::string bytes, err;
  if (!readAll(a.pos[0], bytes)) return fail(2, "IO_ERROR", "cannot open " + a.pos[0]);
  Strings s;
  if (!loadStrings(a.opt["--report-lang"].empty() ? "en" : a.opt["--report-lang"], a.opt["--report-dict"], s, err)) return fail(2, "DICT_ERROR", err);
  QPDF pdf;
  if (!openPdf(pdf, bytes, a.opt["--password"], err)) return fail(2, "PARSE_ERROR", err);
  Inventory inv;
  buildInventory(pdf, inv);
  auto secs = inventorySections(inv, s);
  if (a.pos.size() == 1) {
    std::cout << renderJson(secs, "");
    return 0;
  }
  if (!emit(a.pos[1], secs, s, s.tr("inventory"), "", err)) return fail(2, "IO_ERROR", err);
  std::cout << ok(",\"output\":" + jsonEsc(a.pos[1]) + ",\"pages\":" + std::to_string(inv.pageCount)) << std::endl;
  return 0;
}

int toolSynopsis(const std::vector<std::string>& args) {
  Args a = parseArgs(args, {"--password", "--report-lang", "--report-dict"});
  if (!a.ok || a.pos.empty() || a.pos.size() > 2) return fail(64, "USAGE", "invoice-synopsis in.pdf [out.html|out.xml|out.pdf|out.json]");
  std::string bytes, err;
  if (!readAll(a.pos[0], bytes)) return fail(2, "IO_ERROR", "cannot open " + a.pos[0]);
  Strings s;
  if (!loadStrings(a.opt["--report-lang"].empty() ? "en" : a.opt["--report-lang"], a.opt["--report-dict"], s, err)) return fail(2, "DICT_ERROR", err);
  std::vector<Section> secs;
  if (!invoiceSections(bytes, a.opt["--password"], s, secs)) return fail(1, "NO_EINVOICE", "no readable e-invoice attachment found");
  if (a.pos.size() == 1) {
    std::cout << renderJson(secs, "");
    return 0;
  }
  if (!emit(a.pos[1], secs, s, s.tr("invoice"), "", err)) return fail(2, "IO_ERROR", err);
  std::cout << ok(",\"output\":" + jsonEsc(a.pos[1])) << std::endl;
  return 0;
}

int toolVisualize(const std::vector<std::string>& args) {
  Args a = parseArgs(args, {"--dpi", "--password"});
  if (!a.ok || a.pos.size() != 3) return fail(64, "USAGE", "visualize in.pdf result.json outdir [--dpi 100]");
#ifndef KURA_WITH_PDFIUM
  return fail(2, "NO_RASTERIZER", "this build has no page rasterizer");
#else
  double dpi = a.opt.count("--dpi") ? std::atof(a.opt["--dpi"].c_str()) : 100;
  if (!(dpi >= 10 && dpi <= 600)) return fail(64, "USAGE", "--dpi must be between 10 and 600");
  std::string bytes, rj, err;
  if (!readAll(a.pos[0], bytes)) return fail(2, "IO_ERROR", "cannot open " + a.pos[0]);
  if (!readAll(a.pos[1], rj)) return fail(2, "IO_ERROR", "cannot open " + a.pos[1]);
  Result r;
  if (!parseResult(rj, r, err)) return fail(2, "PARSE_ERROR", err);
  QPDF pdf;
  if (!openPdf(pdf, bytes, a.opt["--password"], err)) return fail(2, "PARSE_ERROR", err);
  int n = int(pdf.getAllPages().size());
  std::map<int, std::vector<const Item*>> hits;
  std::vector<const Item*> docHits;
  for (auto* list : {&r.issues, &r.analysis})
    for (const Item& it : *list) {
      if (it.pages.empty()) docHits.push_back(&it);
      for (int p : it.pages) hits[p].push_back(&it);
    }
  std::error_code ec;
  std::filesystem::create_directories(a.pos[2], ec);
  auto raster = kura::makeRasterizer(bytes.data(), bytes.size(), a.opt["--password"]);
  if (!raster) return fail(2, "RASTER_ERROR", "cannot open document for rendering");
  const char* sevName[4] = {"info", "info", "warning", "error"};
  std::string html = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>Visualizer</title><style>"
                     "body{font-family:Helvetica,Arial,sans-serif;margin:2em}.pg{display:flex;gap:1.5em;margin-bottom:2em}"
                     "img{border:1px solid #ccc;max-width:480px}.error{color:#c62828}.warning{color:#e65100}.info{color:#1565c0}"
                     "</style></head><body><h1>" + htmlEsc(r.file.empty() ? a.pos[0] : r.file) + "</h1>\n";
  if (!docHits.empty()) {
    html += "<h2>Document</h2><ul>";
    for (auto* it : docHits) html += "<li class=\"" + std::string(sevName[std::clamp(it->severity, 0, 3)]) + "\"><b>" + htmlEsc(it->code) + "</b> " + htmlEsc(it->detail) + "</li>";
    html += "</ul>\n";
  }
  int marked = 0;
  for (int p = 1; p <= n; ++p) {
    int w = 0, h = 0;
    std::string rgb;
    if (!raster(p - 1, dpi, w, h, rgb) || w <= 0 || h <= 0) return fail(2, "RASTER_ERROR", "cannot render page " + std::to_string(p));
    auto it = hits.find(p);
    if (it != hits.end()) {
      ++marked;
      int sev = 0;
      for (auto* i : it->second) sev = std::max(sev, i->severity);
      unsigned char col[3] = {0x15, 0x65, 0xC0};
      if (sev >= 3 || sev == 0) col[0] = 0xE5, col[1] = 0x1C, col[2] = 0x23;
      else if (sev == 2) col[0] = 0xF5, col[1] = 0x7C, col[2] = 0x00;
      int t = std::max(4, int(dpi / 12));
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          if (x < t || y < t || x >= w - t || y >= h - t) {
            size_t o = (size_t(y) * w + x) * 3;
            rgb[o] = char(col[0]), rgb[o + 1] = char(col[1]), rgb[o + 2] = char(col[2]);
          }
    }
    char name[32];
    std::snprintf(name, sizeof name, "page-%04d.png", p);
    if (!writeAll((std::filesystem::path(a.pos[2]) / name).string(), encodePng(w, h, rgb))) return fail(2, "IO_ERROR", "cannot write " + std::string(name));
    html += "<div class=\"pg\"><img src=\"" + std::string(name) + "\" alt=\"page " + std::to_string(p) + "\"><div><h2>Page " + std::to_string(p) + "</h2>";
    if (it == hits.end()) html += "<p>-</p>";
    else {
      html += "<ul>";
      for (auto* i : it->second) html += "<li class=\"" + std::string(sevName[std::clamp(i->severity, 0, 3)]) + "\"><b>" + htmlEsc(i->code) + "</b> " + htmlEsc(i->detail) + "</li>";
      html += "</ul>";
    }
    html += "</div></div>\n";
  }
  html += "</body></html>\n";
  std::string index = (std::filesystem::path(a.pos[2]) / "index.html").string();
  if (!writeAll(index, html)) return fail(2, "IO_ERROR", "cannot write " + index);
  std::cout << ok(",\"pages\":" + std::to_string(n) + ",\"marked\":" + std::to_string(marked) + ",\"index\":" + jsonEsc(index)) << std::endl;
  return 0;
#endif
}

int toolCertify(const std::vector<std::string>& args) {
  Args a = parseArgs(args, {"--level", "--password"});
  if (!a.ok || a.pos.size() != 2) return fail(64, "USAGE", "certify in.pdf out.pdf [--level 2b]");
  pdfa::Options opt;
  opt.verifyOnly = true;
  opt.password = a.opt["--password"];
  std::string lv = a.opt.count("--level") ? a.opt["--level"] : "2b";
  if (!pdfa::levelFromString(lv, opt.level)) return fail(64, "USAGE", "unknown level " + lv);
  std::string bytes, err;
  if (!readAll(a.pos[0], bytes)) return fail(2, "IO_ERROR", "cannot open " + a.pos[0]);
  const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
#ifdef KURA_WITH_PDFIUM
  opt.rasterizePage = kura::makeRasterizer(data, bytes.size(), opt.password);
#endif
  pdfa::Result res = pdfa::convert(data, bytes.size(), opt);
  if (!res.ok) return fail(2, res.errorCode.empty() ? "CHECK_FAILED" : res.errorCode, res.error);
  QPDF pdf;
  if (!openPdf(pdf, bytes, opt.password, err)) return fail(2, "PARSE_ERROR", err);
  std::string digest = contentDigest(pdf);
  std::string level = res.level.empty() ? pdfa::levelToString(opt.level) : res.level;
  std::string now = QUtil::qpdf_time_to_pdf_time(QUtil::get_current_qpdf_time());
  std::string engine = std::string(pdfa::kEngineName) + " " + pdfa::kEngineVersion;
  QPDFObjectHandle codes = QPDFObjectHandle::newArray();
  int findings = 0;
  for (const pdfa::Issue& is : res.issues) {
    if (!is.fixed || pdfa::issueIsNormalization(is.code)) continue;
    if (++findings <= 200) codes.appendItem(QPDFObjectHandle::newString(is.code));
  }
  QPDFObjectHandle priv = QPDFObjectHandle::newDictionary();
  priv.replaceKey("/Level", QPDFObjectHandle::newString(level));
  priv.replaceKey("/Compliant", QPDFObjectHandle::newBool(res.compliant));
  priv.replaceKey("/Findings", QPDFObjectHandle::newInteger(findings));
  priv.replaceKey("/Issues", codes);
  priv.replaceKey("/Date", QPDFObjectHandle::newString(now));
  priv.replaceKey("/Engine", QPDFObjectHandle::newString(engine));
  priv.replaceKey("/ProfileFormatVersion", QPDFObjectHandle::newString("1"));
  priv.replaceKey("/DigestMethod", QPDFObjectHandle::newName("/SHA256"));
  priv.replaceKey("/Digest", QPDFObjectHandle::newString(digest));
  QPDFObjectHandle entry = QPDFObjectHandle::newDictionary();
  entry.replaceKey("/LastModified", QPDFObjectHandle::newString(now));
  entry.replaceKey("/Private", pdf.makeIndirectObject(priv));
  QPDFObjectHandle root = pdf.getRoot();
  QPDFObjectHandle pi = root.getKey("/PieceInfo");
  if (!pi.isDictionary()) {
    pi = QPDFObjectHandle::newDictionary();
    root.replaceKey("/PieceInfo", pi);
  }
  pi.replaceKey(kCertKey, entry);
  if (!writePdf(pdf, a.pos[1], err)) return fail(2, "IO_ERROR", err);
  std::cout << ok(",\"output\":" + jsonEsc(a.pos[1]) + ",\"level\":" + jsonEsc(level) + ",\"compliant\":" +
                  (res.compliant ? "true" : "false") + ",\"findings\":" + std::to_string(findings) + ",\"digest\":\"" + digest + "\"")
            << std::endl;
  return res.compliant ? 0 : 1;
}

QPDFObjectHandle findCert(QPDF& pdf) {
  QPDFObjectHandle pi = pdf.getRoot().getKey("/PieceInfo");
  if (!pi.isDictionary()) return QPDFObjectHandle::newNull();
  QPDFObjectHandle e = pi.getKey(kCertKey);
  return e.isDictionary() && e.getKey("/Private").isDictionary() ? e : QPDFObjectHandle::newNull();
}

int toolUncertify(const std::vector<std::string>& args) {
  Args a = parseArgs(args, {"--password"});
  if (!a.ok || a.pos.size() != 2) return fail(64, "USAGE", "uncertify in.pdf out.pdf");
  std::string bytes, err;
  if (!readAll(a.pos[0], bytes)) return fail(2, "IO_ERROR", "cannot open " + a.pos[0]);
  QPDF pdf;
  if (!openPdf(pdf, bytes, a.opt["--password"], err)) return fail(2, "PARSE_ERROR", err);
  bool had = !findCert(pdf).isNull();
  QPDFObjectHandle root = pdf.getRoot();
  QPDFObjectHandle pi = root.getKey("/PieceInfo");
  if (pi.isDictionary()) {
    pi.removeKey(kCertKey);
    if (pi.getKeys().empty()) root.removeKey("/PieceInfo");
  }
  if (!writePdf(pdf, a.pos[1], err)) return fail(2, "IO_ERROR", err);
  std::cout << ok(",\"output\":" + jsonEsc(a.pos[1]) + ",\"removed\":" + (had ? "true" : "false")) << std::endl;
  return 0;
}

int toolVerifyCert(const std::vector<std::string>& args) {
  Args a = parseArgs(args, {"--password"});
  if (!a.ok || a.pos.size() != 1) return fail(64, "USAGE", "verify-cert in.pdf");
  std::string bytes, err;
  if (!readAll(a.pos[0], bytes)) return fail(2, "IO_ERROR", "cannot open " + a.pos[0]);
  QPDF pdf;
  if (!openPdf(pdf, bytes, a.opt["--password"], err)) return fail(2, "PARSE_ERROR", err);
  QPDFObjectHandle cert = findCert(pdf);
  if (cert.isNull()) {
    std::cout << ok(",\"certified\":false") << std::endl;
    return 1;
  }
  QPDFObjectHandle p = cert.getKey("/Private");
  std::string stored = strVal(p, "/Digest"), now = contentDigest(pdf);
  bool unchanged = !stored.empty() && stored == now;
  bool compliant = p.getKey("/Compliant").isBool() && p.getKey("/Compliant").getBoolValue();
  QPDFObjectHandle fi = p.getKey("/Findings");
  std::cout << ok(",\"certified\":true,\"unchanged\":" + std::string(unchanged ? "true" : "false") + ",\"level\":" + jsonEsc(strVal(p, "/Level")) +
                  ",\"compliant\":" + (compliant ? "true" : "false") + ",\"findings\":" + std::to_string(fi.isInteger() ? fi.getIntValue() : 0) +
                  ",\"date\":" + jsonEsc(strVal(p, "/Date")) + ",\"engine\":" + jsonEsc(strVal(p, "/Engine")) +
                  ",\"digest\":\"" + now + "\"")
            << std::endl;
  return unchanged && compliant ? 0 : 1;
}
}

namespace kuratools {
int toolReport(const std::string& name, const std::vector<std::string>& args) {
  if (name == "inventory") return toolInventory(args);
  if (name == "visualize") return toolVisualize(args);
  if (name == "invoice-synopsis") return toolSynopsis(args);
  if (name == "certify") return toolCertify(args);
  if (name == "uncertify") return toolUncertify(args);
  if (name == "verify-cert") return toolVerifyCert(args);
  return kUnknownTool;
}

bool writeReport(const std::string& path, const std::string& resultJson, const std::string& dictPath,
                 const std::string& lang, std::string& err) {
  Strings s;
  if (!loadStrings(lang, dictPath, s, err)) return false;
  Result r;
  if (!parseResult(resultJson, r, err)) return false;
  std::vector<Section> secs = resultSections(r, s);
  std::string bytes;
  if (!r.file.empty() && readAll(r.file, bytes)) {
    std::vector<Section> inv;
    if (invoiceSections(bytes, "", s, inv)) secs.insert(secs.end(), inv.begin(), inv.end());
    QPDF pdf;
    std::string perr;
    if (openPdf(pdf, bytes, "", perr)) {
      Inventory iv;
      try {
        buildInventory(pdf, iv);
        auto is = inventorySections(iv, s);
        secs.insert(secs.end(), is.begin(), is.end());
      } catch (std::exception&) {
      }
    }
  }
  return emit(path, secs, s, s.tr("title"), resultJson, err);
}
}
