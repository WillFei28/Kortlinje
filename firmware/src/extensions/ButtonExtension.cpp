#include "extensions/ButtonExtension.h"

#include "config/secrets.h"
#include "extensions/AlarmExtension.h"
#include "extensions/ProvisioningExtension.h"
#include "services/DisplayService.h"
#include "services/ModesService.h"
#include "services/WebServerService.h"

#include <ArduinoJson.h>
#include <cstdio>
#include <hardware/adc.h>
#include <hardware/irq.h>

void ButtonExtension::configure()
{
    analogReadResolution(static_cast<int>(ADC_RESOLUTION));
    dma_channel_claim(channel);
}

void ButtonExtension::begin()
{
    WebServerService::http.on("/buttons/live", AsyncWebRequestMethod::AsyncWebRequestMethodType::HTTP_GET, &onGetLive);
    WebServerService::http.on("/buttons/log", AsyncWebRequestMethod::AsyncWebRequestMethodType::HTTP_GET, &onGetLog);
    WebServerService::http.on("/buttons", AsyncWebRequestMethod::AsyncWebRequestMethodType::HTTP_GET, &onGetPage);

    adc_init();
    adc_gpio_init(PIN_REAR);
    adc_gpio_init(PIN_TOP);
    adc_select_input(PIN_REAR - __FIRSTANALOGGPIO);
    adc_fifo_setup(true, true, 1U, false, false);
    adc_set_clkdiv(divider);
    dma_channel_config_t config{dma_channel_get_default_config(channel)};
    channel_config_set_read_increment(&config, false);
    channel_config_set_write_increment(&config, false);
    channel_config_set_dreq(&config, dreq_num_rp2350::DREQ_ADC);
    channel_config_set_transfer_data_size(&config, dma_channel_transfer_size::DMA_SIZE_16);
    irq_set_exclusive_handler(irq_num_rp2350::DMA_IRQ_1, &onInterrupt);
    irq_set_enabled(irq_num_rp2350::DMA_IRQ_1, true);
    dma_channel_set_irq1_enabled(channel, true);
    dma_channel_configure(channel, &config, &raw, &adc_hw->fifo, count, true);
    adc_run(true);
}

void ButtonExtension::handle()
{
    // SET: short press on release → AM/PM; hold 3s → SoftAP provisioning
    static unsigned long setHeldMs{0UL};
    static bool setLongHandled{false};
    if (rear.state == Button::SET && rear.pressed)
    {
        if (setHeldMs == 0UL)
        {
            setHeldMs = millis();
            setLongHandled = false;
        }
        else if (!setLongHandled && millis() - setHeldMs >= 3000UL)
        {
            setLongHandled = true;
            ProvisioningExtension::toggle();
        }
    }
    else if (setHeldMs != 0UL)
    {
        if (!setLongHandled)
        {
            if (ProvisioningExtension::isActive())
            {
                ProvisioningExtension::exit();
            }
            else
            {
                config();
            }
        }
        setHeldMs = 0UL;
        setLongHandled = false;
    }

    if (rear.ready)
    {
        rear.ready = false;
        if (rear.state != Button::SET)
        {
            process(rear.state);
        }
    }
    else if (top.ready)
    {
        top.ready = false;
        process(top.state);
    }

    // Log big REAR ADC drops even when ladder miss — distinguishes wiring vs software.
    static uint32_t lastRearLogMs{0UL};
    static uint16_t lastRearLogged{0xFFFFU};
    const uint32_t nowMs{millis()};
    const uint16_t rearRaw{rear.lastRaw};
    if (rear.idle > 0U && rearRaw + 120U < rear.idle && (nowMs - lastRearLogMs) >= 200UL)
    {
        const uint16_t delta{static_cast<uint16_t>(lastRearLogged > rearRaw ? lastRearLogged - rearRaw : rearRaw - lastRearLogged)};
        if (lastRearLogged == 0xFFFFU || delta >= 40U)
        {
            lastRearLogMs = nowMs;
            lastRearLogged = rearRaw;
            char tag[28];
            std::snprintf(tag, sizeof(tag), "rear-drop-%u", static_cast<unsigned>(rearRaw));
            appendLog(tag);
        }
    }
    else if (rearRaw + 40U >= rear.idle)
    {
        lastRearLogged = rearRaw;
    }
}

