/***************************************************************************
Copyright © 2023 Shell M. Shrader <shell at shellware dot com>
----------------------------------------------------------------------------
This work is free. You can redistribute it and/or modify it under the
terms of the Do What The Fuck You Want To Public License, Version 2,
as published by Sam Hocevar. See the COPYING file for more details.
****************************************************************************/
#include "config.h"

void coreSetup() {
    // wire up EEPROM storage and config
    wireConfig();

    // start and mount our littlefs file system
    if (!LittleFS.begin()) {
        LOG_PRINTLN("\nAn Error has occurred while initializing LittleFS\n");
    } else {
#ifdef ENABLE_DEBUG
#ifdef esp32
        const size_t fs_size = LittleFS.totalBytes() / 1000;
        const size_t fs_used = LittleFS.usedBytes() / 1000;
#else
        FSInfo fs_info;
        LittleFS.info(fs_info);
        const size_t fs_size = fs_info.totalBytes / 1000;
        const size_t fs_used = fs_info.usedBytes / 1000;
#endif
        LOG_PRINTLN();
        LOG_PRINTLN("    Filesystem size: [" + String(fs_size) + "] KB");
        LOG_PRINTLN("         Free space: [" + String(fs_size - fs_used) + "] KB");
        LOG_PRINTLN("          Free Heap: [" + String(ESP.getFreeHeap()) + "]");
#endif
    }

    // Connect to Wi-Fi network with SSID and password
    // or fall back to AP mode
    WiFi.persistent(false);
    WiFi.setAutoConnect(false);
    WiFi.setAutoReconnect(false);
    WiFi.hostname(config.hostname);
    WiFi.mode(wifimode);

#ifdef esp32
    static const wifi_event_id_t disconnectHandler = WiFi.onEvent([](WiFiEvent_t event)
        {
            if (event == WIFI_DISCONNECTED && !esp_reboot_requested) {
                LOG_PRINTLN("\nWiFi disconnected");
                LOG_FLUSH();
                wifiState = event;
            }
        });
#else
    static const WiFiEventHandler disconnectHandler = WiFi.onStationModeDisconnected([](WiFiEventStationModeDisconnected event)
        {
            if (!esp_reboot_requested) {
                LOG_PRINTF("\nWiFi disconnected - reason: %d\n", event.reason);
                LOG_FLUSH();
                wifiState = WIFI_DISCONNECTED;
            }
        });
#endif

    // WiFi.scanNetworks will return the number of networks found
    uint8_t nothing = 0;
    uint8_t* bestBssid;
    bestBssid = &nothing;
    short bestRssi = SHRT_MIN;

    LOG_PRINTLN("\nScanning Wi-Fi networks. . .");
    int n = WiFi.scanNetworks();

    // arduino is too stupid to know which AP has the best signal
    // when connecting to an SSID with multiple BSSIDs (WAPs / Repeaters)
    // so we find the best one and tell it to use it
    if (n > 0 ) {
        for (int i = 0; i < n; ++i) {
            LOG_PRINTF("   ssid: %s - rssi: %d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
            if (config.ssid_flag == CFG_SET && WiFi.SSID(i).equals(config.ssid) && WiFi.RSSI(i) > bestRssi) {
                bestRssi = WiFi.RSSI(i);
                bestBssid = WiFi.BSSID(i);
            }
        }
    }

    if (wifimode == WIFI_STA && bestRssi != SHRT_MIN) {
        LOG_PRINTF("\nConnecting to %s / %d dB ", config.ssid, bestRssi);
        WiFi.begin(config.ssid, config.ssid_pwd, 0, bestBssid, true);
        for (tiny_int x = 0; x < 120 && WiFi.status() != WL_CONNECTED; x++) {
            blink();
            LOG_PRINT(".");
        }

        LOG_PRINTLN();

        if (WiFi.status() == WL_CONNECTED) {
            // initialize time
            configTime(0, 0, "pool.ntp.org");
            setenv("TZ", "EST+5EDT,M3.2.0/2,M11.1.0/2", 1);
            tzset();

            LOG_PRINT("\nCurrent Time: ");
            LOG_PRINTLN(getTimestamp());
        }
    }

    if (WiFi.status() != WL_CONNECTED || wifimode == WIFI_AP) {
        wifimode = WIFI_AP;
        WiFi.mode(wifimode);
        WiFi.softAP(config.hostname);
        dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
        LOG_PRINTLN("\nSoftAP [" + String(config.hostname) + "] started");
    }

    LOG_PRINTLN();
    LOG_PRINT("    Hostname: "); LOG_PRINTLN(config.hostname);
    LOG_PRINT("Connected to: "); LOG_PRINTLN(wifimode == WIFI_STA ? config.ssid : config.hostname);
    LOG_PRINT("  IP address: "); LOG_PRINTLN(wifimode == WIFI_STA ? WiFi.localIP().toString() : WiFi.softAPIP().toString());
    LOG_PRINT("        RSSI: "); LOG_PRINTLN(String(WiFi.RSSI()) + " dB");

    // enable mDNS via espota and enable ota
    wireArduinoOTA(config.hostname);

    // begin Elegant OTA
    ElegantOTA.begin(&server);
    ElegantOTA.onStart(onOTAStart);
    ElegantOTA.onProgress(onOTAProgress);
    ElegantOTA.onEnd(onOTAEnd);

    LOG_PRINTLN("ElegantOTA started");


    updateHtmlTemplate("/setup.template.html", false);
    LOG_PRINTLN("setup.html updated");

    // wire up http server and paths
    wireWebServerAndPaths();

    // wire up our custom watchdog
#ifdef esp32
    watchDogTimer = timerBegin(2, 80, true);
    timerAttachInterrupt(watchDogTimer, &watchDogInterrupt, true);
    timerAlarmWrite(watchDogTimer, WATCHDOG_TIMEOUT_S * 1000000, false);
    timerAlarmEnable(watchDogTimer);
#else
    ITimer.attachInterruptInterval(WATCHDOG_TIMEOUT_S * 1000000, TimerHandler);
#endif

    LOG_PRINTLN("Watchdog started");
}

void coreLoop() {
    // handle TelnetSpy if ENABLE_DEBUG is defined
    LOG_HANDLE();

    // handle a reboot request if pending
    if (esp_reboot_requested) {
        ElegantOTA.loop();
        delay(1000);
        LOG_PRINTLN("\nReboot triggered. . .");
        LOG_HANDLE();
        LOG_FLUSH();
        ESP.restart();
        while (1) {} // will never get here
    }

    // captive portal if in AP mode
    if (wifimode == WIFI_AP) {
        dnsServer.processNextRequest();
    } else {
        if (wifiState == WIFI_DISCONNECTED) {
            // LOG_PRINTLN("sleeping for 180 seconds. . .");
            // for (tiny_int x = 0; x < 180; x++) {
            //   delay(1000);
            //   watchDogRefresh();
            // }
            LOG_PRINTLN("\nRebooting due to no wifi connection");
            esp_reboot_requested = true;
            return;
        }

        // check for OTA
        ArduinoOTA.handle();
        ElegantOTA.loop();
    }


    // reboot if in AP mode and no activity for 5 minutes
    if (wifimode == WIFI_AP && !ap_mode_activity && millis() >= 300000UL) {
        LOG_PRINTF("\nNo AP activity for 5 minutes -- triggering reboot");
        esp_reboot_requested = true;
    }

    // // 24 hour mandatory reboot
    // if (millis() >= 86400000UL) {
    //   LOG_PRINTF("\nTriggering mandatory 24 hour reboot");
    //   esp_reboot_requested = true;
    // }

    // rebuild setup.html on main thread
    if (setup_needs_update) {
        LOG_PRINTLN("\n----- rebuilding /setup.html");
        updateHtmlTemplate("/setup.template.html", false);
        LOG_PRINTLN("-----  /setup.html rebuilt");
        setup_needs_update = false;
    }
}

void watchDogRefresh() {
#ifdef esp32
    timerWrite(watchDogTimer, 0);
#else
    if (timer_pinged) {
        timer_pinged = false;
        LOG_PRINTLN("PONG");
        LOG_FLUSH();
    }
#endif
}

void blink() {
    LED_ON;
    delay(200);
    LED_OFF;
    delay(100);
    LED_ON;
    delay(200);
    LED_OFF;
}

void wireConfig() {
    // configuration storage
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.get(0, config);
    EEPROM.end();

    if (config.hostname_flag != CFG_SET) {
        strcpy(config.hostname, DEFAULT_HOSTNAME);
    }

    if (config.ssid_flag == CFG_SET) {
        if (String(config.ssid).length() > 0) wifimode = WIFI_STA;
    } else {
        memset(config.ssid, CFG_NOT_SET, WIFI_SSID_LEN);
        wifimode = WIFI_AP;
    }

    if (config.ssid_pwd_flag != CFG_SET) memset(config.ssid_pwd, CFG_NOT_SET, WIFI_PASSWD_LEN);

    LOG_PRINTLN();
    LOG_PRINTLN("        EEPROM size: [" + String(EEPROM_SIZE) + "]");
    LOG_PRINTLN("        config size: [" + String(sizeof(config)) + "]\n");
    LOG_PRINTLN("        config host: [" + String(config.hostname) + "] stored: " + (config.hostname_flag == CFG_SET ? "true" : "false"));
    LOG_PRINTLN("        config ssid: [" + String(config.ssid) + "] stored: " + (config.ssid_flag == CFG_SET ? "true" : "false"));
    LOG_PRINTLN("    config ssid pwd: [" + String(config.ssid_pwd) + "] stored: " + (config.ssid_pwd_flag == CFG_SET ? "true\n" : "false\n"));
}

void onOTAStart() {
    // Log when OTA has started
    LOG_PRINTLN("\nOTA update started!");
    // <Add your own code here>
}

void onOTAProgress(size_t current, size_t final) {
    // Log every 1 second
    if (millis() - ota_progress_millis > 1000) {
        watchDogRefresh();
        ota_progress_millis = millis();
        LOG_PRINTF("OTA Progress Current: %u bytes, Final: %u bytes\r", current, final);
        LOG_FLUSH();
    }
}

void onOTAEnd(bool success) {
    // Log when OTA has finished
    if (success) {
        LOG_PRINTLN("\nOTA update finished successfully!");
        esp_reboot_requested = true;
    } else {
        LOG_PRINTLN("\nThere was an error during OTA update!");
    }
    LOG_FLUSH();
}

void updateHtmlTemplate(String template_filename, bool showTime = true) {

    String output_filename = template_filename;
    output_filename.replace(".template", "");

    File _template = LittleFS.open(template_filename, FILE_READ);

    if (_template) {
        String html = _template.readString();
        _template.close();

        while (html.indexOf("{hostname}", 0) != -1) {
            html.replace("{hostname}", String(config.hostname));
        }

        while (html.indexOf("{ssid}", 0) != -1) {
            html.replace("{ssid}", String(config.ssid));
        }

        while (html.indexOf("{ssid_pwd}", 0) != -1) {
            html.replace("{ssid_pwd}", String(config.ssid_pwd));
        }

        if (html.indexOf("{timestamp}", 0) != 1) {
            String timestamp = getTimestamp();
            while (html.indexOf("{timestamp}", 0) != -1) {
                html.replace("{timestamp}", timestamp);
            }
            if (showTime)
                LOG_PRINTLN("Timestamp   = " + timestamp);
        }

        File _index = LittleFS.open(output_filename + ".new", FILE_WRITE);
        _index.print(html.c_str());
        _index.close();

        LittleFS.remove(output_filename);
        LittleFS.rename(output_filename + ".new", output_filename);
    }
}

void wireArduinoOTA(const char* hostname) {
    ArduinoOTA.setHostname(hostname);

    ArduinoOTA.onStart([]()
        {
            String type;
            if (ArduinoOTA.getCommand() == U_FLASH)
                type = "sketch";
            else // U_SPIFFS
                type = "filesystem";

            // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
            LOG_PRINTLN("\nOTA triggered for updating " + type);
        });

    ArduinoOTA.onEnd([]()
        {
            LOG_PRINTLN("\nOTA End");
            LOG_FLUSH();
            esp_reboot_requested = true;
        });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
        {
            watchDogRefresh();
            LOG_PRINTF("Progress: %u%%\r", (progress / (total / 100)));
            LOG_FLUSH();
        });

    ArduinoOTA.onError([](ota_error_t error)
        {
            LOG_PRINTF("\nError[%u]: ", error);
            if (error == OTA_AUTH_ERROR) LOG_PRINTLN("Auth Failed");
            else if (error == OTA_BEGIN_ERROR) LOG_PRINTLN("Begin Failed");
            else if (error == OTA_CONNECT_ERROR) LOG_PRINTLN("Connect Failed");
            else if (error == OTA_RECEIVE_ERROR) LOG_PRINTLN("Receive Failed");
            else if (error == OTA_END_ERROR) LOG_PRINTLN("End Failed");
            LOG_FLUSH();
        });

    ArduinoOTA.begin();
    LOG_PRINTLN("\nArduinoOTA started");
}

