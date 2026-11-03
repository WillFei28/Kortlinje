#include "services/EtaService.h"

#include "handlers/ConfigHandler.h"
#include "services/HangTrace.h"
#include "services/IrqSafeLock.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <algorithm>
#include <cstdio>
#include <ctime>

namespace
{
// `stop` is written from core0 (loop + lwIP IRQ) and read by TickerMode on core1.
IrqSafeLock stopLock{};

/** Discard leftover response bytes before http.end(); abrupt close wedges cyw43. */
void drainHttpStream(HTTPClient &http, WiFiClient *stream, uint8_t *buf, size_t bufLen,
                     unsigned long maxMs)
{
    HangTrace::mark(HangTrace::Stage::HttpDrain);
    const unsigned long started{millis()};
    while (http.connected() && (millis() - started) < maxMs)
    {
        HangTrace::feed();
        const size_t avail{stream != nullptr ? stream->available() : 0U};
        if (avail == 0U)
        {
            delay(1);
            continue;
        }
        static_cast<void>(stream->readBytes(buf, avail > bufLen ? bufLen : avail));
    }
}

} // namespace

const std::string &EtaService::stopJson() { return stop; }

std::string EtaService::stopJsonCopy()
{
    stopLock.lock();
    std::string copy{stop};
    stopLock.unlock();
    return copy;
}

bool EtaService::hasStop()
{
    stopLock.lock();
    const bool has{!stop.empty()};
    stopLock.unlock();
    return has;
}

bool EtaService::httpActive() { return inHttp; }

void EtaService::pollIdle()
{
    if (deferredFsSave && !inHttp && !stop.empty())
    {
        deferredFsSave = false;
        HangTrace::mark(HangTrace::Stage::FsSave);
        static_cast<void>(ConfigHandler::set(cfgModule, cfgStopKey, stop));
        Serial.println("[eta] stop saved");
    }
}

void EtaService::enterHttp() { inHttp = true; }

void EtaService::leaveHttp()
{
    inHttp = false;
    finishDeferredFs();
}

bool EtaService::fetchCancelled(uint32_t gen) { return gen != fetchGen || stop.empty(); }

void EtaService::finishDeferredFs()
{
    if (deferredFsClear)
    {
        deferredFsClear = false;
        deferredFsSave = false;
        HangTrace::mark(HangTrace::Stage::FsClear);
        ConfigHandler::remove(cfgModule, cfgStopKey);
    }
    if (deferredFsSave && !stop.empty())
    {
        deferredFsSave = false;
        HangTrace::mark(HangTrace::Stage::FsSave);
        static_cast<void>(ConfigHandler::set(cfgModule, cfgStopKey, stop));
    }
}

void EtaService::loadFromFs()
{
    std::string value{};
    if (ConfigHandler::get(cfgModule, cfgStopKey, value) && !value.empty())
    {
        stopLock.lock();
        stop = std::move(value);
        stopLock.unlock();
    }
}

bool EtaService::setStopJson(std::string_view json)
{
    if (json.empty())
    {
        if (stop.empty())
        {
            return true;
        }
        stopLock.lock();
        stop.clear();
        stopLock.unlock();
        ++fetchGen; // invalidate in-flight operator HTTP
        if (inHttp)
        {
            // Defer LittleFS — writing FS while HTTPClient is active deadlocks WiFi.
            deferredFsClear = true;
            return true;
        }
        HangTrace::markIrq(HangTrace::Stage::FsClear);
        ConfigHandler::remove(cfgModule, cfgStopKey);
        return true;
    }
    // Same stop re-applied (WS reconnect / WebUI echo) — do NOT bump fetchGen mid-HTTP.
    if (json == stop)
    {
        return true;
    }
    // Persisted later by pollIdle(): this runs inside a WS lwIP callback.
    stopLock.lock();
    stop.assign(json.data(), json.size());
    stopLock.unlock();
    ++fetchGen;
    deferredFsClear = false;
    deferredFsSave = true;
    return true;
}

