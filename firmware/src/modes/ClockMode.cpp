#include "modes/ClockMode.h"

#include "fonts/KortlinjeFont.h"
#include "handlers/BitmapHandler.h"
#include "handlers/TextHandler.h"
#include "services/DeviceService.h"
#include "services/DisplayService.h"

void ClockMode::begin()
{
#ifdef TIME_ZONE_POSIX
    setenv("TZ", TIME_ZONE_POSIX, 1);
    tzset();
#endif // TIME_ZONE_POSIX
    DisplayService::setPower(true);
    pending = true;
    showingPlaceholder = false;
    hour = 24;
    minute = 60;
}

void ClockMode::drawPlaceholder()
{
    DisplayService::fillFrame(false);
    KortlinjeFont font{};
    TextHandler("--:--", font).draw();
}

void ClockMode::drawTime()
{
    DisplayService::fillFrame(false);
#if CLOCK_12H
    const int displayHour{(local.tm_hour + 11) % 12 + 1};
    DisplayService::setPixel(local.tm_hour < 12 ? DisplayService::Symbol::AM : DisplayService::Symbol::PM, true);
    if (displayHour >= 10)
    {
        BitmapHandler(KortlinjeFont::digitZero_digitNine[1U])
            .draw((GRID_COLUMNS / 2U) - 20U, (GRID_ROWS / 2U) - 4U);
    }
    BitmapHandler(KortlinjeFont::digitZero_digitNine[displayHour % 10])
        .draw((GRID_COLUMNS / 2U) - 12U, (GRID_ROWS / 2U) - 4U);
#else
    BitmapHandler(KortlinjeFont::digitZero_digitNine[hour / 10])
        .draw(hour / 10 == 1 ? (GRID_COLUMNS / 2U) - 20U : (GRID_COLUMNS / 2U) - 21U, (GRID_ROWS / 2U) - 4U);
    BitmapHandler(KortlinjeFont::digitZero_digitNine[hour % 10])
        .draw((GRID_COLUMNS / 2U) - 12U, (GRID_ROWS / 2U) - 4U);
#endif // CLOCK_12H
    BitmapHandler(KortlinjeFont::colon).draw((GRID_COLUMNS / 2U) - 1U, (GRID_ROWS / 2U) - 3U);
    BitmapHandler(KortlinjeFont::digitZero_digitNine[minute / 10])
        .draw(minute / 10 == 1 ? (GRID_COLUMNS / 2U) + 6U : (GRID_COLUMNS / 2U) + 5U, (GRID_ROWS / 2U) - 4U);
    BitmapHandler(KortlinjeFont::digitZero_digitNine[minute % 10])
        .draw((GRID_COLUMNS / 2U) + 14U, (GRID_ROWS / 2U) - 4U);
}

void ClockMode::handle()
{
    if (!DeviceService::isTimeSynced())
    {
        if (pending || !showingPlaceholder)
        {
            drawPlaceholder();
            pending = false;
            showingPlaceholder = true;
            hour = 24;
            minute = 60;
        }
        return;
    }

    now = time(nullptr);
    if (localtime_r(&now, &local) == nullptr)
    {
        return;
    }

    if (showingPlaceholder || pending || minute != local.tm_min || hour != local.tm_hour)
    {
        pending = false;
        showingPlaceholder = false;
        hour = local.tm_hour;
        minute = local.tm_min;
        drawTime();
    }
}