void wireWebServerAndPaths() {
    // define default document
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request)
        {
            ap_mode_activity = true;
            request->redirect("/index.html");
            LOG_PRINTLN("\n" + request->url() + " handled");
        });

    // define setup document
    server.on("/setup", HTTP_GET, [](AsyncWebServerRequest* request)
        {
            request->send(LittleFS, "/setup.html", "text/html");
            LOG_PRINTLN("\n" + request->url() + " handled");
        });

    // captive portal
    server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest* request)
        {
            ap_mode_activity = true;
            request->send(LittleFS, "/index.html", "text/html");
            LOG_PRINTLN("\n" + request->url() + " handled");
        });
    server.on("/library/test/success.html", HTTP_GET, [](AsyncWebServerRequest* request)
        {
            ap_mode_activity = true;
            request->send(LittleFS, "/index.html", "text/html");
            LOG_PRINTLN("\n" + request->url() + " handled");
        });
    server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest* request)
        {
            ap_mode_activity = true;
            request->send(LittleFS, "/index.html", "text/html");
            LOG_PRINTLN("\n" + request->url() + " handled");
        });
    server.on("/gen_204", HTTP_GET, [](AsyncWebServerRequest* request)
        {
            ap_mode_activity = true;
            request->send(LittleFS, "/index.html", "text/html");
            LOG_PRINTLN("\n" + request->url() + " handled");
        });
    server.on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest* request)
        {
            ap_mode_activity = true;
            request->send(LittleFS, "/index.html", "text/html");
            LOG_PRINTLN("\n" + request->url() + " handled");
        });
    server.on("/check_network_status.txt", HTTP_GET, [](AsyncWebServerRequest* request)
        {
            ap_mode_activity = true;
            request->send(LittleFS, "/index.html", "text/html");
            LOG_PRINTLN("\n" + request->url() + " handled");
        });

    // request reboot
    server.on("/reboot", HTTP_GET, [](AsyncWebServerRequest* request)
        {
            request->redirect("/index.html");
            LOG_PRINTLN("\n" + request->url() + " handled");
            esp_reboot_requested = true;
        });

    // save config
    server.on("/save", HTTP_GET, [](AsyncWebServerRequest* request)
        {
            saveConfig(request->getParam("hostname")->value(),
            request->getParam("ssid")->value(),
            request->getParam("ssid_pwd")->value());

    request->redirect("/index.html");
    LOG_PRINTLN("\n" + request->url() + " handled");
        });

    // load config
    server.on("/load", HTTP_GET, [](AsyncWebServerRequest* request)
        {
            LOG_PRINTLN();
            wireConfig();
            setup_needs_update = true;
            request->redirect("/index.html");
            LOG_PRINTLN("\n" + request->url() + " handled");
        });

    // wipe config
    server.on("/wipe", HTTP_GET, [](AsyncWebServerRequest* request)
        {
            const boolean reboot = !request->hasParam("noreboot");

            wipeConfig();

            request->redirect("/index.html");
            LOG_PRINTLN("\n" + request->url() + " handled");

            // trigger a reboot
            if (reboot) esp_reboot_requested = true;
        });

    // 404 (includes file handling)
    server.onNotFound([](AsyncWebServerRequest* request)
        {
            ap_mode_activity = true;

            if (LittleFS.exists(request->url())) {
                AsyncWebServerResponse* response = request->beginResponse(LittleFS, request->url(), String());
                String url = request->url(); url.toLowerCase();
                // only chache digital assets
                if (url.indexOf(".png") != -1 || url.indexOf(".jpg") != -1 || url.indexOf(".ico") != -1 || url.indexOf(".svg") != -1) {
                    response->addHeader("Cache-Control", "max-age=604800");
                } else {
                    response->addHeader("Cache-Control", "no-store");
                }
                request->send(response);
                LOG_PRINTLN("\n" + request->url() + " handled");
            } else {
                request->send(404, "text/plain", request->url() + " Not found!");
                LOG_PRINTLN("\n" + request->url() + " Not found!");
            }
        });

    // begin the web server
    server.begin();
    LOG_PRINTLN("HTTP server started");
}

