#pragma once

#include <array>

#include "common.hpp"
#include "components/config.hpp"
#include "components/types.hpp"
#include "utils/color.hpp"

POLYBAR_NS

/**
 * Settings for the built-in system popup (volume, brightness, battery).
 *
 * The popup is drawn in its own thread with its own X connection, so it only
 * receives a small snapshot of the bar settings.
 */
struct system_popup_settings {
  int bar_x{0};
  int bar_y{0};
  int bar_width{0};
  int bar_height{0};
  bool bar_bottom{false};
  double dpi{96.0};
  string font{"sans"};
  rgba background{0xFF000000};
  rgba foreground{0xFFFFFFFF};

  /** Backlight device name below /sys/class/backlight, empty to autodetect. */
  string backlight_card{};

  /** Battery device name below /sys/class/power_supply, empty to autodetect. */
  string battery_name{};

  /**
   * Commands for the power section, in row order: lock, logout, reboot,
   * shutdown. Empty commands are disabled.
   */
  std::array<string, 4> power_commands{};

  /** Open the popup with the power section expanded. Defaults to collapsed. */
  bool power_expanded{false};

  /** Center the popup on the screen instead of anchoring it at the pointer. */
  bool centered{false};
};

namespace system_popup {
  /**
   * Build popup settings from the bar settings and the module configuration.
   *
   * `power_section` is the config section that holds the `lock-command`,
   * `logout-command`, `reboot-command`, and `shutdown-command` options for the
   * power section of the popup.
   */
  system_popup_settings make_settings(
      const bar_settings& bar, const config& conf, const string& power_section = "module/power");

  /**
   * Open the system popup below (or above) the bar.
   *
   * If the popup is already open, a close request is issued instead.
   */
  void toggle(const system_popup_settings& settings);

  /**
   * Ask an open system popup to close.
   */
  void close();
} // namespace system_popup

POLYBAR_NS_END
