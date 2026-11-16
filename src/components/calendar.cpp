#include "components/calendar.hpp"

#include <poll.h>
#include <xcb/xcb.h>
#include <cairo/cairo-xcb.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "components/logger.hpp"

POLYBAR_NS

namespace {
  constexpr double PI = 3.14159265358979323846;

  std::atomic<bool> g_open{false};
  std::atomic<bool> g_close_requested{false};

  /**
   * The X connection is kept alive across popup invocations.
   *
   * Cairo caches its XCB backend state by connection pointer. Reconnecting for
   * every popup can hand out the same pointer for a new connection while cairo
   * still holds state for the old (closed) one, which makes all cairo drawing
   * silently go to the dead connection.
   */
  std::mutex g_connection_lock;
  xcb_connection_t* g_connection{nullptr};

  struct rgb {
    double r{0.0};
    double g{0.0};
    double b{0.0};
  };

  struct calendar_state {
    int x{0};
    int y{0};
    int width{0};
    int height{0};

    int pad{0};
    int header_height{0};
    int weekday_height{0};
    int cell_width{0};
    int cell_height{0};
    double scale{1.0};

    int year{0};
    int month{0};
    int today_year{0};
    int today_month{0};
    int today_day{0};
    bool week_starts_on_monday{true};

    string font{"sans"};
    string locale{};

    rgb background{0.0, 0.0, 0.0};
    rgb foreground{1.0, 1.0, 1.0};
    rgb border{0.3, 0.3, 0.3};
    rgb muted{0.6, 0.6, 0.6};
  };

  struct button_area {
    int x{0};
    int y{0};
    int width{0};
    int height{0};

    bool contains(int px, int py) const {
      return px >= x && px < x + width && py >= y && py < y + height;
    }
  };

  rgb make_rgb(const rgba& color, const rgb& fallback) {
    if (!color.has_color() || color.get_type() == rgba::type::ALPHA_ONLY) {
      return fallback;
    }

    return {color.red_d(), color.green_d(), color.blue_d()};
  }

  rgb mix(const rgb& a, const rgb& b, double amount) {
    return {a.r + (b.r - a.r) * amount, a.g + (b.g - a.g) * amount, a.b + (b.b - a.b) * amount};
  }

  void set_source(cairo_t* cr, const rgb& color, double alpha = 1.0) {
    cairo_set_source_rgba(cr, color.r, color.g, color.b, alpha);
  }

