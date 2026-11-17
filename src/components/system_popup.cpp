#include "components/system_popup.hpp"

#include <poll.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <cairo/cairo-xcb.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "components/logger.hpp"
#include "settings.hpp"
#include "utils/command.hpp"
#include "utils/file.hpp"
#include "utils/process.hpp"
#include "utils/string.hpp"

#if ENABLE_PULSEAUDIO
#include "adapters/pulseaudio.hpp"
#endif

POLYBAR_NS

namespace {
  constexpr double PI = 3.14159265358979323846;

  std::atomic<bool> g_open{false};
  std::atomic<bool> g_close_requested{false};

  /**
   * Keep the X connection alive across popup invocations.
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

  enum class drag_target { NONE, VOLUME, BRIGHTNESS };

  struct network_entry {
    string ssid;
    int signal{0};
    bool secure{false};
    bool active{false};
    bool saved{false};

    bool operator==(const network_entry& other) const {
      return ssid == other.ssid && signal == other.signal && secure == other.secure && active == other.active &&
          saved == other.saved;
    }
  };

  struct bluetooth_entry {
    string mac;
    string name;
    bool connected{false};

    bool operator==(const bluetooth_entry& other) const {
      return mac == other.mac && name == other.name && connected == other.connected;
    }
  };

  struct controls_state {
    int x{0};
    int y{0};
    int width{0};
    int height{0};

    int pad{0};
    int header_height{0};
    int row_height{0};
    double scale{1.0};

    string font{"sans"};

    rgb background{0.0, 0.0, 0.0};
    rgb foreground{1.0, 1.0, 1.0};
    rgb border{0.3, 0.3, 0.3};
    rgb muted{0.6, 0.6, 0.6};
    rgb alert{0.77, 0.47, 0.47};

    bool volume_available{false};
    int volume{0};
    bool muted_state{false};

    bool brightness_available{false};
    int brightness{0};
    string backlight_name{};
    string brightness_path{};
    string actual_brightness_path{};
    float backlight_max{0.0f};
    bool backlight_direct{false};

    bool battery_available{false};
    int battery_percent{0};
    string battery_status{};
    string battery_capacity_path{};
    string battery_charge_now_path{};
    string battery_charge_full_path{};
    string battery_status_path{};

    bool network_available{false};
    bool network_expanded{false};
    bool network_connected{false};
    bool network_is_wifi{false};
    bool network_is_ethernet{false};
    bool network_wifi_hw{false};
    bool network_wifi_enabled{false};
    string network_name{};
    string network_device{};
    string network_ip{};
    int network_signal{0};
    std::vector<network_entry> networks;
    int network_scroll{0};
    string network_message{};
    std::chrono::steady_clock::time_point network_message_until{};
    std::chrono::steady_clock::time_point last_network_refresh{};
    bool network_password_prompt{false};
    string network_password_ssid{};
    string network_password_input{};

    bool bluetooth_available{false};
    bool bluetooth_expanded{false};
    bool bluetooth_enabled{false};
    std::vector<bluetooth_entry> bluetooth_devices;
    int bluetooth_scroll{0};
    string bluetooth_message{};
    std::chrono::steady_clock::time_point bluetooth_message_until{};
    std::chrono::steady_clock::time_point last_bluetooth_refresh{};

    drag_target dragging{drag_target::NONE};
    bool volume_pending{false};
    bool brightness_pending{false};
    std::chrono::steady_clock::time_point last_volume_write{};
    std::chrono::steady_clock::time_point last_brightness_write{};

#if ENABLE_PULSEAUDIO
    std::unique_ptr<pulseaudio> pulse;
#endif
  };

  struct network_layout {
    double panel_top{0.0};
    double inner_pad{0.0};
    double wifi_row_y{0.0};
    double wifi_row_height{0.0};
    double list_top{0.0};
    double list_row_height{0.0};
    double hint_height{0.0};
    double message_height{0.0};
    int visible_rows{0};
  };

  network_layout get_network_layout(const controls_state& state) {
    network_layout layout;
    layout.panel_top = state.pad + state.header_height + 4 * state.row_height;
    layout.inner_pad = 14.0 * state.scale;
    layout.wifi_row_y = layout.panel_top + layout.inner_pad;
    layout.wifi_row_height = 38.0 * state.scale;
    layout.list_top = layout.wifi_row_y + layout.wifi_row_height;
    layout.list_row_height = 34.0 * state.scale;
    layout.hint_height = 30.0 * state.scale;
    layout.message_height = 26.0 * state.scale;
    layout.visible_rows = state.network_wifi_hw && state.network_wifi_enabled ? 5 : 0;
    return layout;
  }

  double network_panel_height(const controls_state& state) {
    if (!state.network_expanded || !state.network_available) {
      return 0.0;
    }

    const auto layout = get_network_layout(state);
    const double list_height =
        layout.visible_rows > 0 ? layout.visible_rows * layout.list_row_height : layout.hint_height;

    return layout.inner_pad + layout.wifi_row_height + list_height + layout.message_height + layout.inner_pad;
  }

  struct bluetooth_layout {
    double panel_top{0.0};
    double inner_pad{0.0};
    double switch_row_y{0.0};
    double switch_row_height{0.0};
    double list_top{0.0};
    double list_row_height{0.0};
    double hint_height{0.0};
    double message_height{0.0};
    int visible_rows{0};
  };

  double bluetooth_row_top(const controls_state& state) {
    return state.pad + state.header_height + 4 * state.row_height + network_panel_height(state);
  }

  bluetooth_layout get_bluetooth_layout(const controls_state& state) {
    bluetooth_layout layout;
    layout.panel_top = bluetooth_row_top(state) + state.row_height;
    layout.inner_pad = 14.0 * state.scale;
    layout.switch_row_y = layout.panel_top + layout.inner_pad;
    layout.switch_row_height = 38.0 * state.scale;
    layout.list_top = layout.switch_row_y + layout.switch_row_height;
    layout.list_row_height = 34.0 * state.scale;
    layout.hint_height = 30.0 * state.scale;
    layout.message_height = 26.0 * state.scale;
    layout.visible_rows = state.bluetooth_enabled
        ? std::clamp(static_cast<int>(state.bluetooth_devices.size()), 1, 5)
        : 0;
    return layout;
  }

  double bluetooth_panel_height(const controls_state& state) {
    if (!state.bluetooth_expanded || !state.bluetooth_available) {
      return 0.0;
    }

    const auto layout = get_bluetooth_layout(state);
    const double list_height =
        layout.visible_rows > 0 ? layout.visible_rows * layout.list_row_height : layout.hint_height;

    return layout.inner_pad + layout.switch_row_height + list_height + layout.message_height + layout.inner_pad;
  }

  int popup_height(const controls_state& state) {
    return static_cast<int>(std::round(state.pad + state.header_height + 5 * state.row_height +
        network_panel_height(state) + bluetooth_panel_height(state) + state.pad));
  }

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

  void draw_text_left(cairo_t* cr, const string& text, double x, double center_y, double size, const rgb& color,
      double alpha = 1.0) {
    if (text.empty()) {
      return;
    }

    cairo_set_font_size(cr, size);
    cairo_text_extents_t extents{};
    cairo_text_extents(cr, text.c_str(), &extents);

    set_source(cr, color, alpha);
    cairo_move_to(cr, x, center_y - (extents.y_bearing + extents.height / 2.0));
    cairo_show_text(cr, text.c_str());
  }

  void draw_text_right(cairo_t* cr, const string& text, double x, double center_y, double size, const rgb& color,
      double alpha = 1.0) {
    if (text.empty()) {
      return;
    }

    cairo_set_font_size(cr, size);
    cairo_text_extents_t extents{};
    cairo_text_extents(cr, text.c_str(), &extents);

    set_source(cr, color, alpha);
    cairo_move_to(cr, x - extents.width - extents.x_bearing, center_y - (extents.y_bearing + extents.height / 2.0));
    cairo_show_text(cr, text.c_str());
  }

  double text_width(cairo_t* cr, const string& text, double size) {
    cairo_set_font_size(cr, size);
    cairo_text_extents_t extents{};
    cairo_text_extents(cr, text.c_str(), &extents);
    return extents.x_advance;
  }

  size_t utf8_sequence_length(char c) {
    const auto byte = static_cast<unsigned char>(c);

    if ((byte & 0x80) == 0) {
      return 1;
    }
    if ((byte & 0xE0) == 0xC0) {
      return 2;
    }
    if ((byte & 0xF0) == 0xE0) {
      return 3;
    }
    if ((byte & 0xF8) == 0xF0) {
      return 4;
    }
    return 1;
  }

  /**
   * Shortens text with an ellipsis only when it does not fit into max_width.
   */
  string fit_text(cairo_t* cr, const string& text, double size, double max_width) {
    if (text.empty() || text_width(cr, text, size) <= max_width) {
      return text;
    }

    const string ellipsis = "…";
    string prefix;

    for (size_t i = 0; i < text.size();) {
      const size_t next = std::min(text.size(), i + utf8_sequence_length(text[i]));
      const string candidate = text.substr(0, next);

      if (text_width(cr, candidate + ellipsis, size) > max_width) {
        break;
      }

      prefix = candidate;
      i = next;
    }

    return prefix + ellipsis;
  }

