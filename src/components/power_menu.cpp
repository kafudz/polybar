#include "components/power_menu.hpp"

#include <poll.h>
#include <xcb/xcb.h>
#include <cairo/cairo-xcb.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "components/logger.hpp"
#include "utils/process.hpp"

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

  struct power_menu_state {
    int x{0};
    int y{0};
    int width{0};
    int height{0};

    int pad{0};
    int header_height{0};
    int row_height{0};
    double scale{1.0};

    int active{-1};

    string font{"sans"};

    rgb background{0.0, 0.0, 0.0};
    rgb foreground{1.0, 1.0, 1.0};
    rgb border{0.3, 0.3, 0.3};
    rgb muted{0.6, 0.6, 0.6};
    rgb hover{0.1, 0.1, 0.1};

    std::array<string, 4> commands;
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

  void draw_lock(cairo_t* cr, double x, double y, double size, const rgb& color) {
    const double cx = x + size / 2.0;
    const double body_width = size * 0.60;
    const double body_height = size * 0.42;
    const double body_top = y + size * 0.46;
    const double line_width = std::max(1.4, size * 0.09);

    set_source(cr, color);
    cairo_set_line_width(cr, line_width);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    // Shackle.
    cairo_new_path(cr);
    cairo_arc(cr, cx, y + size * 0.42, size * 0.21, PI, 2.0 * PI);
    cairo_stroke(cr);

    // Body.
    rounded_rect(cr, cx - body_width / 2.0, body_top, body_width, body_height, size * 0.08);
    cairo_stroke(cr);

    // Keyhole.
    cairo_new_path(cr);
    cairo_arc(cr, cx, body_top + body_height * 0.48, size * 0.055, 0.0, 2.0 * PI);
    cairo_fill(cr);
  }

  void draw_logout(cairo_t* cr, double x, double y, double size, const rgb& color) {
    const double line_width = std::max(1.4, size * 0.09);
    const double center_y = y + size * 0.50;

    set_source(cr, color);
    cairo_set_line_width(cr, line_width);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    // Door.
    cairo_move_to(cr, x + size * 0.18, y + size * 0.18);
    cairo_line_to(cr, x + size * 0.18, y + size * 0.82);
    cairo_line_to(cr, x + size * 0.48, y + size * 0.82);
    cairo_stroke(cr);

    // Arrow out of the door.
    cairo_move_to(cr, x + size * 0.40, center_y);
    cairo_line_to(cr, x + size * 0.82, center_y);
    cairo_stroke(cr);

    cairo_move_to(cr, x + size * 0.66, center_y - size * 0.16);
    cairo_line_to(cr, x + size * 0.82, center_y);
    cairo_line_to(cr, x + size * 0.66, center_y + size * 0.16);
    cairo_stroke(cr);
  }

  void draw_reboot(cairo_t* cr, double x, double y, double size, const rgb& color) {
    const double cx = x + size / 2.0;
    const double cy = y + size / 2.0;
    const double radius = size * 0.30;
    const double line_width = std::max(1.4, size * 0.09);

    set_source(cr, color);
    cairo_set_line_width(cr, line_width);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    const double start = -PI / 2.0 + 0.55;
    const double end = 3.0 * PI / 2.0 - 0.55;

    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, radius, start, end);
    cairo_stroke(cr);

    // Arrowhead at the end of the arc.
    const double tip_x = cx + radius * std::cos(end);
    const double tip_y = cy + radius * std::sin(end);
    const double tangent_x = -std::sin(end);
    const double tangent_y = std::cos(end);
    const double radial_x = std::cos(end);
    const double radial_y = std::sin(end);
    const double head = size * 0.17;

    const double back_x = tip_x - tangent_x * head;
    const double back_y = tip_y - tangent_y * head;

    cairo_move_to(cr, back_x + radial_x * head * 0.6, back_y + radial_y * head * 0.6);
    cairo_line_to(cr, tip_x, tip_y);
    cairo_line_to(cr, back_x - radial_x * head * 0.6, back_y - radial_y * head * 0.6);
    cairo_stroke(cr);
  }

  void draw_shutdown(cairo_t* cr, double x, double y, double size, const rgb& color) {
    const double cx = x + size / 2.0;
    const double cy = y + size * 0.56;
    const double radius = size * 0.30;
    const double line_width = std::max(1.4, size * 0.09);

    set_source(cr, color);
    cairo_set_line_width(cr, line_width);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, radius, -PI / 2.0 + 0.55, 3.0 * PI / 2.0 - 0.55);
    cairo_stroke(cr);

    cairo_move_to(cr, cx, y + size * 0.10);
    cairo_line_to(cr, cx, cy - radius * 0.55);
    cairo_stroke(cr);
  }

  void draw_row_icon(cairo_t* cr, int row, double x, double y, double size, const rgb& color) {
    switch (row) {
      case 0:
        draw_lock(cr, x, y, size, color);
        break;
      case 1:
        draw_logout(cr, x, y, size, color);
        break;
      case 2:
        draw_reboot(cr, x, y, size, color);
        break;
      case 3:
        draw_shutdown(cr, x, y, size, color);
        break;
      default:
        break;
    }
  }

  const std::array<const char*, 4> ROW_LABELS = {"Lock", "Log out", "Reboot", "Shut down"};

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

  void run_async(const string& command_line) {
    if (command_line.empty()) {
      return;
    }

    try {
      process_util::fork_detached([command_line] { process_util::exec_sh(command_line.c_str()); });
    } catch (const std::exception& err) {
      logger::make().err("power menu: could not run command (%s)", err.what());
    }
  }

  void execute_command(power_menu_state& state, int row) {
    if (row < 0 || row >= static_cast<int>(state.commands.size())) {
      return;
    }

    run_async(state.commands[row]);
  }

  int row_at(const power_menu_state& state, int px, int py) {
    const int row_top = state.pad + state.header_height;
    const int rows_bottom = row_top + 4 * state.row_height;

    if (px < state.pad || px >= state.width - state.pad || py < row_top || py >= rows_bottom) {
      return -1;
    }

    return (py - row_top) / state.row_height;
  }

  void paint(xcb_connection_t* connection, xcb_pixmap_t pixmap, xcb_visualtype_t* visual,
      const power_menu_state& state) {
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

    const double header_center_y = state.pad + state.header_height / 2.0;

    draw_text_left(cr, "Power", state.pad, header_center_y, 12.0 * state.scale, state.foreground);
    draw_text_right(cr, "Esc to close", state.width - state.pad, header_center_y, 11.0 * state.scale,
        state.muted);

    set_source(cr, state.border);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, state.pad, state.pad + state.header_height);
    cairo_line_to(cr, state.width - state.pad, state.pad + state.header_height);
    cairo_stroke(cr);

    const double row_top = state.pad + state.header_height;
    const double icon_size = 18.0 * state.scale;
    const double icon_x = state.pad + 4.0 * state.scale;
    const double label_x = state.pad + icon_size + 16.0 * state.scale;

    for (int row = 0; row < 4; row++) {
      const double row_y = row_top + row * state.row_height;
      const double center_y = row_y + state.row_height / 2.0;

      if (row == state.active) {
        rounded_rect(cr, state.pad - 4.0 * state.scale, row_y + 2.0 * state.scale,
            state.width - 2.0 * state.pad + 8.0 * state.scale, state.row_height - 4.0 * state.scale,
            5.0 * state.scale);
        set_source(cr, state.hover);
        cairo_fill(cr);
      }

      draw_row_icon(cr, row, icon_x, center_y - icon_size / 2.0, icon_size,
          row == state.active ? state.foreground : state.muted);
      draw_text_left(cr, ROW_LABELS[row], label_x, center_y, 11.5 * state.scale, state.foreground);
    }

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
  }

  void present(xcb_connection_t* connection, xcb_pixmap_t pixmap, xcb_window_t window, xcb_gcontext_t gc,
      const power_menu_state& state) {
    xcb_copy_area(connection, pixmap, window, gc, 0, 0, 0, 0, state.width, state.height);
    xcb_flush(connection);
  }

  void run_popup(power_menu_settings settings) {
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

      power_menu_state state;
      state.scale = scale;
      state.pad = static_cast<int>(std::round(14.0 * scale));
      state.header_height = static_cast<int>(std::round(30.0 * scale));
      state.row_height = static_cast<int>(std::round(44.0 * scale));
      state.width = static_cast<int>(std::round(250.0 * scale));
      state.height = state.pad + state.header_height + 4 * state.row_height + state.pad;
      state.font = settings.font.empty() ? "sans" : settings.font;
      state.commands = {settings.lock_command, settings.logout_command, settings.reboot_command,
          settings.shutdown_command};

      state.background = make_rgb(settings.background, {0.06, 0.06, 0.07});
      state.foreground = make_rgb(settings.foreground, {0.85, 0.85, 0.85});
      state.border = mix(state.background, state.foreground, 0.22);
      state.muted = mix(state.background, state.foreground, 0.58);
      state.hover = mix(state.background, state.foreground, 0.10);

      const int margin = static_cast<int>(std::round(4.0 * scale));

      if (settings.centered) {
        state.x = (static_cast<int>(screen->width_in_pixels) - state.width) / 2;
        state.y = (static_cast<int>(screen->height_in_pixels) - state.height) / 2;
      } else {
        int pointer_x = settings.bar_x + settings.bar_width / 2;

        xcb_query_pointer_reply_t* pointer_reply = xcb_query_pointer_reply(connection,
            xcb_query_pointer(connection, screen->root), nullptr);

        if (pointer_reply != nullptr) {
          pointer_x = pointer_reply->root_x;
          std::free(pointer_reply);
        }

        state.x = pointer_x - state.width / 2;
        state.y = settings.bar_bottom ? settings.bar_y - state.height - margin
                                      : settings.bar_y + settings.bar_height + margin;
      }

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

      const string window_name{"polybar-power-menu"};
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

      // Clicking the power module starts the popup while the button press still
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

      // Keyboard shortcuts such as sxhkd can still hold a keyboard grab while
      // their command is starting, so retry until the key is released and the
      // popup can take over Escape and the arrow keys.
      for (int attempt = 0; attempt < 200; attempt++) {
        xcb_grab_keyboard_reply_t* keyboard_reply = xcb_grab_keyboard_reply(connection,
            xcb_grab_keyboard(connection, 0, window, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC),
            nullptr);

        const bool success = keyboard_reply != nullptr && keyboard_reply->status == XCB_GRAB_STATUS_SUCCESS;

        if (keyboard_reply != nullptr) {
          std::free(keyboard_reply);
        }

        if (success) {
          keyboard_grabbed = true;
          break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }

      if (!keyboard_grabbed) {
        logger::make().warn("power menu: could not grab the keyboard, Escape and arrow keys are unavailable");
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

          case XCB_MOTION_NOTIFY: {
            auto* motion = reinterpret_cast<xcb_motion_notify_event_t*>(event);
            const int pointer_x_pos = motion->root_x - state.x;
            const int pointer_y_pos = motion->root_y - state.y;
            const int active = row_at(state, pointer_x_pos, pointer_y_pos);

            if (active != state.active) {
              state.active = active;
              redraw();
            }

            break;
          }

          case XCB_BUTTON_PRESS: {
            auto* button = reinterpret_cast<xcb_button_press_event_t*>(event);
            const int pointer_x_pos = button->root_x - state.x;
            const int pointer_y_pos = button->root_y - state.y;
            const int row = row_at(state, pointer_x_pos, pointer_y_pos);

            if (row >= 0) {
              execute_command(state, row);
            }

            // Any click closes the popup, whether it selected an action or not.
            running = false;
            break;
          }

          case XCB_KEY_PRESS: {
            auto* key = reinterpret_cast<xcb_key_press_event_t*>(event);
            const xcb_keysym_t keysym = key->detail < keymap.size() ? keymap[key->detail] : 0;

            switch (keysym) {
              case 0xFF1B: // Escape
                running = false;
                break;
              case 0xFF52: // Up
                state.active = state.active <= 0 ? 3 : state.active - 1;
                redraw();
                break;
              case 0xFF54: // Down
                state.active = state.active < 0 || state.active >= 3 ? 0 : state.active + 1;
                redraw();
                break;
              case 0xFF0D: // Return
              case 0xFF8D: // KP_Enter
                if (state.active >= 0) {
                  execute_command(state, state.active);
                  running = false;
                }
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
      logger::make().err("power menu: %s", err.what());
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

namespace power_menu {
  void toggle(const power_menu_settings& settings) {
    if (g_open.exchange(true)) {
      g_close_requested = true;
      return;
    }

    g_close_requested = false;

    try {
      std::thread([settings] { run_popup(settings); }).detach();
    } catch (const std::exception& err) {
      g_open = false;
      logger::make().err("power menu: Could not start popup thread: %s", err.what());
    }
  }

  void close() {
    g_close_requested = true;
  }
} // namespace power_menu

POLYBAR_NS_END
