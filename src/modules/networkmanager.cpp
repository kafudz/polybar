#include "modules/networkmanager.hpp"

#include <algorithm>
#include <utility>

#include "components/logger.hpp"
#include "components/system_popup.hpp"
#include "drawtypes/label.hpp"
#include "modules/meta/base.inl"
#include "utils/command.hpp"
#include "utils/string.hpp"

POLYBAR_NS

namespace {
  std::pair<int, string> run_command(const string& command_line) {
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

  string shell_quote(const string& value) {
    string result;
    result.reserve(value.size() + 2);
    result.push_back('\'');

    for (char c : value) {
      if (c == '\'') {
        // Close the quoted string, append an escaped quote, and reopen it.
        result += "'\\''";
      } else {
        result.push_back(c);
      }
    }

    result.push_back('\'');
    return result;
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

  bool is_connected_state(const string& state) {
    // nmcli reports states like "100 (connected)" and "30 (disconnected)".
    // Matching a plain "connected" substring would also match "disconnected".
    return state.find("(connected)") != string::npos;
  }

  struct device_info {
    string device;
    string type;
    string state;
    string connection;
    string ip;
  };

  vector<device_info> parse_devices(const string& output) {
    vector<device_info> devices;
    device_info current;
    bool have_device = false;

    for (const auto& line : split_lines(output)) {
      const auto separator = line.find(':');

      if (separator == string::npos) {
        continue;
      }

      const auto key = line.substr(0, separator);
      const auto value = unescape_terse(line.substr(separator + 1));

      if (key == "GENERAL.DEVICE") {
        if (have_device) {
          devices.push_back(current);
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
      devices.push_back(current);
    }

    return devices;
  }

  string wifi_signal() {
    auto result = run_command("nmcli -t -f IN-USE,SIGNAL device wifi list --rescan no 2>/dev/null");

    if (result.first != 0) {
      return {};
    }

    for (const auto& line : split_lines(result.second)) {
      const auto fields = split_terse(line);

      if (fields.size() >= 2 && fields[0] == "*") {
        return fields[1];
      }
    }

    return {};
  }

  string connection_icon(const string& type) {
    if (type == "wifi") {
      return "󰖩";
    }

    if (type == "ethernet") {
      return "󰈀";
    }

    if (type == "vpn" || type == "wireguard" || type == "tun") {
      return "󰦝";
    }

    return "󰛳";
  }
} // namespace

namespace modules {
  template class module<networkmanager_module>;

  networkmanager_module::networkmanager_module(const bar_settings& bar, string name_, const config& config)
      : timer_module<networkmanager_module>(bar, move(name_), config) {
    if (m_handle_events) {
      m_router->register_action(EVENT_POPUP, [this]() { action_popup(); });
    }

    m_interface = m_conf.get(name(), "interface", ""s);

    set_interval(5s);

    m_formatter->add(FORMAT_CONNECTED, TAG_LABEL_CONNECTED, {TAG_LABEL_CONNECTED});
    m_formatter->add(FORMAT_DISCONNECTED, TAG_LABEL_DISCONNECTED, {TAG_LABEL_DISCONNECTED});

    if (m_formatter->has(TAG_LABEL_CONNECTED, FORMAT_CONNECTED)) {
      m_label_connected = load_optional_label(m_conf, name(), TAG_LABEL_CONNECTED, "%icon% %connection%");
    }

    if (m_formatter->has(TAG_LABEL_DISCONNECTED, FORMAT_DISCONNECTED)) {
      m_label_disconnected = load_optional_label(m_conf, name(), TAG_LABEL_DISCONNECTED, "No network");
    }
  }

  bool networkmanager_module::update() {
    const string target = m_interface.empty() ? ""s : " " + shell_quote(m_interface);
    auto result =
        run_command("nmcli -t -f GENERAL.DEVICE,GENERAL.TYPE,GENERAL.STATE,GENERAL.CONNECTION,IP4.ADDRESS device show" +
                    target + " 2>/dev/null");

    device_info active;
    bool have_active = false;
    device_info fallback;
    bool have_fallback = false;

    if (result.first == 0) {
      for (const auto& device : parse_devices(result.second)) {
        if (device.type == "loopback" || !is_connected_state(device.state)) {
          continue;
        }

        if (m_interface.empty() || device.device == m_interface) {
          if (device.type == "wifi") {
            active = device;
            have_active = true;
            break;
          }

          if (!have_fallback) {
            fallback = device;
            have_fallback = true;
          }
        }
      }

      if (!have_active && have_fallback) {
        active = fallback;
        have_active = true;
      }
    }

    const connection_state new_state = have_active ? connection_state::CONNECTED : connection_state::DISCONNECTED;

    const string new_connection = have_active ? active.connection : ""s;
    const string new_device = have_active ? active.device : ""s;
    const string new_type = have_active ? active.type : ""s;
    const string new_ip = have_active ? active.ip : ""s;
    const string new_ssid = new_type == "wifi" ? new_connection : ""s;
    const string new_signal = new_type == "wifi" ? wifi_signal() : ""s;

    const bool changed = new_state != m_state || new_connection != m_connection || new_device != m_device ||
                         new_type != m_type || new_ip != m_ip || new_ssid != m_ssid || new_signal != m_signal;

    m_state = new_state;
    m_connection = new_connection;
    m_device = new_device;
    m_type = new_type;
    m_ip = new_ip;
    m_ssid = new_ssid;
    m_signal = new_signal;

    const auto replace_tokens = [&](label_t& label) {
      if (!label) {
        return;
      }

      label->reset_tokens();
      label->replace_token("%connection%", m_connection);
      label->replace_token("%name%", m_connection);
      label->replace_token("%device%", m_device);
      label->replace_token("%interface%", m_device);
      label->replace_token("%type%", m_type);
      label->replace_token("%ip%", m_ip);
      label->replace_token("%local_ip%", m_ip);
      label->replace_token("%ssid%", m_ssid);
      label->replace_token("%essid%", m_ssid);
      label->replace_token("%signal%", m_signal);
      label->replace_token("%icon%", connection_icon(m_type));
      label->replace_token("%status%", m_state == connection_state::CONNECTED ? "connected" : "disconnected");
    };

    replace_tokens(m_label_connected);
    replace_tokens(m_label_disconnected);

    return changed;
  }

  string networkmanager_module::get_format() const {
    return m_state == connection_state::CONNECTED ? FORMAT_CONNECTED : FORMAT_DISCONNECTED;
  }

  string networkmanager_module::get_output() {
    string output{module::get_output()};

    if (m_handle_events) {
      m_builder->action(mousebtn::LEFT, *this, EVENT_POPUP, "");
      m_builder->node(output);
      m_builder->action_close();
      return m_builder->flush();
    }

    return output;
  }

  bool networkmanager_module::build(builder* builder, const string& tag) const {
    if (tag == TAG_LABEL_CONNECTED && m_label_connected) {
      builder->node(m_label_connected);
      return true;
    }

    if (tag == TAG_LABEL_DISCONNECTED && m_label_disconnected) {
      builder->node(m_label_disconnected);
      return true;
    }

    return false;
  }

  void networkmanager_module::action_popup() {
    system_popup::toggle(system_popup::make_settings(m_bar, m_conf));
  }
} // namespace modules

POLYBAR_NS_END
