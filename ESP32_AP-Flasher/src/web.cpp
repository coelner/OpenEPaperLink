#include "web.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <FS.h>
#include "storage.h"
#include "LittleFS.h"
#include "SPIFFSEditor.h"
#include <WiFi.h>
#include <WiFiManager.h>  // https://github.com/tzapu/WiFiManager/tree/feature_asyncwebserver

#include "commstructs.h"
#include "language.h"
#include "leds.h"
#include "newproto.h"
#include "ota.h"
#include "serialap.h"
#include "settings.h"
#include "tag_db.h"
#include "udp.h"

extern uint8_t data_to_send[];

// const char *http_username = "admin";
// const char *http_password = "admin";
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

SemaphoreHandle_t wsMutex;

void webSocketSendProcess(void *parameter) {
    wsMutex = xSemaphoreCreateMutex();
    while (true) {
        ws.cleanupClients();
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
#ifdef HAS_RGB_LED
    shortBlink(CRGB::BlueViolet);
#endif
    switch (type) {
        case WS_EVT_CONNECT:
            ets_printf("ws[%s][%u] connect\n", server->url(), client->id());
            // client->ping();
            break;
        case WS_EVT_DISCONNECT:
            ets_printf("ws[%s][%u] disconnect: %u\n", server->url(), client->id());
            break;
        case WS_EVT_ERROR:
            ets_printf("WS Error received :(\n\n");
            // ets_printf("ws[%s][%u] error(%u): %s\n", server->url(), client->id(), *((uint16_t *)arg), (char *)data);
            break;
        case WS_EVT_PONG:
            ets_printf("ws[%s][%u] pong[%u]: %s\n", server->url(), client->id(), len, (len) ? (char *)data : "");
            break;
        case WS_EVT_DATA:
            break;
    }
}

void wsLog(String text) {
    StaticJsonDocument<250> doc;
    doc["logMsg"] = text;
    if (wsMutex) xSemaphoreTake(wsMutex, portMAX_DELAY);
    ws.textAll(doc.as<String>());
    if (wsMutex) xSemaphoreGive(wsMutex);
}

void wsErr(String text) {
    StaticJsonDocument<250> doc;
    doc["errMsg"] = text;
    if (wsMutex) xSemaphoreTake(wsMutex, portMAX_DELAY);
    ws.textAll(doc.as<String>());
    if (wsMutex) xSemaphoreGive(wsMutex);
}

size_t dbSize(){
    size_t size = tagDB.size() * sizeof(tagRecord);
    for(auto &tag : tagDB) {
        if (tag->data)
            size += tag->len;
        size += tag->modeConfigJson.length();
    }
    return size;
}

void wsSendSysteminfo() {
    DynamicJsonDocument doc(250);
    JsonObject sys = doc.createNestedObject("sys");
    time_t now;
    time(&now);
    sys["currtime"] = now;
    sys["heap"] = ESP.getFreeHeap();
    sys["recordcount"] = tagDB.size();
    sys["dbsize"] = dbSize();
    sys["littlefsfree"] = Storage.freeSpace();
    sys["apstate"] = apInfo.state;
    sys["runstate"] = config.runStatus;
    sys["temp"] = temperatureRead();
    sys["rssi"] = WiFi.RSSI();
    sys["wifistatus"] = WiFi.status();
    sys["wifissid"] = WiFi.SSID();

    xSemaphoreTake(wsMutex, portMAX_DELAY);
    ws.textAll(doc.as<String>());
    xSemaphoreGive(wsMutex);
}

void wsSendTaginfo(uint8_t *mac, uint8_t syncMode) {
    if (syncMode != SYNC_DELETE) {
        String json = "";
        json = tagDBtoJson(mac);
        xSemaphoreTake(wsMutex, portMAX_DELAY);
        ws.textAll(json);
        xSemaphoreGive(wsMutex);
    }
    if (syncMode > SYNC_NOSYNC) {
        tagRecord *taginfo = nullptr;
        taginfo = tagRecord::findByMAC(mac);
        if (taginfo != nullptr) {
            if (taginfo->contentMode != 12 || syncMode == SYNC_DELETE) {
                UDPcomm udpsync;
                struct TagInfo taginfoitem;
                memcpy(taginfoitem.mac, taginfo->mac, sizeof(taginfoitem.mac));
                taginfoitem.syncMode = syncMode;
                taginfoitem.contentMode = taginfo->contentMode;
                if (syncMode == SYNC_USERCFG) {
                    strncpy(taginfoitem.alias, taginfo->alias.c_str(), sizeof(taginfoitem.alias) - 1);
                    taginfoitem.alias[sizeof(taginfoitem.alias) - 1] = '\0';
                    taginfoitem.nextupdate = taginfo->nextupdate;
                }
                if (syncMode == SYNC_TAGSTATUS) {
                    taginfoitem.lastseen = taginfo->lastseen;
                    taginfoitem.nextupdate = taginfo->nextupdate;
                    taginfoitem.pending = taginfo->pending;
                    taginfoitem.expectedNextCheckin = taginfo->expectedNextCheckin;
                    taginfoitem.hwType = taginfo->hwType;
                    taginfoitem.wakeupReason = taginfo->wakeupReason;
                    taginfoitem.capabilities = taginfo->capabilities;
                    taginfoitem.pendingIdle = taginfo->pendingIdle;
                }
                udpsync.netTaginfo(&taginfoitem);
            }
        }
    }
}

void wsSendAPitem(struct APlist *apitem) {
    DynamicJsonDocument doc(250);
    JsonObject ap = doc.createNestedObject("apitem");

    char version_str[6];
    sprintf(version_str, "%04X", apitem->version);

    ap["ip"] = ((IPAddress)apitem->src).toString();
    ap["alias"] = apitem->alias;
    ap["count"] = apitem->tagCount;
    ap["channel"] = apitem->channelId;
    ap["version"] = version_str;

    if (wsMutex) xSemaphoreTake(wsMutex, portMAX_DELAY);
    ws.textAll(doc.as<String>());
    if (wsMutex) xSemaphoreGive(wsMutex);
}

void wsSerial(String text) {
    StaticJsonDocument<250> doc;
    doc["console"] = text;
    Serial.println(text);
    if (wsMutex) xSemaphoreTake(wsMutex, portMAX_DELAY);
    ws.textAll(doc.as<String>());
    if (wsMutex) xSemaphoreGive(wsMutex);
}

uint8_t wsClientCount() {
    return ws.count();
}

void init_web() {
    Storage.begin();
    WiFi.mode(WIFI_STA);

    WiFiManager wm;
    bool res;
    WiFi.setTxPower(static_cast<wifi_power_t>(config.wifiPower));
    wm.setWiFiAutoReconnect(true);
    res = wm.autoConnect("OpenEPaperLink Setup");
    if (!res) {
        Serial.println("Failed to connect");
        ESP.restart();
    }
    Serial.print("Connected! IP address: ");
    Serial.println(WiFi.localIP());

    // server.addHandler(new SPIFFSEditor(*contentFS, http_username, http_password));
    server.addHandler(new SPIFFSEditor(*contentFS));

    ws.onEvent(onEvent);
    server.addHandler(&ws);

    server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "OK Reboot");
        wsErr("REBOOTING");
        ws.enable(false);
        refreshAllPending();
        saveDB("/current/tagDB.json");
        ws.closeAll();
        delay(100);
        ESP.restart();
    });

    server.serveStatic("/current", *contentFS, "/current/").setCacheControl("max-age=604800");
    server.serveStatic("/", *contentFS, "/www/").setDefaultFile("index.html");

    server.on(
        "/imgupload", HTTP_POST, [](AsyncWebServerRequest *request) {
            request->send(200);
        },
        doImageUpload);
    server.on("/jsonupload", HTTP_POST, doJsonUpload);

    server.on("/get_db", HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = "";
        if (request->hasParam("mac")) {
            String dst = request->getParam("mac")->value();
            uint8_t mac[8];
            if (hex2mac(dst, mac)) {
                json = tagDBtoJson(mac);
            } else {
                json = "{\"error\": \"malformatted parameter\"}";
            }
        } else {
            uint8_t startPos = 0;
            if (request->hasParam("pos")) {
                startPos = atoi(request->getParam("pos")->value().c_str());
            }
            json = tagDBtoJson(nullptr, startPos);
        }
        request->send(200, "application/json", json);
    });

    server.on("/getdata", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasParam("mac")) {
            String dst = request->getParam("mac")->value();
            uint8_t mac[8];
            if (hex2mac(dst, mac)) {
                tagRecord *taginfo = nullptr;
                taginfo = tagRecord::findByMAC(mac);
                if (taginfo != nullptr) {
                    if (taginfo->pending == true) {
                        request->send_P(200, "application/octet-stream", taginfo->data, taginfo->len);
                        return;
                    }
                }
            }
        }
        request->send(400, "text/plain", "No data available");
    });

    server.on("/save_cfg", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (request->hasParam("mac", true)) {
            String dst = request->getParam("mac", true)->value();
            uint8_t mac[8];
            if (hex2mac(dst, mac)) {
                tagRecord *taginfo = nullptr;
                taginfo = tagRecord::findByMAC(mac);
                if (taginfo != nullptr) {
                    taginfo->alias = request->getParam("alias", true)->value();
                    taginfo->modeConfigJson = request->getParam("modecfgjson", true)->value();
                    taginfo->contentMode = atoi(request->getParam("contentmode", true)->value().c_str());
                    taginfo->nextupdate = 0;
                    if (request->hasParam("rotate", true)) {
                        taginfo->rotate = atoi(request->getParam("rotate", true)->value().c_str());
                    }
                    if (request->hasParam("lut", true)) {
                        taginfo->lut = atoi(request->getParam("lut", true)->value().c_str());
                    }
                    // memset(taginfo->md5, 0, 16 * sizeof(uint8_t));
                    // memset(taginfo->md5pending, 0, 16 * sizeof(uint8_t));
                    wsSendTaginfo(mac, SYNC_USERCFG);
                    saveDB("/current/tagDB.json");
                    request->send(200, "text/plain", "Ok, saved");
                } else {
                    request->send(200, "text/plain", "Error while saving: mac not found");
                }
            }
        }
        request->send(200, "text/plain", "Ok, saved");
    });

    server.on("/tag_cmd", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (request->hasParam("mac", true) && request->hasParam("cmd", true)) {
            uint8_t mac[8];
            if (hex2mac(request->getParam("mac", true)->value(), mac)) {
                tagRecord *taginfo = nullptr;
                taginfo = tagRecord::findByMAC(mac);
                if (taginfo != nullptr) {
                    const char *cmdValue = request->getParam("cmd", true)->value().c_str();
                    if (strcmp(cmdValue, "del") == 0) {
                        wsSendTaginfo(mac, SYNC_DELETE);
                        deleteRecord(mac);
                    }
                    if (strcmp(cmdValue, "clear") == 0) {
                        clearPending(taginfo);
                        memcpy(taginfo->md5pending, taginfo->md5, sizeof(taginfo->md5pending));
                        wsSendTaginfo(mac, SYNC_TAGSTATUS);
                    }
                    if (strcmp(cmdValue, "refresh") == 0) {
                        updateContent(mac);
                    }
                    if (strcmp(cmdValue, "reboot") == 0) {
                        sendTagCommand(mac, CMD_DO_REBOOT, !taginfo->isExternal);
                    }
                    if (strcmp(cmdValue, "scan") == 0) {
                        sendTagCommand(mac, CMD_DO_SCAN, !taginfo->isExternal);
                    }
                    if (strcmp(cmdValue, "reset") == 0) {
                        sendTagCommand(mac, CMD_DO_RESET_SETTINGS, !taginfo->isExternal);
                    }
                    request->send(200, "text/plain", "Ok, done");
                } else {
                    request->send(200, "text/plain", "Error: mac not found");
                }
            }
        } else {
            request->send(500, "text/plain", "param error");
        }
    });

    server.on("/get_ap_config", HTTP_GET, [](AsyncWebServerRequest *request) {
        UDPcomm udpsync;
        udpsync.getAPList();
        File configFile = contentFS->open("/current/apconfig.json", "r");
        if (!configFile) {
            request->send(500, "text/plain", "Error opening apconfig.json file");
            return;
        }
        request->send(configFile, "application/json");
        configFile.close();
    });

    server.on("/save_apcfg", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (request->hasParam("alias", true) && request->hasParam("channel", true)) {
            String aliasValue = request->getParam("alias", true)->value();
            size_t aliasLength = aliasValue.length();
            if (aliasLength > 31) aliasLength = 31;
            aliasValue.toCharArray(config.alias, aliasLength + 1);
            config.alias[aliasLength] = '\0';

            config.channel = static_cast<uint8_t>(request->getParam("channel", true)->value().toInt());
            if (request->hasParam("led", true)) {
                config.led = static_cast<int16_t>(request->getParam("led", true)->value().toInt());
                updateBrightnessFromConfig();
            }
            if (request->hasParam("language", true)) {
                config.language = static_cast<uint8_t>(request->getParam("language", true)->value().toInt());
                updateLanguageFromConfig();
            }
            if (request->hasParam("maxsleep", true)) {
                config.maxsleep = static_cast<uint8_t>(request->getParam("maxsleep", true)->value().toInt());
            }
            if (request->hasParam("stopsleep", true)) {
                config.stopsleep = static_cast<uint8_t>(request->getParam("stopsleep", true)->value().toInt());
            }
            if (request->hasParam("preview", true)) {
                config.preview = static_cast<uint8_t>(request->getParam("preview", true)->value().toInt());
            }
            if (request->hasParam("wifipower", true)) {
                config.wifiPower = static_cast<uint8_t>(request->getParam("wifipower", true)->value().toInt());
                WiFi.setTxPower(static_cast<wifi_power_t>(config.wifiPower));
            }
            if (request->hasParam("timezone", true)) {
                strncpy(config.timeZone, request->getParam("timezone", true)->value().c_str(), sizeof(config.timeZone) - 1);
                config.timeZone[sizeof(config.timeZone) - 1] = '\0';
                setenv("TZ", config.timeZone, 1);
                tzset();
            }
            saveAPconfig();
            setAPchannel();
        }
        request->send(200, "text/plain", "Ok, saved");
    });

    server.on("/backup_db", HTTP_GET, [](AsyncWebServerRequest *request) {
        saveDB("/current/tagDB.json");
        File file = contentFS->open("/current/tagDB.json", "r");
        AsyncWebServerResponse *response = request->beginResponse(file, "tagDB.json", String(), true);
        request->send(response);
        file.close();
    });

    server.on("/sysinfo", HTTP_GET, handleSysinfoRequest);
    server.on("/check_file", HTTP_GET, handleCheckFile);
    server.on("/getexturl", HTTP_GET, handleGetExtUrl);
    server.on("/rollback", HTTP_POST, handleRollback);
    server.on("/update_actions", HTTP_POST, handleUpdateActions);
    server.on("/update_ota", HTTP_POST, [](AsyncWebServerRequest *request) {
        handleUpdateOTA(request);
    });
    server.on(
        "/littlefs_put", HTTP_POST, [](AsyncWebServerRequest *request) {
            request->send(200);
        },
        handleLittleFSUpload);

    server.onNotFound([](AsyncWebServerRequest *request) {
        if (request->url() == "/" || request->url() == "index.htm") {
            request->send(200, "text/html", "index.html not found. Did you forget to upload the littlefs partition?");
            return;
        }
        request->send(404);
    });

    server.begin();
}

void doImageUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!index) {
        if (request->hasParam("mac", true)) {
            filename = request->getParam("mac", true)->value() + ".jpg";
        } else {
            filename = "unknown.jpg";
        }
        request->_tempFile = contentFS->open("/" + filename, "w");
    }
    if (len) {
        request->_tempFile.write(data, len);
    }
    if (final) {
        request->_tempFile.close();
        if (request->hasParam("mac", true)) {
            String dst = request->getParam("mac", true)->value();
            bool dither = true;
            if (request->hasParam("dither", true)) {
                if (request->getParam("dither", true)->value() == "0") dither = false;
            }
            uint8_t mac[8];
            if (hex2mac(dst, mac)) {
                tagRecord *taginfo = nullptr;
                taginfo = tagRecord::findByMAC(mac);
                if (taginfo != nullptr) {
                    taginfo->modeConfigJson = "{\"filename\":\"" + dst + ".jpg\",\"timetolive\":\"0\",\"dither\":\"" + String(dither) + "\",\"delete\":\"1\"}";
                    taginfo->contentMode = 0;
                    taginfo->nextupdate = 0;
                    wsSendTaginfo(mac, SYNC_USERCFG);
                    request->send(200, "text/plain", "Ok, saved");
                } else {
                    request->send(400, "text/plain", "mac not found");
                }
            }
        } else {
            request->send(400, "text/plain", "parameters incomplete");
        }
    }
}

void doJsonUpload(AsyncWebServerRequest *request) {
    if (request->hasParam("mac", true) && request->hasParam("json", true)) {
        String dst = request->getParam("mac", true)->value();
        File file = LittleFS.open("/" + dst + ".json", "w");
        if (!file) {
            request->send(400, "text/plain", "Failed to create file");
            return;
        }
        file.print(request->getParam("json", true)->value());
        file.close();
        uint8_t mac[8];
        if (hex2mac(dst, mac)) {
            tagRecord *taginfo = nullptr;
            taginfo = tagRecord::findByMAC(mac);
            if (taginfo != nullptr) {
                taginfo->modeConfigJson = "{\"filename\":\"" + dst + ".json\"}";
                taginfo->contentMode = 19;
                taginfo->nextupdate = 0;
                wsSendTaginfo(mac, SYNC_USERCFG);
                request->send(200, "text/plain", "Ok, saved");
            } else {
                request->send(400, "text/plain", "mac not found in tagDB");
            }
        }
        return;
    }
    request->send(400, "text/plain", "Missing parameters");
}
