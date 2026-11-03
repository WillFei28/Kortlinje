#include "extensions/EtaPickerExtension.h"

#include "extensions/WebSocketExtension.h"
#include "modes/TickerMode.h"
#include "services/EtaService.h"
#include "services/HangTrace.h"
#include "services/WebServerService.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <ctime>

void EtaPickerExtension::pathFor(const char *file, char *out, size_t outLen)
{
    snprintf(out, outLen, "/catalog/%s", file);
}

void EtaPickerExtension::stampPathFor(const char *file, char *out, size_t outLen)
{
    snprintf(out, outLen, "/catalog/%s.ts", file);
}

bool EtaPickerExtension::catalogFresh(const char *file)
{
    char path[64]{};
    char stamp[72]{};
    pathFor(file, path, sizeof(path));
    stampPathFor(file, stamp, sizeof(stamp));
    if (!LittleFS.exists(path) || !LittleFS.exists(stamp))
    {
        return false;
    }
    File f{LittleFS.open(stamp, "r")};
    if (!f)
    {
        return false;
    }
    const String raw{f.readString()};
    f.close();
    const long fetched{strtol(raw.c_str(), nullptr, 10)};
    if (fetched <= 0)
    {
        return false;
    }
    const time_t now{time(nullptr)};
    if (now < 1'700'000'000L)
    {
        return true;
    }
    return (now - static_cast<time_t>(fetched)) <= catalogMaxAgeSec;
}

bool EtaPickerExtension::downloadOne(const Catalog &cat)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        return false;
    }
    LittleFS.mkdir("/catalog");
    char path[64]{};
    char stamp[72]{};
    char tmp[72]{};
    pathFor(cat.file, path, sizeof(path));
    stampPathFor(cat.file, stamp, sizeof(stamp));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    HTTPClient http{};
    http.setInsecure();
    http.setTimeout(10000);
    http.setUserAgent("Kortlinje-Catalog/1.0");
    HangTrace::feed();
    HangTrace::mark(HangTrace::Stage::Catalog);
    if (!http.begin(cat.url))
    {
        Serial.printf("catalog begin fail %s\n", cat.file);
        return false;
    }
    const int code{http.GET()};
    HangTrace::feed();
    if (code != HTTP_CODE_OK)
    {
        Serial.printf("catalog HTTP %d %s\n", code, cat.file);
        http.end();
        return false;
    }
    File out{LittleFS.open(tmp, "w")};
    if (!out)
    {
        http.end();
        return false;
    }
    // Chunked read so Async WebSocket keeps getting serviced (avoids WebUI drop).
    int written{0};
    WiFiClient *stream{http.getStreamPtr()};
    uint8_t buf[512]{};
    unsigned long lastPump{millis()};
    while (http.connected())
    {
        HangTrace::feed();
        const size_t avail{stream != nullptr ? stream->available() : 0U};
        if (avail == 0U)
        {
            if (!http.connected())
            {
                break;
            }
            if (millis() - lastPump > 50UL)
            {
                // Skip WS dispatch while catalog HTTP+FS write is open (same deadlock as ETA clear).
                lastPump = millis();
            }
            delay(1);
            continue;
        }
        const size_t got{stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail)};
        if (got == 0U)
        {
            break;
        }
        if (out.write(buf, got) != got)
        {
            out.close();
            http.end();
            LittleFS.remove(tmp);
            return false;
        }
        written += static_cast<int>(got);
        if (millis() - lastPump > 50UL)
        {
            lastPump = millis();
        }
    }
    out.close();
    http.end();
    if (written <= 0)
    {
        LittleFS.remove(tmp);
        Serial.printf("catalog empty %s\n", cat.file);
        return false;
    }
    LittleFS.remove(path);
    if (!LittleFS.rename(tmp, path))
    {
        LittleFS.remove(tmp);
        return false;
    }
    File ts{LittleFS.open(stamp, "w")};
    if (ts)
    {
        const time_t now{time(nullptr)};
        ts.printf("%ld", static_cast<long>(now > 0 ? now : 1L));
        ts.close();
    }
    Serial.printf("catalog ok %s (%d bytes)\n", cat.file, written);
    return true;
}

void EtaPickerExtension::begin()
{
    using Method = AsyncWebRequestMethod::AsyncWebRequestMethodType;
    WebServerService::http.on("/data/mtr_lines_and_stations.csv", Method::HTTP_GET, &onGetCsvMtr);
    WebServerService::http.on("/data/light_rail_routes_and_stops.csv", Method::HTTP_GET, &onGetCsvLrt);
    WebServerService::http.on("/data/mtr_bus_routes.csv", Method::HTTP_GET, &onGetCsvMtrBusRoutes);
    WebServerService::http.on("/data/mtr_bus_stops.csv", Method::HTTP_GET, &onGetCsvMtrBusStops);
    WebServerService::http.on("/api/ticker/stop", Method::HTTP_GET, &onGetStop);
    WebServerService::http.on("/api/ticker/stop", Method::HTTP_POST, &onPostStop);
    LittleFS.mkdir("/catalog");
}