bool EtaService::httpGet(const char *url, std::string &body)
{
    body.clear();
    const uint32_t gen{fetchGen};
    inHttp = true;
    HTTPClient http{};
    http.setInsecure();
    http.setTimeout(8000);
    http.setReuse(false);
    http.setUserAgent("Kortlinje-ETA/1.0");
    HangTrace::feed();
    HangTrace::mark(HangTrace::Stage::HttpBegin);
    if (!http.begin(url))
    {
        inHttp = false;
        finishDeferredFs();
        return false;
    }
    HangTrace::mark(HangTrace::Stage::HttpRequest);
    const int code{http.GET()};
    HangTrace::feed();
    if (fetchCancelled(gen) || code != HTTP_CODE_OK)
    {
        if (code != HTTP_CODE_OK && !fetchCancelled(gen))
        {
            Serial.printf("eta HTTP %d\n", code);
        }
        HangTrace::mark(HangTrace::Stage::HttpEnd);
        http.end();
        inHttp = false;
        finishDeferredFs();
        return false;
    }
    WiFiClient *stream{http.getStreamPtr()};
    // getSize(): Content-Length, or -1 when chunked/unknown.
    const int contentLen{http.getSize()};
    if (contentLen > 0)
    {
        body.reserve(static_cast<size_t>(std::min(contentLen, 48 * 1024)));
    }
    constexpr size_t kMaxBody{48U * 1024U};
    uint8_t buf[512]{};
    const unsigned long started{millis()};
    unsigned long lastDataMs{started};
    HangTrace::mark(HangTrace::Stage::HttpRead);
    while (http.connected())
    {
        HangTrace::feed();
        if (fetchCancelled(gen) || (millis() - started) > 8'000UL)
        {
            break;
        }
        // LRT/KMB send Content-Length — stop when complete. Old loop spun until the
        // 8s timeout with avail==0, blocking core0 (~AsyncWebServer) every fetch.
        if (contentLen > 0 && static_cast<int>(body.size()) >= contentLen)
        {
            break;
        }
        const size_t avail{stream != nullptr ? stream->available() : 0U};
        if (avail == 0U)
        {
            // Chunked/unknown length: idle after first byte ⇒ peer done.
            if (!body.empty() && (millis() - lastDataMs) > 400UL)
            {
                break;
            }
            delay(1);
            continue;
        }
        const size_t got{stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail)};
        if (got == 0U)
        {
            break;
        }
        lastDataMs = millis();
        if (body.size() + got > kMaxBody)
        {
            Serial.println("eta body cap");
            break;
        }
        body.append(reinterpret_cast<const char *>(buf), got);
    }
    // Only drain if we stopped early (cap/cancel) with bytes still pending.
    if (contentLen < 0 || static_cast<int>(body.size()) < contentLen)
    {
        drainHttpStream(http, stream, buf, sizeof(buf), 1'500UL);
    }
    // Do not stream->stop() — that has hard-locked cyw43 after LRT/MTR fetches.
    HangTrace::mark(HangTrace::Stage::HttpEnd);
    http.end();
    inHttp = false;
    finishDeferredFs();
    if (fetchCancelled(gen))
    {
        body.clear();
        return false;
    }
    lastHttpOk = !body.empty();
    return lastHttpOk;
}

bool EtaService::httpPostJson(const char *url, const char *jsonBody, std::string &body)
{
    body.clear();
    const uint32_t gen{fetchGen};
    inHttp = true;
    HTTPClient http{};
    http.setInsecure();
    http.setTimeout(10000);
    http.setUserAgent("Kortlinje-ETA/1.0");
    HangTrace::feed();
    HangTrace::mark(HangTrace::Stage::HttpBegin);
    if (!http.begin(url))
    {
        inHttp = false;
        finishDeferredFs();
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    HangTrace::mark(HangTrace::Stage::HttpRequest);
    const int code{http.POST(jsonBody)};
    HangTrace::feed();
    if (fetchCancelled(gen) || code != HTTP_CODE_OK)
    {
        if (code != HTTP_CODE_OK && !fetchCancelled(gen))
        {
            Serial.printf("eta POST %d\n", code);
        }
        HangTrace::mark(HangTrace::Stage::HttpEnd);
        http.end();
        inHttp = false;
        finishDeferredFs();
        return false;
    }
    HangTrace::mark(HangTrace::Stage::HttpRead);
    body = http.getString().c_str();
    HangTrace::mark(HangTrace::Stage::HttpEnd);
    http.end();
    inHttp = false;
    finishDeferredFs();
    if (fetchCancelled(gen))
    {
        body.clear();
        return false;
    }
    lastHttpOk = !body.empty();
    return lastHttpOk;
}

time_t EtaService::parseEtaValue(JsonVariantConst v)
{
    if (v.isNull())
    {
        return 0;
    }
    if (v.is<long>() || v.is<unsigned long>() || v.is<int>())
    {
        const long n{v.as<long>()};
        if (n <= 0)
        {
            return 0;
        }
        return static_cast<time_t>(n < 1'000'000'000'000L ? n : n / 1000L);
    }
    if (!v.is<const char *>())
    {
        return 0;
    }
    const char *s{v.as<const char *>()};
    if (s == nullptr || s[0] == '\0')
    {
        return 0;
    }
    bool digits{true};
    for (const char *p{s}; *p; ++p)
    {
        if (*p < '0' || *p > '9')
        {
            digits = false;
            break;
        }
    }
    if (digits)
    {
        const long n{strtol(s, nullptr, 10)};
        return static_cast<time_t>(n < 1'000'000'000'000L ? n : n / 1000L);
    }
    std::tm tm{};
    int y{0};
    int mo{0};
    int d{0};
    int h{0};
    int mi{0};
    int se{0};
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) >= 5 ||
        sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) >= 5)
    {
        tm.tm_year = y - 1900;
        tm.tm_mon = mo - 1;
        tm.tm_mday = d;
        tm.tm_hour = h;
        tm.tm_min = mi;
        tm.tm_sec = se;
        tm.tm_isdst = -1;
        return mktime(&tm);
    }
    return 0;
}

