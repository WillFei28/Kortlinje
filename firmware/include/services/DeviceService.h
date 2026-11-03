#pragma once

#include "modules/ServiceModule.h"
#include "services/DisplayService.h"
#include "services/ExtensionsService.h"
#include "services/FontsService.h"
#include "services/ModesService.h"
#include "services/WebServerService.h"

#include <ArduinoJson.h> // NOLINT(misc-include-cleaner)
#include <ESPAsyncWebServer.h>
#include <time.h>

class DeviceService final : public ServiceModule
{

public:
    static constexpr std::string_view name{"Device"};

    explicit DeviceService() : ServiceModule(name) {};

    void begin0();
    void begin1();
    void handle0();
    void handle1();

    static void receive(JsonObjectConst payload, std::string_view source, std::string_view destination);
    /** Safe from either core; extensions (WebSocket etc.) are only invoked on core0. */
    static void transmit(JsonObjectConst payload, std::string_view source, bool retain = true);

    /** Snapshot of retained transmits, serialized under the cross-core lock. */
    static void serializeTransmits(std::string &out);
    [[nodiscard]] static bool isTimeSynced();
    [[nodiscard]] static float getLoad0();
    [[nodiscard]] static float getLoad1();
    [[nodiscard]] static float getHandle0Us();
    [[nodiscard]] static float getHandle1Us();
    [[nodiscard]] static uint32_t getLoop0Hz();
    [[nodiscard]] static uint32_t getLoop1Hz();

private:
    struct CoreLoad
    {
        uint64_t windowStart;
        uint64_t busy;
        uint32_t loops;
        float pct;
        float handleUs;
        uint32_t loopHz;
    };

    static constexpr uint8_t samples{(0b1U << 16U) / (0b1U << ADC_RESOLUTION)};
    static constexpr time_t minValidEpoch{1'577'836'800L}; // 2020-01-01 UTC
    static constexpr unsigned long ntpRetryMs{15'000UL};
    static constexpr unsigned long wifiRetryMs{20'000UL};
    /** After the first successful join, a link lost this long triggers a clean reboot. */
    static constexpr unsigned long wifiRebootMs{60'000UL};

    float temperature{.0F};

    float voltage{.0F};

    static inline CoreLoad load0{0ULL, 0ULL, 0UL, .0F, .0F, 0UL};
    static inline CoreLoad load1{0ULL, 0ULL, 0UL, .0F, .0F, 0UL};

    DisplayService display{};

    ExtensionsService extensions{};

    FontsService fonts{};

    static inline JsonDocument transmits{};
    /** Per-source latest payloads awaiting core0 dispatch to extensions. */
    static inline JsonDocument pendingTransmits{};
    static inline bool flushingTransmits{false};

    static void flushTransmits();

    ModesService modes{};

    WebServerService webServer{};

    static inline bool timeSynced{false};
    static inline bool ntpStarted{false};
    static inline bool wifiConnected{false};
    static inline bool wifiEverConnected{false};
    static inline unsigned long wifiLostMs{0UL};
    static inline unsigned long lastNtpAttemptMs{0UL};
    static inline unsigned long lastWifiAttemptMs{0UL};

    void transmit();
    void pollTime();
    void pollWifi();

    static void onReceive(JsonObjectConst payload, std::string_view source);
    static void onWaitSet();
    static void onGetStats(AsyncWebServerRequest *request);
    static void sampleLoad(CoreLoad &load, uint64_t started, uint64_t finished);

    static uint16_t getSamples();
};