void saveConfig(String hostname,
                String ssid,
                String ssid_pwd) {

    memset(config.hostname, CFG_NOT_SET, HOSTNAME_LEN);
    if (hostname.length() > 0) {
        config.hostname_flag = CFG_SET;
    } else {
        config.hostname_flag = CFG_NOT_SET;
        hostname = DEFAULT_HOSTNAME;
    }
    hostname.toCharArray(config.hostname, HOSTNAME_LEN);

    memset(config.ssid, CFG_NOT_SET, WIFI_SSID_LEN);
    if (ssid.length() > 0) {
        ssid.toCharArray(config.ssid, WIFI_SSID_LEN);
        config.ssid_flag = CFG_SET;
    } else {
        config.ssid_flag = CFG_NOT_SET;
    }

    memset(config.ssid_pwd, CFG_NOT_SET, WIFI_PASSWD_LEN);
    if (ssid_pwd.length() > 0) {
        ssid_pwd.toCharArray(config.ssid_pwd, WIFI_PASSWD_LEN);
        config.ssid_pwd_flag = CFG_SET;
    } else {
        config.ssid_pwd_flag = CFG_NOT_SET;
    }

    EEPROM.begin(EEPROM_SIZE);
    EEPROM.put(0, config);
    EEPROM.commit();
    EEPROM.end();

    setup_needs_update = true;
}

