#include "services/DeviceService.h"

#include "config/IkeaKortlinje.h"
#include "config/secrets.h"
#include "config/version.h"
#include "extensions/AlarmExtension.h"
#include "extensions/ButtonExtension.h"
#include "modes/TickerMode.h"
#include "services/DisplayService.h"
#include "services/ExtensionsService.h"
#include "services/FontsService.h"
#include "services/ModesService.h"
#include "services/WebServerService.h"

#include <LittleFS.h>
#include <WiFi.h>
#include <hardware/adc.h>
#include <pico/cyw43_arch.h>
#include <time.h>

void DeviceService::begin0()
{
    Serial.begin();
    extensions.configure();
    WiFi.setHostname(HOSTNAME);
    WiFi.begin(WIFI_SSID, WIFI_KEY);
    lastWifiAttemptMs = millis();
#ifdef TIME_ZONE_POSIX
    setenv("TZ", TIME_ZONE_POSIX, 1);
    tzset();
#endif // TIME_ZONE_POSIX
    webServer.begin();
    WebServerService::http.on("/stats", AsyncWebRequestMethod::AsyncWebRequestMethodType::HTTP_GET, &onGetStats);
    extensions.begin();
    rp2040.fifo.push(1UL);
}

void DeviceService::begin1()
{
    LittleFS.begin();
    display.begin();
    fonts.begin();
    static_cast<void>(rp2040.fifo.pop());
    modes.begin();
}

void DeviceService::handle0()
{
    const uint64_t started{rp2040.getCycleCount64()};
    extensions.handle();
    if (!ButtonExtension::isEnabled())
    {
        adc_set_clkdiv(.0F);
        adc_fifo_setup(true, false, 0U, false, false);
        adc_set_temp_sensor_enabled(true);
        adc_select_input(__GPIOCNT - __FIRSTANALOGGPIO);
        const uint16_t sumTemperature{getSamples()};
        adc_set_temp_sensor_enabled(false);
#ifdef PICO_CYW43_SUPPORTED
        cyw43_thread_enter();
#endif // PICO_CYW43_SUPPORTED
        adc_gpio_init(A3);
        adc_select_input(A3 - __FIRSTANALOGGPIO);
        const uint16_t sumVsys{getSamples()};
#ifdef PICO_CYW43_SUPPORTED
        cyw43_thread_exit();
#endif // PICO_CYW43_SUPPORTED
        ButtonExtension::enable();
        const float _temperature{static_cast<float>(sumTemperature) *
                                 (3.3F / static_cast<float>(samples * ((0b1U << ADC_RESOLUTION) - 1U)))};
#if TEMPERATURE_FAHRENHEIT
        temperature = 80.6F - ((_temperature - .706F) * (1.8F / .001721F));
#elif TEMPERATURE_KELVIN
        temperature = 300.15F - ((_temperature - .706F) * (1.0F / .001721F));
#else
        temperature = 27.0F - ((_temperature - .706F) * (1.0F / .001721F));
#endif // TEMPERATURE_FAHRENHEIT
        voltage = static_cast<float>(sumVsys) * (9.9F / static_cast<float>(samples * ((0b1U << ADC_RESOLUTION) - 1U)));
        transmit();
    }
    display.handle();
    pollWifi();
    pollTime();
    sampleLoad(load0, started, rp2040.getCycleCount64());
}

void DeviceService::handle1()
{
    const uint64_t started{rp2040.getCycleCount64()};
    if (display.getPower())
    {
        modes.handle();
        display.flush();
    }
    sampleLoad(load1, started, rp2040.getCycleCount64());
}

float DeviceService::getLoad0() { return load0.pct; }

float DeviceService::getLoad1() { return load1.pct; }

float DeviceService::getHandle0Us() { return load0.handleUs; }

float DeviceService::getHandle1Us() { return load1.handleUs; }

uint32_t DeviceService::getLoop0Hz() { return load0.loopHz; }

uint32_t DeviceService::getLoop1Hz() { return load1.loopHz; }

void DeviceService::sampleLoad(CoreLoad &load, uint64_t started, uint64_t finished)
{
    if (finished < started)
    {
        return;
    }
    load.busy += finished - started;
    ++load.loops;
    if (load.windowStart == 0ULL)
    {
        load.windowStart = started;
    }
    const uint64_t elapsed{finished - load.windowStart};
    const uint64_t window{static_cast<uint64_t>(rp2040.f_cpu())};
    if (elapsed >= window && elapsed > 0ULL && load.loops > 0UL)
    {
        const float cpu{static_cast<float>(rp2040.f_cpu())};
        load.pct = 100.0F * static_cast<float>(load.busy) / static_cast<float>(elapsed);
        if (load.pct > 100.0F)
        {
            load.pct = 100.0F;
        }
        load.handleUs = (static_cast<float>(load.busy) / static_cast<float>(load.loops)) * (1'000'000.0F / cpu);
        load.loopHz = static_cast<uint32_t>((static_cast<uint64_t>(load.loops) * window) / elapsed);
        load.busy = 0ULL;
        load.loops = 0UL;
        load.windowStart = finished;
    }
}

