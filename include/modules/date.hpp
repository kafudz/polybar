#pragma once

#include <atomic>
#include <ctime>
#include <iomanip>
#include <iostream>

#include "modules/meta/timer_module.hpp"
#include "modules/meta/types.hpp"

POLYBAR_NS

namespace modules {
  class date_module : public timer_module<date_module> {
   public:
    explicit date_module(const bar_settings&, string, const config&);
    ~date_module() override;

    bool update();
    bool build(builder* builder, const string& tag) const;

    static constexpr auto TYPE = DATE_TYPE;

    static constexpr auto EVENT_TOGGLE = "toggle";
    static constexpr auto EVENT_CALENDAR = "calendar";

   protected:
    void action_toggle();
    void action_calendar();

   private:
    static constexpr auto TAG_LABEL = "<label>";

    // @deprecated: Use <label>
    static constexpr auto TAG_DATE = "<date>";

    label_t m_label;

    string m_dateformat;
    string m_dateformat_alt;
    string m_timeformat;
    string m_timeformat_alt;

    string m_date;
    string m_time;

    // Single stringstream to be used to gather the results of std::put_time
    std::stringstream datetime_stream;

    bool m_calendar{false};
    bool m_week_starts_on_monday{true};
    string m_calendar_font{"sans"};
    bool m_weather{false};
    string m_weather_location{};
  bool m_media{false};
  string m_media_socket{};

    std::atomic<bool> m_toggled{false};
  };
} // namespace modules

POLYBAR_NS_END
