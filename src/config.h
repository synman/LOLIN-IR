/***************************************************************************
Copyright © 2023 Shell M. Shrader <shell at shellware dot com>
----------------------------------------------------------------------------
This work is free. You can redistribute it and/or modify it under the
terms of the Do What The Fuck You Want To Public License, Version 2,
as published by Sam Hocevar. See the COPYING file for more details.
****************************************************************************/
#include <TelnetSpy.h>
TelnetSpy SerialAndTelnet;

#ifdef ENABLE_DEBUG
#define LOG_BEGIN(baudrate)  SerialAndTelnet.begin(baudrate)
#define LOG_PRINT(...)       SerialAndTelnet.print(__VA_ARGS__)
#define LOG_PRINTLN(...)     SerialAndTelnet.println(__VA_ARGS__)
#define LOG_PRINTF(...)      SerialAndTelnet.printf(__VA_ARGS__)
#define LOG_HANDLE()         SerialAndTelnet.handle() ; checkForRemoteCommand()
#define LOG_FLUSH()          SerialAndTelnet.flush()
#define LOG_WELCOME_MSG(msg) SerialAndTelnet.setWelcomeMsg(msg)
#else
#define LOG_BEGIN(baudrate)
#define LOG_PRINT(...) 
#define LOG_PRINTLN(...)
#define LOG_PRINTF(...) 
#define LOG_HANDLE()
#define LOG_FLUSH()
#define LOG_WELCOME_MSG(msg)
#endif

#define WATCHDOG_TIMEOUT_S 15
volatile bool timer_pinged;

#include <Arduino.h>
#include <ArduinoOTA.h>
#include "time.h"

#include <Wire.h>
#include <SPI.h>

#ifdef esp32
#include <WiFi.h>
#include <AsyncTCP.h>
#include <WiFiClientSecure.h>

#define WIFI_DISCONNECTED WIFI_EVENT_STA_DISCONNECTED

hw_timer_t* watchDogTimer = NULL;

void IRAM_ATTR watchDogInterrupt() {
    LOG_PRINTLN("watchdog triggered reboot");
    LOG_FLUSH();
    ESP.restart();
}
#else
#define USING_TIM_DIV256 true
#define WIFI_DISCONNECTED WIFI_EVENT_STAMODE_DISCONNECTED
#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

#include <ESP8266Wifi.h>
// #include <ESP8266WiFiGeneric.h>
#include <ESPAsyncTCP.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include "ESP8266TimerInterrupt.h"

ESP8266Timer ITimer;

void IRAM_ATTR TimerHandler() {
    if (timer_pinged) {
        LOG_PRINTLN("watchdog triggered reboot");
        LOG_FLUSH();
        ESP.restart();
    } else {
        timer_pinged = true;
        LOG_PRINTLN("\nPING");
    }
}
#endif

#include <DNSServer.h>
#include <EEPROM.h>
#include "LittleFS.h"

#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>

#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

#if DECODE_AC
#include <ir_Daikin.h> 
#include <ir_Fujitsu.h>
#include <ir_Gree.h>
#include <ir_Haier.h>
#include <ir_Kelvinator.h>
#include <ir_Midea.h>
#include <ir_Toshiba.h>
#endif  // DECODE_AC


// LED is connected to GPIO2 on these boards
#ifdef esp32
#define INIT_LED { pinMode(2, OUTPUT); digitalWrite(2, LOW); }
#define LED_ON   { digitalWrite(2, HIGH); }
#define LED_OFF  { digitalWrite(2, LOW); }
#else
#define INIT_LED { pinMode(2, OUTPUT); digitalWrite(2, HIGH); }
#define LED_ON   { digitalWrite(2, LOW); }
#define LED_OFF  { digitalWrite(2, HIGH); }
#endif

#define EEPROM_SIZE 256
#define HOSTNAME_LEN 32
#define WIFI_SSID_LEN 32
#define WIFI_PASSWD_LEN 64

#define DEFAULT_HOSTNAME            "lolin-ir-blaster"

#define CFG_NOT_SET                 0x0
#define CFG_SET                     0x9

typedef unsigned char tiny_int;

typedef struct config_type {
    tiny_int hostname_flag;
    char hostname[HOSTNAME_LEN];
    tiny_int ssid_flag;
    char ssid[WIFI_SSID_LEN];
    tiny_int ssid_pwd_flag;
    char ssid_pwd[WIFI_PASSWD_LEN];
} CONFIG_TYPE;

