#include "services/DeviceService.h"

#include "config/IkeaKortlinje.h"
#include "config/secrets.h"
#include "config/version.h"
#include "extensions/AlarmExtension.h"
#include "extensions/ButtonExtension.h"
#include "extensions/ProvisioningExtension.h"
#include "handlers/ConfigHandler.h"
#include "modes/TickerMode.h"
#include "services/DisplayService.h"
#include "services/EtaService.h"
#include "services/ExtensionsService.h"
#include "services/FontsService.h"
#include "services/HangTrace.h"
#include "services/IrqSafeLock.h"
#include "services/ModesService.h"
#include "services/WebServerService.h"

#include <LittleFS.h>
#include <WiFi.h>
#include <hardware/adc.h>
#include <pico/cyw43_arch.h>
#include <string>
#include <time.h>

namespace
{
// Written from core1 modes, core0 loop and core0 lwIP IRQ (/stats, WS connect).
IrqSafeLock transmitsLock{};
} // namespace

void DeviceService::begin0()
{
    HangTrace::captureBoot();
    Serial.begin(115200);
    Serial.println("[boot] core0");
    Serial.printf("[boot] reset=%s last_stage=%lu at %lums irq_stage=%lu at %lums\n",
                  HangTrace::lastWasWatchdog ? "WATCHDOG" : "other",
                  static_cast<unsigned long>(HangTrace::lastStage),
                  static_cast<unsigned long>(HangTrace::lastStageMs),
                  static_cast<unsigned long>(HangTrace::lastIrqStage),
                  static_cast<unsigned long>(HangTrace::lastIrqMs));
    // Only core0 mounts LittleFS (never on core1 — dual mount races).
    const bool fsOk{LittleFS.begin()};
    Serial.printf("[boot] LittleFS %s\n", fsOk ? "ok" : "FAIL");
    EtaService::loadFromFs();
#if defined(PROVISION_CLEAR_WIFI) && PROVISION_CLEAR_WIFI
    static_cast<void>(ConfigHandler::remove("Wifi", "ssid"));
    static_cast<void>(ConfigHandler::remove("Wifi", "key"));
    Serial.println("[provision] cleared LittleFS Wifi (PROVISION_CLEAR_WIFI)");
#endif
    extensions.configure();
    Serial.println("[boot] configured");
    WiFi.setHostname(HOSTNAME);
#ifdef TIME_ZONE_POSIX
    setenv("TZ", TIME_ZONE_POSIX, 1);
    tzset();
#endif // TIME_ZONE_POSIX
    // Join via LittleFS / secrets before the web server binds. NTP stays in pollTime (non-blocking).
    const bool station{ProvisioningExtension::beginStation()};
    Serial.printf("[boot] station=%d\n", station ? 1 : 0);
    lastWifiAttemptMs = millis();
    webServer.begin();
    WebServerService::http.on("/stats", AsyncWebRequestMethod::AsyncWebRequestMethodType::HTTP_GET, &onGetStats);
    extensions.begin();
    Serial.println("[boot] extensions ready");
    rp2040.fifo.push(1UL);
    HangTrace::start();
    Serial.println("[boot] core0 done");
}

void DeviceService::begin1()
{
    // Display must not wait on FS/WiFi — otherwise a LittleFS hang leaves a black panel forever.
    // display.begin() already loads saved brightness from LittleFS — do not force DUTY14 here.
    display.begin();
    fonts.begin();
    static_cast<void>(rp2040.fifo.pop());
    modes.begin();
}

