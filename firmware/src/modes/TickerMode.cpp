#include "modes/TickerMode.h"

#include "fonts/KortlinjeFont.h"
#include "modes/ClockMode.h"
#include "services/DeviceService.h"
#include "services/DisplayService.h"
#include "services/EtaService.h"
#include "services/FontsService.h"
#include "services/HangTrace.h"
#include "services/IrqSafeLock.h"
#include "services/ModesService.h"

#include <WiFi.h>
#include <algorithm>
#include <string>

namespace
{
IrqSafeLock messageLock{};
} // namespace

TickerMode::~TickerMode() { stopTimer(); }

void TickerMode::storeMessage(std::string_view value)
{
    messageLock.lock();
    message.assign(value.data(), value.size());
    messageLock.unlock();
}

std::string TickerMode::loadMessage()
{
    messageLock.lock();
    std::string copy{message};
    messageLock.unlock();
    return copy;
}

void TickerMode::begin()
{
    if (!font)
    {
        setFont(KortlinjeFont::name);
    }
    if (loadMessage().empty())
    {
        storeMessage(EtaService::hasStop() ? "ETA" : "No stop");
    }
    loopsDone = 0U;
    DisplayService::setPower(true);
    pending = true;
    Serial.printf("ticker begin [%s]\n", loadMessage().c_str());
}

void TickerMode::prepareScroll()
{
    const std::string message{loadMessage()};
    text = std::make_unique<TextHandler>(message, *font);
    width = static_cast<int16_t>(text->getWidth());
    offsetY = static_cast<int8_t>((GRID_ROWS - text->getHeight()) / 2U);

    const size_t split{message.find(' ')};
    const std::string firstGroup{split == std::string::npos ? message : message.substr(0U, split)};
    const TextHandler firstText{firstGroup, *font};
    const int firstWidth{static_cast<int>(firstText.getWidth())};
    // If first token wider than panel, start at 0 — centering hides the left (e.g. "Updating" → "pdating").
    if (firstWidth > static_cast<int>(GRID_COLUMNS))
    {
        startOffsetX = 0;
    }
    else
    {
        startOffsetX = static_cast<int16_t>((static_cast<int>(GRID_COLUMNS) - firstWidth) / 2);
    }
    offsetX = startOffsetX;
    holdTicks = holdTicksOnStart;
}

void TickerMode::handle()
{
    if (pending)
    {
        prepareScroll();
        transmit();
        pending = false;
        if (!text || width <= 0)
        {
            // Nothing drawable — bounce back so CYCLE never looks "dead".
            Serial.println("ticker empty frame -> clock");
            ModesService::requestMode(ClockMode::name);
            return;
        }
        startTimer();
        DisplayService::fillFrame(false);
        text->draw(offsetX, offsetY);
        return;
    }

    if (!tick || !text)
    {
        return;
    }
    tick = false;

    if (holdTicks > 0U)
    {
        --holdTicks;
        DisplayService::fillFrame(false);
        text->draw(offsetX, offsetY);
        return;
    }

    offsetX = static_cast<int16_t>(offsetX - scrollStep);
    if (width + offsetX < 0)
    {
        if (++loopsDone >= scrollLoops)
        {
            // No stop = config idle on LED — stay in Ticker so WebUI picker is not kicked out.
            if (!EtaService::hasStop())
            {
                loopsDone = 0U;
                offsetX = startOffsetX;
                holdTicks = holdTicksOnStart;
                return;
            }
            Serial.println("ticker done");
            ModesService::requestMode(ClockMode::name);
            return;
        }
        offsetX = startOffsetX;
        holdTicks = holdTicksOnStart;
    }

    DisplayService::fillFrame(false);
    text->draw(offsetX, offsetY);
}

void TickerMode::end()
{
    stopTimer();
    text.reset();
}

bool TickerMode::applyStopJson(std::string_view json)
{
    const bool sameStop{!json.empty() && json == EtaService::stopJson()};
    if (!EtaService::setStopJson(json))
    {
        return false;
    }
    // Do not HTTP here — would freeze WebSocket/core0. pollCache pulls ASAP.
    if (json.empty())
    {
        cacheReady = true;
        fetchRequested = false;
        lastFetchAttemptMs = 0UL;
        lastFetchMs = 0UL;
        storeMessage("No stop");
        pending = true;
        // Leave Ticker immediately — avoid scroll/timer path after clear.
        ModesService::requestMode(ClockMode::name);
        JsonDocument doc{};
        doc["message"].set("No stop");
        doc["stop"].to<JsonObject>();
        DeviceService::transmit(doc.as<JsonObjectConst>(), name);
        return true;
    }
    // WS reconnect / WebUI re-send of the same stop must not cancel an in-flight fetch
    // or force a back-to-back HTTPS.
    if (sameStop && (EtaService::httpActive() || fetchRequested || cacheReady))
    {
        return true;
    }
    cacheReady = false;
    fetchRequested = true;
    lastFetchAttemptMs = 0UL;
    lastFetchMs = 0UL;
    storeMessage("...");
    pending = true;
    JsonDocument doc{};
    doc["message"].set("...");
    JsonDocument stopDoc{};
    if (deserializeJson(stopDoc, EtaService::stopJson()) == DeserializationError::Ok)
    {
        doc["stop"].set(stopDoc.as<JsonObjectConst>());
    }
    DeviceService::transmit(doc.as<JsonObjectConst>(), name);
    return true;
}