  void draw_speaker(cairo_t* cr, double x, double y, double size, const rgb& color, bool muted) {
    const double u = size / 20.0;

    cairo_new_path(cr);
    cairo_move_to(cr, x + 2 * u, y + 8 * u);
    cairo_line_to(cr, x + 7 * u, y + 8 * u);
    cairo_line_to(cr, x + 12 * u, y + 3 * u);
    cairo_line_to(cr, x + 12 * u, y + 17 * u);
    cairo_line_to(cr, x + 7 * u, y + 12 * u);
    cairo_line_to(cr, x + 2 * u, y + 12 * u);
    cairo_close_path(cr);
    set_source(cr, color);
    cairo_fill(cr);

    if (muted) {
      cairo_set_line_width(cr, std::max(1.0, 1.6 * u));
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      cairo_move_to(cr, x + 14 * u, y + 6 * u);
      cairo_line_to(cr, x + 19 * u, y + 14 * u);
      cairo_move_to(cr, x + 19 * u, y + 6 * u);
      cairo_line_to(cr, x + 14 * u, y + 14 * u);
      cairo_stroke(cr);
    } else {
      cairo_set_line_width(cr, std::max(1.0, 1.4 * u));
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      cairo_arc(cr, x + 12 * u, y + 10 * u, 5 * u, -0.9, 0.9);
      cairo_stroke(cr);
      cairo_arc(cr, x + 12 * u, y + 10 * u, 8 * u, -0.9, 0.9);
      cairo_stroke(cr);
    }
  }

  void draw_sun(cairo_t* cr, double x, double y, double size, const rgb& color) {
    const double u = size / 20.0;

    set_source(cr, color);
    cairo_arc(cr, x + 10 * u, y + 10 * u, 4 * u, 0.0, 2.0 * PI);
    cairo_fill(cr);

    cairo_set_line_width(cr, std::max(1.0, 1.4 * u));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (int i = 0; i < 8; i++) {
      const double angle = i * PI / 4.0;
      cairo_move_to(cr, x + 10 * u + std::cos(angle) * 6.5 * u, y + 10 * u + std::sin(angle) * 6.5 * u);
      cairo_line_to(cr, x + 10 * u + std::cos(angle) * 9.0 * u, y + 10 * u + std::sin(angle) * 9.0 * u);
    }
    cairo_stroke(cr);
  }

  void draw_battery(cairo_t* cr, double x, double y, double size, int percentage, const rgb& color,
      const rgb& track) {
    const double u = size / 20.0;
    const double body_w = 15 * u;
    const double body_h = 10 * u;
    const double body_y = y + 5 * u;

    rounded_rect(cr, x + 1 * u, body_y, body_w, body_h, 2 * u);
    set_source(cr, track);
    cairo_fill(cr);

    const double fill_w = std::max(0.0, (body_w - 3 * u) * std::clamp(percentage, 0, 100) / 100.0);
    if (fill_w > 0.0) {
      rounded_rect(cr, x + 2.5 * u, body_y + 1.5 * u, fill_w, body_h - 3 * u, 1 * u);
      set_source(cr, color);
      cairo_fill(cr);
    }

    rounded_rect(cr, x + 17 * u, body_y + 3 * u, 2.5 * u, 4 * u, 1 * u);
    set_source(cr, track);
    cairo_fill(cr);
  }

  void draw_slider(cairo_t* cr, double x, double center_y, double width, double height, int percentage,
      const rgb& track, const rgb& fill, const rgb& knob, bool show_knob) {
    const double clamped = std::clamp(percentage, 0, 100) / 100.0;

    cairo_rectangle(cr, x, center_y - height / 2.0, width, height);
    set_source(cr, track);
    cairo_fill(cr);

    const double filled = width * clamped;
    if (filled > 0.0) {
      cairo_rectangle(cr, x, center_y - height / 2.0, std::max(filled, height), height);
      set_source(cr, fill);
      cairo_fill(cr);
    }

    if (show_knob) {
      const double knob_radius = height * 0.9;
      const double knob_x = std::clamp(x + filled, x + knob_radius, x + width - knob_radius);
      set_source(cr, knob);
      cairo_arc(cr, knob_x, center_y, knob_radius, 0.0, 2.0 * PI);
      cairo_fill(cr);
    }
  }

  void draw_wifi(cairo_t* cr, double x, double y, double size, int signal, const rgb& color) {
    const double u = size / 20.0;
    const double center_x = x + 10.0 * u;
    const double base_y = y + 16.0 * u;
    const int arcs = signal >= 70 ? 3 : (signal >= 40 ? 2 : 1);

    cairo_new_path(cr);
    set_source(cr, color);
    cairo_arc(cr, center_x, base_y, 1.8 * u, 0.0, 2.0 * PI);
    cairo_fill(cr);

    cairo_set_line_width(cr, std::max(1.0, 1.6 * u));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    for (int i = 0; i < arcs; i++) {
      const double radius = (5.0 + i * 4.0) * u;
      cairo_arc(cr, center_x, base_y, radius, PI * 1.2, PI * 1.8);
      cairo_stroke(cr);
    }
  }

  void draw_ethernet(cairo_t* cr, double x, double y, double size, const rgb& color) {
    const double u = size / 20.0;

    cairo_new_path(cr);
    set_source(cr, color);
    cairo_set_line_width(cr, std::max(1.0, 1.5 * u));
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    rounded_rect(cr, x + 3.0 * u, y + 8.0 * u, 14.0 * u, 9.0 * u, 1.5 * u);
    cairo_stroke(cr);

    for (int i = 0; i < 3; i++) {
      const double pin_x = x + (6.5 + i * 3.5) * u;
      cairo_move_to(cr, pin_x, y + 8.0 * u);
      cairo_line_to(cr, pin_x, y + 5.0 * u);
    }
    cairo_stroke(cr);
  }

  void draw_bluetooth(cairo_t* cr, double x, double y, double size, const rgb& color) {
    const double u = size / 20.0;

    set_source(cr, color);
    cairo_set_line_width(cr, std::max(1.0, 1.6 * u));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    // Vertical stem.
    cairo_move_to(cr, x + 10.0 * u, y + 3.0 * u);
    cairo_line_to(cr, x + 10.0 * u, y + 17.0 * u);

    // Upper and lower halves of the rune.
    cairo_move_to(cr, x + 10.0 * u, y + 3.0 * u);
    cairo_line_to(cr, x + 15.0 * u, y + 7.0 * u);
    cairo_line_to(cr, x + 5.0 * u, y + 13.0 * u);

    cairo_move_to(cr, x + 10.0 * u, y + 17.0 * u);
    cairo_line_to(cr, x + 15.0 * u, y + 13.0 * u);
    cairo_line_to(cr, x + 5.0 * u, y + 7.0 * u);

    cairo_stroke(cr);
  }

  void draw_lock(cairo_t* cr, double x, double y, double size, const rgb& color) {
    const double u = size / 20.0;

    cairo_new_path(cr);
    set_source(cr, color);
    cairo_set_line_width(cr, std::max(1.0, 1.4 * u));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_arc(cr, x + 10.0 * u, y + 9.0 * u, 3.5 * u, PI, 2.0 * PI);
    cairo_stroke(cr);

    cairo_new_path(cr);
    rounded_rect(cr, x + 5.0 * u, y + 9.0 * u, 10.0 * u, 8.0 * u, 1.5 * u);
    set_source(cr, color);
    cairo_fill(cr);
  }

