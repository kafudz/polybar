#pragma once

#include "modules/meta/static_module.hpp"
#include "modules/meta/types.hpp"

POLYBAR_NS

namespace modules {
  /**
   * Power menu module.
   *
   * Left-clicking the module opens the built-in system popup, where the power
   * section (Lock, Log out, Reboot, Shut down) starts collapsed and can be
   * expanded by clicking its row. The commands are configurable through
   * `lock-command`, `logout-command`, `reboot-command`, and `shutdown-command`.
   * Both the `popup` and `popup-center` actions open the same popup;
   * `popup-center` places it in the middle of the screen.
   */
  class power_module : public static_module<power_module> {
   public:
    explicit power_module(const bar_settings&, string, const config&);
    ~power_module() override;

    void update() {}
    string get_output();
    bool build(builder* builder, const string& tag) const;

    static constexpr auto TYPE = POWER_TYPE;

    static constexpr auto EVENT_POPUP = "popup";
    static constexpr auto EVENT_POPUP_CENTER = "popup-center";

   protected:
    void action_popup(bool centered = false);

   private:
    static constexpr const char* TAG_LABEL{"<label>"};

    label_t m_label;

    string m_lock_command;
    string m_logout_command;
    string m_reboot_command;
    string m_shutdown_command;
  };
} // namespace modules

POLYBAR_NS_END
