#include "modules/launcher.hpp"

#include "components/launcher_menu.hpp"
#include "drawtypes/label.hpp"
#include "modules/meta/base.inl"
#include "utils/env.hpp"

POLYBAR_NS

namespace modules {
  template class module<launcher_module>;

  launcher_module::launcher_module(const bar_settings& bar, string name_, const config& config)
      : static_module<launcher_module>(bar, move(name_), config) {
    m_router->register_action(EVENT_POPUP, [this]() { action_popup(); });

    m_terminal = m_conf.get(name(), "terminal", ""s);

    if (m_terminal.empty()) {
      const string terminal = env_util::get("TERMINAL");

      if (!terminal.empty()) {
        m_terminal = terminal + " -e";
      } else {
        m_terminal = "xterm -e";
      }
    }

    m_formatter->add(DEFAULT_FORMAT, TAG_LABEL, {TAG_LABEL});

    if (m_formatter->has(TAG_LABEL)) {
      m_label = load_optional_label(m_conf, name(), "label", "Applications");
    }
  }

  launcher_module::~launcher_module() {
    launcher_menu::close();
  }

  void launcher_module::start() {
    launcher_menu::preload();
    static_module<launcher_module>::start();
  }

  string launcher_module::get_output() {
    string output{module::get_output()};

    m_builder->action(mousebtn::LEFT, *this, EVENT_POPUP, "");
    m_builder->node(output);
    m_builder->action_close();

    return m_builder->flush();
  }

  bool launcher_module::build(builder* builder, const string& tag) const {
    if (tag != TAG_LABEL) {
      return false;
    }

    builder->node(m_label);
    return true;
  }

  void launcher_module::action_popup() {
    launcher_menu_settings settings;
    settings.bar_x = m_bar.pos.x;
    settings.bar_y = m_bar.pos.y;
    settings.bar_width = m_bar.size.w;
    settings.bar_height = m_bar.size.h;
    settings.bar_bottom = m_bar.bottom;
    settings.dpi = m_bar.dpi_x > 0 ? m_bar.dpi_x : 96.0;
    settings.background = m_bar.background;
    settings.foreground = m_bar.foreground;
    settings.terminal = m_terminal;

    auto fonts = m_conf.get_list<string>(m_conf.section(), "font", {});
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

    launcher_menu::toggle(settings);
  }
} // namespace modules

POLYBAR_NS_END
