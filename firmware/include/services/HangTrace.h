#pragma once

#include <Arduino.h>
#include <hardware/structs/watchdog.h>
#include <hardware/watchdog.h>

/**
 * Core0 hang diagnostics + auto-recovery.
 * Stage codes live in watchdog scratch[0..1] (bootrom only uses 4..7), so they
 * survive the watchdog reset and tell us where core0 stopped.
 */
namespace HangTrace
{
enum Stage : uint32_t
{
    None = 0U,
    Loop = 1U,
    FsSave = 2U,
    FsClear = 3U,
    WifiBegin = 4U,
    Flush = 5U,
    Adc = 6U,
    Display = 7U,
    PollWifi = 8U,
    EtaPoll = 9U,
    HttpBegin = 10U,
    HttpRequest = 11U,
    HttpRead = 12U,
    HttpDrain = 13U,
    HttpEnd = 14U,
    EtaParse = 15U,
    EtaRerender = 16U,
    PollTime = 17U,
    WsConnect = 20U,
    WsData = 21U,
    HttpStats = 22U,
    HttpStopPost = 23U,
    HttpStopGet = 24U,
    WsOther = 25U,
    Catalog = 30U,
    Ota = 40U,
    /** ExtensionsService::handle(): ExtensionBase + index into ExtensionsService::modules. */
    ExtensionBase = 100U,
};

inline void markExtension(size_t index)
{
    watchdog_hw->scratch[0] = static_cast<uint32_t>(ExtensionBase) + static_cast<uint32_t>(index);
    watchdog_hw->scratch[1] = millis();
}

/** Watchdog period. RP2350 hardware limit is ~16.7s; TLS GET is capped at 8s. */
inline constexpr uint32_t watchdogMs{15'000U};

inline uint32_t lastStage{0U};
inline uint32_t lastStageMs{0U};
inline uint32_t lastIrqStage{0U};
inline uint32_t lastIrqMs{0U};
inline bool lastWasWatchdog{false};

/** Main-loop (core0 thread) stage. */
inline void mark(Stage stage)
{
    watchdog_hw->scratch[0] = static_cast<uint32_t>(stage);
    watchdog_hw->scratch[1] = millis();
}

/** lwIP/AsyncTCP callback stage — separate slot so it never masks the loop stage. */
inline void markIrq(Stage stage)
{
    watchdog_hw->scratch[2] = static_cast<uint32_t>(stage);
    watchdog_hw->scratch[3] = millis();
}

/** IRQ handler scope: non-zero irq stage after a watchdog reset ⇒ died inside that handler. */
class IrqScope
{
public:
    explicit IrqScope(Stage stage) { markIrq(stage); }
    ~IrqScope() { markIrq(None); }
    IrqScope(const IrqScope &) = delete;
    IrqScope &operator=(const IrqScope &) = delete;
};

inline void feed() { watchdog_update(); }

/** Call first thing in begin0(): capture previous stages, then clear. */
inline void captureBoot()
{
    lastWasWatchdog = watchdog_enable_caused_reboot();
    lastStage = watchdog_hw->scratch[0];
    lastStageMs = watchdog_hw->scratch[1];
    lastIrqStage = watchdog_hw->scratch[2];
    lastIrqMs = watchdog_hw->scratch[3];
    for (uint8_t idx{0U}; idx < 4U; ++idx)
    {
        watchdog_hw->scratch[idx] = 0U;
    }
}

inline void start() { watchdog_enable(watchdogMs, true); }
} // namespace HangTrace
