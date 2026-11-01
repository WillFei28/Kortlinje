#pragma once

#include "modes/ClockMode.h"  //NOLINT(misc-include-cleaner)
#include "modes/TickerMode.h" //NOLINT(misc-include-cleaner)
#include "modules/ModeModule.h"
#include "modules/ServiceModule.h"

#include <array>
#include <memory>

class ModesService final : public ServiceModule
{

public:
    static constexpr std::string_view name{"Modes"};

    static constexpr std::array<std::string_view, 2U> names{
        ClockMode::name,
        TickerMode::name,
    };

    explicit ModesService() : ServiceModule(name) {};

    void begin();
    void handle();

    static void onReceive(JsonObjectConst payload, std::string_view source);
    static void setMode(std::string_view modeName, bool power = true);
    static void requestMode(std::string_view modeName, bool power = true);
    static void setModeNext();
    static void setModePrevious();

    [[nodiscard]] static ModeModule *getMode();

    [[nodiscard]] static std::unique_ptr<ModeModule> getMode(std::string_view modeName);

private:
    unsigned long lastMillis{0UL};

    static inline bool pendingPower{true};
    static inline std::string_view pendingMode{};

    static inline std::unique_ptr<ModeModule> mode{};

    static void transmit();
};
