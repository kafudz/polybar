#pragma once

#include "common.hpp"
#include "utils/color.hpp"

POLYBAR_NS

/**
 * Settings for the built-in calendar popup.
 *
 * Only a small snapshot of the bar settings is needed because the popup is
 * drawn in its own thread with its own X connection.
 */
struct calendar_settings {
  int bar_x{0};
  int bar_y{0};
  int bar_width{0};
  int bar_height{0};
  bool bar_bottom{false};
  bool week_starts_on_monday{true};
  double dpi{96.0};
  string locale{};
  string font{"sans"};
  rgba background{0xFF000000};
  rgba foreground{0xFFFFFFFF};

  /** Show current weather conditions below the calendar. */
  bool weather{false};

  /** Location passed to wttr.in, empty to let wttr.in detect it. */
  string weather_location{};
};

namespace calendar {
  /**
   * Open the calendar popup below (or above) the bar.
   *
   * If the popup is already open, a close request is issued instead.
   */
  void toggle(const calendar_settings& settings);

  /**
   * Ask an open calendar popup to close.
   */
  void close();
} // namespace calendar

POLYBAR_NS_END