void wipeConfig() {
    config.hostname_flag = CFG_NOT_SET;
    strcpy(config.hostname, DEFAULT_HOSTNAME);
    config.ssid_flag = CFG_NOT_SET;
    memset(config.ssid, CFG_NOT_SET, WIFI_SSID_LEN);
    config.ssid_pwd_flag = CFG_NOT_SET;
    memset(config.ssid_pwd, CFG_NOT_SET, WIFI_PASSWD_LEN);

    EEPROM.begin(EEPROM_SIZE);
    EEPROM.put(0, config);
    EEPROM.commit();
    EEPROM.end();

    LOG_PRINTLN("\nConfig wiped");
}

bool isSampleValid(float value) {
    return value < SHRT_MAX && value > SHRT_MIN;
}

String toFloatStr(float value, short decimal_places) {
    char buf[20];
    String fmt;

    fmt = "%." + String(decimal_places) + "f";
    sprintf(buf, fmt.c_str(), value);

    return String(buf);
}

#ifdef ENABLE_DEBUG
void checkForRemoteCommand() {
    if (SerialAndTelnet.available() > 0) {
        char c = SerialAndTelnet.read();
        switch (c) {
        case '\r':
            LOG_PRINT("\r");
            break;
        case '\n':
            LOG_PRINT("\n");
            break;
        case 'D':
            LOG_PRINTLN("\nDisconnecting Wi-Fi. . .");
            LOG_FLUSH();
            WiFi.disconnect();
            break;
        case 'F':
        {
#ifdef esp32
            const size_t fs_size = LittleFS.totalBytes() / 1000;
            const size_t fs_used = LittleFS.usedBytes() / 1000;
#else
            FSInfo fs_info;
            LittleFS.info(fs_info);
            const size_t fs_size = fs_info.totalBytes / 1000;
            const size_t fs_used = fs_info.usedBytes / 1000;
#endif
            LOG_PRINTLN("\n    Filesystem size: [" + String(fs_size) + "] KB");
            LOG_PRINTLN("         Free space: [" + String(fs_size - fs_used) + "] KB\n");
        }
        break;
        case 'S':
        {
            LOG_PRINTLN("\nType SSID and press <ENTER>");
            LOG_FLUSH();

            String ssid;
            do {
                if (SerialAndTelnet.available() > 0) {
                    c = SerialAndTelnet.read();
                    if (c != 10 && c != 13) {
                        LOG_PRINT(c);
                        LOG_FLUSH();
                        ssid = ssid + String(c);
                    }
                }
                watchDogRefresh();
            } while (c != 13);

            LOG_PRINTLN("\nType PASSWORD and press <ENTER>");
            LOG_FLUSH();
            String ssid_pwd;
            do {
                if (SerialAndTelnet.available() > 0) {
                    c = SerialAndTelnet.read();
                    if (c != 10 && c != 13) {
                        LOG_PRINT(c);
                        LOG_FLUSH();
                        ssid_pwd = ssid_pwd + String(c);
                    }
                }
                watchDogRefresh();
            } while (c != 13);

            LOG_PRINTLN("\n\nSSID=[" + ssid + "] PWD=[" + ssid_pwd + "]\n");
            LOG_FLUSH();

            memset(config.ssid, CFG_NOT_SET, WIFI_SSID_LEN);
            if (ssid.length() > 0) {
                ssid.toCharArray(config.ssid, WIFI_SSID_LEN);
                config.ssid_flag = CFG_SET;
            } else {
                config.ssid_flag = CFG_NOT_SET;
            }

            memset(config.ssid_pwd, CFG_NOT_SET, WIFI_PASSWD_LEN);
            if (ssid_pwd.length() > 0) {
                ssid_pwd.toCharArray(config.ssid_pwd, WIFI_PASSWD_LEN);
                config.ssid_pwd_flag = CFG_SET;
            } else {
                config.ssid_pwd_flag = CFG_NOT_SET;
            }

            EEPROM.begin(EEPROM_SIZE);
            EEPROM.put(0, config);
            EEPROM.commit();
            EEPROM.end();

            LOG_PRINTLN("SSID and Password saved - reload config or reboot\n");
            LOG_FLUSH();
        }
        break;
        case 'L':
            wireConfig();
            setup_needs_update = true;
            break;
        case 'W':
            wipeConfig();
            break;
        case 'X':
            LOG_PRINTLN(F("\r\nClosing session..."));
            SerialAndTelnet.disconnectClient();
            break;
        case 'R':
            LOG_PRINTLN(F("\r\nsubmitting reboot request..."));
            esp_reboot_requested = true;
            break;
        case ' ':
            // do nothing -- just a simple echo
            break;
        case 'C':
            // current time
            LOG_PRINTF("Current timestamp: [%s]\n", getTimestamp().c_str());
            break;
        case 'T':
        {
            File file = LittleFS.open("/last_signal.txt", FILE_READ);

            if (file.size() > 0) {
                String rawString = file.readString();
                file.close();

                irrecv.pause();
                irsend.sendRaw((uint16_t*)rawString.c_str(), rawString.length(), 38);  // Send a raw data capture at 38kHz.
                irrecv.resume();

                LOG_PRINTF("IRsend: [%s]\n", rawString.c_str());
            } else {
                LOG_PRINTLN("Nothing to transmit");
            }

            file.close();
        }
        break;
        case 'H':
        {
            File file = LittleFS.open("/signals.txt", FILE_READ);

            if (file.size() > 0) {
                String history = file.readString();
                LOG_PRINTLN("\nSignal History\n");
                LOG_PRINTLN(history);
                LOG_PRINTLN();
            } else {
                LOG_PRINTLN("No signal history available");
            }

            file.close();
        }
        break;
        default:
            LOG_PRINT("\n\nCommands:\n\nT = Transmit Received Code\nH = Received History\nC = Current Timestamp\nD = Disconnect WiFi\nF = Filesystem Info\nS - Set SSID / Password\nL = Reload Config\nW = Wipe Config\nX = Close Session\nR = Reboot ESP\n\n");
            break;
        }
        SerialAndTelnet.flush();
    }
}
#endif

