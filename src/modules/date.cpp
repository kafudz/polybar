#include "modules/date.hpp"

#include "components/calendar.hpp"
#include "drawtypes/label.hpp"
#include "modules/meta/base.inl"

POLYBAR_NS

namespace modules {
  template class module<date_module>;

  date_module::date_module(const bar_settings& bar, string name_, const config& config)
      : timer_module<date_module>(bar, move(name_), config) {
    if (!m_bar.locale.empty()) {
      datetime_stream.imbue(std::locale(m_bar.locale.c_str()));
    }

    m_router->register_action(EVENT_TOGGLE, [this]() { action_toggle(); });
    m_router->register_action(EVENT_CALENDAR, [this]() { action_calendar(); });

    m_calendar = m_conf.get(name(), "calendar", false);
    m_week_starts_on_monday = m_conf.get(name(), "calendar-week-start", "monday"s) != "sunday"s;
    m_weather = m_conf.get(name(), "calendar-weather", false);
    m_weather_location = m_conf.get(name(), "calendar-weather-location", ""s);

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
        m_calendar_font = move(font);
      }
    }

    m_dateformat = m_conf.get(name(), "date", ""s);
    m_dateformat_alt = m_conf.get(name(), "date-alt", ""s);
    m_timeformat = m_conf.get(name(), "time", ""s);
    m_timeformat_alt = m_conf.get(name(), "time-alt", ""s);

    if (m_dateformat.empty() && m_timeformat.empty()) {
      throw module_error("No date or time format specified");
    }

    set_interval(1s);

    m_formatter->add(DEFAULT_FORMAT, TAG_LABEL, {TAG_LABEL, TAG_DATE});

    if (m_formatter->has(TAG_DATE)) {
      m_log.warn("%s: The format tag `<date>` is deprecated, use `<label>` instead.", name());

      m_formatter->get(DEFAULT_FORMAT)->value =
          string_util::replace_all(m_formatter->get(DEFAULT_FORMAT)->value, TAG_DATE, TAG_LABEL);
    }

    if (m_formatter->has(TAG_LABEL)) {
      m_label = load_optional_label(m_conf, name(), "label", "%date%");
    }
  }

  date_module::~date_module() {
    calendar::close();
  }

  bool date_module::update() {
    auto time = std::time(nullptr);

    auto date_format = m_toggled ? m_dateformat_alt : m_dateformat;
    // Clear stream contents
    datetime_stream.str("");
    datetime_stream << std::put_time(localtime(&time), date_format.c_str());
    auto date_string = datetime_stream.str();

    auto time_format = m_toggled ? m_timeformat_alt : m_timeformat;
    // Clear stream contents
    datetime_stream.str("");
    datetime_stream << std::put_time(localtime(&time), time_format.c_str());
    auto time_string = datetime_stream.str();

    if (m_date == date_string && m_time == time_string) {
      return false;
    }

    m_date = date_string;
    m_time = time_string;

    if (m_label) {
      m_label->reset_tokens();
      m_label->replace_token("%date%", m_date);
      m_label->replace_token("%time%", m_time);
    }

    return true;
  }

  bool date_module::build(builder* builder, const string& tag) const {
    if (tag != TAG_LABEL) {
      return false;
    }

    const bool has_alt_format = !m_dateformat_alt.empty() || !m_timeformat_alt.empty();
    int action_count = 0;

    if (has_alt_format) {
      // When the calendar is enabled, left click is reserved for the calendar
      // and the alternative format is toggled with a right click instead.
      builder->action(m_calendar ? mousebtn::RIGHT : mousebtn::LEFT, *this, EVENT_TOGGLE, "");
      action_count++;
    }

    if (m_calendar) {
      builder->action(mousebtn::LEFT, *this, EVENT_CALENDAR, "");
      action_count++;
    }

    builder->node(m_label);

    while (action_count-- > 0) {
      builder->action_close();
    }

    return true;
  }

  void date_module::action_toggle() {
    m_toggled = !m_toggled;
    wakeup();
  }

  void date_module::action_calendar() {
    calendar_settings settings;
    settings.bar_x = m_bar.pos.x;
    settings.bar_y = m_bar.pos.y;
    settings.bar_width = m_bar.size.w;
    settings.bar_height = m_bar.size.h;
    settings.bar_bottom = m_bar.bottom;
    settings.week_starts_on_monday = m_week_starts_on_monday;
    settings.weather = m_weather;
    settings.weather_location = m_weather_location;
    settings.dpi = m_bar.dpi_x > 0 ? m_bar.dpi_x : 96.0;
    settings.locale = m_bar.locale;
    settings.font = m_calendar_font;
    settings.background = m_bar.background;
    settings.foreground = m_bar.foreground;

    calendar::toggle(settings);
  }
}  // namespace modules

POLYBAR_NS_END
