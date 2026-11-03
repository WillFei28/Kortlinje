#include "extensions/OtaExtension.h"

#include "fonts/KortlinjeFont.h"
#include "handlers/TextHandler.h"
#include "services/DisplayService.h"
#include "services/HangTrace.h"

void OtaExtension::begin()
{
    ArduinoOTA.setHostname(HOSTNAME);
#ifdef OTA_KEY
    ArduinoOTA.setPasswordHash(OTA_KEY);
#endif // OTA_KEY
    ArduinoOTA.onStart(&onStart);
    ArduinoOTA.onError(&onError);
    // OTA upload runs in one blocking loop inside ArduinoOTA.handle().
    ArduinoOTA.onProgress([](unsigned int, unsigned int) { HangTrace::feed(); });
    ArduinoOTA.begin();
}

void OtaExtension::handle() { ArduinoOTA.handle(); }

void OtaExtension::onStart()
{
    HangTrace::mark(HangTrace::Stage::Ota);
    HangTrace::feed();
    updating = true;
    DisplayService::fillFrame(false);
    KortlinjeFont font{};
    TextHandler(std::string("OTA"), font).draw();
}

void OtaExtension::onError(ota_error_t error)
{
    Serial.printf("[ota] error %u — resume modes\n", static_cast<unsigned>(error));
    updating = false;
}
