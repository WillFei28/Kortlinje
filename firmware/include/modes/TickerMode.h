#pragma once

#include "config/IkeaKortlinje.h"
#include "handlers/TextHandler.h"
#include "modules/ModeModule.h"

#include <bits/unique_ptr.h>
#include <pico/time.h>
#include <string_view>

class TickerMode final : public ModeModule
{
private:
    static constexpr uint8_t scrollLoops{2U};
    static constexpr uint8_t holdTicksOnStart{5U}; // ~0.5s at 102ms/tick
    static constexpr int16_t scrollStep{1};
    static constexpr int32_t tickMs{-102L}; // 128ms / 1.25 ≈ 102ms
    /** Min gap between ETA HTTP attempts — prevents core0/WiFi death from back-to-back TLS. */
    static constexpr unsigned long fetchRetryMs{15'000UL};
    /** After a failed fetch, try again this soon (normal cadence is EtaService::refreshMs). */
    static constexpr unsigned long failRetryMs{30'000UL};
    /** Saved-stop fetch waits this long after boot so OTA/WebUI stay reachable. */
    static constexpr unsigned long bootFetchDelayMs{20'000UL};

    static inline std::string message{"ETA"};
    static inline bool pending{false};
    static inline bool cacheReady{false};
    static inline bool fetchRequested{false};
    static inline unsigned long lastFetchMs{0UL};
    static inline unsigned long lastFetchAttemptMs{0UL};

    int8_t offsetY{static_cast<int8_t>(GRID_ROWS / 2U)};

    int16_t offsetX{0};
    int16_t startOffsetX{0};
    int16_t width{0};

    uint8_t holdTicks{0U};
    uint8_t loopsDone{0U};

    bool timerRunning{false};
    static inline volatile bool tick{false};

    repeating_timer timer{};

    std::unique_ptr<const FontModule> font{};

    std::unique_ptr<TextHandler> text{};

    /** `message` is written on core0 (ETA/WS) and drawn on core1. */
    static void storeMessage(std::string_view value);
    [[nodiscard]] static std::string loadMessage();

    void prepareScroll();
    void setFont(std::string_view fontName);
    void setMessage(std::string_view _message);
    void startTimer();
    void stopTimer();
    void transmit();

    static bool onTimer(repeating_timer *rt);
    /** core0: drop departed times from the cache once per minute (no HTTP). */
    static void rerenderOnMinute();

public:
    static constexpr std::string_view name{"Ticker"};

    explicit TickerMode() : ModeModule(name) {};
    ~TickerMode() override;

    void begin() override;
    void handle() override;
    void end() override;
    void onReceive(JsonObjectConst payload, std::string_view source) override;

    /** Persist stop from WebUI; schedules immediate ETA fetch (non-blocking for WS). */
    static bool applyStopJson(std::string_view json);

    static void refreshCache(bool force = false);
    static void pollCache();
};
