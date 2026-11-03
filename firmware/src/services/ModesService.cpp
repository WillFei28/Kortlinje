#include "services/ModesService.h"

#include "extensions/OtaExtension.h"
#include "extensions/ProvisioningExtension.h"
#include "services/DeviceService.h"
#include "services/DisplayService.h"

void ModesService::begin()
{
    if (mode == nullptr)
    {
        setMode(ClockMode::name);
    }
}

void ModesService::handle()
{
    if (OtaExtension::isUpdating())
    {
        // Keep the OTA frame; DeviceService still flushes.
        return;
    }
    static bool provisioningShown{false};
    if (ProvisioningExtension::isActive())
    {
        provisioningShown = true;
        ProvisioningExtension::draw();
        return;
    }
    if (provisioningShown && mode != nullptr)
    {
        provisioningShown = false;
        mode->begin(); // re-arm pending so modes that only redraw on change repaint
    }
    if (mode != nullptr)
    {
        mode->handle();
    }
    if (!pendingMode.empty())
    {
        const std::string_view next{pendingMode};
        const bool power{pendingPower};
        pendingMode = {};
        setMode(next, power);
    }
}

void ModesService::requestMode(std::string_view modeName, bool power)
{
    pendingMode = modeName;
    pendingPower = power;
}

std::unique_ptr<ModeModule> ModesService::getMode(std::string_view modeName)
{
    if (modeName == ClockMode::name)
    {
        return std::make_unique<ClockMode>();
    }
    if (modeName == TickerMode::name)
    {
        return std::make_unique<TickerMode>();
    }
    return nullptr;
}

void ModesService::setMode(std::string_view modeName, bool power)
{
    if (std::unique_ptr<ModeModule> _mode{getMode(modeName)})
    {
        if (mode)
        {
            mode->end();
            mode.reset();
        }
        mode = std::move(_mode);
        if (power)
        {
            DisplayService::setPower(true);
        }
        mode->begin();
        transmit();
    }
}

ModeModule *ModesService::getMode() { return mode.get(); }

void ModesService::onReceive(JsonObjectConst payload, std::string_view source)
{
    static_cast<void>(source);
    if (payload["mode"].is<std::string_view>())
    {
        // WebSocket arrives on core0 — defer swap to ModesService::handle (core1).
        requestMode(payload["mode"].as<std::string_view>(), true);
    }
}

void ModesService::setModeNext()
{
    // Defer to core1 (ModesService::handle) — never swap mode from core0/button IRQ path.
    DisplayService::setPower(true);
    if (mode == nullptr)
    {
        requestMode(ClockMode::name, true);
        return;
    }
    const std::string_view current{mode->name};
    for (size_t idx{0U}; idx < names.size(); ++idx)
    {
        if (names[idx] == current)
        {
            requestMode(names[(idx + 1U) % names.size()], true);
            return;
        }
    }
    requestMode(names.front(), true);
}

void ModesService::setModePrevious()
{
    DisplayService::setPower(true);
    if (mode == nullptr)
    {
        requestMode(ClockMode::name, true);
        return;
    }
    const std::string_view current{mode->name};
    for (size_t idx{0U}; idx < names.size(); ++idx)
    {
        if (names[idx] == current)
        {
            requestMode(names[(idx + names.size() - 1U) % names.size()], true);
            return;
        }
    }
    requestMode(names.front(), true);
}

void ModesService::transmit()
{
    JsonDocument doc{};
    JsonArray list{doc["list"].to<JsonArray>()};
    for (const std::string_view _name : names)
    {
        list.add(_name);
    }
    if (mode != nullptr)
    {
        doc["mode"].set(mode->name);
    }
    DeviceService::transmit(doc.as<JsonObjectConst>(), name);
}
