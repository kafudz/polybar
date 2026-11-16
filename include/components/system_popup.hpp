#pragma once

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
};

namespace system_popup {
  /**
   * Build popup settings from the bar settings and the module configuration.
   */
  system_popup_settings make_settings(const bar_settings& bar, const config& conf);

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
