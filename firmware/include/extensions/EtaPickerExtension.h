#pragma once

#include "modules/ExtensionModule.h"

#include <ESPAsyncWebServer.h>
#include <string_view>

/**
 * Same-origin CSV proxy for WebUI (MTR opendata has no browser CORS).
 * Downloads official CSVs on demand into LittleFS — not embedded, not on Clock boot.
 */
class EtaPickerExtension final : public ExtensionModule
{
private:
    static constexpr std::string_view name{"ETA Picker"};
    static constexpr long catalogMaxAgeSec{7L * 24L * 60L * 60L};
    static constexpr unsigned long downloadGapMs{2'000UL};

    struct Catalog
    {
        const char *file;
        const char *url;
    };

    static constexpr Catalog catalogs[4]{
        {"mtr_lines_and_stations.csv", "https://opendata.mtr.com.hk/data/mtr_lines_and_stations.csv"},
        {"light_rail_routes_and_stops.csv", "https://opendata.mtr.com.hk/data/light_rail_routes_and_stops.csv"},
        {"mtr_bus_routes.csv", "https://opendata.mtr.com.hk/data/mtr_bus_routes.csv"},
        {"mtr_bus_stops.csv", "https://opendata.mtr.com.hk/data/mtr_bus_stops.csv"},
    };

    static inline uint8_t fetchIndex{0U};
    static inline bool fetchBusy{false};
    static inline bool demandFetch{false};
    static inline unsigned long lastDownloadMs{0UL};

    static void pathFor(const char *file, char *out, size_t outLen);
    static void stampPathFor(const char *file, char *out, size_t outLen);
    static bool catalogFresh(const char *file);
    static bool downloadOne(const Catalog &cat);
    static void serveCsv(AsyncWebServerRequest *request, const char *file);

    static void onGetCsvMtr(AsyncWebServerRequest *request);
    static void onGetCsvLrt(AsyncWebServerRequest *request);
    static void onGetCsvMtrBusRoutes(AsyncWebServerRequest *request);
    static void onGetCsvMtrBusStops(AsyncWebServerRequest *request);
    static void onGetStop(AsyncWebServerRequest *request);
    static void onPostStop(AsyncWebServerRequest *request);

public:
    explicit EtaPickerExtension() : ExtensionModule(name) {};

    void begin() override;
    void handle() override;
};
