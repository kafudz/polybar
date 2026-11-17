#pragma once

#include "modules/meta/static_module.hpp"
#include "modules/meta/types.hpp"

POLYBAR_NS

namespace modules {
  /**
   * Application launcher module.
   *
   * Clicking the module opens a native popup with a searchable list of
   * installed desktop applications. The list can be filtered by typing and
   * navigated with the mouse or the arrow keys.
   */
  class launcher_module : public static_module<launcher_module> {
   public:
    explicit launcher_module(const bar_settings&, string, const config&);
    ~launcher_module() override;

    void start() override;
    void update() {}
    string get_output();
    bool build(builder* builder, const string& tag) const;

    static constexpr auto TYPE = LAUNCHER_TYPE;

    static constexpr auto EVENT_POPUP = "popup";

   protected:
    void action_popup();

   private:
    static constexpr const char* TAG_LABEL{"<label>"};

    label_t m_label;

    string m_terminal;
  };
} // namespace modules

POLYBAR_NS_END
