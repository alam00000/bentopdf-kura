#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace kura {
using Rasterizer = std::function<bool(int pageIndex, double dpi, int& width, int& height,
                                      std::string& rgb)>;

Rasterizer makeRasterizer(const void* data, std::size_t size, const std::string& password);
}
