#pragma once

#include <array>
#include <cstddef>
#include <string>

inline constexpr std::array<double, 4> kNierNpcSizeBins = {0.03, 0.04, 0.05, 0.06};
inline constexpr double kNierNpcSizeSentinel = 99999.0;

size_t NierNpcSizeBin(double size);
double NierNpcScaledSize(double size, double scale);
void NierNpcCount(double size);
std::string NierNpcLodReport(double scale);
