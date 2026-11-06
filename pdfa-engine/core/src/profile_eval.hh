#pragma once

#include <string>

#include "check_registry.hh"
#include "profile_events.hh"
#include "profile_types.hh"

namespace pdfa {

struct XmpNodeScope {
  XmpNodeScope();
  ~XmpNodeScope();
  XmpNodeScope(const XmpNodeScope&) = delete;
  XmpNodeScope& operator=(const XmpNodeScope&) = delete;
  void* prev;
  void* sets = nullptr;
};

bool boxesIntersect(const Box& a, const Box& b);
const PageFacts* pageFor(const Events& ev, int page);

}