void DeviceService::onGetStats(AsyncWebServerRequest *request)
{
    JsonDocument doc{};
    doc["core0_load_pct"].set(load0.pct);
    doc["core1_load_pct"].set(load1.pct);
    doc["core0_handle_us"].set(load0.handleUs);
    doc["core1_handle_us"].set(load1.handleUs);
    doc["core0_loop_hz"].set(load0.loopHz);
    doc["core1_loop_hz"].set(load1.loopHz);
    doc["f_cpu"].set(rp2040.f_cpu());
    doc["version"].set(VERSION);
    if (!transmits["Device"].isNull())
    {
        doc["temperature_chip"].set(transmits["Device"]["temperature"]);
        doc["voltage"].set(transmits["Device"]["voltage"]);
    }
    if (!transmits["Climate"].isNull())
    {
        doc["temperature_climate"].set(transmits["Climate"]["temperature"]);
        doc["humidity"].set(transmits["Climate"]["humidity"]);
    }
    AsyncResponseStream *response{request->beginResponseStream("application/json")};
    serializeJson(doc, *response);
    request->send(response);
}

uint16_t DeviceService::getSamples()
{
    adc_fifo_drain();
    sleep_us(0b1U << 4U);
    adc_run(true);
    uint16_t sum{0U};
    for (uint8_t idx{0U}; idx < samples; ++idx)
    {
        sum += adc_fifo_get_blocking();
    }
    adc_run(false);
    return sum;
}

JsonObjectConst DeviceService::getTransmits() { return transmits.as<JsonObjectConst>(); }

bool DeviceService::isTimeSynced() { return timeSynced; }

void DeviceService::pollWifi()
{
    const unsigned long nowMs{millis()};
    if (WiFi.status() == WL_CONNECTED)
    {
        if (!wifiConnected)
        {
            wifiConnected = true;
            Serial.print("wifi ");
            Serial.println(WiFi.localIP());
            ntpStarted = false;
            lastNtpAttemptMs = 0UL;
            TickerMode::refreshCache(true);
        }
        else
        {
            TickerMode::pollCache();
        }
        return;
    }

    if (wifiConnected)
    {
        wifiConnected = false;
        Serial.println("wifi lost");
    }

    if ((nowMs - lastWifiAttemptMs) >= wifiRetryMs)
    {
        lastWifiAttemptMs = nowMs;
        Serial.println("wifi retry");
        WiFi.disconnect(true);
        delay(100);
        WiFi.begin(WIFI_SSID, WIFI_KEY);
    }
}

void DeviceService::pollTime()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        return;
    }

    if (time(nullptr) >= minValidEpoch)
    {
        if (!timeSynced)
        {
            timeSynced = true;
            Serial.println("ntp ok");
            onWaitSet();
        }
        return;
    }

    timeSynced = false;
    const unsigned long nowMs{millis()};
    if (!ntpStarted || (nowMs - lastNtpAttemptMs) >= ntpRetryMs)
    {
        lastNtpAttemptMs = nowMs;
        ntpStarted = true;
#ifdef TIME_ZONE_POSIX
        setenv("TZ", TIME_ZONE_POSIX, 1);
        tzset();
#endif // TIME_ZONE_POSIX
        NTP.begin("1.pool.ntp.org", "2.pool.ntp.org");
        NTP.waitSet(&onWaitSet);
        Serial.println("ntp begin");
    }
}

void DeviceService::onReceive(JsonObjectConst payload, std::string_view source) {}

void DeviceService::onWaitSet() { AlarmExtension::reschedule(); }

void DeviceService::receive(JsonObjectConst payload, std::string_view source, std::string_view destination)
{
    if (name == destination)
    {
        onReceive(payload, source);
        return;
    }
    if (DisplayService::name == destination)
    {
        DisplayService::onReceive(payload, source);
        return;
    }
    if (ModesService::name == destination)
    {
        ModesService::onReceive(payload, source);
        return;
    }
    for (ExtensionModule *extension : ExtensionsService::getAll())
    {
        if (extension->name == destination)
        {
            extension->onReceive(payload, source);
            return;
        }
    }
    ModeModule *mode{ModesService::getMode()};
    if (mode != nullptr && mode->name == destination)
    {
        mode->onReceive(payload, source);
        return;
    }
    std::unique_ptr<ModeModule> _mode{ModesService::getMode(destination)};
    if (_mode != nullptr)
    {
        _mode->onReceive(payload, source);
    }
}

void DeviceService::transmit()
{
    JsonDocument doc{};
    doc["name"].set(NAME);
    doc["repository"].set("https://github.com/VIPnytt/Kortlinje");
    doc["temperature"].set(temperature);
    doc["version"].set(VERSION);
    doc["voltage"].set(voltage);
    doc["core0_load_pct"].set(load0.pct);
    doc["core1_load_pct"].set(load1.pct);
    doc["core0_handle_us"].set(load0.handleUs);
    doc["core1_handle_us"].set(load1.handleUs);
    doc["core0_loop_hz"].set(load0.loopHz);
    doc["core1_loop_hz"].set(load1.loopHz);
    doc["f_cpu"].set(rp2040.f_cpu());
    transmit(doc.as<JsonObjectConst>(), name);
}

void DeviceService::transmit(JsonObjectConst payload, std::string_view source, bool retain)
{
    if (retain)
    {
        transmits[source].set(payload);
    }
    for (ExtensionModule *extension : ExtensionsService::getAll())
    {
        extension->onTransmit(payload, source);
    }
}
