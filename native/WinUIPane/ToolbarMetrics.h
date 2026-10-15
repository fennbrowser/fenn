#pragma once

// Size of the controls in the navigation row and beside the first tab, matched
// to Dia's default toolbar: 14-DIP glyphs in 28-DIP targets, spaced so the
// button pitch stays 36 DIP. Supersedes the earlier 16/32/4 Firefox-like density.
namespace ToolbarMetrics {
constexpr double kButton = 28;
constexpr double kIcon = 14;
constexpr double kGap = 8;
}