void EtaPickerExtension::handle()
{
    // Only fetch when WebUI asked (/data 503) — never background-download on Clock boot.
    if (!demandFetch || fetchBusy || WiFi.status() != WL_CONNECTED)
    {
        return;
    }
    const unsigned long now{millis()};
    if (lastDownloadMs != 0UL && (now - lastDownloadMs) < downloadGapMs)
    {
        return;
    }
    for (uint8_t n{0U}; n < 4U; ++n)
    {
        const uint8_t i{static_cast<uint8_t>((fetchIndex + n) % 4U)};
        if (catalogFresh(catalogs[i].file))
        {
            continue;
        }
        fetchBusy = true;
        fetchIndex = static_cast<uint8_t>((i + 1U) % 4U);
        lastDownloadMs = now;
        static_cast<void>(downloadOne(catalogs[i]));
        fetchBusy = false;
        // Keep demandFetch until all fresh, so WebUI retries can finish the set.
        bool anyStale{false};
        for (const Catalog &c : catalogs)
        {
            if (!catalogFresh(c.file))
            {
                anyStale = true;
                break;
            }
        }
        if (!anyStale)
        {
            demandFetch = false;
        }
        return;
    }
    demandFetch = false;
}

void EtaPickerExtension::serveCsv(AsyncWebServerRequest *request, const char *file)
{
    char path[64]{};
    pathFor(file, path, sizeof(path));
    if (request->hasParam("refresh"))
    {
        char stamp[72]{};
        stampPathFor(file, stamp, sizeof(stamp));
        LittleFS.remove(stamp);
        demandFetch = true;
        request->send(503, "text/plain", "catalog refreshing");
        return;
    }
    if (!LittleFS.exists(path) || !catalogFresh(file))
    {
        demandFetch = true;
        request->send(503, "text/plain", "catalog downloading");
        return;
    }
    AsyncWebServerResponse *response{request->beginResponse(LittleFS, path, "text/csv")};
    response->addHeader("Cache-Control", "public, max-age=3600", false);
    request->send(response);
}

void EtaPickerExtension::onGetCsvMtr(AsyncWebServerRequest *request)
{
    serveCsv(request, "mtr_lines_and_stations.csv");
}

void EtaPickerExtension::onGetCsvLrt(AsyncWebServerRequest *request)
{
    serveCsv(request, "light_rail_routes_and_stops.csv");
}

void EtaPickerExtension::onGetCsvMtrBusRoutes(AsyncWebServerRequest *request)
{
    serveCsv(request, "mtr_bus_routes.csv");
}

void EtaPickerExtension::onGetCsvMtrBusStops(AsyncWebServerRequest *request)
{
    serveCsv(request, "mtr_bus_stops.csv");
}

void EtaPickerExtension::onGetStop(AsyncWebServerRequest *request)
{
    const HangTrace::IrqScope scope{HangTrace::Stage::HttpStopGet};
    AsyncResponseStream *response{request->beginResponseStream("application/json")};
    if (EtaService::stopJson().empty())
    {
        response->print("{}");
    }
    else
    {
        response->print(EtaService::stopJson().c_str());
    }
    request->send(response);
}

void EtaPickerExtension::onPostStop(AsyncWebServerRequest *request)
{
    const HangTrace::IrqScope scope{HangTrace::Stage::HttpStopPost};
    if (!request->hasParam("stop", true))
    {
        request->send(t_http_codes::HTTP_CODE_BAD_REQUEST, "application/json", "{\"ok\":false,\"err\":\"missing stop\"}");
        return;
    }
    const String value{request->getParam("stop", true)->value()};
    if (value.length() < 8)
    {
        request->send(t_http_codes::HTTP_CODE_BAD_REQUEST, "application/json", "{\"ok\":false,\"err\":\"short\"}");
        return;
    }
    JsonDocument probe{};
    if (deserializeJson(probe, value) != DeserializationError::Ok || !probe["operator"].is<const char *>())
    {
        request->send(t_http_codes::HTTP_CODE_BAD_REQUEST, "application/json", "{\"ok\":false,\"err\":\"json\"}");
        return;
    }
    if (!TickerMode::applyStopJson(std::string_view(value.c_str(), value.length())))
    {
        request->send(t_http_codes::HTTP_CODE_INTERNAL_SERVER_ERROR, "application/json",
                      "{\"ok\":false,\"err\":\"save\"}");
        return;
    }
    request->send(200, "application/json", "{\"ok\":true}");
}
