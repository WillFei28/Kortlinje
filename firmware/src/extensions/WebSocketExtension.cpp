#include "extensions/WebSocketExtension.h"

#include "extensions/WebUiExtension.h"
#include "services/DeviceService.h"
#include "services/EtaService.h"
#include "services/HangTrace.h"
#include "services/WebServerService.h"

void WebSocketExtension::begin()
{
    socket.onEvent(&onEvent);
    WebServerService::http.addHandler(&socket);
}

void WebSocketExtension::handle()
{
    if (EtaService::httpActive())
    {
        return;
    }
    socket.cleanupClients();
}

void WebSocketExtension::onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg,
                                 const uint8_t *data, size_t len)
{
    const HangTrace::IrqScope scope{type == AwsEventType::WS_EVT_CONNECT ? HangTrace::Stage::WsConnect
                                    : type == AwsEventType::WS_EVT_DATA  ? HangTrace::Stage::WsData
                                                                         : HangTrace::Stage::WsOther};
    switch (type)
    {
    case AwsEventType::WS_EVT_CONNECT:
    {
        std::string message{};
        DeviceService::serializeTransmits(message);
        client->text(message.data(), message.size());
        // Default on until client says otherwise (home clock canvas is the usual first view).
        WebUiExtension::previewWanted = true;
        WebUiExtension::render = true;
    }
    break;
    case AwsEventType::WS_EVT_DATA:
    {
        const AwsFrameInfo *info{static_cast<AwsFrameInfo *>(arg)};
        if (info->message_opcode == AwsFrameType::WS_TEXT)
        {
            if (info->final == 0U || info->index != 0U || info->index + len != info->len)
            {
                if (info->num == 0U && info->index == 0U)
                {
                    buffer.clear();
                }
                const std::span<const uint8_t> chunk{data, len};
                buffer.insert(buffer.end(), chunk.begin(), chunk.end());
                if (info->final == 0U || info->index + len != info->len)
                {
                    return;
                }
                data = buffer.data();
                len = buffer.size();
            }
            JsonDocument doc{};
            if (deserializeJson(doc, data, len) == DeserializationError::Code::Ok && doc.is<JsonObjectConst>())
            {
                for (const JsonPairConst pair : doc.as<JsonObjectConst>())
                {
                    if (pair.value().is<JsonObjectConst>())
                    {
                        DeviceService::receive(pair.value().as<JsonObjectConst>(),
                                       name,
                                       std::string_view(pair.key().c_str(), pair.key().size()));
                    }
                }
            }
        }
    }
    break;
    case AwsEventType::WS_EVT_DISCONNECT:
        if (server->count() == 0U)
        {
            WebUiExtension::previewWanted = false;
            WebUiExtension::render = false;
        }
        break;
    default:
        break;
    }
}

void WebSocketExtension::onTransmit(JsonObjectConst payload, std::string_view source)
{
    JsonDocument doc{};
    doc[source].set(payload);
    const size_t length{measureJson(doc)};
    std::vector<char> message(length + 1U);
    serializeJson(doc, message.data(), length + 1U);
    socket.textAll(message.data(), length);
}

AsyncWebSocket &WebSocketExtension::server() { return socket; }
