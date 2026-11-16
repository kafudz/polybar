#include "components/system_popup.hpp"

#include <poll.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <cairo/cairo-xcb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "components/logger.hpp"
#include "settings.hpp"
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

    drag_target dragging{drag_target::NONE};
    bool volume_pending{false};
    bool brightness_pending{false};
    std::chrono::steady_clock::time_point last_volume_write{};
    std::chrono::steady_clock::time_point last_brightness_write{};

#if ENABLE_PULSEAUDIO
    std::unique_ptr<pulseaudio> pulse;
#endif
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
    const double radius = height / 2.0;
    const double clamped = std::clamp(percentage, 0, 100) / 100.0;

    rounded_rect(cr, x, center_y - height / 2.0, width, height, radius);
    set_source(cr, track);
    cairo_fill(cr);

    const double filled = width * clamped;
    if (filled > 0.0) {
      rounded_rect(cr, x, center_y - height / 2.0, std::max(filled, height), height, radius);
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
        auto status = string_util::trim(file_util::contents(state.battery_status_path));
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

    return changed;
  }

  void draw_controls(cairo_t* cr, const controls_state& state) {
    const double title_center_y = state.pad + state.header_height / 2.0;
    const double value_size = 11.0 * state.scale;
    const double label_size = 11.0 * state.scale;

    draw_text_left(cr, "System", state.pad, title_center_y, 12.0 * state.scale, state.foreground);
    draw_text_right(cr, "Esc to close", state.width - state.pad, title_center_y, 9.5 * state.scale, state.muted);

    // Separator below the header.
    set_source(cr, state.border);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, state.pad, state.pad + state.header_height);
    cairo_line_to(cr, state.width - state.pad, state.pad + state.header_height);
    cairo_stroke(cr);

    const double icon_size = 20.0 * state.scale;
    const double label_x = state.pad + icon_size + 10.0 * state.scale;
    const double track_x = label_x;
    const double track_width = state.width - state.pad - track_x;
    const double slider_height = std::max(4.0, 6.0 * state.scale);

    for (int row = 0; row < 3; row++) {
      const double row_y = state.pad + state.header_height + row * state.row_height;
      const double label_y = row_y + 15.0 * state.scale;
      const double track_y = row_y + 38.0 * state.scale;

      if (row == 0) {
        const rgb icon_color = state.volume_available ? state.foreground : state.muted;
        draw_speaker(cr, state.pad, row_y + 6.0 * state.scale, icon_size, icon_color, state.muted_state);
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
        draw_sun(cr, state.pad, row_y + 6.0 * state.scale, icon_size, icon_color);
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
        draw_battery(cr, state.pad, row_y + 6.0 * state.scale, icon_size,
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
  }

  void paint(xcb_connection_t* connection, xcb_pixmap_t pixmap, xcb_visualtype_t* visual,
      const controls_state& state) {
    cairo_surface_t* surface =
        cairo_xcb_surface_create(connection, pixmap, visual, state.width, state.height);
    cairo_t* cr = cairo_create(surface);

    cairo_select_font_face(cr, state.font.c_str(), CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    set_source(cr, state.background);
    cairo_paint(cr);

    rounded_rect(cr, 0.5, 0.5, state.width - 1.0, state.height - 1.0, 6.0 * state.scale);
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
      state.pad = static_cast<int>(std::round(14.0 * scale));
      state.header_height = static_cast<int>(std::round(30.0 * scale));
      state.row_height = static_cast<int>(std::round(56.0 * scale));
      state.width = static_cast<int>(std::round(300.0 * scale));
      state.height = state.pad + state.header_height + 3 * state.row_height + state.pad;
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

      const double icon_size = 20.0 * state.scale;
      const double label_x = state.pad + icon_size + 10.0 * state.scale;
      const double track_x = label_x;
      const double track_width = state.width - state.pad - track_x;
      const double volume_row_y = state.pad + state.header_height;
      const double brightness_row_y = volume_row_y + state.row_height;

      const auto redraw = [&] {
        paint(connection, pixmap, visual, state);
        present(connection, pixmap, window, gc, state);
      };

      redraw();

      bool running = true;
      auto last_refresh = std::chrono::steady_clock::now();

      const auto refresh_if_due = [&] {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_refresh >= std::chrono::seconds(1)) {
          last_refresh = now;
          if (values_changed_by_refresh(state)) {
            redraw();
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

            if (button->detail == 4 || button->detail == 5) {
              const int delta = button->detail == 4 ? 5 : -5;

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
              }

              break;
            }

            if (button->detail == 1 &&
                pointer_y_pos >= volume_row_y && pointer_y_pos < volume_row_y + state.row_height) {
              if (state.volume_available) {
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
                state.brightness = percentage_from_pointer(pointer_x_pos, track_x, track_width);
                state.dragging = drag_target::BRIGHTNESS;
                apply_brightness(state, true);
                redraw();
              }
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
            const xcb_keysym_t keysym = key->detail < keymap.size() ? keymap[key->detail] : 0;

            if (keysym == 0xFF1B) { // Escape
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