time_t EtaService::parseApiDateTime(const char *s)
{
    if (s == nullptr || s[0] == '\0')
    {
        return 0;
    }
    std::tm tm{};
    int y{0};
    int mo{0};
    int d{0};
    int h{0};
    int mi{0};
    int se{0};
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 5 &&
        sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 5)
    {
        return 0;
    }
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = se;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

std::string EtaService::formatHm(time_t t)
{
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[8]{};
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

std::string EtaService::formatTicker(std::string_view route, const std::vector<time_t> &times)
{
    std::string out{};
    out.assign(route.data(), route.size());
    if (times.empty())
    {
        out += " --";
        return out;
    }
    for (time_t t : times)
    {
        out += ' ';
        out += formatHm(t);
    }
    return out;
}

std::string EtaService::routeLabel(JsonObjectConst stopObj)
{
    const char *op{stopObj["operator"] | ""};
    if (strcmp(op, "kmb") == 0 || strcmp(op, "ctb") == 0)
    {
        return stopObj["route"] | "?";
    }
    if (strcmp(op, "mtr") == 0 || strcmp(op, "lrt") == 0)
    {
        return stopObj["line"] | "?";
    }
    if (strcmp(op, "mtrbus") == 0)
    {
        return stopObj["routeName"] | "?";
    }
    if (strcmp(op, "gmb") == 0)
    {
        return stopObj["route_code"] | "?";
    }
    return "?";
}

void EtaService::collectKmb(JsonObjectConst stopObj, std::vector<time_t> &out)
{
    const char *stopId{stopObj["stop_id"] | ""};
    const char *route{stopObj["route"] | ""};
    char serviceBuf[8]{};
    const char *service{stopObj["service_type"] | ""};
    if (!service[0])
    {
        if (stopObj["service_type"].is<int>() || stopObj["service_type"].is<long>())
        {
            snprintf(serviceBuf, sizeof(serviceBuf), "%ld", stopObj["service_type"].as<long>());
            service = serviceBuf;
        }
        else
        {
            service = "1";
        }
    }
    const char *bound{stopObj["bound"] | ""};
    if (!stopId[0] || !route[0])
    {
        return;
    }
    char url[192]{};
    snprintf(url, sizeof(url), "https://data.etabus.gov.hk/v1/transport/kmb/eta/%s/%s/%s", stopId, route, service);
    std::string body{};
    if (!httpGet(url, body))
    {
        return;
    }
    JsonDocument filter{};
    filter["data"][0]["eta"] = true;
    filter["data"][0]["dir"] = true;
    JsonDocument doc{};
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) != DeserializationError::Ok)
    {
        Serial.println("eta kmb json fail");
        return;
    }
    for (JsonObjectConst row : doc["data"].as<JsonArrayConst>())
    {
        const char *dir{row["dir"] | ""};
        if (bound[0] && dir[0] && strcmp(bound, dir) != 0)
        {
            continue;
        }
        const time_t t{parseEtaValue(row["eta"])};
        if (t > 0)
        {
            out.push_back(t);
        }
    }
}