  void rounded_rect(cairo_t* cr, double x, double y, double width, double height, double radius) {
    radius = std::min(radius, std::min(width, height) / 2.0);

    cairo_new_sub_path(cr);
    cairo_arc(cr, x + width - radius, y + radius, radius, -PI / 2.0, 0.0);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0.0, PI / 2.0);
    cairo_arc(cr, x + radius, y + height - radius, radius, PI / 2.0, PI);
    cairo_arc(cr, x + radius, y + radius, radius, PI, 3.0 * PI / 2.0);
    cairo_close_path(cr);
  }

  void draw_text(cairo_t* cr, const string& text, double center_x, double center_y, double size, const rgb& color,
      double alpha = 1.0) {
    if (text.empty()) {
      return;
    }

    cairo_set_font_size(cr, size);

    cairo_text_extents_t extents{};
    cairo_text_extents(cr, text.c_str(), &extents);

    set_source(cr, color, alpha);
    cairo_move_to(cr, center_x - (extents.x_bearing + extents.width / 2.0),
        center_y - (extents.y_bearing + extents.height / 2.0));
    cairo_show_text(cr, text.c_str());
  }

  void draw_chevron(cairo_t* cr, double center_x, double center_y, double size, bool pointing_left, const rgb& color) {
    set_source(cr, color);
    cairo_set_line_width(cr, std::max(1.0, size * 0.12));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    double direction = pointing_left ? -1.0 : 1.0;
    cairo_move_to(cr, center_x - direction * size / 2.0, center_y - size / 2.0);
    cairo_line_to(cr, center_x + direction * size / 2.0, center_y);
    cairo_line_to(cr, center_x - direction * size / 2.0, center_y + size / 2.0);
    cairo_stroke(cr);
  }

  string format_time(const std::tm& time, const char* format, const string& locale) {
    std::ostringstream stream;

    if (!locale.empty()) {
      try {
        stream.imbue(std::locale(locale));
      } catch (const std::exception&) {
        // Fall back to the default locale if the configured one is not available.
      }
    }

    stream << std::put_time(&time, format);
    return stream.str();
  }

  int days_in_month(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (month == 1) {
      const bool leap_year = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
      return leap_year ? 29 : 28;
    }

    return days[month];
  }

  int first_day_offset(int year, int month, bool week_starts_on_monday) {
    std::tm time{};
    time.tm_year = year - 1900;
    time.tm_mon = month;
    time.tm_mday = 1;
    time.tm_isdst = -1;
    std::mktime(&time);

    return week_starts_on_monday ? (time.tm_wday + 6) % 7 : time.tm_wday;
  }

  xcb_visualtype_t* find_visualtype(xcb_screen_t* screen) {
    xcb_depth_iterator_t depth_iter = xcb_screen_allowed_depths_iterator(screen);

    for (; depth_iter.rem; xcb_depth_next(&depth_iter)) {
      xcb_visualtype_iterator_t visual_iter = xcb_depth_visuals_iterator(depth_iter.data);

      for (; visual_iter.rem; xcb_visualtype_next(&visual_iter)) {
        if (visual_iter.data->visual_id == screen->root_visual) {
          return visual_iter.data;
        }
      }
    }

    return nullptr;
  }

  button_area previous_button(const calendar_state& state) {
    return {state.pad, state.pad, state.header_height, state.header_height};
  }

  button_area next_button(const calendar_state& state) {
    return {state.width - state.pad - state.header_height, state.pad, state.header_height, state.header_height};
  }

  void paint(xcb_connection_t* connection, xcb_pixmap_t pixmap, xcb_visualtype_t* visual,
      const calendar_state& state) {
    cairo_surface_t* surface =
        cairo_xcb_surface_create(connection, pixmap, visual, state.width, state.height);
    cairo_t* cr = cairo_create(surface);

    cairo_select_font_face(cr, state.font.c_str(), CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    // Background
    set_source(cr, state.background);
    cairo_paint(cr);

    // Subtle border around the popup
    rounded_rect(cr, 0.5, 0.5, state.width - 1.0, state.height - 1.0, 6.0 * state.scale);
    set_source(cr, state.border);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    const double header_center_y = state.pad + state.header_height / 2.0;

    // Month/year title
    std::tm view_time{};
    view_time.tm_year = state.year - 1900;
    view_time.tm_mon = state.month;
    view_time.tm_mday = 1;
    draw_text(cr, format_time(view_time, "%B %Y", state.locale), state.width / 2.0, header_center_y,
        14.0 * state.scale, state.foreground);

    // Navigation arrows
    const button_area prev = previous_button(state);
    const button_area next = next_button(state);
    draw_chevron(cr, prev.x + prev.width / 2.0, prev.y + prev.height / 2.0, 10.0 * state.scale, true,
        state.foreground);
    draw_chevron(cr, next.x + next.width / 2.0, next.y + next.height / 2.0, 10.0 * state.scale, false,
        state.foreground);

    const int first_day = first_day_offset(state.year, state.month, state.week_starts_on_monday);
    const int number_of_days = days_in_month(state.year, state.month);

    const double grid_x = state.pad;
    const double grid_y = state.pad + state.header_height;

    // Weekday headers
    for (int column = 0; column < 7; column++) {
      std::tm day{};
      day.tm_year = state.year - 1900;
      day.tm_mon = state.month;
      day.tm_mday = 1 - first_day + column;
      day.tm_isdst = -1;
      std::mktime(&day);

      const double center_x = grid_x + column * state.cell_width + state.cell_width / 2.0;
      const double center_y = grid_y + state.weekday_height / 2.0;

      draw_text(cr, format_time(day, "%a", state.locale), center_x, center_y, 10.0 * state.scale,
          state.muted);
    }

    // Day cells
    const double days_y = grid_y + state.weekday_height;

    for (int day = 1; day <= number_of_days; day++) {
      const int index = first_day + day - 1;
      const int row = index / 7;
      const int column = index % 7;

      if (row >= 6) {
        break;
      }

      const double center_x = grid_x + column * state.cell_width + state.cell_width / 2.0;
      const double center_y = days_y + row * state.cell_height + state.cell_height / 2.0;

      const bool is_today = state.year == state.today_year && state.month == state.today_month &&
          day == state.today_day;

      if (is_today) {
        rounded_rect(cr, center_x - (state.cell_width - 6.0 * state.scale) / 2.0,
            center_y - (state.cell_height - 4.0 * state.scale) / 2.0, state.cell_width - 6.0 * state.scale,
            state.cell_height - 4.0 * state.scale, 5.0 * state.scale);
        set_source(cr, state.foreground);
        cairo_fill(cr);
      }

      draw_text(cr, std::to_string(day), center_x, center_y, 11.0 * state.scale,
          is_today ? state.background : state.foreground);
    }

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
  }

  void present(xcb_connection_t* connection, xcb_pixmap_t pixmap, xcb_window_t window, xcb_gcontext_t gc,
      const calendar_state& state) {
    xcb_copy_area(connection, pixmap, window, gc, 0, 0, 0, 0, state.width, state.height);
    xcb_flush(connection);
  }

  /**
   * Builds a keycode -> keysym table so the popup can handle Escape and the
   * arrow keys without depending on libxcb-keysyms.
   */
  std::vector<xcb_keysym_t> get_keymap(xcb_connection_t* connection) {
    const xcb_setup_t* setup = xcb_get_setup(connection);
    const xcb_keycode_t min_keycode = setup->min_keycode;
    const xcb_keycode_t max_keycode = setup->max_keycode;

    std::vector<xcb_keysym_t> keymap(max_keycode + 1, 0);

    xcb_get_keyboard_mapping_reply_t* reply = xcb_get_keyboard_mapping_reply(connection,
        xcb_get_keyboard_mapping(connection, min_keycode, max_keycode - min_keycode + 1), nullptr);

    if (reply == nullptr) {
      return keymap;
    }

    const xcb_keysym_t* keysyms = xcb_get_keyboard_mapping_keysyms(reply);

    for (int keycode = min_keycode; keycode <= max_keycode; keycode++) {
      const int index = (keycode - min_keycode) * reply->keysyms_per_keycode;
      keymap[keycode] = keysyms[index];
    }

    std::free(reply);
    return keymap;
  }

  void update_month(calendar_state& state, int delta) {
    state.month += delta;

    while (state.month < 0) {
      state.month += 12;
      state.year--;
    }

    while (state.month > 11) {
      state.month -= 12;
      state.year++;
    }
  }

  void run_popup(calendar_settings settings) {
    xcb_connection_t* connection = nullptr;
    xcb_window_t window = XCB_NONE;
    xcb_pixmap_t pixmap = XCB_NONE;
    xcb_gcontext_t gc = XCB_NONE;
    bool pointer_grabbed = false;
    bool keyboard_grabbed = false;

    try {
      {
        std::lock_guard<std::mutex> lock(g_connection_lock);
        connection = g_connection;
        g_connection = nullptr;
      }

      if (connection != nullptr && xcb_connection_has_error(connection)) {
        xcb_disconnect(connection);
        connection = nullptr;
      }

      if (connection == nullptr) {
        connection = xcb_connect(nullptr, nullptr);
      }

      if (connection == nullptr || xcb_connection_has_error(connection)) {
        throw std::runtime_error("Could not connect to the X server");
      }

      // Drop events left over from a previous popup.
      while (xcb_generic_event_t* event = xcb_poll_for_event(connection)) {
        std::free(event);
      }

      xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;

      if (screen == nullptr) {
        throw std::runtime_error("No X screen available");
      }

      xcb_visualtype_t* visual = find_visualtype(screen);

      if (visual == nullptr) {
        throw std::runtime_error("Could not find the root visual");
      }

      const double scale = std::max(1.0, settings.dpi / 96.0);

      calendar_state state;
      state.scale = scale;
      state.pad = static_cast<int>(std::round(10.0 * scale));
      state.header_height = static_cast<int>(std::round(32.0 * scale));
      state.weekday_height = static_cast<int>(std::round(18.0 * scale));
      state.cell_width = static_cast<int>(std::round(34.0 * scale));
      state.cell_height = static_cast<int>(std::round(28.0 * scale));
      state.width = 7 * state.cell_width + 2 * state.pad;
      state.height = state.pad + state.header_height + state.weekday_height + 6 * state.cell_height + state.pad;
      state.week_starts_on_monday = settings.week_starts_on_monday;
      state.font = settings.font.empty() ? "sans" : settings.font;
      state.locale = settings.locale;

      state.background = make_rgb(settings.background, {0.06, 0.06, 0.07});
      state.foreground = make_rgb(settings.foreground, {0.85, 0.85, 0.85});
      state.border = mix(state.background, state.foreground, 0.25);
      state.muted = mix(state.background, state.foreground, 0.6);

      std::time_t now = std::time(nullptr);
      std::tm today{};
      localtime_r(&now, &today);

      state.today_year = today.tm_year + 1900;
      state.today_month = today.tm_mon;
      state.today_day = today.tm_mday;
      state.year = state.today_year;
      state.month = state.today_month;

      // Center the popup below the clock, or above it if the bar is at the bottom.
      const int margin = static_cast<int>(std::round(4.0 * scale));
      int pointer_x = settings.bar_x + settings.bar_width / 2;
      int pointer_y = settings.bar_y + settings.bar_height / 2;

      xcb_query_pointer_reply_t* pointer_reply = xcb_query_pointer_reply(connection,
          xcb_query_pointer(connection, screen->root), nullptr);

      if (pointer_reply != nullptr) {
        pointer_x = pointer_reply->root_x;
        pointer_y = pointer_reply->root_y;
        std::free(pointer_reply);
      }

      state.x = pointer_x - state.width / 2;
      state.y = settings.bar_bottom ? settings.bar_y - state.height - margin
                                    : settings.bar_y + settings.bar_height + margin;

      const int max_x = std::max(margin, static_cast<int>(screen->width_in_pixels) - state.width - margin);
      const int max_y = std::max(margin, static_cast<int>(screen->height_in_pixels) - state.height - margin);
      state.x = std::clamp(state.x, margin, max_x);
      state.y = std::clamp(state.y, margin, max_y);

      // Silence unused variable warning on platforms where the pointer reply is not available.
      (void)pointer_y;

      const uint32_t background_pixel = (static_cast<uint32_t>(state.background.r * 255.0) << 16) |
          (static_cast<uint32_t>(state.background.g * 255.0) << 8) |
          static_cast<uint32_t>(state.background.b * 255.0);

      uint32_t window_values[] = {background_pixel, 1,
          XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_KEY_PRESS |
              XCB_EVENT_MASK_POINTER_MOTION};

      window = xcb_generate_id(connection);
      xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root, state.x, state.y, state.width,
          state.height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
          XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, window_values);

      const string window_name{"polybar-calendar"};
      xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
          window_name.size(), window_name.c_str());

      pixmap = xcb_generate_id(connection);
      xcb_create_pixmap(connection, screen->root_depth, pixmap, window, state.width, state.height);
      gc = xcb_generate_id(connection);
      xcb_create_gc(connection, gc, window, 0, nullptr);

      xcb_map_window(connection, window);
      const uint32_t stack_above[] = {XCB_STACK_MODE_ABOVE};
      xcb_configure_window(connection, window, XCB_CONFIG_WINDOW_STACK_MODE, stack_above);
      xcb_flush(connection);

      xcb_grab_pointer_reply_t* grab_reply = xcb_grab_pointer_reply(connection,
          xcb_grab_pointer(connection, 0, window,
              XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
              XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME),
          nullptr);

      if (grab_reply != nullptr) {
        pointer_grabbed = grab_reply->status == XCB_GRAB_STATUS_SUCCESS;
        std::free(grab_reply);
      }

      xcb_grab_keyboard_reply_t* keyboard_reply = xcb_grab_keyboard_reply(connection,
          xcb_grab_keyboard(connection, 0, window, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC),
          nullptr);

      if (keyboard_reply != nullptr) {
        keyboard_grabbed = keyboard_reply->status == XCB_GRAB_STATUS_SUCCESS;
        std::free(keyboard_reply);
      }

      const std::vector<xcb_keysym_t> keymap = get_keymap(connection);

      const auto redraw = [&] {
        paint(connection, pixmap, visual, state);
        present(connection, pixmap, window, gc, state);
      };

      redraw();

      bool running = true;

      while (running && !g_close_requested.load()) {
        xcb_generic_event_t* event = xcb_poll_for_event(connection);

        if (event == nullptr) {
          if (xcb_connection_has_error(connection)) {
            break;
          }

          pollfd descriptor{xcb_get_file_descriptor(connection), POLLIN, 0};

          if (poll(&descriptor, 1, 100) <= 0) {
            continue;
          }

          event = xcb_poll_for_event(connection);

          if (event == nullptr) {
            continue;
          }
        }

        const uint8_t event_type = event->response_type & ~0x80;

        switch (event_type) {
          case XCB_EXPOSE:
            redraw();
            break;

          case XCB_BUTTON_PRESS: {
            auto* button = reinterpret_cast<xcb_button_press_event_t*>(event);
            const int pointer_x_pos = button->root_x - state.x;
            const int pointer_y_pos = button->root_y - state.y;

            if (pointer_x_pos < 0 || pointer_y_pos < 0 || pointer_x_pos >= state.width ||
                pointer_y_pos >= state.height) {
              running = false;
              break;
            }

            if (button->detail == 4) {
              update_month(state, -1);
              redraw();
            } else if (button->detail == 5) {
              update_month(state, 1);
              redraw();
            } else if (previous_button(state).contains(pointer_x_pos, pointer_y_pos)) {
              update_month(state, -1);
              redraw();
            } else if (next_button(state).contains(pointer_x_pos, pointer_y_pos)) {
              update_month(state, 1);
              redraw();
            } else {
              const int first_day = first_day_offset(state.year, state.month, state.week_starts_on_monday);
              const int days = days_in_month(state.year, state.month);
              const int grid_y = state.pad + state.header_height + state.weekday_height;

              if (pointer_x_pos >= state.pad && pointer_y_pos >= grid_y) {
                const int column = (pointer_x_pos - state.pad) / state.cell_width;
                const int row = (pointer_y_pos - grid_y) / state.cell_height;

                if (column >= 0 && column < 7 && row >= 0 && row < 6) {
                  const int day = row * 7 + column - first_day + 1;

                  if (day >= 1 && day <= days) {
                    running = false;
                  }
                }
              }
            }

            break;
          }

          case XCB_KEY_PRESS: {
            auto* key = reinterpret_cast<xcb_key_press_event_t*>(event);
            const xcb_keysym_t keysym = key->detail < keymap.size() ? keymap[key->detail] : 0;

            switch (keysym) {
              case 0xFF1B: // Escape
                running = false;
                break;
              case 0xFF51: // Left
              case 0xFF55: // Page Up
                update_month(state, -1);
                redraw();
                break;
              case 0xFF53: // Right
              case 0xFF56: // Page Down
                update_month(state, 1);
                redraw();
                break;
              case 0xFF50: // Home
                state.year = state.today_year;
                state.month = state.today_month;
                redraw();
                break;
              default:
                break;
            }

            break;
          }

          default:
            break;
        }

        std::free(event);
      }
    } catch (const std::exception& err) {
      logger::make().err("calendar: %s", err.what());
    }

    if (connection != nullptr) {
      if (xcb_connection_has_error(connection) == 0) {
        if (pointer_grabbed) {
          xcb_ungrab_pointer(connection, XCB_CURRENT_TIME);
        }

        if (keyboard_grabbed) {
          xcb_ungrab_keyboard(connection, XCB_CURRENT_TIME);
        }

        if (gc != XCB_NONE) {
          xcb_free_gc(connection, gc);
        }

        if (pixmap != XCB_NONE) {
          xcb_free_pixmap(connection, pixmap);
        }

        if (window != XCB_NONE) {
          xcb_destroy_window(connection, window);
        }

        xcb_flush(connection);

        std::lock_guard<std::mutex> lock(g_connection_lock);
        if (g_connection == nullptr) {
          g_connection = connection;
          connection = nullptr;
        }
      }

      if (connection != nullptr) {
        xcb_disconnect(connection);
      }
    }

    g_open = false;
    g_close_requested = false;
  }
} // namespace

namespace calendar {
  void toggle(const calendar_settings& settings) {
    if (g_open.exchange(true)) {
      g_close_requested = true;
      return;
    }

    g_close_requested = false;

    try {
      std::thread([settings] { run_popup(settings); }).detach();
    } catch (const std::exception& err) {
      g_open = false;
      logger::make().err("calendar: Could not start popup thread: %s", err.what());
    }
  }

  void close() {
    g_close_requested = true;
  }
} // namespace calendar

POLYBAR_NS_END
