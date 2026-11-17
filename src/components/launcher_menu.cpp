#include "components/launcher_menu.hpp"

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
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include "components/logger.hpp"
#include "utils/env.hpp"
#include "utils/process.hpp"
#include "utils/string.hpp"

POLYBAR_NS

namespace {
  constexpr int MAX_VISIBLE_ROWS = 15;

  std::atomic<bool> g_open{false};
  std::atomic<bool> g_close_requested{false};

  /**
   * The X connection is kept alive across popup invocations so cairo does not
   * cache state for a connection that has already been closed.
   */
  std::mutex g_connection_lock;
  xcb_connection_t* g_connection{nullptr};

  struct rgb {
    double r{0.0};
    double g{0.0};
    double b{0.0};
  };

  struct launcher_entry {
    string name{};
    string exec{};
    bool terminal{false};
  };

  struct launcher_state {
    int x{0};
    int y{0};
    int width{0};
    int height{0};

    int pad{0};
    int search_height{0};
    int list_top{0};
    int row_height{0};
    int footer_top{0};
    int visible_rows{0};
    double scale{1.0};

    string query{};
    int active{0};
    int scroll{0};

    string font{"sans"};
    vector<launcher_entry> entries;

    rgb background{0.0, 0.0, 0.0};
    rgb foreground{1.0, 1.0, 1.0};
    rgb border{0.3, 0.3, 0.3};
    rgb muted{0.6, 0.6, 0.6};
    rgb hover{0.1, 0.1, 0.1};
    rgb input{0.08, 0.08, 0.09};
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