void DeviceService::handle0()
{
    const uint64_t started{rp2040.getCycleCount64()};
    HangTrace::feed();
    HangTrace::mark(HangTrace::Stage::Loop);
    extensions.handle();
    if (!ButtonExtension::isEnabled())
    {
        HangTrace::mark(HangTrace::Stage::Adc);
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
    HangTrace::mark(HangTrace::Stage::Display);
    display.handle();
    HangTrace::mark(HangTrace::Stage::PollWifi);
    pollWifi();
    HangTrace::mark(HangTrace::Stage::PollTime);
    pollTime();
    flushTransmits();
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
    const HangTrace::IrqScope scope{HangTrace::Stage::HttpStats};
    JsonDocument doc{};
    doc["core0_load_pct"].set(load0.pct);
    doc["core1_load_pct"].set(load1.pct);
    doc["core0_handle_us"].set(load0.handleUs);
    doc["core1_handle_us"].set(load1.handleUs);
    doc["core0_loop_hz"].set(load0.loopHz);
    doc["core1_loop_hz"].set(load1.loopHz);
    doc["f_cpu"].set(rp2040.f_cpu());
    doc["version"].set(VERSION);
    doc["uptime_ms"].set(millis());
    doc["last_reset_watchdog"].set(HangTrace::lastWasWatchdog);
    doc["last_hang_stage"].set(HangTrace::lastStage);
    if (HangTrace::lastStage >= HangTrace::Stage::ExtensionBase)
    {
        const auto all{ExtensionsService::getAll()};
        const size_t index{HangTrace::lastStage - HangTrace::Stage::ExtensionBase};
        if (index < all.size())
        {
            doc["last_hang_extension"].set(all[index]->name);
        }
    }
    doc["last_hang_ms"].set(HangTrace::lastStageMs);
    doc["last_irq_stage"].set(HangTrace::lastIrqStage);
    doc["last_irq_ms"].set(HangTrace::lastIrqMs);
    transmitsLock.lock();
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
    transmitsLock.unlock();
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

void DeviceService::serializeTransmits(std::string &out)
{
    out.clear();
    transmitsLock.lock();
    serializeJson(transmits, out);
    transmitsLock.unlock();
}

bool DeviceService::isTimeSynced() { return timeSynced; }

void DeviceService::pollWifi()
{
    if (ProvisioningExtension::isActive())
    {
        return;
    }

    const unsigned long nowMs{millis()};
    if (WiFi.status() == WL_CONNECTED)
    {
        if (!wifiConnected)
        {
            wifiConnected = true;
            wifiEverConnected = true;
            Serial.print("wifi ");
            Serial.println(WiFi.localIP());
            ntpStarted = false;
            lastNtpAttemptMs = 0UL;
        }
        EtaService::pollIdle();
        // Throttle — pollCache may block in HTTPS; calling it every handle0 starved
        // AsyncWebServer and stacked TLS after fail (hang ~15s into soak).
        static unsigned long lastEtaPollMs{0UL};
        constexpr unsigned long kEtaPollMs{500UL};
        if ((nowMs - lastEtaPollMs) >= kEtaPollMs)
        {
            lastEtaPollMs = nowMs;
            HangTrace::mark(HangTrace::Stage::EtaPoll);
            TickerMode::pollCache();
        }
        return;
    }

    if (wifiConnected)
    {
        wifiConnected = false;
        wifiLostMs = nowMs;
        Serial.println("wifi lost");
    }

    if (wifiEverConnected)
    {
        // WiFi.begin()/disconnect() tear down the netif under live AsyncTCP/OTA PCBs — core0
        // hung right after such a rejoin. Let CYW43 rejoin on its own; reboot if it can't.
        if ((nowMs - wifiLostMs) >= wifiRebootMs)
        {
            Serial.println("wifi lost too long — reboot");
            delay(50);
            rp2040.reboot();
        }
        return;
    }

    if ((nowMs - lastWifiAttemptMs) < wifiRetryMs)
    {
        return;
    }
    lastWifiAttemptMs = nowMs;

    std::string ssid{};
    std::string key{};
    if (!ConfigHandler::get("Wifi", "ssid", ssid) || ssid.empty())
    {
#ifdef WIFI_SSID
        const char *compiled{WIFI_SSID};
        if (compiled != nullptr && compiled[0] != '\0')
        {
            ssid = compiled;
#ifdef WIFI_KEY
            key = WIFI_KEY;
#endif
        }
#endif
    }
    else
    {
        static_cast<void>(ConfigHandler::get("Wifi", "key", key));
    }
    if (ssid.empty())
    {
        return;
    }

    Serial.println("wifi retry");
    HangTrace::mark(HangTrace::Stage::WifiBegin);
    WiFi.disconnect(true);
    delay(100);
    // WiFi.begin blocks up to 2×timeout (default 15s → 30s) — keep under the watchdog.
    WiFi.setTimeout(5'000);
    HangTrace::feed();
    static_cast<void>(WiFi.begin(ssid.c_str(), key.empty() ? nullptr : key.c_str()));
    HangTrace::feed();
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
    transmitsLock.lock();
    if (retain)
    {
        transmits[source].set(payload);
    }
    pendingTransmits[source].set(payload);
    transmitsLock.unlock();
    // core0 loop only: in an lwIP IRQ, textAll could interrupt the loop's own textAll.
    if (get_core_num() == 0U && __get_current_exception() == 0U)
    {
        flushTransmits();
    }
}

void DeviceService::flushTransmits()
{
    // Nested lwIP callbacks run while sync HTTPS polls; WS writes there wedge cyw43.
    if (flushingTransmits || EtaService::httpActive())
    {
        return;
    }
    JsonDocument batch{};
    transmitsLock.lock();
    if (pendingTransmits.isNull() || pendingTransmits.as<JsonObjectConst>().size() == 0U)
    {
        transmitsLock.unlock();
        return;
    }
    batch.set(pendingTransmits);
    pendingTransmits.clear();
    transmitsLock.unlock();

    HangTrace::mark(HangTrace::Stage::Flush);
    flushingTransmits = true;
    for (const JsonPairConst pair : batch.as<JsonObjectConst>())
    {
        const std::string_view source{pair.key().c_str(), pair.key().size()};
        const JsonObjectConst payload{pair.value().as<JsonObjectConst>()};
        for (ExtensionModule *extension : ExtensionsService::getAll())
        {
            extension->onTransmit(payload, source);
        }
    }
    flushingTransmits = false;
}
