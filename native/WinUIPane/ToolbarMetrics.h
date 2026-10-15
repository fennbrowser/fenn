#pragma once

// Size of the controls in the navigation row and beside the first tab, matched
// to Dia's default toolbar: 14-DIP glyphs in 32-DIP targets with 4-DIP gaps
// (hover wash measured at 32x32 in Dia's strip). Only the glyph is smaller than
// the earlier 16-DIP Firefox-like density.
namespace ToolbarMetrics {
constexpr double kButton = 32;
constexpr double kIcon = 14;
constexpr double kGap = 4;
}