void EtaService::collectCtb(JsonObjectConst stopObj, std::vector<time_t> &out)
{
    const char *stopId{stopObj["stop_id"] | ""};
    const char *route{stopObj["route"] | ""};
    const char *direction{stopObj["direction"] | ""};
    if (!stopId[0] || !route[0])
    {
        return;
    }
    const char dirChar{strcmp(direction, "inbound") == 0 ? 'I' : 'O'};
    char url[192]{};
    snprintf(url, sizeof(url), "https://rt.data.gov.hk/v2/transport/citybus/eta/CTB/%s/%s", stopId, route);
    std::string body{};
    if (!httpGet(url, body))
    {
        return;
    }
    JsonDocument filter{};
    filter["data"][0]["eta"] = true;
    filter["data"][0]["dir"] = true;
    JsonDocument doc{};
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) != DeserializationError::Ok)
    {
        return;
    }
    for (JsonObjectConst row : doc["data"].as<JsonArrayConst>())
    {
        const char *dir{row["dir"] | ""};
        if (dir[0] && dir[0] != dirChar)
        {
            continue;
        }
        const time_t t{parseEtaValue(row["eta"])};
        if (t > 0)
        {
            out.push_back(t);
        }
    }
}

void EtaService::collectMtr(JsonObjectConst stopObj, std::vector<time_t> &out)
{
    const char *line{stopObj["line"] | ""};
    const char *sta{stopObj["station_code"] | ""};
    if (!line[0] || !sta[0])
    {
        return;
    }
    char url[192]{};
    snprintf(url, sizeof(url),
             "https://rt.data.gov.hk/v1/transport/mtr/getSchedule.php?line=%s&sta=%s&lang=TC", line, sta);
    std::string body{};
    if (!httpGet(url, body))
    {
        return;
    }
    char key[32]{};
    snprintf(key, sizeof(key), "%s-%s", line, sta);
    JsonDocument doc{};
    // Body is ~1KB — parse fully (Filter+dynamic key has been flaky on Pico).
    if (deserializeJson(doc, body) != DeserializationError::Ok)
    {
        return;
    }
    if (!doc["status"].isNull() && doc["status"].as<int>() == 0)
    {
        Serial.println("mtr status 0");
        return;
    }
    JsonObjectConst block{doc["data"][key]};
    // Anchor + arrival HH:MM come from API (HKT). Device NTP has been ~1h off.
    const char *curr{doc["curr_time"] | ""};
    if (!curr[0])
    {
        curr = block["curr_time"] | "";
    }
    const time_t apiNow{parseApiDateTime(curr)};
    if (apiNow > 0)
    {
        windowAnchor = apiNow;
    }
    // CSV uses UT/DT plus branch variants LMC-UT/LMC-DT (EAL) and TKS-UT/TKS-DT (TKL);
    // API uses UP/DOWN. Prefer matching direction when known.
    const std::string_view direction{stopObj["direction"] | ""};
    const bool wantUp{direction == "UP" || direction.ends_with("UT")};
    const bool wantDown{direction == "DOWN" || direction.ends_with("DT")};
    const bool filterDir{wantUp || wantDown};
    // Northbound branches share track: drop trains for the other branch terminus,
    // keep short-workings (e.g. EAL to TAP). Southbound branches merge — nothing to drop.
    const char *otherBranch{""};
    if (wantUp && strcmp(line, "EAL") == 0)
    {
        otherBranch = direction == "LMC-UT" ? "LOW" : "LMC";
    }
    else if (wantUp && strcmp(line, "TKL") == 0)
    {
        otherBranch = direction == "TKS-UT" ? "POA" : "LHP";
    }

    auto readMins = [](JsonVariantConst v) -> long {
        if (v.isNull())
        {
            return -1L;
        }
        if (v.is<long>() || v.is<unsigned long>() || v.is<int>())
        {
            return v.as<long>();
        }
        if (v.is<const char *>())
        {
            const char *raw{v.as<const char *>()};
            if (raw != nullptr && raw[0] != '\0')
            {
                return strtol(raw, nullptr, 10);
            }
        }
        return -1L;
    };

    auto take = [&](JsonArrayConst rows) {
        for (JsonObjectConst e : rows)
        {
            const char *valid{e["valid"] | "Y"};
            if (valid[0] == 'N' || valid[0] == 'n')
            {
                continue;
            }
            if (otherBranch[0] && strcmp(e["dest"] | "", otherBranch) == 0)
            {
                continue;
            }
            const long mins{readMins(e["ttnt"])};
            if (mins > windowSec / 60L)
            {
                continue;
            }
            // Prefer API arrival wall-clock string — correct even when device RTC is wrong.
            time_t t{parseApiDateTime(e["time"] | "")};
            if (t <= 0 && apiNow > 0 && mins >= 0L)
            {
                t = apiNow + static_cast<time_t>(mins * 60L);
            }
            if (t > 0)
            {
                out.push_back(t);
            }
        }
    };

    if (!filterDir || wantUp)
    {
        take(block["UP"].as<JsonArrayConst>());
    }
    if (!filterDir || wantDown)
    {
        take(block["DOWN"].as<JsonArrayConst>());
    }
}

