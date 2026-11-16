#include "modules/power.hpp"

#include "components/power_menu.hpp"
#include "drawtypes/label.hpp"
#include "modules/meta/base.inl"

POLYBAR_NS

namespace modules {
  template class module<power_module>;

  power_module::power_module(const bar_settings& bar, string name_, const config& config)
      : static_module<power_module>(bar, move(name_), config) {
    m_router->register_action(EVENT_POPUP, [this]() { action_popup(); });
    m_router->register_action(EVENT_POPUP_CENTER, [this]() { action_popup(true); });

    m_lock_command = m_conf.get(name(), "lock-command", "i3lock -n"s);
    m_logout_command = m_conf.get(name(), "logout-command", "bspc quit"s);
    m_reboot_command = m_conf.get(name(), "reboot-command", "systemctl reboot"s);
    m_shutdown_command = m_conf.get(name(), "shutdown-command", "systemctl poweroff"s);

    m_formatter->add(DEFAULT_FORMAT, TAG_LABEL, {TAG_LABEL});

    if (m_formatter->has(TAG_LABEL)) {
      m_label = load_optional_label(m_conf, name(), "label", "Power");
    }
  }

  power_module::~power_module() {
    power_menu::close();
  }

  string power_module::get_output() {
    // Get the module output early so that the format prefix/suffix also gets
    // wrapped with the action handler.
    string output{module::get_output()};

    m_builder->action(mousebtn::LEFT, *this, EVENT_POPUP, "");
    m_builder->node(output);
    m_builder->action_close();

    return m_builder->flush();
  }

  bool power_module::build(builder* builder, const string& tag) const {
    if (tag != TAG_LABEL) {
      return false;
    }

    builder->node(m_label);
    return true;
  }

  void power_module::action_popup(bool centered) {
    power_menu_settings settings;
    settings.bar_x = m_bar.pos.x;
    settings.bar_y = m_bar.pos.y;
    settings.bar_width = m_bar.size.w;
    settings.bar_height = m_bar.size.h;
    settings.bar_bottom = m_bar.bottom;
    settings.centered = centered;
    settings.dpi = m_bar.dpi_x > 0 ? m_bar.dpi_x : 96.0;
    settings.background = m_bar.background;
    settings.foreground = m_bar.foreground;

    auto fonts = m_conf.get_list<string>(m_conf.section(), "font", {});
    if (!fonts.empty()) {
      // The bar font specification may contain a style and offset suffix, for example:
      // "VictorMono Nerd Font:style=Bold;1"
      auto font = fonts.front();
      auto suffix = font.find_first_of(":;");
      if (suffix != string::npos) {
        font.erase(suffix);
      }

      if (!font.empty()) {
        settings.font = move(font);
      }
    }

    settings.lock_command = m_lock_command;
    settings.logout_command = m_logout_command;
    settings.reboot_command = m_reboot_command;
    settings.shutdown_command = m_shutdown_command;

    power_menu::toggle(settings);
  }
} // namespace modules

POLYBAR_NS_END