  void next_codepoint(const string& text, size_t& pos) {
    if (pos >= text.size()) {
      return;
    }

    ++pos;
    while (pos < text.size() && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) {
      ++pos;
    }
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

  string tail_fit_text(cairo_t* cr, const string& text, double max_width, double size) {
    if (text.empty() || max_width <= 0.0) {
      return "";
    }

    if (text_width(cr, text, size) <= max_width) {
      return text;
    }

    const string ellipsis{"…"};
    size_t pos = 0;

    while (pos < text.size()) {
      next_codepoint(text, pos);
      const string candidate = ellipsis + text.substr(pos);

      if (text_width(cr, candidate, size) <= max_width) {
        return candidate;
      }
    }

    return ellipsis;
  }

  string desktop_unescape(const string& value) {
    string result;
    result.reserve(value.size());

    for (size_t i = 0; i < value.size(); i++) {
      if (value[i] != '\\' || i + 1 >= value.size()) {
        result += value[i];
        continue;
      }

      const char escaped = value[++i];
      switch (escaped) {
        case 's':
          result += ' ';
          break;
        case 'n':
          result += '\n';
          break;
        case 't':
          result += '\t';
          break;
        case 'r':
          result += '\r';
          break;
        case '\\':
          result += '\\';
          break;
        default:
          result += escaped;
          break;
      }
    }

    return result;
  }

  bool desktop_bool(const string& value) {
    return string_util::lower(string_util::trim(string{value})) == "true";
  }

  string clean_exec(const string& exec) {
    string result;
    result.reserve(exec.size());

    for (size_t i = 0; i < exec.size(); i++) {
      if (exec[i] == '%' && i + 1 < exec.size()) {
        const char code = exec[i + 1];

        if (code == '%') {
          result += '%';
          ++i;
          continue;
        }

        if (string{"fFuUickdDnNvm"}.find(code) != string::npos) {
          ++i;
          continue;
        }
      }

      result += exec[i];
    }

    return string_util::trim(std::move(result));
  }

  bool program_in_path(const string& program) {
    if (program.empty()) {
      return false;
    }

    if (program.find('/') != string::npos) {
      return access(program.c_str(), X_OK) == 0;
    }

    const vector<string> paths = string_util::split(env_util::get("PATH", "/usr/bin:/bin"), ':');

    for (const auto& path : paths) {
      if (!path.empty() && access((path + "/" + program).c_str(), X_OK) == 0) {
        return true;
      }
    }

    return false;
  }

  bool parse_desktop_file(const std::filesystem::path& path, launcher_entry& entry) {
    std::ifstream file(path);
    if (!file) {
      return false;
    }

    bool in_desktop_entry = false;
    bool is_application = false;
    bool hidden = false;
    bool no_display = false;
    bool terminal = false;
    string name;
    string exec;
    string try_exec;
    string line;

    while (std::getline(file, line)) {
      // getline already removes the newline, but files with CRLF line endings
      // leave a trailing carriage return that would end up in the values.
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      if (line.empty() || line[0] == '#') {
        continue;
      }

      if (line.front() == '[') {
        in_desktop_entry = line == "[Desktop Entry]";
        continue;
      }

      if (!in_desktop_entry) {
        continue;
      }

      const auto separator = line.find('=');

      if (separator == string::npos) {
        continue;
      }

      string key = string_util::trim(string{line.substr(0, separator)});
      string value = string_util::trim(string{line.substr(separator + 1)});

      if (key == "Name" && name.empty()) {
        name = desktop_unescape(value);
      } else if (key == "Exec" && exec.empty()) {
        exec = value;
      } else if (key == "Type") {
        is_application = value == "Application";
      } else if (key == "Terminal") {
        terminal = desktop_bool(value);
      } else if (key == "NoDisplay") {
        no_display = desktop_bool(value);
      } else if (key == "Hidden") {
        hidden = desktop_bool(value);
      } else if (key == "TryExec" && try_exec.empty()) {
        try_exec = desktop_unescape(value);
      }
    }

    if (!is_application || hidden || no_display || name.empty() || exec.empty()) {
      return false;
    }

    if (!try_exec.empty() && !program_in_path(try_exec)) {
      return false;
    }

    entry.name = std::move(name);
    entry.exec = clean_exec(exec);
    entry.terminal = terminal;
    return !entry.exec.empty();
  }

  vector<launcher_entry> load_entries() {
    vector<std::filesystem::path> data_dirs;

    const string data_home = env_util::get("XDG_DATA_HOME");
    if (!data_home.empty()) {
      data_dirs.emplace_back(data_home);
    } else {
      const string home = env_util::get("HOME");
      if (!home.empty()) {
        data_dirs.emplace_back(home + "/.local/share");
      }
    }

    for (const auto& dir : string_util::split(env_util::get("XDG_DATA_DIRS", "/usr/local/share:/usr/share"), ':')) {
      if (!dir.empty()) {
        data_dirs.emplace_back(dir);
      }
    }

    std::set<string> seen_ids;
    vector<launcher_entry> entries;

    for (const auto& data_dir : data_dirs) {
      const std::filesystem::path applications_dir = data_dir / "applications";
      std::error_code error;

      if (!std::filesystem::is_directory(applications_dir, error)) {
        continue;
      }

      std::filesystem::recursive_directory_iterator iterator(
          applications_dir, std::filesystem::directory_options::skip_permission_denied, error);
      const std::filesystem::recursive_directory_iterator end;

      while (!error && iterator != end) {
        const std::filesystem::path path = iterator->path();
        iterator.increment(error);

        if (path.extension() != ".desktop") {
          continue;
        }

        std::error_code relative_error;
        const auto relative = std::filesystem::relative(path, applications_dir, relative_error);
        string id = relative_error ? path.filename().string() : relative.generic_string();
        std::replace(id.begin(), id.end(), '/', '-');

        if (!seen_ids.insert(id).second) {
          continue;
        }

        launcher_entry entry;
        if (parse_desktop_file(path, entry)) {
          entries.emplace_back(std::move(entry));
        }
      }
    }

    std::sort(entries.begin(), entries.end(), [](const launcher_entry& a, const launcher_entry& b) {
      const string lhs = string_util::lower(a.name);
      const string rhs = string_util::lower(b.name);

      if (lhs == rhs) {
        return a.name < b.name;
      }

      return lhs < rhs;
    });

    return entries;
  }

  vector<const launcher_entry*> filtered_entries(const launcher_state& state) {
    vector<const launcher_entry*> result;

    if (state.query.empty()) {
      result.reserve(state.entries.size());

      for (const auto& entry : state.entries) {
        result.emplace_back(&entry);
      }

      return result;
    }

    const string query = string_util::lower(state.query);
    result.reserve(state.entries.size());

    for (const auto& entry : state.entries) {
      if (string_util::lower(entry.name).find(query) != string::npos) {
        result.emplace_back(&entry);
      }
    }

    return result;
  }

  void reset_selection(launcher_state& state) {
    state.active = 0;
    state.scroll = 0;
  }

  /**
   * Keeps the popup at a fixed size: the list always reserves
   * MAX_VISIBLE_ROWS rows so filtering does not resize the window.
   */
  void update_layout(launcher_state& state, const launcher_menu_settings& settings, int screen_width,
      int screen_height, int margin) {
    state.visible_rows = MAX_VISIBLE_ROWS;
    state.footer_top = state.list_top + state.visible_rows * state.row_height +
        static_cast<int>(std::round(8.0 * state.scale));
    state.height = state.footer_top + static_cast<int>(std::round(28.0 * state.scale)) + state.pad;

    state.x = settings.bar_x + settings.bar_width / 2 - state.width / 2;
    state.y = settings.bar_bottom ? settings.bar_y - state.height - margin
                                  : settings.bar_y + settings.bar_height + margin;

    const int max_x = std::max(margin, screen_width - state.width - margin);
    const int max_y = std::max(margin, screen_height - state.height - margin);
    state.x = std::clamp(state.x, margin, max_x);
    state.y = std::clamp(state.y, margin, max_y);
  }

  void move_selection(launcher_state& state, int delta) {
    const int count = static_cast<int>(filtered_entries(state).size());

    if (count <= 0) {
      state.active = 0;
      state.scroll = 0;
      return;
    }

    state.active = std::clamp(state.active + delta, 0, count - 1);

    if (state.active < state.scroll) {
      state.scroll = state.active;
    } else if (state.active >= state.scroll + state.visible_rows) {
      state.scroll = state.active - state.visible_rows + 1;
    }
  }

  void run_async(const string& command_line) {
    if (command_line.empty()) {
      return;
    }

    try {
      process_util::fork_detached([command_line] { process_util::exec_sh(command_line.c_str()); });
    } catch (const std::exception& err) {
      logger::make().err("launcher: could not run command (%s)", err.what());
    }
  }

  void launch_entry(const launcher_entry& entry, const launcher_menu_settings& settings) {
    string command = entry.exec;

    if (entry.terminal && !settings.terminal.empty()) {
      command = settings.terminal + " " + command;
    }

    run_async(command);
  }

  void paint(xcb_connection_t* connection, xcb_pixmap_t pixmap, xcb_visualtype_t* visual,
      const launcher_state& state) {
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

    // Search field
    cairo_rectangle(cr, state.pad, state.pad, state.width - 2.0 * state.pad, state.search_height);
    set_source(cr, state.input);
    cairo_fill(cr);

    const double input_text_x = state.pad + 14.0 * state.scale;
    const double input_text_right = state.width - state.pad - 14.0 * state.scale;
    const double input_center_y = state.pad + state.search_height / 2.0;
    const double input_size = 12.0 * state.scale;
    const double input_max_width = input_text_right - input_text_x - 2.0 * state.scale;

    if (state.query.empty()) {
      draw_text_left(cr, "Search applications…", input_text_x, input_center_y, input_size, state.muted);
    } else {
      const string visible_query = tail_fit_text(cr, state.query, input_max_width, input_size);
      draw_text_left(cr, visible_query, input_text_x, input_center_y, input_size, state.foreground);

      const double caret_x = std::min(input_text_right, input_text_x + text_width(cr, visible_query, input_size) + 1.0);
      set_source(cr, state.foreground);
      cairo_set_line_width(cr, 1.0);
      cairo_move_to(cr, caret_x, state.pad + 11.0 * state.scale);
      cairo_line_to(cr, caret_x, state.pad + state.search_height - 11.0 * state.scale);
      cairo_stroke(cr);
    }

    const vector<const launcher_entry*> entries = filtered_entries(state);
    const double name_size = 12.0 * state.scale;
    const double command_size = 12.0 * state.scale;

    if (entries.empty()) {
      draw_text_left(cr, "No applications found", state.pad + 16.0 * state.scale,
          state.list_top + state.row_height / 2.0, 12.0 * state.scale, state.muted);
    } else {
      const int scroll = std::clamp(state.scroll, 0, std::max(0, static_cast<int>(entries.size()) - 1));

      for (int row = 0; row < state.visible_rows; row++) {
        const int index = scroll + row;

        if (index >= static_cast<int>(entries.size())) {
          break;
        }

        const double row_top = state.list_top + row * state.row_height;
        const double center_y = row_top + state.row_height / 2.0;

        if (index == state.active) {
          cairo_rectangle(cr, state.pad, row_top + 2.0 * state.scale, state.width - 2.0 * state.pad,
              state.row_height - 4.0 * state.scale);
          set_source(cr, state.hover);
          cairo_fill(cr);
        }

        const launcher_entry& entry = *entries[index];
        const double row_text_x = state.pad + 16.0 * state.scale;
        const double row_text_right = state.width - state.pad - 16.0 * state.scale;
        const double row_text_width = row_text_right - row_text_x;
        const double command_max_width = row_text_width * 0.5;
        const double command_width = std::min(command_max_width, text_width(cr, entry.exec, command_size));
        const double name_max_width = std::max(0.0, row_text_width - command_width - 12.0 * state.scale);

        draw_text_left(cr, fit_text(cr, entry.name, name_max_width, name_size), row_text_x, center_y,
            name_size, state.foreground);

        if (command_width > 0.0) {
          draw_text_right(cr, fit_text(cr, entry.exec, command_width, command_size), row_text_right,
              center_y, command_size, state.muted);
        }
      }
    }

    const double footer_y = state.footer_top + 14.0 * state.scale;
    const string count = entries.empty() ? "No matches"
                                         : std::to_string(entries.size()) +
            (entries.size() == 1 ? " application" : " applications");

    draw_text_left(cr, count, state.pad, footer_y, 12.0 * state.scale, state.muted);
    draw_text_right(cr, "Up/Down · Enter · Esc", state.width - state.pad, footer_y,
        12.0 * state.scale, state.muted);

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
  }

  void present(xcb_connection_t* connection, xcb_pixmap_t pixmap, xcb_window_t window, xcb_gcontext_t gc,
      const launcher_state& state) {
    xcb_copy_area(connection, pixmap, window, gc, 0, 0, 0, 0, state.width, state.height);
    xcb_flush(connection);
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
    if ((keysym & 0xFF000000) == 0x01000000) {
      return static_cast<int>(keysym & 0x00FFFFFF);
    }
    return -1;
  }

  void append_utf8(string& out, int codepoint) {
    if (codepoint < 0x80) {
      out += static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
      out += static_cast<char>(0xC0 | (codepoint >> 6));
      out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
      out += static_cast<char>(0xE0 | (codepoint >> 12));
      out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (codepoint >> 18));
      out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
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

  void remove_last_codepoint(string& text) {
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) {
      text.pop_back();
    }

    if (!text.empty()) {
      text.pop_back();
    }
  }

  int row_at(const launcher_state& state, const vector<const launcher_entry*>& entries, int px, int py) {
    if (px < state.pad || px >= state.width - state.pad || py < state.list_top) {
      return -1;
    }

    const int row = (py - state.list_top) / state.row_height;

    if (row < 0 || row >= state.visible_rows) {
      return -1;
    }

    const int index = state.scroll + row;
    return index < static_cast<int>(entries.size()) ? index : -1;
  }

  xcb_visualtype_t* find_visualtype(xcb_screen_t* screen) {
    xcb_depth_iterator_t depth_iter = xcb_screen_allowed_depths_iterator(screen);
    xcb_visualtype_iterator_t visual_iter;

    for (; depth_iter.rem; xcb_depth_next(&depth_iter)) {
      visual_iter = xcb_depth_visuals_iterator(depth_iter.data);

      for (; visual_iter.rem; xcb_visualtype_next(&visual_iter)) {
        if (visual_iter.data->visual_id == screen->root_visual) {
          return visual_iter.data;
        }
      }
    }

    return nullptr;
  }

  void run_popup(launcher_menu_settings settings) {
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
      launcher_state state;
      state.scale = scale;
      state.pad = static_cast<int>(std::round(16.0 * scale));
      state.search_height = static_cast<int>(std::round(42.0 * scale));
      state.row_height = static_cast<int>(std::round(38.0 * scale));
      state.width = static_cast<int>(std::round(520.0 * scale));
      state.list_top = state.pad + state.search_height + static_cast<int>(std::round(10.0 * scale));
      state.font = settings.font.empty() ? "sans" : settings.font;
      state.entries = load_entries();

      state.background = make_rgb(settings.background, {0.06, 0.06, 0.07});
      state.foreground = make_rgb(settings.foreground, {0.85, 0.85, 0.85});
      state.border = mix(state.background, state.foreground, 0.22);
      state.muted = mix(state.background, state.foreground, 0.58);
      state.hover = mix(state.background, state.foreground, 0.10);
      state.input = mix(state.background, state.foreground, 0.06);

      const int margin = static_cast<int>(std::round(4.0 * scale));
      update_layout(state, settings, screen->width_in_pixels, screen->height_in_pixels, margin);

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

      const string window_name{"polybar-launcher"};
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

      for (int attempt = 0; attempt < 100; attempt++) {
        xcb_grab_pointer_reply_t* grab_reply = xcb_grab_pointer_reply(connection,
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

      // Keyboard shortcuts can still hold a keyboard grab while the popup is
      // starting, so retry briefly before giving up on Escape and the arrow keys.
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
        logger::make().warn("launcher: could not grab the keyboard, Escape and arrow keys are unavailable");
      }

      const auto keymap = get_keymap(connection);

      const auto redraw = [&] {
        paint(connection, pixmap, visual, state);
        present(connection, pixmap, window, gc, state);
      };

      const auto resize = [&] {
        const int old_height = state.height;
        update_layout(state, settings, screen->width_in_pixels, screen->height_in_pixels, margin);

        if (state.height == old_height) {
          return;
        }

        if (pixmap != XCB_NONE) {
          xcb_free_pixmap(connection, pixmap);
        }

        pixmap = xcb_generate_id(connection);
        xcb_create_pixmap(connection, screen->root_depth, pixmap, window, state.width, state.height);

        const uint32_t values[] = {static_cast<uint32_t>(state.x), static_cast<uint32_t>(state.y),
            static_cast<uint32_t>(state.width), static_cast<uint32_t>(state.height)};
        xcb_configure_window(connection, window,
            XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
            values);
        xcb_flush(connection);
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
            const auto entries = filtered_entries(state);
            const int active = row_at(state, entries, pointer_x_pos, pointer_y_pos);

            if (active >= 0 && active != state.active) {
              state.active = active;
              redraw();
            }

            break;
          }

          case XCB_BUTTON_PRESS: {
            auto* button = reinterpret_cast<xcb_button_press_event_t*>(event);
            const int pointer_x_pos = button->root_x - state.x;
            const int pointer_y_pos = button->root_y - state.y;

            if (button->detail == 4 || button->detail == 5) {
              move_selection(state, button->detail == 4 ? -1 : 1);
              redraw();
              break;
            }

            if (pointer_x_pos < 0 || pointer_x_pos >= state.width || pointer_y_pos < 0 ||
                pointer_y_pos >= state.height) {
              running = false;
              break;
            }

            const auto entries = filtered_entries(state);
            const int row = row_at(state, entries, pointer_x_pos, pointer_y_pos);

            if (row >= 0 && row < static_cast<int>(entries.size())) {
              launch_entry(*entries[row], settings);
              running = false;
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
            const bool control = key->state & XCB_MOD_MASK_CONTROL;
            const bool alt = key->state & XCB_MOD_MASK_1;
            const bool super = key->state & XCB_MOD_MASK_4;

            if (keysym == 0xFF1B) { // Escape
              running = false;
            } else if (keysym == 0xFF0D || keysym == 0xFF8D) { // Return, KP_Enter
              const auto entries = filtered_entries(state);

              if (!entries.empty()) {
                const int active = std::clamp(state.active, 0, static_cast<int>(entries.size()) - 1);
                launch_entry(*entries[active], settings);
                running = false;
              }
            } else if (keysym == 0xFF52) { // Up
              move_selection(state, -1);
              redraw();
            } else if (keysym == 0xFF54) { // Down
              move_selection(state, 1);
              redraw();
            } else if (keysym == 0xFF55) { // Page Up
              move_selection(state, -state.visible_rows);
              redraw();
            } else if (keysym == 0xFF56) { // Page Down
              move_selection(state, state.visible_rows);
              redraw();
            } else if (keysym == 0xFF08) { // BackSpace
              remove_last_codepoint(state.query);
              reset_selection(state);
              resize();
              redraw();
            } else if (control && (keysym == 'u' || keysym == 'U')) {
              state.query.clear();
              reset_selection(state);
              resize();
              redraw();
            } else if (!control) {
              const bool altgr = key->state & XCB_MOD_MASK_5;
              size_t level = 0;

              if (altgr) {
                level = shift ? 3 : 2;
              } else if (shift) {
                level = 1;
              }

              xcb_keysym_t typed = keysym;

              if (level < symbols.size() && symbols[level] != 0) {
                typed = symbols[level];
              }

              const string text = keysym_to_text(typed, caps && !shift);

              if (!alt && !super && !text.empty() && state.query.size() < 128) {
                state.query += text;
                reset_selection(state);
                resize();
                redraw();
              }
            }

            break;
          }

          default:
            break;
        }

        std::free(event);
      }
    } catch (const std::exception& err) {
      logger::make().err("launcher: %s", err.what());
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

namespace launcher_menu {
  void toggle(const launcher_menu_settings& settings) {
    if (g_open.exchange(true)) {
      g_close_requested = true;
      return;
    }

    g_close_requested = false;

    try {
      std::thread([settings] { run_popup(settings); }).detach();
    } catch (const std::exception& err) {
      g_open = false;
      logger::make().err("launcher: Could not start popup thread: %s", err.what());
    }
  }

  void close() {
    g_close_requested = true;
  }
} // namespace launcher_menu

POLYBAR_NS_END