std::string EtaService::lrtTerminus(std::string_view directionLabel)
{
    static constexpr std::string_view arrow{"→"};
    const size_t at{directionLabel.rfind(arrow)};
    if (at == std::string_view::npos)
    {
        return {};
    }
    std::string_view tail{directionLabel.substr(at + arrow.size())};
    while (!tail.empty() && tail.front() == ' ')
    {
        tail.remove_prefix(1U);
    }
    while (!tail.empty() && tail.back() == ' ')
    {
        tail.remove_suffix(1U);
    }
    return std::string{tail};
}

void EtaService::collectLrt(JsonObjectConst stopObj, std::vector<time_t> &out)
{
    const char *stopId{stopObj["stop_id"] | ""};
    const char *line{stopObj["line"] | ""};
    if (!stopId[0])
    {
        return;
    }
    char url[160]{};
    snprintf(url, sizeof(url), "https://rt.data.gov.hk/v1/transport/mtr/lrt/getSchedule?station_id=%s", stopId);
    std::string body{};
    if (!httpGet(url, body))
    {
        return;
    }
    JsonDocument filter{};
    filter["status"] = true;
    filter["system_time"] = true;
    filter["platform_list"][0]["platform_id"] = true;
    filter["platform_list"][0]["route_list"][0]["route_no"] = true;
    filter["platform_list"][0]["route_list"][0]["dest_ch"] = true;
    filter["platform_list"][0]["route_list"][0]["time_en"] = true;
    filter["platform_list"][0]["route_list"][0]["time_ch"] = true;
    JsonDocument doc{};
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) != DeserializationError::Ok)
    {
        return;
    }
    body.clear();
    if (!doc["status"].isNull() && doc["status"].as<int>() == 0)
    {
        Serial.println("lrt status 0");
        return;
    }
    // system_time is HKT wall clock from MTR — do not use device NTP (has been ~1h fast).
    const time_t apiNow{parseApiDateTime(doc["system_time"] | "")};
    if (apiNow > 0)
    {
        windowAnchor = apiNow;
    }
    auto parseLrtText = [&](const char *src) -> time_t {
        if (src == nullptr || src[0] == '\0' || src[0] == '-')
        {
            return 0;
        }
        int mins{-1};
        // "2 min" / "2 mins" / "2 分鐘"
        if (sscanf(src, "%d min", &mins) == 1 || sscanf(src, "%d 分鐘", &mins) == 1)
        {
            if (mins >= 0 && mins <= static_cast<int>(windowSec / 60L) && apiNow > 0)
            {
                return apiNow + static_cast<time_t>(mins * 60);
            }
        }
        if (strstr(src, "rriving") != nullptr || strstr(src, "Departing") != nullptr ||
            strstr(src, "即將") != nullptr || strstr(src, "正在") != nullptr)
        {
            if (apiNow > 0)
            {
                return apiNow + 60;
            }
        }
        return 0;
    };

    // Both directions of a line usually call at the same stop on opposite platforms.
    // direction_label is "起點 → 終點"; keep only platforms where a train to that terminus
    // is listed (also catches short-workings there). Circular 705/706 have dest
    // "天水圍循環綫" and are one-way by route number, so they always pass.
    const std::string terminus{lrtTerminus(stopObj["direction_label"] | "")};
    std::vector<int> platforms{};
    if (!terminus.empty())
    {
        for (JsonObjectConst p : doc["platform_list"].as<JsonArrayConst>())
        {
            for (JsonObjectConst r : p["route_list"].as<JsonArrayConst>())
            {
                if (strcmp(line, r["route_no"] | "") == 0 && terminus == (r["dest_ch"] | ""))
                {
                    platforms.push_back(p["platform_id"] | -1);
                    break;
                }
            }
        }
    }

    for (JsonObjectConst p : doc["platform_list"].as<JsonArrayConst>())
    {
        const int platformId{p["platform_id"] | -1};
        const bool platformOk{terminus.empty() ||
                              std::find(platforms.begin(), platforms.end(), platformId) != platforms.end()};
        for (JsonObjectConst r : p["route_list"].as<JsonArrayConst>())
        {
            const char *routeNo{r["route_no"] | ""};
            // Only the picked line — no borrowing other routes at this stop.
            if (line[0] && routeNo[0] && strcmp(line, routeNo) != 0)
            {
                continue;
            }
            if (!platformOk && strstr(r["dest_ch"] | "", "循環") == nullptr)
            {
                continue;
            }
            const char *tEn{r["time_en"] | ""};
            const char *tCh{r["time_ch"] | ""};
            const char *src{tEn[0] ? tEn : tCh};
            const time_t t{parseLrtText(src)};
            if (t > 0)
            {
                out.push_back(t);
            }
        }
    }
}

