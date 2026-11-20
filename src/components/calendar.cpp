#include "components/calendar.hpp"

#include <poll.h>
#include <xcb/xcb.h>
#include <cairo/cairo-xcb.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "components/logger.hpp"
#include "settings.hpp"
#include "utils/string.hpp"

#if ENABLE_CURL
#include "utils/http.hpp"
#endif

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

  enum class weather_status { DISABLED, LOADING, READY, ERROR };

  struct weather_info {
    weather_status status{weather_status::DISABLED};
    string condition{};
    string temperature{};
    string feels_like{};
    string humidity{};
    string wind{};
    string location{};
  };

  struct weather_shared {
    std::mutex mutex;
    weather_info info;
    std::atomic<bool> updated{false};
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

    bool weather_enabled{false};
    int weather_top{0};
    int weather_height{0};
    weather_info weather;

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

  void draw_text_left(cairo_t* cr, const string& text, double x, double center_y, double size,
      const rgb& color) {
    if (text.empty()) {
      return;
    }

    cairo_set_font_size(cr, size);

    cairo_text_extents_t extents{};
    cairo_text_extents(cr, text.c_str(), &extents);

    set_source(cr, color);
    cairo_move_to(cr, x, center_y - (extents.y_bearing + extents.height / 2.0));
    cairo_show_text(cr, text.c_str());
  }

  void draw_text_right(cairo_t* cr, const string& text, double right_x, double center_y, double size,
      const rgb& color) {
    if (text.empty()) {
      return;
    }

    cairo_set_font_size(cr, size);

    cairo_text_extents_t extents{};
    cairo_text_extents(cr, text.c_str(), &extents);

    set_source(cr, color);
    cairo_move_to(cr, right_x - (extents.x_bearing + extents.width),
        center_y - (extents.y_bearing + extents.height / 2.0));
    cairo_show_text(cr, text.c_str());
  }

  double text_width(cairo_t* cr, const string& text, double size) {
    if (text.empty()) {
      return 0.0;
    }

    cairo_set_font_size(cr, size);

    cairo_text_extents_t extents{};
    cairo_text_extents(cr, text.c_str(), &extents);
    return extents.width;
  }

  string fit_text(cairo_t* cr, const string& text, double max_width, double size) {
    if (text.empty() || max_width <= 0.0) {
      return "";
    }

    if (text_width(cr, text, size) <= max_width) {
      return text;
    }

    const string ellipsis{"…"};

    for (size_t length = text.size(); length > 0; --length) {
      string candidate = string_util::utf8_truncate(string{text}, length);

      if (candidate.empty()) {
        continue;
      }

      if (text_width(cr, candidate + ellipsis, size) <= max_width) {
        return candidate + ellipsis;
      }
    }

    return ellipsis;
  }

  void draw_cloud_shape(cairo_t* cr, double x, double y, double size, const rgb& color) {
    set_source(cr, color);

    for (const auto& blob : {std::array<double, 3>{0.32, 0.60, 0.18}, std::array<double, 3>{0.52, 0.48, 0.22},
             std::array<double, 3>{0.72, 0.60, 0.16}}) {
      cairo_new_path(cr);
      cairo_arc(cr, x + size * blob[0], y + size * blob[1], size * blob[2], 0.0, 2.0 * PI);
      cairo_fill(cr);
    }

    rounded_rect(cr, x + size * 0.14, y + size * 0.56, size * 0.74, size * 0.22, size * 0.11);
    cairo_fill(cr);
  }

  void draw_sun(cairo_t* cr, double x, double y, double size, const rgb& color) {
    const double center_x = x + size * 0.5;
    const double center_y = y + size * 0.5;
    const double radius = size * 0.16;

    set_source(cr, color);
    cairo_new_path(cr);
    cairo_arc(cr, center_x, center_y, radius, 0.0, 2.0 * PI);
    cairo_fill(cr);

    cairo_set_line_width(cr, std::max(1.1, size * 0.07));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    for (int i = 0; i < 8; i++) {
      const double angle = i * PI / 4.0;
      cairo_move_to(cr, center_x + std::cos(angle) * radius * 1.5, center_y + std::sin(angle) * radius * 1.5);
      cairo_line_to(cr, center_x + std::cos(angle) * radius * 2.0, center_y + std::sin(angle) * radius * 2.0);
    }

    cairo_stroke(cr);
  }

  void draw_rain(cairo_t* cr, double x, double y, double size, const rgb& color) {
    draw_cloud_shape(cr, x, y - size * 0.10, size, color);

    set_source(cr, color);
    cairo_set_line_width(cr, std::max(1.1, size * 0.07));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    for (int i = 0; i < 3; i++) {
      const double drop_x = x + size * (0.32 + i * 0.18);
      cairo_move_to(cr, drop_x, y + size * 0.66);
      cairo_line_to(cr, drop_x - size * 0.06, y + size * 0.84);
    }

    cairo_stroke(cr);
  }

  void draw_snow(cairo_t* cr, double x, double y, double size, const rgb& color) {
    draw_cloud_shape(cr, x, y - size * 0.10, size, color);

    set_source(cr, color);

    for (int i = 0; i < 3; i++) {
      cairo_new_path(cr);
      cairo_arc(cr, x + size * (0.32 + i * 0.18), y + size * 0.76, size * 0.035, 0.0, 2.0 * PI);
      cairo_fill(cr);
    }
  }

  void draw_thunder(cairo_t* cr, double x, double y, double size, const rgb& color) {
    draw_cloud_shape(cr, x, y - size * 0.10, size, color);

    set_source(cr, color);
    cairo_set_line_width(cr, std::max(1.1, size * 0.08));
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    cairo_move_to(cr, x + size * 0.56, y + size * 0.62);
    cairo_line_to(cr, x + size * 0.46, y + size * 0.78);
    cairo_line_to(cr, x + size * 0.56, y + size * 0.78);
    cairo_line_to(cr, x + size * 0.48, y + size * 0.94);
    cairo_stroke(cr);
  }

  void draw_fog(cairo_t* cr, double x, double y, double size, const rgb& color) {
    draw_cloud_shape(cr, x, y - size * 0.10, size, color);

    set_source(cr, color);
    cairo_set_line_width(cr, std::max(1.1, size * 0.07));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    cairo_move_to(cr, x + size * 0.18, y + size * 0.76);
    cairo_line_to(cr, x + size * 0.78, y + size * 0.76);
    cairo_move_to(cr, x + size * 0.28, y + size * 0.90);
    cairo_line_to(cr, x + size * 0.68, y + size * 0.90);
    cairo_stroke(cr);
  }

  void draw_weather_icon(cairo_t* cr, const string& condition, double x, double y, double size,
      const rgb& color) {
    const string lower = string_util::lower(condition);

    if (lower.find("thunder") != string::npos || lower.find("storm") != string::npos) {
      draw_thunder(cr, x, y, size, color);
    } else if (lower.find("snow") != string::npos || lower.find("sleet") != string::npos ||
        lower.find("ice") != string::npos) {
      draw_snow(cr, x, y, size, color);
    } else if (lower.find("rain") != string::npos || lower.find("drizzle") != string::npos ||
        lower.find("shower") != string::npos) {
      draw_rain(cr, x, y, size, color);
    } else if (lower.find("fog") != string::npos || lower.find("mist") != string::npos ||
        lower.find("haze") != string::npos) {
      draw_fog(cr, x, y, size, color);
    } else if (lower.find("cloud") != string::npos || lower.find("overcast") != string::npos) {
      draw_cloud_shape(cr, x, y, size, color);
    } else if (lower.find("sun") != string::npos || lower.find("clear") != string::npos) {
      draw_sun(cr, x, y, size, color);
    } else {
      draw_cloud_shape(cr, x, y, size, color);
    }
  }

#if ENABLE_CURL
  string url_encode(const string& value) {
    static const char* hex = "0123456789ABCDEF";
    string encoded;
    encoded.reserve(value.size());

    for (unsigned char c : value) {
      if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
        encoded += static_cast<char>(c);
      } else if (c == ' ') {
        encoded += "%20";
      } else {
        encoded += '%';
        encoded += hex[c >> 4];
        encoded += hex[c & 0x0F];
      }
    }

    return encoded;
  }

  string weather_url(const string& location) {
    string url{"https://wttr.in/"};

    if (!location.empty()) {
      url += url_encode(location);
    }

    url += "?format=%C%7C%t%7C%f%7C%h%7C%w%7C%l";
    return url;
  }

  bool parse_weather(const string& response, weather_info& info) {
    string text = response;

    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
      text.pop_back();
    }

    const auto fields = string_util::split(text, '|');

    if (fields.size() < 6) {
      return false;
    }

    info.condition = fields[0];
    info.temperature = fields[1];
    info.feels_like = fields[2];
    info.humidity = fields[3];
    info.wind = fields[4];
    info.location = fields[5];

    for (size_t i = 6; i < fields.size(); i++) {
      info.location += "|" + fields[i];
    }

    return !info.temperature.empty();
  }
