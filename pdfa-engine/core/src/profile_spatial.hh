#pragma once

#include <map>
#include <string>
#include <vector>

#include "profile_events.hh"
#include "profile_types.hh"

namespace pdfa {

long long sweepFormObjects(const std::vector<const PfCondition*>& conds, int logic, const Events& ev,
                           std::map<int, long long>& perPage, bool& supported);
void loadProfileShapes(const PfProfile& prof, Events& ev);

}