void TickerMode::refreshCache(bool force)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("eta no wifi");
        if (!cacheReady)
        {
            storeMessage("No WiFi");
            pending = true;
        }
        return;
    }

    if (EtaService::stopJson().empty())
    {
        storeMessage("No stop");
        cacheReady = true;
        fetchRequested = false;
        return;
    }

    const unsigned long now{millis()};
    if (!force && cacheReady && (now - lastFetchMs) < EtaService::refreshMs)
    {
        return;
    }
    // Always throttle HTTP attempts (even force) — applyStopJson zeroes lastFetchAttemptMs
    // so Confirm still pulls on the next poll without retry-hammering every loop.
    static_cast<void>(force);
    if (lastFetchAttemptMs != 0UL && (now - lastFetchAttemptMs) < fetchRetryMs)
    {
        return;
    }

    lastFetchAttemptMs = now;
    fetchRequested = false;
    std::string text{};
    if (EtaService::refresh(text))
    {
        storeMessage(text);
        cacheReady = true;
        lastFetchMs = now;
        pending = true;
        Serial.print("eta text ");
        Serial.println(text.c_str());
    }
    else
    {
        Serial.print("eta fail ");
        Serial.println(text.c_str());
        if (fetchRequested)
        {
            // Stop changed mid-fetch — leave "..." up; pollCache fetches the new stop.
            return;
        }
        // Keep showing the last good times (they expire per minute); else the fail text.
        std::string cached{};
        if (EtaService::render(cached))
        {
            text = std::move(cached);
        }
        else if (text.empty())
        {
            text = "ETA fail";
        }
        if (text != loadMessage())
        {
            storeMessage(text);
            pending = true;
        }
        cacheReady = true;
        // Retry after failRetryMs instead of the full refreshMs.
        lastFetchMs = now - (EtaService::refreshMs - failRetryMs);
    }
}

void TickerMode::rerenderOnMinute()
{
    static time_t lastMinute{0};
    const time_t minute{EtaService::clockMinute()};
    if (minute == lastMinute)
    {
        return;
    }
    lastMinute = minute;
    if (!cacheReady || fetchRequested || EtaService::httpActive())
    {
        return;
    }
    HangTrace::mark(HangTrace::Stage::EtaRerender);
    std::string text{};
    if (EtaService::render(text) && text != loadMessage())
    {
        storeMessage(text);
        pending = true;
        Serial.printf("eta rerender %s\n", text.c_str());
    }
}

void TickerMode::pollCache()
{
    rerenderOnMinute();
    if (WiFi.status() != WL_CONNECTED)
    {
        return;
    }
    // Never start/stack another operator HTTP while one is open.
    if (EtaService::httpActive())
    {
        return;
    }
    if (fetchRequested)
    {
        refreshCache(true);
        return;
    }
    if (!cacheReady)
    {
        if (millis() < bootFetchDelayMs)
        {
            return;
        }
        refreshCache(true);
        return;
    }
    refreshCache(false);
}

void TickerMode::onReceive(JsonObjectConst payload, std::string_view source)
{
    static_cast<void>(source);
    if (payload["font"].is<std::string_view>())
    {
        setFont(payload["font"].as<std::string_view>());
    }
    if (payload["clear"].is<bool>() && payload["clear"].as<bool>())
    {
        applyStopJson("");
    }
    else if (payload["stop"].isNull())
    {
        applyStopJson("");
    }
    else if (payload["stop"].is<JsonObjectConst>())
    {
        JsonObjectConst stopObj{payload["stop"].as<JsonObjectConst>()};
        if (stopObj.size() == 0U)
        {
            applyStopJson("");
        }
        else
        {
            std::string json{};
            serializeJson(stopObj, json);
            applyStopJson(json);
        }
    }
    else if (payload["stop"].is<std::string_view>())
    {
        applyStopJson(payload["stop"].as<std::string_view>());
    }
    if (payload["message"].is<std::string_view>())
    {
        setMessage(payload["message"].as<std::string_view>());
    }
}

bool __time_critical_func(TickerMode::onTimer)(repeating_timer *rt)
{
    static_cast<void>(rt);
    tick = true;
    return true;
}

void TickerMode::setFont(std::string_view fontName)
{
    if (std::ranges::find(FontsService::names, fontName) != FontsService::names.end())
    {
        font = FontsService::get(fontName);
        pending = true;
    }
}

void TickerMode::setMessage(std::string_view _message)
{
    if (_message.length())
    {
        storeMessage(_message);
        pending = true;
    }
}

void TickerMode::startTimer()
{
    if (!timerRunning)
    {
        timerRunning = add_repeating_timer_ms(tickMs, &onTimer, nullptr, &timer);
    }
}

void TickerMode::stopTimer()
{
    if (timerRunning)
    {
        cancel_repeating_timer(&timer);
        timerRunning = false;
        tick = false;
    }
}

void TickerMode::transmit()
{
    JsonDocument doc{};
    doc["font"].set(font->name);
    doc["message"].set(loadMessage());
    const std::string stopJson{EtaService::stopJsonCopy()};
    if (!stopJson.empty())
    {
        JsonDocument stopDoc{};
        if (deserializeJson(stopDoc, stopJson) == DeserializationError::Ok)
        {
            doc["stop"].set(stopDoc.as<JsonObjectConst>());
        }
    }
    DeviceService::transmit(doc.as<JsonObjectConst>(), name);
}