void EtaService::collectMtrBus(JsonObjectConst stopObj, std::vector<time_t> &out)
{
    const char *routeName{stopObj["routeName"] | ""};
    const char *stationId{stopObj["station_id"] | ""};
    if (!routeName[0] || !stationId[0])
    {
        return;
    }

    // Schedule JSON is ~80KB. Stream busStop objects one-by-one (~1KB each);
    // busStopId comes AFTER the bus[] array so a small lookbehind window is not enough.
    char needle[64]{};
    snprintf(needle, sizeof(needle), "\"busStopId\":\"%s\"", stationId);
    static constexpr char kArrayMarker[]{"\"busStop\":["};

    char req[96]{};
    snprintf(req, sizeof(req), "{\"language\":\"zh\",\"routeName\":\"%s\"}", routeName);

    const uint32_t gen{fetchGen};
    inHttp = true;
    HTTPClient http{};
    http.setInsecure();
    http.setTimeout(10000);
    http.setUserAgent("Kortlinje-ETA/1.0");
    HangTrace::feed();
    HangTrace::mark(HangTrace::Stage::HttpBegin);
    if (!http.begin("https://rt.data.gov.hk/v1/transport/mtr/bus/getSchedule"))
    {
        inHttp = false;
        finishDeferredFs();
        return;
    }
    http.addHeader("Content-Type", "application/json");
    HangTrace::mark(HangTrace::Stage::HttpRequest);
    const int code{http.POST(req)};
    HangTrace::feed();
    if (fetchCancelled(gen) || code != HTTP_CODE_OK)
    {
        if (code != HTTP_CODE_OK && !fetchCancelled(gen))
        {
            Serial.printf("mtrbus POST %d\n", code);
        }
        HangTrace::mark(HangTrace::Stage::HttpEnd);
        http.end();
        inHttp = false;
        finishDeferredFs();
        return;
    }
    lastHttpOk = true;

    WiFiClient *stream{http.getStreamPtr()};
    constexpr size_t kSliceMax{8'000U};
    std::string markerWin{};
    markerWin.reserve(24U);
    std::string slice{};
    enum class Phase : uint8_t
    {
        FindArray,
        SkipToObj,
        CaptureObj
    };
    Phase phase{Phase::FindArray};
    int depth{0};
    bool found{false};
    const unsigned long started{millis()};
    uint8_t buf[256]{};

    HangTrace::mark(HangTrace::Stage::HttpRead);
    while (http.connected() && !fetchCancelled(gen) && (millis() - started) < 15'000UL)
    {
        HangTrace::feed();
        const size_t avail{stream != nullptr ? stream->available() : 0U};
        if (avail == 0U)
        {
            delay(1);
            continue;
        }
        const size_t got{stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail)};
        if (got == 0U)
        {
            break;
        }
        for (size_t i{0U}; i < got; ++i)
        {
            const char c{static_cast<char>(buf[i])};
            if (phase == Phase::FindArray)
            {
                markerWin.push_back(c);
                if (markerWin.size() > sizeof(kArrayMarker) - 1U)
                {
                    markerWin.erase(0U, markerWin.size() - (sizeof(kArrayMarker) - 1U));
                }
                if (markerWin == kArrayMarker)
                {
                    phase = Phase::SkipToObj;
                    markerWin.clear();
                }
                continue;
            }
            if (phase == Phase::SkipToObj)
            {
                if (c == '{')
                {
                    slice.assign(1U, '{');
                    depth = 1;
                    phase = Phase::CaptureObj;
                }
                else if (c == ']')
                {
                    // end of busStop array
                    goto stream_done;
                }
                continue;
            }
            // CaptureObj
            slice.push_back(c);
            if (c == '{')
            {
                ++depth;
            }
            else if (c == '}')
            {
                --depth;
                if (depth == 0)
                {
                    if (slice.find(needle) != std::string::npos)
                    {
                        found = true;
                        goto stream_done;
                    }
                    slice.clear();
                    phase = Phase::SkipToObj;
                }
            }
            if (slice.size() > kSliceMax)
            {
                Serial.println("mtrbus slice overflow");
                slice.clear();
                phase = Phase::SkipToObj;
                depth = 0;
            }
        }
    }
stream_done:
    drainHttpStream(http, stream, buf, sizeof(buf), 2'500UL);
    HangTrace::mark(HangTrace::Stage::HttpEnd);
    http.end();
    inHttp = false;
    finishDeferredFs();

    if (!found || slice.empty())
    {
        Serial.printf("mtrbus stop miss %s\n", stationId);
        return;
    }

    JsonDocument doc{};
    if (deserializeJson(doc, slice) != DeserializationError::Ok)
    {
        Serial.println("mtrbus json fail");
        return;
    }
    const time_t now{time(nullptr)};
    auto readSec = [](JsonVariantConst v) -> long {
        if (v.isNull())
        {
            return -1L;
        }
        if (v.is<long>() || v.is<unsigned long>() || v.is<int>())
        {
            return v.as<long>();
        }
        if (v.is<const char *>())
        {
            const char *raw{v.as<const char *>()};
            if (raw != nullptr && raw[0] != '\0')
            {
                return strtol(raw, nullptr, 10);
            }
        }
        return -1L;
    };
    // Termini often set arrivalTimeInSecond=108000; real ETA is departureTimeInSecond.
    constexpr long kSentinelSec{86'400L};
    for (JsonObjectConst b : doc["bus"].as<JsonArrayConst>())
    {
        const long dep{readSec(b["departureTimeInSecond"])};
        const long arr{readSec(b["arrivalTimeInSecond"])};
        long sec{-1L};
        if (dep >= 0L && dep < kSentinelSec)
        {
            sec = dep;
        }
        else if (arr >= 0L && arr < kSentinelSec)
        {
            sec = arr;
        }
        if (sec >= 0L)
        {
            out.push_back(now + static_cast<time_t>(sec));
        }
    }
}

void EtaService::collectGmb(JsonObjectConst stopObj, std::vector<time_t> &out)
{
    const char *routeId{stopObj["route_id"] | ""};
    // route_id may be number in JSON
    char routeIdBuf[24]{};
    if (!routeId[0] && stopObj["route_id"].is<long>())
    {
        snprintf(routeIdBuf, sizeof(routeIdBuf), "%ld", stopObj["route_id"].as<long>());
        routeId = routeIdBuf;
    }
    const int routeSeq{stopObj["route_seq"] | 0};
    const int seq{stopObj["seq"] | 0};
    if (!routeId[0] || routeSeq <= 0 || seq <= 0)
    {
        return;
    }
    char url[160]{};
    snprintf(url, sizeof(url), "https://data.etagmb.gov.hk/eta/route-stop/%s/%d/%d", routeId, routeSeq, seq);
    std::string body{};
    if (!httpGet(url, body))
    {
        return;
    }
    JsonDocument filter{};
    filter["data"]["eta"][0]["timestamp"] = true;
    filter["data"]["eta"][0]["eta"] = true;
    filter["data"]["eta"][0]["time"] = true;
    JsonDocument doc{};
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) != DeserializationError::Ok)
    {
        return;
    }
    for (JsonObjectConst e : doc["data"]["eta"].as<JsonArrayConst>())
    {
        time_t t{parseEtaValue(e["timestamp"])};
        if (t <= 0)
        {
            t = parseEtaValue(e["eta"]);
        }
        if (t <= 0)
        {
            t = parseEtaValue(e["time"]);
        }
        if (t > 0)
        {
            out.push_back(t);
        }
    }
}

void EtaService::collectTimes(JsonObjectConst stopObj, std::vector<time_t> &out)
{
    const char *op{stopObj["operator"] | ""};
    if (strcmp(op, "kmb") == 0)
    {
        collectKmb(stopObj, out);
    }
    else if (strcmp(op, "ctb") == 0)
    {
        collectCtb(stopObj, out);
    }
    else if (strcmp(op, "mtr") == 0)
    {
        collectMtr(stopObj, out);
    }
    else if (strcmp(op, "lrt") == 0)
    {
        collectLrt(stopObj, out);
    }
    else if (strcmp(op, "mtrbus") == 0)
    {
        collectMtrBus(stopObj, out);
    }
    else if (strcmp(op, "gmb") == 0)
    {
        collectGmb(stopObj, out);
    }
}

bool EtaService::refresh(std::string &outMessage)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        outMessage = "No WiFi";
        return false;
    }
    if (stop.empty())
    {
        outMessage = "No stop";
        return false;
    }