void ButtonExtension::enable()
{
    enabled = true;
    adc_select_input(active->pin - __FIRSTANALOGGPIO);
    adc_fifo_setup(true, true, 1U, false, false);
    adc_fifo_drain();
    adc_set_clkdiv(divider);
    dma_channel_set_trans_count(channel, count, true);
    adc_run(true);
}

bool ButtonExtension::isEnabled() { return enabled; }

void ButtonExtension::appendLog(const char *event)
{
    LogLine &line{logBuf[logHead]};
    line.seq = logNextSeq++;
    std::snprintf(line.text,
                  sizeof(line.text),
                  "#%lu %s rear raw=%u idle=%u h=%u m=%u l=%u mg=%u | top raw=%u idle=%u",
                  static_cast<unsigned long>(line.seq),
                  event != nullptr ? event : "-",
                  static_cast<unsigned>(rear.lastRaw),
                  static_cast<unsigned>(rear.idle),
                  static_cast<unsigned>(rear.high),
                  static_cast<unsigned>(rear.medium),
                  static_cast<unsigned>(rear.low),
                  static_cast<unsigned>(rear.margin),
                  static_cast<unsigned>(top.lastRaw),
                  static_cast<unsigned>(top.idle));
    logHead = static_cast<uint8_t>((logHead + 1U) % logCap);
    if (logCount < logCap)
    {
        ++logCount;
    }
}

void ButtonExtension::onGetPage(AsyncWebServerRequest *request)
{
    static constexpr char page[] =
        "<!doctype html><meta charset=utf-8><title>Buttons</title>"
        "<style>body{font:14px/1.35 ui-monospace,Menlo,monospace;margin:12px;background:#111;color:#ddd}"
        "#live{white-space:pre;padding:8px;background:#1b1b1b;border:1px solid #333;margin-bottom:8px}"
        "#log{white-space:pre;height:70vh;overflow:auto;padding:8px;background:#000;border:1px solid #333;"
        "user-select:text}button{font:inherit;margin:0 6px 8px 0;padding:4px 10px}</style>"
        "<div id=live>loading…</div>"
        "<button id=pauseBtn onclick=\"paused=!paused;pauseBtn.textContent=paused?'resume':'pause'\">pause</button>"
        "<button onclick=\"navigator.clipboard.writeText(log.textContent)\">copy log</button>"
        "<div>TOP: high=CYCLE med=SLEEP/BRIGHT low=DISMISS zero=SNOOZE · REAR: high=DOWN med=UP low=ALARM "
        "zero=SET</div>"
        "<pre id=log></pre>"
        "<script>"
        "let after=0,paused=false;"
        "const live=document.getElementById('live');"
        "const log=document.getElementById('log');"
        "const pauseBtn=document.getElementById('pauseBtn');"
        "async function tick(){"
        "  if(paused) return;"
        "  try{"
        "    const L=await (await fetch('/buttons/live')).json();"
        "    live.textContent="
        "      'LIVE rear raw='+L.rear.raw+' idle='+L.rear.idle+' high='+L.rear.high+"
        "      ' med='+L.rear.med+' low='+L.rear.low+' margin='+L.rear.margin+"
        "      '\\nLIVE top  raw='+L.top.raw+' idle='+L.top.idle+' high='+L.top.high+"
        "      ' med='+L.top.med+' low='+L.top.low+' margin='+L.top.margin;"
        "    const t=await (await fetch('/buttons/log?after='+after)).text();"
        "    if(t){"
        "      for(const row of t.trim().split('\\n')){"
        "        const i=row.indexOf(' '); if(i<0) continue;"
        "        after=parseInt(row.slice(0,i),10)||after;"
        "        log.textContent+=row.slice(i+1)+'\\n';"
        "      }"
        "      log.scrollTop=log.scrollHeight;"
        "    }"
        "  }catch(e){}"
        "}"
        "setInterval(tick,500);tick();"
        "</script>";
    request->send(200, "text/html", page);
}

