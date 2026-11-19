#include "modules/power.hpp"

#include "components/system_popup.hpp"
#include "drawtypes/label.hpp"
#include "modules/meta/base.inl"

POLYBAR_NS

namespace modules {
  template class module<power_module>;

  power_module::power_module(const bar_settings& bar, string name_, const config& config)
      : static_module<power_module>(bar, move(name_), config) {
    m_router->register_action(EVENT_POPUP, [this]() { action_popup(false); });
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
    system_popup::close();
  }

  string power_module::get_output() {
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
    // Power actions live in the system popup, so the module opens the same
    // window as the volume and network modules, with the power list expanded.
    auto settings = system_popup::make_settings(m_bar, m_conf, name());
    settings.power_commands = {m_lock_command, m_logout_command, m_reboot_command, m_shutdown_command};
    settings.power_expanded = true;
    settings.centered = centered;

    system_popup::toggle(settings);
  }
} // namespace modules

POLYBAR_NS_END