#endif

  void fetch_weather(const std::shared_ptr<weather_shared>& weather, const string& location) {
    weather_info result;

#if ENABLE_CURL
    try {
      http_downloader downloader(4, 8);
      const string response = downloader.get(weather_url(location));

      if (parse_weather(response, result)) {
        result.status = weather_status::READY;
      } else {
        result.status = weather_status::ERROR;
      }
    } catch (const std::exception& err) {
      logger::make().err("calendar: could not fetch weather (%s)", err.what());
      result.status = weather_status::ERROR;
    }
#else
    (void)location;
    result.status = weather_status::ERROR;
#endif

    {
      std::lock_guard<std::mutex> lock(weather->mutex);
      weather->info = move(result);
    }

    weather->updated = true;
  }

  void draw_weather(cairo_t* cr, const calendar_state& state) {
    const double scale = state.scale;

    set_source(cr, state.border);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, state.pad, state.weather_top);
    cairo_line_to(cr, state.width - state.pad, state.weather_top);
    cairo_stroke(cr);

    const double line1_y = state.weather_top + 15.0 * scale;
    const double line2_y = state.weather_top + 33.0 * scale;
    const double line3_y = state.weather_top + 51.0 * scale;
    const double status_y = state.weather_top + state.weather_height / 2.0;

    if (state.weather.status == weather_status::LOADING) {
      draw_text_left(cr, "Loading weather…", state.pad, status_y, 12.0 * scale, state.muted);
      return;
    }

    if (state.weather.status != weather_status::READY) {
      draw_text_left(cr, "Weather unavailable", state.pad, status_y, 12.0 * scale, state.muted);
      return;
    }

    const double icon_size = 18.0 * scale;
    const double icon_x = state.pad;
    const double icon_y = line1_y - icon_size / 2.0;
    draw_weather_icon(cr, state.weather.condition, icon_x, icon_y, icon_size, state.foreground);

    const double condition_size = 12.0 * scale;
    const double temperature_size = 12.0 * scale;
    const double temperature_width = text_width(cr, state.weather.temperature, temperature_size);
    const double condition_x = icon_x + icon_size + 8.0 * scale;
    const double condition_right = state.width - state.pad - temperature_width - 8.0 * scale;
    const string condition = state.weather.condition.empty() ? "Weather" : state.weather.condition;

    draw_text_left(cr, fit_text(cr, condition, condition_right - condition_x, condition_size), condition_x,
        line1_y, condition_size, state.foreground);
    draw_text_right(cr, state.weather.temperature, state.width - state.pad, line1_y, temperature_size,
        state.foreground);

    string detail = state.weather.location;
    const auto append = [&detail](const string& value, const string& prefix) {
      if (value.empty()) {
        return;
      }

      if (!detail.empty()) {
        detail += "  ·  ";
      }

      detail += prefix + value;
    };

    append(state.weather.feels_like, "feels ");
    append(state.weather.humidity, "");
    append(state.weather.wind, "");

    const double detail_size = 12.0 * scale;
    const double detail_width = state.width - 2.0 * state.pad;

    string first_line = detail;
    string second_line;

    // Keep the whole status visible: if it does not fit on one line, move the
    // trailing sections to a second line instead of truncating them.
    if (text_width(cr, first_line, detail_size) > detail_width) {
      const string separator = "  ·  ";

      while (!first_line.empty()) {
        const auto split = first_line.rfind(separator);

        if (split == string::npos) {
          break;
        }

        const string tail = first_line.substr(split + separator.size());
        first_line.erase(split);
        second_line = second_line.empty() ? tail : tail + separator + second_line;

        if (text_width(cr, first_line, detail_size) <= detail_width) {
          break;
        }
      }
    }

    draw_text_left(cr, fit_text(cr, first_line, detail_width, detail_size), state.pad, line2_y, detail_size,
        state.muted);

    if (!second_line.empty()) {
      draw_text_left(cr, fit_text(cr, second_line, detail_width, detail_size), state.pad, line3_y,
          detail_size, state.muted);
    }
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
    cairo_rectangle(cr, 0.5, 0.5, state.width - 1.0, state.height - 1.0);
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

      draw_text(cr, format_time(day, "%a", state.locale), center_x, center_y, 12.0 * state.scale,
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
        cairo_rectangle(cr, center_x - (state.cell_width - 6.0 * state.scale) / 2.0,
            center_y - (state.cell_height - 4.0 * state.scale) / 2.0, state.cell_width - 6.0 * state.scale,
            state.cell_height - 4.0 * state.scale);
        set_source(cr, state.foreground);
        cairo_fill(cr);
      }

      draw_text(cr, std::to_string(day), center_x, center_y, 12.0 * state.scale,
          is_today ? state.background : state.foreground);
    }

    if (state.weather_enabled) {
      draw_weather(cr, state);
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
      state.cell_width = static_cast<int>(std::round(46.0 * scale));
      state.cell_height = static_cast<int>(std::round(28.0 * scale));
      state.width = 7 * state.cell_width + 2 * state.pad;

      const int grid_bottom = state.pad + state.header_height + state.weekday_height + 6 * state.cell_height;
      state.weather_enabled = settings.weather;
      state.weather_top = grid_bottom + static_cast<int>(std::round(4.0 * scale));
      state.weather_height = state.weather_enabled ? static_cast<int>(std::round(62.0 * scale)) : 0;
      state.height = state.weather_enabled ? state.weather_top + state.weather_height : grid_bottom + state.pad;

      state.week_starts_on_monday = settings.week_starts_on_monday;
      state.font = settings.font.empty() ? "sans" : settings.font;
      state.locale = settings.locale;

      state.background = make_rgb(settings.background, {0.06, 0.06, 0.07});
      state.foreground = make_rgb(settings.foreground, {0.85, 0.85, 0.85});
      state.border = mix(state.background, state.foreground, 0.25);
      state.muted = mix(state.background, state.foreground, 0.6);

      std::shared_ptr<weather_shared> weather;

      if (state.weather_enabled) {
        state.weather.status = weather_status::LOADING;
        weather = std::make_shared<weather_shared>();

        {
          std::lock_guard<std::mutex> lock(weather->mutex);
          weather->info.status = weather_status::LOADING;
        }

        try {
          const string location = settings.weather_location;
          std::thread([weather, location] { fetch_weather(weather, location); }).detach();
        } catch (const std::exception& err) {
          state.weather.status = weather_status::ERROR;
          logger::make().err("calendar: could not start weather thread (%s)", err.what());
        }
      }

      std::time_t now = std::time(nullptr);
      std::tm today{};
      localtime_r(&now, &today);

      state.today_year = today.tm_year + 1900;
      state.today_month = today.tm_mon;
      state.today_day = today.tm_mday;
      state.year = state.today_year;
      state.month = state.today_month;

      // Keep the calendar at a fixed position just below the bar, centered on
      // the screen (below the clock).
      const int margin = static_cast<int>(std::round(4.0 * scale));

      state.x = (static_cast<int>(screen->width_in_pixels) - state.width) / 2;
      state.y = settings.bar_bottom ? settings.bar_y - state.height - margin
                                    : settings.bar_y + settings.bar_height + margin;

      const int max_x = std::max(margin, static_cast<int>(screen->width_in_pixels) - state.width - margin);
      const int max_y = std::max(margin, static_cast<int>(screen->height_in_pixels) - state.height - margin);
      state.x = std::clamp(state.x, margin, max_x);
      state.y = std::clamp(state.y, margin, max_y);

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

      xcb_grab_pointer_reply_t* grab_reply = nullptr;

      // Clicking the clock starts the popup while the button press still holds
      // an implicit pointer grab on the bar. Retry briefly so the popup ends up
      // with the grab and can dismiss itself on any outside click.
      for (int attempt = 0; attempt < 100; attempt++) {
        grab_reply = xcb_grab_pointer_reply(connection,
            xcb_grab_pointer(connection, 0, window,
                XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION,
                XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME),
            nullptr);

        const bool success = grab_reply != nullptr && grab_reply->status == XCB_GRAB_STATUS_SUCCESS;

        if (grab_reply != nullptr) {
          std::free(grab_reply);
        }

        if (success) {
          pointer_grabbed = true;
          break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
        if (weather && weather->updated.exchange(false)) {
          {
            std::lock_guard<std::mutex> lock(weather->mutex);
            state.weather = weather->info;
          }

          redraw();
        }

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

            bool handled = false;

            if (button->detail == 4) {
              handled = true;
              update_month(state, -1);
              redraw();
            } else if (button->detail == 5) {
              handled = true;
              update_month(state, 1);
              redraw();
            } else if (previous_button(state).contains(pointer_x_pos, pointer_y_pos)) {
              handled = true;
              update_month(state, -1);
              redraw();
            } else if (next_button(state).contains(pointer_x_pos, pointer_y_pos)) {
              handled = true;
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

            if (!handled) {
              running = false;
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