#ifdef TIME_ZONE_POSIX
    setenv("TZ", TIME_ZONE_POSIX, 1);
    tzset();
#endif
    windowAnchor = 0;
    const uint32_t gen{fetchGen};
    JsonDocument doc{};
    if (deserializeJson(doc, stop) != DeserializationError::Ok || !doc.is<JsonObject>())
    {
        outMessage = "Bad stop";
        return false;
    }
    JsonObjectConst stopObj{doc.as<JsonObjectConst>()};
    const std::string route{routeLabel(stopObj)};
    std::vector<time_t> times{};
    times.reserve(8U);
    lastHttpOk = false;
    collectTimes(stopObj, times);
    HangTrace::mark(HangTrace::Stage::EtaParse);
    if (fetchCancelled(gen))
    {
        outMessage = "No stop";
        return false;
    }
    if (times.empty() && !lastHttpOk)
    {
        // HTTP failed — not "no service". Keep the previous cache; it expires on its own.
        outMessage = route + " ?";
        Serial.println("eta http fail");
        return false;
    }
    std::sort(times.begin(), times.end());
    cachedRoute = route;
    cachedTimes = std::move(times);
    // windowAnchor (API clock) preferred — device NTP has been observed ~1h fast, and a
    // fetch can land before NTP sync (device clock still 1970).
    cachedAnchor = windowAnchor;
    cachedOkMs = millis();
    cachedGen = gen;
    hasCache = true;
    render(outMessage);
    Serial.printf("eta ok %s\n", outMessage.c_str());
    return true;
}