void ButtonExtension::onGetLive(AsyncWebServerRequest *request)
{
    JsonDocument doc{};
    JsonObject rearObj{doc["rear"].to<JsonObject>()};
    rearObj["raw"].set(rear.lastRaw);
    rearObj["idle"].set(rear.idle);
    rearObj["high"].set(rear.high);
    rearObj["med"].set(rear.medium);
    rearObj["low"].set(rear.low);
    rearObj["margin"].set(rear.margin);
    JsonObject topObj{doc["top"].to<JsonObject>()};
    topObj["raw"].set(top.lastRaw);
    topObj["idle"].set(top.idle);
    topObj["high"].set(top.high);
    topObj["med"].set(top.medium);
    topObj["low"].set(top.low);
    topObj["margin"].set(top.margin);
    AsyncResponseStream *response{request->beginResponseStream("application/json")};
    serializeJson(doc, *response);
    request->send(response);
}

void ButtonExtension::onGetLog(AsyncWebServerRequest *request)
{
    uint32_t after{0UL};
    if (request->hasParam("after"))
    {
        after = static_cast<uint32_t>(request->getParam("after")->value().toInt());
    }
    AsyncResponseStream *response{request->beginResponseStream("text/plain")};
    const uint8_t start{static_cast<uint8_t>((logHead + logCap - logCount) % logCap)};
    for (uint8_t i{0U}; i < logCount; ++i)
    {
        const LogLine &line{logBuf[(start + i) % logCap]};
        if (line.seq > after)
        {
            response->printf("%lu %s\n", static_cast<unsigned long>(line.seq), line.text);
        }
    }
    request->send(response);
}

void ButtonExtension::process(Button button)
{
    switch (button)
    {
    case Button::ALARM:
        Serial.println("button alarm");
        appendLog("alarm");
        alarm();
        break;
    case Button::BRIGHTNESS:
        Serial.println("button brightness/sleep");
        appendLog("sleep");
        brightness();
        break;
    case Button::SET:
        Serial.println("button set");
        appendLog("set");
        config();
        break;
    case Button::CYCLE:
        Serial.println("button cycle");
        appendLog("cycle");
        cycle();
        break;
    case Button::DISMISS:
        Serial.println("button dismiss");
        appendLog("dismiss");
        dismiss();
        break;
    case Button::DOWN:
        Serial.println("button down");
        appendLog("down");
        minus();
        break;
    case Button::UP:
        Serial.println("button up");
        appendLog("up");
        plus();
        break;
    case Button::SNOOZE:
        Serial.println("button snooze");
        appendLog("snooze");
        snooze();
        break;
    default:
        break;
    }
}

void __time_critical_func(ButtonExtension::onInterrupt)()
{
    dma_channel_acknowledge_irq1(channel);
    Ladder &ladder{*active};
    const Button _state{ladder.state};
    const uint16_t _raw{raw};
    ladder.lastRaw = _raw;
    if (_raw >= ladder.idle - ladder.margin && _raw <= ladder.idle)
    {
        ladder.state = Button::NONE;
        if (ladder.state == _state)
        {
            ladder.pressed = false;
        }
    }
    else if (_raw >= ladder.high - ladder.margin && _raw <= ladder.high + ladder.margin)
    {
        ladder.state = ladder._high;
        if (!ladder.pressed && ladder.state == _state)
        {
            ladder.pressed = true;
            ladder.ready = true;
        }
    }
    else if (_raw >= ladder.medium - ladder.margin && _raw <= ladder.medium + ladder.margin)
    {
        ladder.state = ladder._medium;
        if (!ladder.pressed && ladder.state == _state)
        {
            ladder.pressed = true;
            ladder.ready = true;
        }
    }
    else if (_raw >= ladder.low - ladder.margin && _raw <= ladder.low + ladder.margin)
    {
        ladder.state = ladder._low;
        if (!ladder.pressed && ladder.state == _state)
        {
            ladder.pressed = true;
            ladder.ready = true;
        }
    }
    else if (_raw <= ladder.margin)
    {
        ladder.state = ladder._zero;
        if (!ladder.pressed && ladder.state == _state)
        {
            ladder.pressed = true;
            ladder.ready = true;
        }
    }
    else
    {
        ladder.state = Button::UNKNOWN;
        if (_raw > ladder.idle)
        {
            ladder.idle = _raw;
            ladder.high = ladder.idle * 3U / 4U;
            ladder.medium = ladder.idle * 2U / 3U;
            ladder.low = ladder.idle / 2U;
            const uint8_t computed{static_cast<uint8_t>((ladder.high - ladder.medium) / 3U)};
            const uint8_t floor{static_cast<uint8_t>(ladder.idle / 16U)};
            ladder.margin = computed > floor ? computed : floor;
            if (ladder.margin < 8U)
            {
                ladder.margin = 8U;
            }
        }
    }
    active = (active == &rear) ? &top : &rear;
    if (!rear.pressed && !top.pressed && rear.state == Button::NONE && top.state == Button::NONE &&
        ++bursts == 0b1U << 13U)
    {
        adc_run(false);
        enabled = false;
    }
    else
    {
        adc_select_input(active->pin - __FIRSTANALOGGPIO);
        dma_channel_set_trans_count(channel, count, true);
    }
}