CONFIG_TYPE config;

void watchDogRefresh();
void blink();

void wireConfig();
void wireWebServerAndPaths();
void wireArduinoOTA(const char* hostname);

void onOTAStart();
void onOTAProgress(size_t current, size_t final);
void onOTAEnd(bool success);

void updateHtmlTemplate(String template_filename, bool showTime);

#ifdef ENABLE_DEBUG 
void checkForRemoteCommand();
#endif

void saveConfig(String hostname,
                String ssid,
                String ssid_pwd);

void wipeConfig();

String getTimestamp();
boolean isNumeric(String str);
void printHeapStats();

WiFiMode_t wifimode = WIFI_AP;

bool esp_reboot_requested = false;
unsigned long ota_progress_millis = 0;

bool setup_needs_update = false;
bool ap_mode_activity = false;

// Set web server port number to 80
AsyncWebServer server(80);

DNSServer dnsServer;
const byte DNS_PORT = 53;

WiFiClient wifiClient;
int wifiState = WIFI_EVENT_MAX;


// ==================== start of TUNEABLE PARAMETERS ====================
// An IR detector/demodulator is connected to GPIO pin 14
// e.g. D5 on a NodeMCU board.
#define RECV_PIN D4

// As this program is a special purpose capture/decoder, let us use a larger
// than normal buffer so we can handle Air Conditioner remote codes.
#define CAPTURE_BUFFER_SIZE 1024

// TIMEOUT is the Nr. of milli-Seconds of no-more-data before we consider a
// message ended.
// This parameter is an interesting trade-off. The longer the timeout, the more
// complex a message it can capture. e.g. Some device protocols will send
// multiple message packets in quick succession, like Air Conditioner remotes.
// Air Coniditioner protocols often have a considerable gap (20-40+ms) between
// packets.
// The downside of a large timeout value is a lot of less complex protocols
// send multiple messages when the remote's button is held down. The gap between
// them is often also around 20+ms. This can result in the raw data be 2-3+
// times larger than needed as it has captured 2-3+ messages in a single
// capture. Setting a low timeout value can resolve this.
// So, choosing the best TIMEOUT value for your use particular case is
// quite nuanced. Good luck and happy hunting.
// NOTE: Don't exceed MAX_TIMEOUT_MS. Typically 130ms.
#if DECODE_AC
#define TIMEOUT 50U  // Some A/C units have gaps in their protocols of ~40ms.
                     // e.g. Kelvinator
                     // A value this large may swallow repeats of some protocols
#else  // DECODE_AC
#define TIMEOUT 15U  // Suits most messages, while not swallowing many repeats.
#endif  // DECODE_AC
// Alternatives:
// #define TIMEOUT 90U  // Suits messages with big gaps like XMP-1 & some aircon
                        // units, but can accidentally swallow repeated messages
                        // in the rawData[] output.
// #define TIMEOUT MAX_TIMEOUT_MS  // This will set it to our currently allowed
                                   // maximum. Values this high are problematic
                                   // because it is roughly the typical boundary
                                   // where most messages repeat.
                                   // e.g. It will stop decoding a message and
                                   //   start sending it to serial at precisely
                                   //   the time when the next message is likely
                                   //   to be transmitted, and may miss it.

// Set the smallest sized "UNKNOWN" message packets we actually care about.
// This value helps reduce the false-positive detection rate of IR background
// noise as real messages. The chances of background IR noise getting detected
// as a message increases with the length of the TIMEOUT value. (See above)
// The downside of setting this message too large is you can miss some valid
// short messages for protocols that this library doesn't yet decode.
//
// Set higher if you get lots of random short UNKNOWN messages when nothing
// should be sending a message.
// Set lower if you are sure your setup is working, but it doesn't see messages
// from your device. (e.g. Other IR remotes work.)
// NOTE: Set this value very high to effectively turn off UNKNOWN detection.
#define MIN_UNKNOWN_SIZE 20
// ==================== end of TUNEABLE PARAMETERS ====================

decode_results results;  // Somewhere to store the results// Use turn on the save buffer feature for more complete capture coverage.
IRrecv irrecv(RECV_PIN, CAPTURE_BUFFER_SIZE, TIMEOUT, true);

#define IR_LED D3  
IRsend irsend(IR_LED);  // Set the GPIO to be used to sending the message.

#ifdef DECODE_AC
void dumpACInfo(decode_results* results);
#endif