time_t EtaService::referenceNow()
{
    if (cachedAnchor > 0)
    {
        return cachedAnchor + static_cast<time_t>((millis() - cachedOkMs) / 1000UL);
    }
    const time_t now{time(nullptr)};
    return now >= 1'700'000'000L ? now : 0;
}

time_t EtaService::clockMinute() { return referenceNow() / 60; }

bool EtaService::render(std::string &outMessage)
{
    if (!hasCache || cachedGen != fetchGen || stop.empty())
    {
        return false;
    }
    if (millis() - cachedOkMs > staleMs)
    {
        outMessage = cachedRoute + " ?";
        return true;
    }
    std::vector<time_t> times{cachedTimes};
    const time_t ref{referenceNow()};
    if (ref > 0)
    {
        // Drop once its HH:MM has passed — a 09:01:40 bus must not still read 09:01 at 09:02.
        times.erase(std::remove_if(times.begin(),
                                   times.end(),
                                   [ref](time_t t) {
                                       return t <= 0 || t / 60 < ref / 60 ||
                                              static_cast<long>(t - ref) > windowSec;
                                   }),
                    times.end());
    }
    else if (times.size() > 4U)
    {
        // NTP not ready and no API anchor — show soonest few absolute times.
        times.resize(4U);
    }
    outMessage = formatTicker(cachedRoute, times);
    return true;
}
