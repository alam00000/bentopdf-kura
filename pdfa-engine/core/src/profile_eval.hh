#pragma once

#include <string>

#include "profile_events.hh"
#include "profile_types.hh"

namespace pdfa {

enum class Domain { kNone, kPaint, kText, kImage, kDoc, kPage, kAny, kAnnot, kFont };

Domain atomDomain(const std::string& token);
bool evalPaintAtom(const PfAtom& a, const PaintEvent& e, const Events& ev, bool& supported);
bool evalTextAtom(const PfAtom& a, const TextEvent& e, const Events& ev, bool& supported);
bool evalImageAtom(const PfAtom& a, const ImageEvent& e, const Events& ev, bool& supported);
bool evalAnnotAtom(const PfAtom& a, const AnnotFacts& an, const Events& ev, bool& supported);
bool evalFontAtom(const PfAtom& a, const FontFacts& f, const Events& ev, bool& supported);
bool evalDocAtom(const PfAtom& a, const Events& ev, bool& supported);
bool evalPageAtom(const PfAtom& a, const PageFacts& p, bool& supported);
bool boxContains(const Box& outer, const Box& inner, double tol);
bool boxesIntersect(const Box& a, const Box& b);
const PageFacts* pageFor(const Events& ev, int page);

}
