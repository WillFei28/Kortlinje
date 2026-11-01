#include "services/ModesService.h"

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
    if (payload["mode"].is<std::string_view>())
    {
        setMode(payload["mode"].as<std::string_view>());
    }
}

void ModesService::setModeNext()
{
    DisplayService::setPower(true);
    for (size_t idx{0U}; idx < names.size(); ++idx)
    {
        if (names[idx] == mode->name)
        {
            const size_t index{(idx + 1U) % names.size()};
            setMode(names[index], true);
            return;
        }
    }
}

void ModesService::setModePrevious()
{
    DisplayService::setPower(true);
    for (size_t idx{0U}; idx < names.size(); ++idx)
    {
        if (names[idx] == mode->name)
        {
            const size_t index{(idx + names.size() - 1U) % names.size()};
            setMode(names[index], true);
            return;
        }
    }
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
