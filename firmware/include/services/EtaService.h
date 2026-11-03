#pragma once

#include <ArduinoJson.h>
#include <string>
#include <string_view>
#include <vector>

/** Direct HK operator ETA → LED ticker string. No VPS. */
class EtaService
{
public:
    static constexpr unsigned long refreshMs{180'000UL};
    /** Last good fetch older than this ⇒ show `ROUTE ?` instead of times. */
    static constexpr unsigned long staleMs{10UL * 60'000UL};
    static constexpr long windowSec{30 * 60L};

    static constexpr std::string_view cfgModule{"Ticker"};
    static constexpr std::string_view cfgStopKey{"stop"};

    /** Saved stop JSON (may be empty). core0 only — core1 must use stopJsonCopy()/hasStop(). */
    [[nodiscard]] static const std::string &stopJson();
    [[nodiscard]] static std::string stopJsonCopy();
    [[nodiscard]] static bool hasStop();

    /** Persist stop selection JSON; returns false on write failure. */
    static bool setStopJson(std::string_view json);

    /** Load stop from LittleFS (call after FS mount). */
    static void loadFromFs();

    /**
     * Fetch live ETAs for saved stop and format `ROUTE HH:MM …`.
     * On success updates outMessage and returns true.
     */
    static bool refresh(std::string &outMessage);

    /** Re-filter the cached times against the clock (no HTTP). False when nothing cached. */
    static bool render(std::string &outMessage);
    /** Current minute on the clock render() filters against. */
    [[nodiscard]] static time_t clockMinute();
    /** Clock render() filters against: API anchor + elapsed, else NTP, else 0. */
    [[nodiscard]] static time_t referenceNow();

    /** True while an operator/catalog HTTP call is on core0 — clear must not re-enter FS/WS. */
    [[nodiscard]] static bool httpActive();

    /** Catalog download shares the same core0 HTTP re-entry guard as ETA. */
    static void enterHttp();
    static void leaveHttp();

    /** core0 idle hook: persist a Confirmed stop outside WS callbacks / HTTPS. */
    static void pollIdle();

    [[nodiscard]] static std::string routeLabel(JsonObjectConst stop);

private:
    static inline std::string stop{};
    static inline bool inHttp{false};
    static inline uint32_t fetchGen{0U};
    static inline bool deferredFsClear{false};
    static inline bool deferredFsSave{false};

    static bool httpGet(const char *url, std::string &body);
    static bool httpPostJson(const char *url, const char *jsonBody, std::string &body);
    static void finishDeferredFs();
    [[nodiscard]] static bool fetchCancelled(uint32_t gen);

    static void collectTimes(JsonObjectConst stop, std::vector<time_t> &out);
    static void collectKmb(JsonObjectConst stop, std::vector<time_t> &out);
    static void collectCtb(JsonObjectConst stop, std::vector<time_t> &out);
    static void collectMtr(JsonObjectConst stop, std::vector<time_t> &out);
    static void collectLrt(JsonObjectConst stop, std::vector<time_t> &out);
    /** "屯門碼頭 → 元朗" → "元朗"; empty when the label has no arrow (legacy stops). */
    static std::string lrtTerminus(std::string_view directionLabel);
    static void collectMtrBus(JsonObjectConst stop, std::vector<time_t> &out);
    static void collectGmb(JsonObjectConst stop, std::vector<time_t> &out);

    static time_t parseEtaValue(JsonVariantConst v);
    /** Parse "YYYY-MM-DD HH:MM:SS" as local civil time (HKT via TZ). */
    static time_t parseApiDateTime(const char *s);
    static std::string formatTicker(std::string_view route, const std::vector<time_t> &times);
    static std::string formatHm(time_t t);

    /** API clock (MTR/LRT) at fetch; render() uses it instead of device NTP. */
    static inline time_t windowAnchor{0};
    /** Cleared before each fetch; set when the operator HTTP returned a body. */
    static inline bool lastHttpOk{false};

    /** Cache belongs to the stop that was current at this fetchGen (stop change invalidates it). */
    static inline uint32_t cachedGen{0U};
    static inline bool hasCache{false};
    static inline std::string cachedRoute{};
    static inline std::vector<time_t> cachedTimes{};
    /** API clock at fetch (0 = none; fall back to NTP). Advanced by millis(), not the RTC. */
    static inline time_t cachedAnchor{0};
    static inline unsigned long cachedOkMs{0UL};
};