  void draw_check(cairo_t* cr, double x, double y, double size, const rgb& color) {
    const double u = size / 20.0;

    cairo_new_path(cr);
    set_source(cr, color);
    cairo_set_line_width(cr, std::max(1.0, 1.8 * u));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, x + 4.0 * u, y + 10.0 * u);
    cairo_line_to(cr, x + 8.5 * u, y + 14.5 * u);
    cairo_line_to(cr, x + 16.0 * u, y + 5.5 * u);
    cairo_stroke(cr);
  }

  void draw_chevron(cairo_t* cr, double x, double y, double size, const rgb& color, bool up) {
    const double u = size / 20.0;

    set_source(cr, color);
    cairo_set_line_width(cr, std::max(1.0, 1.6 * u));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    if (up) {
      cairo_move_to(cr, x + 5.0 * u, y + 12.0 * u);
      cairo_line_to(cr, x + 10.0 * u, y + 7.0 * u);
      cairo_line_to(cr, x + 15.0 * u, y + 12.0 * u);
    } else {
      cairo_move_to(cr, x + 5.0 * u, y + 8.0 * u);
      cairo_line_to(cr, x + 10.0 * u, y + 13.0 * u);
      cairo_line_to(cr, x + 15.0 * u, y + 8.0 * u);
    }

    cairo_stroke(cr);
  }

  void draw_switch(cairo_t* cr, double x, double y, double width, double height, bool on, const rgb& track_on,
      const rgb& track_off, const rgb& knob_on, const rgb& knob_off) {
    const double radius = height / 2.0;
    const double knob_radius = height * 0.36;

    rounded_rect(cr, x, y, width, height, radius);
    set_source(cr, on ? track_on : track_off);
    cairo_fill(cr);

    const double knob_x = on ? x + width - radius : x + radius;
    set_source(cr, on ? knob_on : knob_off);
    cairo_arc(cr, knob_x, y + radius, knob_radius, 0.0, 2.0 * PI);
    cairo_fill(cr);
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

  std::vector<std::vector<xcb_keysym_t>> get_keymap(xcb_connection_t* connection) {
    const xcb_setup_t* setup = xcb_get_setup(connection);
    const xcb_keycode_t min_keycode = setup->min_keycode;
    const xcb_keycode_t max_keycode = setup->max_keycode;

    std::vector<std::vector<xcb_keysym_t>> keymap(max_keycode + 1);

    xcb_get_keyboard_mapping_reply_t* reply = xcb_get_keyboard_mapping_reply(connection,
        xcb_get_keyboard_mapping(connection, min_keycode, max_keycode - min_keycode + 1), nullptr);

    if (reply == nullptr) {
      return keymap;
    }

    const xcb_keysym_t* keysyms = xcb_get_keyboard_mapping_keysyms(reply);

    for (int keycode = min_keycode; keycode <= max_keycode; keycode++) {
      const int index = (keycode - min_keycode) * reply->keysyms_per_keycode;
      keymap[keycode].resize(reply->keysyms_per_keycode);

      for (int level = 0; level < reply->keysyms_per_keycode; level++) {
        keymap[keycode][level] = keysyms[index + level];
      }
    }

    std::free(reply);
    return keymap;
  }

  int keysym_to_codepoint(xcb_keysym_t keysym) {
    if (keysym >= 0x20 && keysym <= 0x7E) {
      return static_cast<int>(keysym);
    }
    if (keysym >= 0xA0 && keysym <= 0xFF) {
      return static_cast<int>(keysym);
    }
    if (keysym >= 0xFFB0 && keysym <= 0xFFB9) {
      return static_cast<int>('0' + (keysym - 0xFFB0));
    }
    if (keysym == 0xFFAE) {
      return '.';
    }
    return -1;
  }

  void append_utf8(string& out, int codepoint) {
    if (codepoint < 0x80) {
      out += static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
      out += static_cast<char>(0xC0 | (codepoint >> 6));
      out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
      out += static_cast<char>(0xE0 | (codepoint >> 12));
      out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
  }

  string keysym_to_text(xcb_keysym_t keysym, bool caps) {
    int codepoint = keysym_to_codepoint(keysym);

    if (codepoint < 0) {
      return {};
    }

    if (caps && std::isalpha(static_cast<unsigned char>(codepoint))) {
      codepoint = std::islower(static_cast<unsigned char>(codepoint))
          ? std::toupper(static_cast<unsigned char>(codepoint))
          : std::tolower(static_cast<unsigned char>(codepoint));
    }

    string text;
    append_utf8(text, codepoint);
    return text;
  }

  int percentage_from_pointer(double pointer_x, double track_x, double track_width) {
    if (track_width <= 0.0) {
      return 0;
    }

    const double ratio = (pointer_x - track_x) / track_width;
    return static_cast<int>(std::round(std::clamp(ratio, 0.0, 1.0) * 100.0));
  }

  /** Reads the first backlight device with the required sysfs files. */
  bool init_backlight(controls_state& state, const string& card) {
    string base;

    if (!card.empty()) {
      base = string_util::replace(PATH_BACKLIGHT, "%card%", card);
    } else {
      for (const auto& name : file_util::list_files(string_util::replace(PATH_BACKLIGHT, "%card%", ""))) {
        auto dir = string_util::replace(PATH_BACKLIGHT, "%card%", name);
        if (file_util::is_file(dir + "/brightness") && file_util::is_file(dir + "/max_brightness")) {
          base = dir;
          break;
        }
      }
    }

    if (base.empty() || !file_util::is_file(base + "/brightness") || !file_util::is_file(base + "/max_brightness")) {
      return false;
    }

    const auto separator = base.find_last_of('/');
    state.backlight_name = separator == string::npos ? base : base.substr(separator + 1);
    state.brightness_path = base + "/brightness";
    state.actual_brightness_path = base + "/actual_brightness";
    state.backlight_max = std::strtof(file_util::contents(base + "/max_brightness").c_str(), nullptr);
    state.backlight_direct = access(state.brightness_path.c_str(), W_OK) == 0;

    if (state.backlight_max <= 0.0f) {
      return false;
    }

    state.brightness_available = true;
    return true;
  }

  bool read_brightness(controls_state& state) {
    if (!state.brightness_available) {
      return false;
    }

    const string& path = file_util::is_file(state.actual_brightness_path) ? state.actual_brightness_path
                                                                          : state.brightness_path;
    try {
      const float value = std::strtof(file_util::contents(path).c_str(), nullptr);
      const int percentage =
          static_cast<int>(std::round(std::clamp(value * 100.0f / state.backlight_max, 0.0f, 100.0f)));

      if (percentage != state.brightness) {
        state.brightness = percentage;
        return true;
      }
    } catch (const std::exception&) {
      // Ignore transient read errors.
    }

    return false;
  }

  void apply_brightness(controls_state& state, bool force) {
    if (!state.brightness_available) {
      return;
    }

    const auto now = std::chrono::steady_clock::now();

    if (!force && now - state.last_brightness_write < std::chrono::milliseconds(40)) {
      state.brightness_pending = true;
      return;
    }

    state.last_brightness_write = now;
    state.brightness_pending = false;

    const int percentage = std::clamp(state.brightness, 0, 100);
    const int raw = static_cast<int>(std::round(percentage / 100.0 * state.backlight_max));

    if (state.backlight_direct) {
      try {
        file_util::write_contents(state.brightness_path, std::to_string(raw));
        return;
      } catch (const std::exception&) {
        // Fall back to brightnessctl, which can use udev/logind permissions.
        state.backlight_direct = false;
      }
    }

    const string device = state.backlight_name;
    try {
      process_util::fork_detached([device, percentage] {
        string value = std::to_string(percentage) + "%";
        char* args[] = {const_cast<char*>("brightnessctl"), const_cast<char*>("-q"), const_cast<char*>("-d"),
            const_cast<char*>(device.c_str()), const_cast<char*>("set"), const_cast<char*>(value.c_str()), nullptr};
        process_util::exec(args[0], args);
      });
    } catch (const std::exception&) {
      // Ignore failures to spawn brightnessctl.
    }
  }

  bool init_battery(controls_state& state, const string& name) {
    string base;

    if (!name.empty()) {
      base = string_util::replace(PATH_BATTERY, "%battery%", name);
    } else {
      for (const auto& candidate : file_util::list_files(string_util::replace(PATH_BATTERY, "%battery%", ""))) {
        if (candidate.compare(0, 3, "BAT") != 0) {
          continue;
        }
        auto dir = string_util::replace(PATH_BATTERY, "%battery%", candidate);
        if (file_util::exists(dir + "/capacity")) {
          base = dir;
          break;
        }
      }
    }

    if (base.empty()) {
      return false;
    }

    state.battery_capacity_path = base + "/capacity";
    state.battery_charge_now_path = base + "/charge_now";
    state.battery_charge_full_path = base + "/charge_full";
    state.battery_status_path = base + "/status";

    if (!file_util::exists(state.battery_capacity_path) &&
        !(file_util::exists(state.battery_charge_now_path) && file_util::exists(state.battery_charge_full_path))) {
      return false;
    }

    state.battery_available = true;
    return true;
  }

  bool read_battery(controls_state& state) {
    if (!state.battery_available) {
      return false;
    }

    bool changed = false;

    try {
      int percentage = state.battery_percent;

      if (file_util::exists(state.battery_capacity_path)) {
        percentage = std::atoi(file_util::contents(state.battery_capacity_path).c_str());
      } else {
        const long now = std::atol(file_util::contents(state.battery_charge_now_path).c_str());
        const long full = std::atol(file_util::contents(state.battery_charge_full_path).c_str());
        if (full > 0) {
          percentage = static_cast<int>(std::round(now * 100.0 / full));
        }
      }

      percentage = std::clamp(percentage, 0, 100);
      if (percentage != state.battery_percent) {
        state.battery_percent = percentage;
        changed = true;
      }

      if (file_util::exists(state.battery_status_path)) {
        auto status = string_util::trim(file_util::contents(state.battery_status_path),
            [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
        if (status != state.battery_status) {
          state.battery_status = std::move(status);
          changed = true;
        }
      }
    } catch (const std::exception&) {
      // Ignore transient read errors.
    }

    return changed;
  }

  std::pair<int, string> run_command_with_status(const string& command_line) {
    std::pair<int, string> result{-1, {}};

    try {
      command<output_policy::REDIRECTED> proc(logger::make(), command_line);
      proc.exec(false);
      proc.tail([&](string line) {
        result.second += line;
        result.second += '\n';
      });
      result.first = proc.wait();
    } catch (const std::exception&) {
      // Treat a failure to start the command as an unavailable command.
    }

    return result;
  }

  void run_async(const string& command_line) {
    try {
      process_util::fork_detached([command_line] { process_util::exec_sh(command_line.c_str()); });
    } catch (const std::exception& err) {
      logger::make().err("system popup: could not run command (%s)", err.what());
    }
  }

  string shell_quote(const string& value) {
    string result{"'"};

    for (char c : value) {
      if (c == '\'') {
        result += "'\\''";
      } else {
        result += c;
      }
    }

    result += "'";
    return result;
  }

  string unescape_terse(const string& value) {
    string result;
    result.reserve(value.size());

    for (size_t i = 0; i < value.size(); i++) {
      if (value[i] == '\\' && i + 1 < value.size()) {
        result += value[++i];
      } else {
        result += value[i];
      }
    }

    return result;
  }

  vector<string> split_terse(const string& line) {
    vector<string> fields;
    string current;
    bool escaped = false;

    for (char c : line) {
      if (escaped) {
        current += c;
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == ':') {
        fields.push_back(current);
        current.clear();
      } else {
        current += c;
      }
    }

    fields.push_back(current);
    return fields;
  }

  vector<string> split_lines(const string& text) {
    vector<string> lines;
    string current;

    for (char c : text) {
      if (c == '\n') {
        if (!current.empty()) {
          lines.push_back(current);
          current.clear();
        }
      } else if (c != '\r') {
        current += c;
      }
    }

    if (!current.empty()) {
      lines.push_back(current);
    }

    return lines;
  }

  string lower_copy(string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
  }

  string truncate_text(const string& text, size_t max_chars) {
    size_t count = 0;

    for (size_t i = 0; i < text.size(); i++) {
      if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
        if (count == max_chars) {
          return text.substr(0, i) + "…";
        }
        count++;
      }
    }

    return text;
  }

  bool read_network(controls_state& state, bool rescan = false) {
    bool changed = false;

    auto radio = run_command_with_status("nmcli -t -f WIFI-HW,WIFI radio 2>/dev/null");

    if (radio.first != 0) {
      if (state.network_available) {
        state.network_available = false;
        changed = true;
      }
      return changed;
    }

    bool wifi_hw = state.network_wifi_hw;
    bool wifi_enabled = state.network_wifi_enabled;

    for (const auto& line : split_lines(radio.second)) {
      auto fields = split_terse(line);

      if (fields.size() < 2) {
        continue;
      }

      if (fields[0] == "WIFI-HW") {
        wifi_hw = fields[1] == "enabled";
      } else if (fields[0] == "WIFI") {
        wifi_enabled = fields[1] == "enabled";
      } else {
        // Terse output for selected radio fields omits the field names.
        wifi_hw = fields[0] == "enabled";
        wifi_enabled = fields[1] == "enabled";
      }
    }

    struct device_info {
      string device;
      string type;
      string state;
      string connection;
      string ip;
    };

    auto devices = run_command_with_status(
        "nmcli -t -f GENERAL.DEVICE,GENERAL.TYPE,GENERAL.STATE,GENERAL.CONNECTION,IP4.ADDRESS device show "
        "2>/dev/null");

    if (devices.first != 0) {
      if (state.network_available) {
        state.network_available = false;
        changed = true;
      }
      return changed;
    }

    vector<device_info> device_list;
    device_info current;
    bool have_device = false;

    for (const auto& line : split_lines(devices.second)) {
      const auto separator = line.find(':');

      if (separator == string::npos) {
        continue;
      }

      const auto key = line.substr(0, separator);
      const auto value = unescape_terse(line.substr(separator + 1));

      if (key == "GENERAL.DEVICE") {
        if (have_device) {
          device_list.push_back(current);
        }
        current = {};
        current.device = value;
        have_device = true;
      } else if (!have_device) {
        continue;
      } else if (key == "GENERAL.TYPE") {
        current.type = value;
      } else if (key == "GENERAL.STATE") {
        current.state = value;
      } else if (key == "GENERAL.CONNECTION") {
        current.connection = value;
      } else if (key.compare(0, 11, "IP4.ADDRESS") == 0 && current.ip.empty()) {
        const auto prefix = value.find('/');
        current.ip = prefix == string::npos ? value : value.substr(0, prefix);
      }
    }

    if (have_device) {
      device_list.push_back(current);
    }

    const device_info* active = nullptr;

    for (const auto& device : device_list) {
      if (device.type == "loopback" || device.state.find("connected") == string::npos) {
        continue;
      }

      if (device.type == "wifi") {
        active = &device;
        break;
      }

      if (active == nullptr) {
        active = &device;
      }
    }

    const bool new_connected = active != nullptr;
    const bool new_is_wifi = active != nullptr && active->type == "wifi";
    const bool new_is_ethernet = active != nullptr && active->type == "ethernet";
    const string new_name = active != nullptr ? active->connection : "";
    const string new_device = active != nullptr ? active->device : "";
    const string new_ip = active != nullptr ? active->ip : "";

    if (!state.network_available) {
      state.network_available = true;
      changed = true;
    }
    if (state.network_wifi_hw != wifi_hw) {
      state.network_wifi_hw = wifi_hw;
      changed = true;
    }
    if (state.network_wifi_enabled != wifi_enabled) {
      state.network_wifi_enabled = wifi_enabled;
      changed = true;
    }
    if (state.network_connected != new_connected) {
      state.network_connected = new_connected;
      changed = true;
    }
    if (state.network_is_wifi != new_is_wifi) {
      state.network_is_wifi = new_is_wifi;
      changed = true;
    }
    if (state.network_is_ethernet != new_is_ethernet) {
      state.network_is_ethernet = new_is_ethernet;
      changed = true;
    }
    if (state.network_name != new_name) {
      state.network_name = new_name;
      changed = true;
    }
    if (state.network_device != new_device) {
      state.network_device = new_device;
      changed = true;
    }
    if (state.network_ip != new_ip) {
      state.network_ip = new_ip;
      changed = true;
    }

    vector<network_entry> new_networks;

    if (wifi_hw && wifi_enabled && state.network_expanded) {
      const string rescan_option = rescan ? "" : " --rescan no";
      auto wifi_list = run_command_with_status(
          "nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY device wifi list" + rescan_option + " 2>/dev/null");

      if (wifi_list.first == 0) {
        std::set<string> saved_networks;
        auto profiles = run_command_with_status("nmcli -t -f NAME,TYPE connection show 2>/dev/null");

        if (profiles.first == 0) {
          for (const auto& line : split_lines(profiles.second)) {
            auto fields = split_terse(line);

            if (fields.size() < 2 || fields[1] != "802-11-wireless") {
              continue;
            }

            saved_networks.insert(lower_copy(fields[0]));

            auto profile_ssid = run_command_with_status(
                "nmcli -t -f 802-11-wireless.ssid connection show id " + shell_quote(fields[0]) + " 2>/dev/null");

            if (profile_ssid.first != 0) {
              continue;
            }

            for (const auto& profile_line : split_lines(profile_ssid.second)) {
              auto profile_fields = split_terse(profile_line);

              if (profile_fields.size() >= 2 && profile_fields[0] == "802-11-wireless.ssid" &&
                  !profile_fields[1].empty()) {
                saved_networks.insert(lower_copy(profile_fields[1]));
              }
            }
          }
        }

        for (const auto& line : split_lines(wifi_list.second)) {
          auto fields = split_terse(line);

          if (fields.size() < 4 || fields[1].empty()) {
            continue;
          }

          network_entry entry;
          entry.ssid = fields[1];
          entry.signal = std::clamp(std::atoi(fields[2].c_str()), 0, 100);
          entry.secure = !fields[3].empty() && fields[3] != "--";
          entry.active = fields[0] == "*";
          entry.saved = saved_networks.count(lower_copy(entry.ssid)) > 0;

          auto existing = std::find_if(new_networks.begin(), new_networks.end(),
              [&](const network_entry& other) { return other.ssid == entry.ssid; });

          if (existing == new_networks.end()) {
            new_networks.push_back(std::move(entry));
          } else {
            existing->active = existing->active || entry.active;
            existing->saved = existing->saved || entry.saved;

            if (entry.signal > existing->signal) {
              existing->signal = entry.signal;
            }
          }
        }

        std::sort(new_networks.begin(), new_networks.end(), [](const network_entry& a, const network_entry& b) {
          if (a.active != b.active) {
            return a.active;
          }
          if (a.saved != b.saved) {
            return a.saved;
          }
          if (a.signal != b.signal) {
            return a.signal > b.signal;
          }
          return a.ssid < b.ssid;
        });
      }
    }

    if ((state.network_expanded || !wifi_enabled) && state.networks != new_networks) {
      state.networks = std::move(new_networks);
      changed = true;
    }

    int signal = 0;

    if (wifi_enabled && new_is_wifi) {
      for (const auto& entry : state.networks) {
        if (entry.active) {
          signal = entry.signal;
          break;
        }
      }
    }

    if (state.network_signal != signal) {
      state.network_signal = signal;
      changed = true;
    }

    const int max_scroll = std::max(0, static_cast<int>(state.networks.size()) - 5);

    if (state.network_scroll > max_scroll) {
      state.network_scroll = max_scroll;
      changed = true;
    }

    return changed;
  }

  void set_bluetooth_message(controls_state& state, string message, int seconds = 3) {
    state.bluetooth_message = std::move(message);
    state.bluetooth_message_until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  }

  bool read_bluetooth(controls_state& state) {
    bool changed = false;

    auto show = run_command_with_status("bluetoothctl show 2>/dev/null");
    const bool available = show.first == 0 && show.second.find("Controller ") != string::npos;

    if (!available) {
      if (state.bluetooth_available) {
        state.bluetooth_available = false;
        changed = true;
      }
      if (state.bluetooth_enabled) {
        state.bluetooth_enabled = false;
        changed = true;
      }
      if (!state.bluetooth_devices.empty()) {
        state.bluetooth_devices.clear();
        changed = true;
      }
      return changed;
    }

    bool enabled = state.bluetooth_enabled;

    for (const auto& line : split_lines(show.second)) {
      const auto separator = line.find(':');

      if (separator == string::npos) {
        continue;
      }

      const auto key = string_util::trim(line.substr(0, separator),
          [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
      const auto value = string_util::trim(line.substr(separator + 1),
          [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });

      if (key == "Powered") {
        enabled = value == "yes";
      }
    }

    if (!state.bluetooth_available) {
      state.bluetooth_available = true;
      changed = true;
    }
    if (state.bluetooth_enabled != enabled) {
      state.bluetooth_enabled = enabled;
      changed = true;
    }

    std::vector<bluetooth_entry> devices;

    if (enabled) {
      auto add_devices = [&](const string& output, bool connected) {
        for (const auto& line : split_lines(output)) {
          if (line.compare(0, 7, "Device ") != 0) {
            continue;
          }

          const auto rest = line.substr(7);
          const auto separator = rest.find(' ');

          if (separator == string::npos) {
            continue;
          }

          const auto mac = rest.substr(0, separator);
          auto name = string_util::trim(rest.substr(separator + 1));

          if (name.empty()) {
            name = mac;
          }

          auto existing = std::find_if(devices.begin(), devices.end(),
              [&](const bluetooth_entry& entry) { return entry.mac == mac; });

          if (existing == devices.end()) {
            bluetooth_entry entry;
            entry.mac = mac;
            entry.name = name;
            entry.connected = connected;
            devices.push_back(std::move(entry));
          } else {
            existing->connected = existing->connected || connected;
          }
        }
      };

      auto paired = run_command_with_status("bluetoothctl devices Paired 2>/dev/null");

      if (paired.first == 0) {
        add_devices(paired.second, false);
      }

      auto connected = run_command_with_status("bluetoothctl devices Connected 2>/dev/null");

      if (connected.first == 0) {
        add_devices(connected.second, true);
      }

      std::sort(devices.begin(), devices.end(), [](const bluetooth_entry& a, const bluetooth_entry& b) {
        if (a.connected != b.connected) {
          return a.connected;
        }
        return a.name < b.name;
      });
    }

    if (state.bluetooth_devices != devices) {
      state.bluetooth_devices = std::move(devices);
      changed = true;
    }

    const auto layout = get_bluetooth_layout(state);
    const int max_scroll = std::max(0, static_cast<int>(state.bluetooth_devices.size()) - layout.visible_rows);

    if (state.bluetooth_scroll > max_scroll) {
      state.bluetooth_scroll = max_scroll;
      changed = true;
    }

    return changed;
  }

  void set_network_message(controls_state& state, string message, int seconds = 3) {
    state.network_message = std::move(message);
    state.network_message_until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  }

  void toggle_wifi(controls_state& state) {
    if (!state.network_wifi_hw) {
      return;
    }

    if (state.network_wifi_enabled) {
      set_network_message(state, "Disabling Wi-Fi…");
      run_async("nmcli radio wifi off >/dev/null 2>&1");
    } else {
      set_network_message(state, "Enabling Wi-Fi…", 5);
      run_async("rfkill unblock wifi >/dev/null 2>&1; nmcli radio wifi on >/dev/null 2>&1");
    }

    state.last_network_refresh = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  }

  void cancel_password_prompt(controls_state& state) {
    state.network_password_prompt = false;
    state.network_password_ssid.clear();
    state.network_password_input.clear();
  }

  void submit_network_password(controls_state& state) {
    if (state.network_password_input.empty()) {
      set_network_message(state, "Password cannot be empty");
      return;
    }

    const string ssid = state.network_password_ssid;
    const string password = state.network_password_input;
    const string quoted_ssid = shell_quote(ssid);

    cancel_password_prompt(state);
    set_network_message(state, "Connecting to " + truncate_text(ssid, 18) + "…", 10);
    run_async("nmcli device wifi connect " + quoted_ssid + " password " + shell_quote(password) +
        "; status=$?; if [ $status -ne 0 ]; then nmcli connection delete id " + quoted_ssid +
        " >/dev/null 2>&1; fi");
    state.last_network_refresh = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  }

  void connect_network(controls_state& state, const network_entry& entry) {
    if (entry.active) {
      set_network_message(state, "Already connected");
      return;
    }

    if (entry.secure && !entry.saved) {
      state.network_password_prompt = true;
      state.network_password_ssid = entry.ssid;
      state.network_password_input.clear();
      state.network_message.clear();
      return;
    }

    cancel_password_prompt(state);
    set_network_message(state, "Connecting to " + truncate_text(entry.ssid, 18) + "…", 10);
    run_async("nmcli device wifi connect " + shell_quote(entry.ssid) + " >/dev/null 2>&1");
    state.last_network_refresh = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  }

  void toggle_bluetooth(controls_state& state) {
    if (!state.bluetooth_available) {
      return;
    }

    if (state.bluetooth_enabled) {
      set_bluetooth_message(state, "Disabling Bluetooth…");
      run_async("bluetoothctl power off >/dev/null 2>&1");
    } else {
      set_bluetooth_message(state, "Enabling Bluetooth…", 6);
      run_async("rfkill unblock bluetooth >/dev/null 2>&1; bluetoothctl power on >/dev/null 2>&1");
    }

    state.last_bluetooth_refresh = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  }

  void toggle_bluetooth_device(controls_state& state, const bluetooth_entry& entry) {
    if (entry.connected) {
      set_bluetooth_message(state, "Disconnecting from " + truncate_text(entry.name, 18) + "…", 8);
      run_async("bluetoothctl disconnect " + shell_quote(entry.mac) + " >/dev/null 2>&1");
    } else {
      set_bluetooth_message(state, "Connecting to " + truncate_text(entry.name, 18) + "…", 10);
      run_async("bluetoothctl connect " + shell_quote(entry.mac) + " >/dev/null 2>&1");
    }

    state.last_bluetooth_refresh = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  }

#if ENABLE_PULSEAUDIO
  void init_volume(controls_state& state) {
    try {
      state.pulse = std::make_unique<pulseaudio>(logger::make(), ""s, true);
      state.volume = state.pulse->get_volume();
      state.muted_state = state.pulse->is_muted();
      state.volume_available = true;
    } catch (const std::exception& err) {
      logger::make().err("system popup: pulseaudio unavailable (%s)", err.what());
    }
  }

  bool read_volume(controls_state& state) {
    if (!state.pulse) {
      return false;
    }

    try {
      if (state.pulse->wait()) {
        state.pulse->process_events();
      }

      bool changed = false;
      const int volume = state.pulse->get_volume();
      const bool muted = state.pulse->is_muted();

      if (volume != state.volume) {
        state.volume = volume;
        changed = true;
      }
      if (muted != state.muted_state) {
        state.muted_state = muted;
        changed = true;
      }

      return changed;
    } catch (const std::exception&) {
      return false;
    }
  }

  void apply_volume(controls_state& state, bool force) {
    if (!state.pulse) {
      return;
    }

    const auto now = std::chrono::steady_clock::now();

    if (!force && now - state.last_volume_write < std::chrono::milliseconds(30)) {
      state.volume_pending = true;
      return;
    }

    state.last_volume_write = now;
    state.volume_pending = false;

    try {
      state.pulse->set_volume(static_cast<float>(std::clamp(state.volume, 0, 100)));
    } catch (const std::exception& err) {
      logger::make().err("system popup: failed to set volume (%s)", err.what());
    }
  }

  void toggle_mute(controls_state& state) {
    if (!state.pulse) {
      return;
    }

    try {
      state.pulse->toggle_mute();
      state.muted_state = !state.muted_state;
    } catch (const std::exception& err) {
      logger::make().err("system popup: failed to toggle mute (%s)", err.what());
    }
  }
#else
  void init_volume(controls_state&) {}
  bool read_volume(controls_state&) {
    return false;
  }
  void apply_volume(controls_state&, bool) {}
  void toggle_mute(controls_state&) {}
#endif

  bool values_changed_by_refresh(controls_state& state) {
    bool changed = false;

    if (state.dragging != drag_target::VOLUME) {
      changed = read_volume(state) || changed;
    }
    if (state.dragging != drag_target::BRIGHTNESS) {
      changed = read_brightness(state) || changed;
    }
    changed = read_battery(state) || changed;

    const auto now = std::chrono::steady_clock::now();

    if (!state.network_password_prompt && now - state.last_network_refresh >= std::chrono::seconds(2)) {
      state.last_network_refresh = now;
      changed = read_network(state) || changed;
    }

    if (now - state.last_bluetooth_refresh >= std::chrono::seconds(2)) {
      state.last_bluetooth_refresh = now;
      changed = read_bluetooth(state) || changed;
    }

    if (!state.network_message.empty() && now >= state.network_message_until) {
      state.network_message.clear();
      changed = true;
    }

    if (!state.bluetooth_message.empty() && now >= state.bluetooth_message_until) {
      state.bluetooth_message.clear();
      changed = true;
    }

    return changed;
  }

  void draw_network_panel(cairo_t* cr, const controls_state& state) {
    const double label_size = 12.0 * state.scale;
    const auto layout = get_network_layout(state);
    const double panel_x = state.pad;
    const double panel_right = state.width - state.pad;
    const double switch_width = 38.0 * state.scale;
    const double switch_height = 20.0 * state.scale;

    set_source(cr, state.border);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, panel_x, layout.panel_top);
    cairo_line_to(cr, panel_right, layout.panel_top);
    cairo_stroke(cr);

    const double wifi_center_y = layout.wifi_row_y + layout.wifi_row_height / 2.0;
    draw_wifi(cr, panel_x, wifi_center_y - 9.0 * state.scale, 18.0 * state.scale,
        state.network_wifi_enabled ? 70 : 0,
        state.network_wifi_hw && state.network_wifi_enabled ? state.foreground : state.muted);
    draw_text_left(cr, "Wi-Fi", panel_x + 30.0 * state.scale, wifi_center_y, label_size,
        state.network_wifi_hw ? state.foreground : state.muted);

    if (!state.network_wifi_hw) {
      draw_text_right(cr, "No hardware", panel_right, wifi_center_y, 12.0 * state.scale, state.muted);
    } else {
      const double switch_x = panel_right - switch_width;
      draw_switch(cr, switch_x, wifi_center_y - switch_height / 2.0, switch_width, switch_height,
          state.network_wifi_enabled, state.foreground, state.border, state.background, state.muted);
    }

    const double list_height =
        layout.visible_rows > 0 ? layout.visible_rows * layout.list_row_height : layout.hint_height;

    if (state.network_password_prompt) {
      const double prompt_label_y = layout.list_top + 10.0 * state.scale;
      const double input_height = 36.0 * state.scale;
      const double input_top = prompt_label_y + 12.0 * state.scale;
      const double input_width = panel_right - panel_x;
      const double input_text_x = panel_x + 14.0 * state.scale;
      const double input_center_y = input_top + input_height / 2.0;

      draw_text_left(cr, "Password for " + truncate_text(state.network_password_ssid, 20), panel_x,
          prompt_label_y, 12.0 * state.scale, state.muted);

      cairo_rectangle(cr, panel_x, input_top, input_width, input_height);
      set_source(cr, mix(state.background, state.foreground, 0.06));
      cairo_fill_preserve(cr);
      set_source(cr, state.border);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);

      const string masked(std::min<size_t>(state.network_password_input.size(), 30), '*');
      draw_text_left(cr, masked, input_text_x, input_center_y, 12.0 * state.scale, state.foreground);

      const double caret_x = input_text_x + text_width(cr, masked, 12.0 * state.scale) + 1.0;
      cairo_new_path(cr);
      set_source(cr, state.foreground);
      cairo_set_line_width(cr, 1.5);
      cairo_move_to(cr, caret_x, input_top + 10.0 * state.scale);
      cairo_line_to(cr, caret_x, input_top + input_height - 10.0 * state.scale);
      cairo_stroke(cr);

      draw_text_left(cr, "Enter to connect · Esc to cancel", panel_x,
          input_top + input_height + 16.0 * state.scale, 12.0 * state.scale, state.muted);
    } else if (layout.visible_rows > 0) {
      if (state.networks.empty()) {
        draw_text_left(cr, "No networks found", panel_x, layout.list_top + layout.list_row_height / 2.0,
            12.0 * state.scale, state.muted);
      } else {
        for (int i = 0; i < layout.visible_rows; i++) {
          const int index = state.network_scroll + i;

          if (index >= static_cast<int>(state.networks.size())) {
            break;
          }

          const auto& entry = state.networks[index];
          const double item_y = layout.list_top + i * layout.list_row_height;
          const double center_y = item_y + layout.list_row_height / 2.0;
          const double item_pad = 6.0 * state.scale;
          const rgb text_color =
              entry.active || entry.saved ? state.foreground : state.muted;

          if (entry.active) {
            cairo_rectangle(cr, panel_x - item_pad, item_y + 2.0, panel_right - panel_x + 2.0 * item_pad,
                layout.list_row_height - 4.0);
            set_source(cr, mix(state.background, state.foreground, 0.10));
            cairo_fill(cr);
          }

          draw_wifi(cr, panel_x, center_y - 8.0 * state.scale, 16.0 * state.scale, entry.signal, text_color);
          draw_text_left(cr, truncate_text(entry.ssid, 22), panel_x + 28.0 * state.scale, center_y,
              12.0 * state.scale, text_color);

          double right = panel_right;
          if (entry.active) {
            draw_check(cr, right - 12.0 * state.scale, center_y - 6.0 * state.scale, 12.0 * state.scale,
                state.foreground);
            right -= 17.0 * state.scale;
          }
          if (entry.secure) {
            draw_lock(cr, right - 11.0 * state.scale, center_y - 7.0 * state.scale, 11.0 * state.scale,
                state.muted);
          }
        }
      }
    } else {
      const string hint = !state.network_wifi_hw ? "No Wi-Fi hardware" : "Wi-Fi is off";
      draw_text_left(cr, hint, panel_x, layout.list_top + layout.hint_height / 2.0, 12.0 * state.scale,
          state.muted);
    }

    if (!state.network_message.empty()) {
      const double message_y = layout.list_top + list_height + layout.message_height / 2.0;
      draw_text_left(cr, state.network_message, panel_x, message_y, 12.0 * state.scale, state.foreground);
    }
  }

  void draw_bluetooth_panel(cairo_t* cr, const controls_state& state) {
    const double label_size = 12.0 * state.scale;
    const auto layout = get_bluetooth_layout(state);
    const double panel_x = state.pad;
    const double panel_right = state.width - state.pad;
    const double switch_width = 38.0 * state.scale;
    const double switch_height = 20.0 * state.scale;

    set_source(cr, state.border);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, panel_x, layout.panel_top);
    cairo_line_to(cr, panel_right, layout.panel_top);
    cairo_stroke(cr);

    const double switch_center_y = layout.switch_row_y + layout.switch_row_height / 2.0;
    const rgb icon_color = state.bluetooth_enabled ? state.foreground : state.muted;

    draw_bluetooth(cr, panel_x, switch_center_y - 9.0 * state.scale, 18.0 * state.scale, icon_color);
    draw_text_left(cr, "Bluetooth", panel_x + 30.0 * state.scale, switch_center_y, label_size,
        state.bluetooth_available ? state.foreground : state.muted);

    const double switch_x = panel_right - switch_width;
    draw_switch(cr, switch_x, switch_center_y - switch_height / 2.0, switch_width, switch_height,
        state.bluetooth_enabled, state.foreground, state.border, state.background, state.muted);

    const double list_height =
        layout.visible_rows > 0 ? layout.visible_rows * layout.list_row_height : layout.hint_height;

    if (layout.visible_rows > 0) {
      if (state.bluetooth_devices.empty()) {
        draw_text_left(cr, "No paired devices", panel_x, layout.list_top + layout.list_row_height / 2.0,
            12.0 * state.scale, state.muted);
      } else {
        for (int i = 0; i < layout.visible_rows; i++) {
          const int index = state.bluetooth_scroll + i;

          if (index >= static_cast<int>(state.bluetooth_devices.size())) {
            break;
          }

          const auto& entry = state.bluetooth_devices[index];
          const double item_y = layout.list_top + i * layout.list_row_height;
          const double center_y = item_y + layout.list_row_height / 2.0;
          const double item_pad = 6.0 * state.scale;
          const rgb text_color = entry.connected ? state.foreground : state.muted;

          if (entry.connected) {
            cairo_rectangle(cr, panel_x - item_pad, item_y + 2.0, panel_right - panel_x + 2.0 * item_pad,
                layout.list_row_height - 4.0);
            set_source(cr, mix(state.background, state.foreground, 0.10));
            cairo_fill(cr);
          }

          draw_bluetooth(cr, panel_x, center_y - 8.0 * state.scale, 16.0 * state.scale, text_color);

          const double name_max = panel_right - 20.0 * state.scale - (panel_x + 28.0 * state.scale);
          draw_text_left(cr, fit_text(cr, entry.name, 12.0 * state.scale, name_max),
              panel_x + 28.0 * state.scale, center_y, 12.0 * state.scale, text_color);

          if (entry.connected) {
            draw_check(cr, panel_right - 12.0 * state.scale, center_y - 6.0 * state.scale, 12.0 * state.scale,
                state.foreground);
          }
        }
      }
    } else {
      draw_text_left(cr, "Bluetooth is off", panel_x, layout.list_top + layout.hint_height / 2.0,
          12.0 * state.scale, state.muted);
    }

    if (!state.bluetooth_message.empty()) {
      const double message_y = layout.list_top + list_height + layout.message_height / 2.0;
      draw_text_left(cr, state.bluetooth_message, panel_x, message_y, 12.0 * state.scale, state.foreground);
    }
  }

  void draw_controls(cairo_t* cr, const controls_state& state) {
    const double title_center_y = state.pad + state.header_height / 2.0;
    const double value_size = 12.0 * state.scale;
    const double label_size = 12.0 * state.scale;

    draw_text_left(cr, "System", state.pad, title_center_y, 12.0 * state.scale, state.foreground);
    draw_text_right(cr, "Esc to close", state.width - state.pad, title_center_y, 12.0 * state.scale, state.muted);

    // Separator below the header.
    set_source(cr, state.border);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, state.pad, state.pad + state.header_height);
    cairo_line_to(cr, state.width - state.pad, state.pad + state.header_height);
    cairo_stroke(cr);

    const double icon_size = 20.0 * state.scale;
    const double label_x = state.pad + icon_size + 12.0 * state.scale;
    const double track_x = label_x;
    const double track_width = state.width - state.pad - track_x;
    const double slider_height = std::max(4.0, 6.0 * state.scale);

    const double row_top = state.pad + state.header_height;

    for (int row = 0; row < 3; row++) {
      const double row_y = row_top + row * state.row_height;
      const double label_y = row_y + 18.0 * state.scale;
      const double track_y = row_y + 44.0 * state.scale;

      if (row == 0) {
        const rgb icon_color = state.volume_available ? state.foreground : state.muted;
        draw_speaker(cr, state.pad, row_y + 10.0 * state.scale, icon_size, icon_color, state.muted_state);
        draw_text_left(cr, "Volume", label_x, label_y, label_size,
            state.volume_available ? state.foreground : state.muted);

        string value = "Unavailable";
        if (state.volume_available) {
          value = state.muted_state ? "Muted" : std::to_string(state.volume) + "%";
        }
        draw_text_right(cr, value, state.width - state.pad, label_y, value_size, state.muted);

        const rgb fill = state.volume_available ? (state.muted_state ? state.muted : state.foreground) : state.border;
        draw_slider(cr, track_x, track_y, track_width, slider_height, state.volume_available ? state.volume : 0,
            state.border, fill, state.foreground, state.volume_available);
      } else if (row == 1) {
        const rgb icon_color = state.brightness_available ? state.foreground : state.muted;
        draw_sun(cr, state.pad, row_y + 10.0 * state.scale, icon_size, icon_color);
        draw_text_left(cr, "Brightness", label_x, label_y, label_size,
            state.brightness_available ? state.foreground : state.muted);

        const string value =
            state.brightness_available ? std::to_string(state.brightness) + "%" : "Unavailable";
        draw_text_right(cr, value, state.width - state.pad, label_y, value_size, state.muted);

        draw_slider(cr, track_x, track_y, track_width, slider_height, state.brightness_available ? state.brightness : 0,
            state.border, state.brightness_available ? state.foreground : state.border, state.foreground,
            state.brightness_available);
      } else {
        const bool low = state.battery_available && state.battery_percent <= 15 &&
            state.battery_status != "Charging" && state.battery_status != "Full";
        const rgb icon_color = !state.battery_available ? state.muted : (low ? state.alert : state.foreground);
        draw_battery(cr, state.pad, row_y + 10.0 * state.scale, icon_size,
            state.battery_available ? state.battery_percent : 0, icon_color, state.border);
        draw_text_left(cr, "Battery", label_x, label_y, label_size,
            state.battery_available ? state.foreground : state.muted);

        string value = "Not found";
        if (state.battery_available) {
          value = std::to_string(state.battery_percent) + "%";
          if (!state.battery_status.empty()) {
            value += "  " + state.battery_status;
          }
        }
        draw_text_right(cr, value, state.width - state.pad, label_y, value_size, low ? state.alert : state.muted);

        draw_slider(cr, track_x, track_y, track_width, slider_height, state.battery_available ? state.battery_percent : 0,
            state.border, icon_color, state.foreground, false);
      }
    }

    // Network row.
    {
      const double row_y = row_top + 3 * state.row_height;
      const double label_y = row_y + 18.0 * state.scale;
      const double detail_y = row_y + 42.0 * state.scale;
      const double chevron_size = 14.0 * state.scale;
      const double chevron_space = state.network_available ? 18.0 * state.scale : 0.0;
      const rgb icon_color = state.network_available ? state.foreground : state.muted;

      if (state.network_is_wifi) {
        draw_wifi(cr, state.pad, row_y + 10.0 * state.scale, icon_size, state.network_signal, icon_color);
      } else if (state.network_is_ethernet) {
        draw_ethernet(cr, state.pad, row_y + 10.0 * state.scale, icon_size, icon_color);
      } else {
        draw_wifi(cr, state.pad, row_y + 10.0 * state.scale, icon_size, 0, state.muted);
      }

      draw_text_left(cr, "Network", label_x, label_y, label_size,
          state.network_available ? state.foreground : state.muted);

      string value = "Unavailable";
      string detail;

      if (state.network_available) {
        if (!state.network_connected) {
          value = "Disconnected";
        } else if (state.network_is_wifi) {
          value = "Wi-Fi";
        } else if (state.network_is_ethernet) {
          value = "Ethernet";
        } else {
          value = state.network_name;
        }

        if (state.network_connected) {
          if (!state.network_name.empty()) {
            detail = state.network_name;
          }
          if (!state.network_ip.empty()) {
            if (!detail.empty()) {
              detail += "  ";
            }
            detail += state.network_ip;
          }
          if (detail.empty()) {
            detail = state.network_device;
          }

          detail = fit_text(cr, detail, 12.0 * state.scale, state.width - state.pad - label_x);
        }
      }

      draw_text_right(cr, value, state.width - state.pad - chevron_space, label_y, value_size,
          state.network_available ? state.foreground : state.muted);
      draw_text_right(cr, detail, state.width - state.pad, detail_y, 12.0 * state.scale, state.muted);

      if (state.network_available) {
        draw_chevron(cr, state.width - state.pad - chevron_size, row_y + 12.0 * state.scale, chevron_size,
            state.muted, state.network_expanded);
      }
    }

    // Bluetooth row.
    {
      const double row_y = bluetooth_row_top(state);
      const double label_y = row_y + 18.0 * state.scale;
      const double detail_y = row_y + 42.0 * state.scale;
      const double chevron_size = 14.0 * state.scale;
      const double chevron_space = state.bluetooth_available ? 18.0 * state.scale : 0.0;
      const rgb icon_color = state.bluetooth_available && state.bluetooth_enabled ? state.foreground : state.muted;

      draw_bluetooth(cr, state.pad, row_y + 10.0 * state.scale, icon_size, icon_color);
      draw_text_left(cr, "Bluetooth", label_x, label_y, label_size,
          state.bluetooth_available ? state.foreground : state.muted);

      string value = "Unavailable";
      string detail;

      if (state.bluetooth_available) {
        value = state.bluetooth_enabled ? "On" : "Off";

        if (state.bluetooth_enabled) {
          const auto connected = std::find_if(state.bluetooth_devices.begin(), state.bluetooth_devices.end(),
              [](const bluetooth_entry& entry) { return entry.connected; });

          if (connected != state.bluetooth_devices.end()) {
            detail = fit_text(cr, connected->name, 12.0 * state.scale, state.width - state.pad - label_x);
          } else if (!state.bluetooth_devices.empty()) {
            detail = std::to_string(state.bluetooth_devices.size()) + " paired";
          }
        }
      }

      draw_text_right(cr, value, state.width - state.pad - chevron_space, label_y, value_size,
          state.bluetooth_available ? state.foreground : state.muted);
      draw_text_right(cr, detail, state.width - state.pad, detail_y, 12.0 * state.scale, state.muted);

      if (state.bluetooth_available) {
        draw_chevron(cr, state.width - state.pad - chevron_size, row_y + 12.0 * state.scale, chevron_size,
            state.muted, state.bluetooth_expanded);
      }
    }

    if (state.network_expanded && state.network_available) {
      draw_network_panel(cr, state);
    }

    if (state.bluetooth_expanded && state.bluetooth_available) {
      draw_bluetooth_panel(cr, state);
    }
  }

  void paint(xcb_connection_t* connection, xcb_pixmap_t pixmap, xcb_visualtype_t* visual,
      const controls_state& state) {
    cairo_surface_t* surface =
        cairo_xcb_surface_create(connection, pixmap, visual, state.width, state.height);
    cairo_t* cr = cairo_create(surface);

    cairo_select_font_face(cr, state.font.c_str(), CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    set_source(cr, state.background);
    cairo_paint(cr);

    cairo_rectangle(cr, 0.5, 0.5, state.width - 1.0, state.height - 1.0);
    set_source(cr, state.border);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    draw_controls(cr, state);

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
  }

  void present(xcb_connection_t* connection, xcb_pixmap_t pixmap, xcb_window_t window, xcb_gcontext_t gc,
      const controls_state& state) {
    xcb_copy_area(connection, pixmap, window, gc, 0, 0, 0, 0, state.width, state.height);
    xcb_flush(connection);
  }

  void run_popup(system_popup_settings settings) {
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

      controls_state state;
      state.scale = scale;
      state.pad = static_cast<int>(std::round(18.0 * scale));
      state.header_height = static_cast<int>(std::round(36.0 * scale));
      state.row_height = static_cast<int>(std::round(64.0 * scale));
      state.width = static_cast<int>(std::round(320.0 * scale));
      state.height = popup_height(state);
      state.font = settings.font.empty() ? "sans" : settings.font;

      state.background = make_rgb(settings.background, {0.06, 0.06, 0.07});
      state.foreground = make_rgb(settings.foreground, {0.85, 0.85, 0.85});
      state.border = mix(state.background, state.foreground, 0.22);
      state.muted = mix(state.background, state.foreground, 0.58);
      state.alert = {0.77, 0.47, 0.47};

      init_volume(state);
      init_backlight(state, settings.backlight_card);
      read_brightness(state);
      init_battery(state, settings.battery_name);
      read_battery(state);
      read_network(state);
      state.last_network_refresh = std::chrono::steady_clock::now();
      read_bluetooth(state);
      state.last_bluetooth_refresh = std::chrono::steady_clock::now();

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

      (void)pointer_y;

      const uint32_t background_pixel = (static_cast<uint32_t>(state.background.r * 255.0) << 16) |
          (static_cast<uint32_t>(state.background.g * 255.0) << 8) |
          static_cast<uint32_t>(state.background.b * 255.0);

      uint32_t window_values[] = {background_pixel, 1,
          XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
              XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_POINTER_MOTION};

      window = xcb_generate_id(connection);
      xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root, state.x, state.y, state.width,
          state.height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT,
          XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, window_values);

      const string window_name{"polybar-system-popup"};
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

      // Clicking a bar module starts the popup while the button press still
      // holds an implicit pointer grab on the bar. Retry briefly so the popup
      // ends up with the grab and can dismiss itself on any outside click.
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

      const auto keymap = get_keymap(connection);

      const double icon_size = 20.0 * state.scale;
      const double label_x = state.pad + icon_size + 12.0 * state.scale;
      const double track_x = label_x;
      const double track_width = state.width - state.pad - track_x;
      const double volume_row_y = state.pad + state.header_height;
      const double brightness_row_y = volume_row_y + state.row_height;
      const double network_row_y = volume_row_y + 3 * state.row_height;

      const auto redraw = [&] {
        paint(connection, pixmap, visual, state);
        present(connection, pixmap, window, gc, state);
      };

      const auto sync_size = [&] {
        const int new_height = popup_height(state);

        if (new_height == state.height) {
          return false;
        }

        state.height = new_height;

        const int new_y = settings.bar_bottom ? settings.bar_y - state.height - margin
                                              : settings.bar_y + settings.bar_height + margin;
        const int max_y = std::max(margin, static_cast<int>(screen->height_in_pixels) - state.height - margin);
        state.y = std::clamp(new_y, margin, max_y);

        if (pixmap != XCB_NONE) {
          xcb_free_pixmap(connection, pixmap);
        }

        pixmap = xcb_generate_id(connection);
        xcb_create_pixmap(connection, screen->root_depth, pixmap, window, state.width, state.height);

        const uint32_t geometry[] = {static_cast<uint32_t>(state.y), static_cast<uint32_t>(state.height)};
        xcb_configure_window(connection, window, XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_HEIGHT, geometry);

        redraw();
        xcb_flush(connection);
        return true;
      };

      redraw();

      bool running = true;
      auto last_refresh = std::chrono::steady_clock::now();

      const auto refresh_if_due = [&] {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_refresh >= std::chrono::seconds(1)) {
          last_refresh = now;
          if (values_changed_by_refresh(state)) {
            if (!sync_size()) {
              redraw();
            }
          }
        }
      };

      while (running && !g_close_requested.load()) {
        xcb_generic_event_t* event = xcb_poll_for_event(connection);

        if (event == nullptr) {
          if (xcb_connection_has_error(connection)) {
            break;
          }

          refresh_if_due();

          pollfd descriptor{xcb_get_file_descriptor(connection), POLLIN, 0};

          if (poll(&descriptor, 1, 200) <= 0) {
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

            if (button->detail == 4 || button->detail == 5) {
              const int delta = button->detail == 4 ? 5 : -5;
              handled = true;

              if (pointer_y_pos >= volume_row_y && pointer_y_pos < volume_row_y + state.row_height) {
                if (state.volume_available) {
                  state.volume = std::clamp(state.volume + delta, 0, 100);
                  apply_volume(state, true);
                  redraw();
                }
              } else if (pointer_y_pos >= brightness_row_y &&
                  pointer_y_pos < brightness_row_y + state.row_height) {
                if (state.brightness_available) {
                  state.brightness = std::clamp(state.brightness + delta, 0, 100);
                  apply_brightness(state, true);
                  redraw();
                }
              } else if (state.network_expanded && state.network_available && !state.network_password_prompt) {
                const auto layout = get_network_layout(state);

                if (layout.visible_rows > 0) {
                  const double list_bottom = layout.list_top + layout.visible_rows * layout.list_row_height;

                  if (pointer_y_pos >= layout.list_top && pointer_y_pos < list_bottom) {
                    const int max_scroll = std::max(
                        0, static_cast<int>(state.networks.size()) - layout.visible_rows);
                    state.network_scroll =
                        std::clamp(state.network_scroll + (button->detail == 4 ? -1 : 1), 0, max_scroll);
                    redraw();
                  }
                }
              } else if (state.bluetooth_expanded && state.bluetooth_available) {
                const auto layout = get_bluetooth_layout(state);

                if (layout.visible_rows > 0) {
                  const double list_bottom = layout.list_top + layout.visible_rows * layout.list_row_height;

                  if (pointer_y_pos >= layout.list_top && pointer_y_pos < list_bottom) {
                    const int max_scroll = std::max(
                        0, static_cast<int>(state.bluetooth_devices.size()) - layout.visible_rows);
                    state.bluetooth_scroll =
                        std::clamp(state.bluetooth_scroll + (button->detail == 4 ? -1 : 1), 0, max_scroll);
                    redraw();
                  }
                }
              }

              break;
            }

            if (button->detail == 1 &&
                pointer_y_pos >= volume_row_y && pointer_y_pos < volume_row_y + state.row_height) {
              if (state.volume_available) {
                handled = true;
                if (pointer_x_pos < track_x - 6.0 * state.scale) {
                  toggle_mute(state);
                } else {
                  state.volume = percentage_from_pointer(pointer_x_pos, track_x, track_width);
                  state.dragging = drag_target::VOLUME;
                  apply_volume(state, true);
                }
                redraw();
              }
            } else if (button->detail == 1 &&
                pointer_y_pos >= brightness_row_y && pointer_y_pos < brightness_row_y + state.row_height) {
              if (state.brightness_available) {
                handled = true;
                state.brightness = percentage_from_pointer(pointer_x_pos, track_x, track_width);
                state.dragging = drag_target::BRIGHTNESS;
                apply_brightness(state, true);
                redraw();
              }
            } else if (button->detail == 1 && pointer_y_pos >= network_row_y &&
                pointer_y_pos < network_row_y + state.row_height) {
              if (state.network_available) {
                handled = true;
                state.network_expanded = !state.network_expanded;
                state.network_scroll = 0;
                cancel_password_prompt(state);

                if (state.network_expanded) {
                  read_network(state, true);
                  state.last_network_refresh = std::chrono::steady_clock::now();
                }

                if (!sync_size()) {
                  redraw();
                }
              }
            } else if (button->detail == 1 && pointer_y_pos >= bluetooth_row_top(state) &&
                pointer_y_pos < bluetooth_row_top(state) + state.row_height) {
              if (state.bluetooth_available) {
                handled = true;
                state.bluetooth_expanded = !state.bluetooth_expanded;
                state.bluetooth_scroll = 0;

                if (state.bluetooth_expanded) {
                  read_bluetooth(state);
                  state.last_bluetooth_refresh = std::chrono::steady_clock::now();
                }

                if (!sync_size()) {
                  redraw();
                }
              }
            } else if (button->detail == 1 && state.bluetooth_expanded && state.bluetooth_available) {
              const auto layout = get_bluetooth_layout(state);
              const double switch_center_y = layout.switch_row_y + layout.switch_row_height / 2.0;
              const double switch_width = 38.0 * state.scale;
              const double switch_height = 20.0 * state.scale;
              const double switch_x = state.width - state.pad - switch_width;
              const double switch_top = switch_center_y - switch_height / 2.0;

              if (pointer_x_pos >= switch_x && pointer_x_pos < switch_x + switch_width &&
                  pointer_y_pos >= switch_top && pointer_y_pos < switch_top + switch_height) {
                handled = true;
                toggle_bluetooth(state);

                if (!sync_size()) {
                  redraw();
                }
              } else if (layout.visible_rows > 0 && pointer_y_pos >= layout.list_top &&
                  pointer_y_pos < layout.list_top + layout.visible_rows * layout.list_row_height) {
                const int row = static_cast<int>((pointer_y_pos - layout.list_top) / layout.list_row_height);
                const int index = state.bluetooth_scroll + row;

                if (index >= 0 && index < static_cast<int>(state.bluetooth_devices.size())) {
                  handled = true;
                  toggle_bluetooth_device(state, state.bluetooth_devices[index]);
                  redraw();
                }
              }
            } else if (button->detail == 1 && state.network_expanded && state.network_available) {
              const auto layout = get_network_layout(state);
              const double wifi_center_y = layout.wifi_row_y + layout.wifi_row_height / 2.0;
              const double switch_width = 38.0 * state.scale;
              const double switch_height = 20.0 * state.scale;
              const double switch_x = state.width - state.pad - switch_width;
              const double switch_top = wifi_center_y - switch_height / 2.0;

              if (state.network_wifi_hw && pointer_x_pos >= switch_x &&
                  pointer_x_pos < switch_x + switch_width && pointer_y_pos >= switch_top &&
                  pointer_y_pos < switch_top + switch_height) {
                handled = true;
                cancel_password_prompt(state);
                toggle_wifi(state);

                if (!sync_size()) {
                  redraw();
                }
              } else if (!state.network_password_prompt && layout.visible_rows > 0 &&
                  pointer_y_pos >= layout.list_top &&
                  pointer_y_pos < layout.list_top + layout.visible_rows * layout.list_row_height) {
                const int row = static_cast<int>((pointer_y_pos - layout.list_top) / layout.list_row_height);
                const int index = state.network_scroll + row;

                if (index >= 0 && index < static_cast<int>(state.networks.size())) {
                  handled = true;
                  connect_network(state, state.networks[index]);
                  redraw();
                }
              } else if (state.network_password_prompt) {
                handled = true;
              }
            }

            if (!handled) {
              running = false;
            }

            break;
          }

          case XCB_BUTTON_RELEASE: {
            auto* button = reinterpret_cast<xcb_button_release_event_t*>(event);

            if (button->detail == 1) {
              if (state.dragging == drag_target::VOLUME) {
                apply_volume(state, true);
              } else if (state.dragging == drag_target::BRIGHTNESS) {
                apply_brightness(state, true);
              }

              state.dragging = drag_target::NONE;
            }

            break;
          }

          case XCB_MOTION_NOTIFY: {
            auto* motion = reinterpret_cast<xcb_motion_notify_event_t*>(event);

            if ((motion->state & XCB_BUTTON_MASK_1) == 0) {
              break;
            }

            const int pointer_x_pos = motion->root_x - state.x;

            if (state.dragging == drag_target::VOLUME) {
              state.volume = percentage_from_pointer(pointer_x_pos, track_x, track_width);
              apply_volume(state, false);
              redraw();
            } else if (state.dragging == drag_target::BRIGHTNESS) {
              state.brightness = percentage_from_pointer(pointer_x_pos, track_x, track_width);
              apply_brightness(state, false);
              redraw();
            }

            break;
          }

          case XCB_KEY_PRESS: {
            auto* key = reinterpret_cast<xcb_key_press_event_t*>(event);
            static const std::vector<xcb_keysym_t> no_symbols;
            const auto& symbols = key->detail < keymap.size() ? keymap[key->detail] : no_symbols;
            const xcb_keysym_t keysym = symbols.empty() ? 0 : symbols[0];
            const bool shift = key->state & XCB_MOD_MASK_SHIFT;
            const bool caps = key->state & XCB_MOD_MASK_LOCK;
            const bool control = key->state & (XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_1);

            if (state.network_password_prompt) {
              if (keysym == 0xFF1B) { // Escape
                cancel_password_prompt(state);
                redraw();
              } else if (keysym == 0xFF0D || keysym == 0xFF8D) { // Return, KP_Enter
                submit_network_password(state);
                redraw();
              } else if (keysym == 0xFF08) { // BackSpace
                if (!state.network_password_input.empty()) {
                  state.network_password_input.pop_back();
                  redraw();
                }
              } else if (!control) {
                xcb_keysym_t typed = keysym;
                const bool altgr = key->state & XCB_MOD_MASK_5;
                size_t level = 0;

                if (altgr) {
                  level = shift ? 3 : 2;
                } else if (shift) {
                  level = 1;
                }

                if (level < symbols.size() && symbols[level] != 0) {
                  typed = symbols[level];
                }

                const string text = keysym_to_text(typed, caps);

                if (!text.empty() && state.network_password_input.size() < 128) {
                  state.network_password_input += text;
                  redraw();
                }
              }
            } else if (keysym == 0xFF1B) { // Escape
              running = false;
            }

            break;
          }

          default:
            break;
        }

        std::free(event);
        refresh_if_due();
      }
    } catch (const std::exception& err) {
      logger::make().err("system popup: %s", err.what());
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

namespace system_popup {
  system_popup_settings make_settings(const bar_settings& bar, const config& conf) {
    system_popup_settings settings;
    settings.bar_x = bar.pos.x;
    settings.bar_y = bar.pos.y;
    settings.bar_width = bar.size.w;
    settings.bar_height = bar.size.h;
    settings.bar_bottom = bar.bottom;
    settings.dpi = bar.dpi_x > 0 ? bar.dpi_x : 96.0;
    settings.background = bar.background;
    settings.foreground = bar.foreground;

    auto fonts = conf.get_list<string>(conf.section(), "font", {});
    if (!fonts.empty()) {
      auto font = fonts.front();
      auto suffix = font.find_first_of(":;");
      if (suffix != string::npos) {
        font.erase(suffix);
      }

      if (!font.empty()) {
        settings.font = move(font);
      }
    }

    return settings;
  }

  void toggle(const system_popup_settings& settings) {
    if (g_open.exchange(true)) {
      g_close_requested = true;
      return;
    }

    g_close_requested = false;

    try {
      std::thread([settings] { run_popup(settings); }).detach();
    } catch (const std::exception& err) {
      g_open = false;
      logger::make().err("system popup: Could not start popup thread: %s", err.what());
    }
  }

  void close() {
    g_close_requested = true;
  }
} // namespace system_popup

POLYBAR_NS_END