String getTimestamp() {
    struct tm timeinfo;
    char timebuf[255];

    if (wifimode == WIFI_AP || !getLocalTime(&timeinfo)) {
        const unsigned long now = millis();
        sprintf(timebuf, "%06lu.%03lu", now / 1000, now % 1000);
    } else {
        sprintf(timebuf, "%4d-%2.2d-%2.2d %2.2d:%2.2d:%2.2d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }

    return String(timebuf);
}

boolean isNumeric(String str) {
    unsigned int stringLength = str.length();

    if (stringLength == 0) {
        return false;
    }

    boolean seenDecimal = false;

    for (unsigned int i = 0; i < stringLength; ++i) {
        if (isDigit(str.charAt(i))) {
            continue;
        }

        if (str.charAt(i) == '.') {
            if (seenDecimal) {
                return false;
            }
            seenDecimal = true;
            continue;
        }
        return false;
    }
    return true;
}

void printHeapStats() {
#ifdef ENABLE_DEBUG
    uint32_t myfree;
    uint32_t mymax;
    uint8_t myfrag;

#ifdef esp32 
    const uint32_t size = ESP.getHeapSize();
    const uint32_t free = ESP.getFreeHeap();
    const uint32_t max = ESP.getMaxAllocHeap();
    const uint32_t min = ESP.getMinFreeHeap();
    LOG_PRINTF("\n(%ld) -> size: %5d - free: %5d - max: %5d - min: %5d <-\n", millis(), size, free, max, min);
#else
    ESP.getHeapStats(&myfree, &mymax, &myfrag);
    LOG_PRINTF("\n(%ld) -> free: %5d - max: %5d - frag: %3d%% <-\n", millis(), myfree, mymax, myfrag);
#endif
#endif

    return;
}

#ifdef DECODE_AC
// Display the human readable state of an A/C message if we can.
void dumpACInfo(decode_results* results) {
    String description = "";
#if DECODE_DAIKIN
    if (results->decode_type == DAIKIN) {
        IRDaikinESP ac(0);
        ac.setRaw(results->state);
        description = ac.toString();
    }
#endif  // DECODE_DAIKIN
#if DECODE_FUJITSU_AC
    if (results->decode_type == FUJITSU_AC) {
        IRFujitsuAC ac(0);
        ac.setRaw(results->state, results->bits / 8);
        description = ac.toString();
    }
#endif  // DECODE_FUJITSU_AC
#if DECODE_KELVINATOR
    if (results->decode_type == KELVINATOR) {
        IRKelvinatorAC ac(0);
        ac.setRaw(results->state);
        description = ac.toString();
    }
#endif  // DECODE_KELVINATOR
#if DECODE_TOSHIBA_AC
    if (results->decode_type == TOSHIBA_AC) {
        IRToshibaAC ac(0);
        ac.setRaw(results->state);
        description = ac.toString();
    }
#endif  // DECODE_TOSHIBA_AC
#if DECODE_GREE
    if (results->decode_type == GREE) {
        IRGreeAC ac(0);
        ac.setRaw(results->state);
        description = ac.toString();
    }
#endif  // DECODE_GREE
#if DECODE_MIDEA
    if (results->decode_type == MIDEA) {
        IRMideaAC ac(0);
        ac.setRaw(results->value);  // Midea uses value instead of state.
        description = ac.toString();
    }
#endif  // DECODE_MIDEA
#if DECODE_HAIER_AC
    if (results->decode_type == HAIER_AC) {
        IRHaierAC ac(0);
        ac.setRaw(results->state);
        description = ac.toString();
    }
#endif  // DECODE_HAIER_AC
    // If we got a human-readable description of the message, display it.
    if (description != "")  Serial.println("Mesg Desc.: " + description);
}
#endif