#pragma once

#include "common.hpp"
#include "utils/color.hpp"

POLYBAR_NS

/**
 * Settings for the built-in application launcher popup.
 *
 * The popup is drawn in its own thread with its own X connection, so it only
 * receives a small snapshot of the bar settings.
 */
struct launcher_menu_settings {
  int bar_x{0};
  int bar_y{0};
  int bar_width{0};
  int bar_height{0};
  bool bar_bottom{false};
  double dpi{96.0};
  string font{"sans"};
  rgba background{0xFF000000};
  rgba foreground{0xFFFFFFFF};

  /** Terminal command prefix used for desktop entries with `Terminal=true`. */
  string terminal{};
};

namespace launcher_menu {
  /**
   * Open the application launcher popup below (or above) the bar.
   *
   * If the popup is already open, a close request is issued instead.
   */
  void toggle(const launcher_menu_settings& settings);

  /**
   * Ask an open launcher popup to close.
   */
  void close();
} // namespace launcher_menu

POLYBAR_NS_END
