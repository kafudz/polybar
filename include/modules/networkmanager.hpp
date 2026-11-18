#pragma once

#include "components/config.hpp"
#include "modules/meta/timer_module.hpp"
#include "modules/meta/types.hpp"

POLYBAR_NS

namespace modules {
  /**
   * NetworkManager status module.
   *
   * Reads the active connection through `nmcli`, so wired connections,
   * Wi-Fi networks, and VPNs are all reported by NetworkManager itself.
   * Clicking the module opens the built-in system popup, which contains the
   * NetworkManager Wi-Fi controls.
   */
  class networkmanager_module : public timer_module<networkmanager_module> {
   public:
    explicit networkmanager_module(const bar_settings&, string, const config&);

    bool update();
    string get_format() const;
    string get_output();
    bool build(builder* builder, const string& tag) const;

    static constexpr auto TYPE = NETWORKMANAGER_TYPE;

    static constexpr auto EVENT_POPUP = "popup";

   protected:
    void action_popup();

   private:
    static constexpr auto FORMAT_CONNECTED = "format-connected";
    static constexpr auto FORMAT_DISCONNECTED = "format-disconnected";

    static constexpr auto TAG_LABEL_CONNECTED = "<label-connected>";
    static constexpr auto TAG_LABEL_DISCONNECTED = "<label-disconnected>";

    enum class connection_state { CONNECTED, DISCONNECTED };

    label_t m_label_connected;
    label_t m_label_disconnected;

    connection_state m_state{connection_state::DISCONNECTED};

    string m_interface{};

    string m_connection{};
    string m_device{};
    string m_type{};
    string m_ip{};
    string m_ssid{};
    string m_signal{};
  };
} // namespace modules

POLYBAR_NS_END