void ButtonExtension::alarm()
{
    const bool alarm1{AlarmExtension::getAlarm1()};
    const bool alarm2{AlarmExtension::getAlarm2()};
    if (alarm1 && alarm2)
    {
        AlarmExtension::setAlarm1(false);
        AlarmExtension::setAlarm2(false);
    }
    else if (alarm1)
    {
        AlarmExtension::setAlarm1(false);
        AlarmExtension::setAlarm2(true);
    }
    else if (alarm2)
    {
        AlarmExtension::setAlarm1(true);
    }
    else
    {
        AlarmExtension::setAlarm1(true);
    }
}

void ButtonExtension::brightness() { DisplayService::setPower(!DisplayService::getPower()); }

void ButtonExtension::config()
{
    const bool _am{DisplayService::getPixel(DisplayService::Symbol::AM)};
    const bool _pm{DisplayService::getPixel(DisplayService::Symbol::PM)};
    if (_am && _pm)
    {
        DisplayService::setPixel(DisplayService::Symbol::AM, false);
        DisplayService::setPixel(DisplayService::Symbol::PM, false);
    }
    else if (_am)
    {
        DisplayService::setPixel(DisplayService::Symbol::AM, false);
        DisplayService::setPixel(DisplayService::Symbol::PM, true);
    }
    else if (_pm)
    {
        DisplayService::setPixel(DisplayService::Symbol::AM, true);
    }
    else
    {
        DisplayService::setPixel(DisplayService::Symbol::AM, true);
    }
}

void ButtonExtension::cycle()
{
    // Local: Cycle switches Clock <-> Ticker. Upstream toggles Playlist instead.
    if (DisplayService::getPower())
    {
        ModesService::setModeNext();
    }
    else
    {
        DisplayService::setPower(true);
    }
}

void ButtonExtension::dismiss() { AlarmExtension::dismiss(); }

void ButtonExtension::minus()
{
    if (!DisplayService::getPower())
    {
        DisplayService::setPower(true);
        return;
    }
    const DisplayService::Brightness _brightness{DisplayService::getBrightness()};
    if (_brightness != DisplayService::Brightness::DUTY1)
    {
        DisplayService::setBrightness(static_cast<DisplayService::Brightness>(static_cast<uint8_t>(_brightness) - 1U));
    }
}

void ButtonExtension::plus()
{
    if (!DisplayService::getPower())
    {
        DisplayService::setPower(true);
        return;
    }
    const DisplayService::Brightness _brightness{DisplayService::getBrightness()};
    if (_brightness != DisplayService::Brightness::DUTY14)
    {
        DisplayService::setBrightness(static_cast<DisplayService::Brightness>(static_cast<uint8_t>(_brightness) + 1U));
    }
}

void ButtonExtension::snooze() { AlarmExtension::snooze(); }
