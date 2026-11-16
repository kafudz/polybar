#pragma once

#include "common.hpp"
#include "utils/color.hpp"

POLYBAR_NS

/**
 * Settings for the built-in power menu popup.
 *
 * The popup is drawn in its own thread with its own X connection, so it only
 * receives a small snapshot of the bar settings and the configured commands.
 */
struct power_menu_settings {
  int bar_x{0};
  int bar_y{0};
  int bar_width{0};
  int bar_height{0};
  bool bar_bottom{false};
  double dpi{96.0};
  string font{"sans"};
  rgba background{0xFF000000};
  rgba foreground{0xFFFFFFFF};

  string lock_command{};
  string logout_command{};
  string reboot_command{};
  string shutdown_command{};
};

namespace power_menu {
  /**
   * Open the power menu popup below (or above) the bar.
   *
   * If the popup is already open, a close request is issued instead.
   */
  void toggle(const power_menu_settings& settings);

  /**
   * Ask an open power menu popup to close.
   */
  void close();
} // namespace power_menu

POLYBAR_NS_END
