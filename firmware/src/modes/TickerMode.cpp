#include "modes/TickerMode.h"

#include "config/secrets.h"
#include "fonts/KortlinjeFont.h"
#include "modes/ClockMode.h"
#include "services/DeviceService.h"
#include "services/DisplayService.h"
#include "services/FontsService.h"
#include "services/ModesService.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <algorithm>
#include <string>

TickerMode::~TickerMode() { stopTimer(); }

void TickerMode::begin()
{
    if (!font)
    {
        setFont(KortlinjeFont::name);
    }
    loopsDone = 0U;
    DisplayService::setPower(true);
    pending = true;
}

void TickerMode::prepareScroll()
{
    text = std::make_unique<TextHandler>(message, *font);
    width = static_cast<int16_t>(text->getWidth());
    offsetY = static_cast<int8_t>((GRID_ROWS - text->getHeight()) / 2U);

    const size_t split{message.find(' ')};
    const std::string firstGroup{split == std::string::npos ? message : message.substr(0U, split)};
    const TextHandler firstText{firstGroup, *font};
    const int firstWidth{static_cast<int>(firstText.getWidth())};
    startOffsetX = static_cast<int16_t>((static_cast<int>(GRID_COLUMNS) - firstWidth) / 2);
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
        (text && width > 0) ? startTimer() : stopTimer();
        if (text && width > 0)
        {
            DisplayService::fillFrame(false);
            text->draw(offsetX, offsetY);
        }
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

void TickerMode::refreshCache(bool force)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("eta no wifi");
        if (!cacheReady)
        {
            message = "No WiFi";
        }
        return;
    }

    const unsigned long now{millis()};
    if (!force && cacheReady && (now - lastFetchMs) < refreshMs)
    {
        return;
    }

    HTTPClient http{};
    http.setInsecure();
    http.setTimeout(10000);
    if (!http.begin(ETA_URL))
    {
        Serial.println("eta begin fail");
        if (!cacheReady)
        {
            message = "ETA fail";
        }
        return;
    }

    const int code{http.GET()};
    Serial.print("eta ");
    Serial.println(code);
    if (code == HTTP_CODE_OK)
    {
        String body{http.getString()};
        body.trim();
        if (body.length() > 0)
        {
            message = body.c_str();
            cacheReady = true;
            lastFetchMs = now;
            Serial.print("eta text ");
            Serial.println(message.c_str());
        }
        else if (!cacheReady)
        {
            message = "ETA empty";
        }
    }
    else if (!cacheReady)
    {
        message = "ETA ";
        message += std::to_string(code);
    }
    http.end();
}

void TickerMode::pollCache()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        return;
    }
    if (!cacheReady)
    {
        refreshCache(true);
        return;
    }
    refreshCache(false);
}

void TickerMode::onReceive(JsonObjectConst payload, std::string_view source)
{
    if (payload["font"].is<std::string_view>())
    {
        setFont(payload["font"].as<std::string_view>());
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
        message = _message;
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
    doc["message"].set(message);
    DeviceService::transmit(doc.as<JsonObjectConst>(), name);
}
