#include <M5Atom.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Ambient.h>
#include <M5_ENV.h>
#include <HTTPClient.h>
#include <UrlEncode.h>
#include <ArduinoOTA.h>
#include ".mysecret.h"

// .mysecret.h には下記内容が含まれる必要がある
// 
// const char* SSID = "xxx";
// const char* PASSPHRASE = "xxx";
// const char* AMB_WRITEKEY = "xxx";
// const uint32_t AMB_CHANNELID = xxx;
// const char* LINE_TOKEN = "xxx";


const uint8_t WIFI_CONNECTION_RETRIES = 5;
const uint8_t DIGITAL_PIN = 23;
const uint8_t ANALOG_PIN = 33;
const float MAX_VOLTAGE = 3.3;  // アナログ入力は0.0Vから3.3Vの電圧として入力される
const uint16_t MAX_RAIN_VAL = 4095; // AD変換後の値の最大値。実際にプログラムから見えるアナログ入力はAD変換された0から4095の整数値
const uint16_t RAIN_VAL_THRESHOLD = MAX_RAIN_VAL - 5;   // AD変換後の値がどれくらいだったら雨が降り始めたと判定するか。
                                                        // 乾いているときは安定してMAX_RAIN_VALになるので多分 -1 でも大丈夫なくらい。
                                                        // ただし雨が降っているときでもMAX_RAIN_VALに近い値になることもあるので、
                                                        // ここでは -5 くらいにしておく。
const uint16_t RENOTIFICATION_RAIN_VAL_THRESHOLD = 1500;    // AD変換後の値がこの値未満だったら降雨中でも再通知する。
                                                            // ただし前回降雨中通知からRENOTIFICATION_INTERVALの時間が
                                                            // 経過している場合のみ通知。
                                                            // これは水滴の不純物によって乾燥後も
                                                            // 雨滴センサに通電し続けていると思われる現象が見られており、
                                                            // その状態でまた雨が降った場合の見落とし対策である。
const uint32_t RENOTIFICATION_INTERVAL = 1800000;   // 再通知の抑止期間(ミリ秒)。
                                                    // ここでいう再通知は、RENOTIFICATION_RAIN_VAL_THRESHOLDの説明を参照。
                                                    // 前回通知から当項目の時間(ミリ秒)が経過している場合のみ再通知する。
                                                    // 前回の降雨中通知から30分間は油断しないだろうと思われることと、
                                                    // 短すぎると通知が多すぎるのでこの値とする。
const uint16_t CONTINUOUS_DRY_TIME = 10000; // 乾燥したと思われる値(RAIN_VAL_THRESHOLD)以上の観測が、
                                            // 何ミリ秒連続したら本当に乾燥したとみなすか

WebServer server(80);
WiFiClient client;
Ambient ambient;
SHT3X sht30;
QMP6988 qmp6988;

void sendToLine(const char *msg) {
    Serial.println(msg);

    HTTPClient http;
    http.begin("https://notify-api.line.me/api/notify");
    http.addHeader("Authorization", "Bearer " + String(LINE_TOKEN));
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    int statusCode = http.POST("message=" + urlEncode(msg));
}

void setupOTA() {
    // [reference] 
    // https://github.com/espressif/arduino-esp32/blob/master/libraries/ArduinoOTA/examples/BasicOTA/BasicOTA.ino
    // https://docs.platformio.org/en/latest/platforms/espressif32.html#over-the-air-ota-update

    ArduinoOTA
        .onStart([]() {
                String type;
                if (ArduinoOTA.getCommand() == U_FLASH)
                type = "sketch";
                else // U_SPIFFS
                type = "filesystem";

                // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
                Serial.println("Start updating " + type);
                })
    .onEnd([]() {
            Serial.println("\nEnd");
            })
    .onProgress([](unsigned int progress, unsigned int total) {
            Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
            })
    .onError([](ota_error_t error) {
            Serial.printf("Error[%u]: ", error);
            if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
            else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
            else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
            else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
            else if (error == OTA_END_ERROR) Serial.println("End Failed");
            });

    ArduinoOTA.setHostname("m5atom"); // ArduinoOTA内でホスト名が管理されているっぽいので独自にMDNS.begin()は呼ばないこと

    ArduinoOTA.begin();
}

void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) {
        return;
    }

    WiFi.begin(SSID, PASSPHRASE);
    // うまく繋がらず無限ループ状態になるときは WiFi.status() が 6 (WL_DISCONNECTED) を返している。
    // M5Stackをリセットするとすぐ繋がるので一定回数試みたらリセットする。
    uint8_t i;
    for (i = 0; i < WIFI_CONNECTION_RETRIES; i++) {
        if (WiFi.status() == WL_CONNECTED) {
            break;
        } else {
            delay(500);
            Serial.print(".");
        }
    }

    if (i == WIFI_CONNECTION_RETRIES) {
        ESP.restart();
    }

    Serial.print("\r\nWiFi connected\r\nIP address: ");
    Serial.println(WiFi.localIP());
}

void handleRoot() {
    server.send(200, "text/plain", "rain-detection is working.");
}

void handleNotFound() {
    server.send(404, "text/plain", "File Not Found.");
    Serial.println("File Not Found");
}

void setupHTTPServer() {
    server.on("/", handleRoot);
    server.onNotFound(handleNotFound);

    server.begin();
    Serial.println("HTTP server started");
}

void setup() {
    M5.begin();

    pinMode(DIGITAL_PIN, INPUT);
    Wire.begin();
    qmp6988.init();

    connectWiFi();
    setupOTA();
    setupHTTPServer();

    ambient.begin(AMB_CHANNELID, AMB_WRITEKEY, &client);
    sendToLine("起動完了");
}

void loop() {
    char msg[256];
    static bool isRaining = false;
    static unsigned long lastSentTime = 0L;
    static unsigned long lastRainingTime = 0L;
    static unsigned long lastRainingNotificationTime = 0L;
    float pressure;
    float temperature;
    float humidity;
    uint16_t rainVal;

    ArduinoOTA.handle();
    M5.update();

    server.handleClient();

    if (M5.Btn.wasPressed()) {
    }

    // 雨の降り始め検出は即応性が欲しいので毎ループ確認
    rainVal = analogRead(ANALOG_PIN);

    if (rainVal < RAIN_VAL_THRESHOLD) {
        // 初めて降雨中になったタイミングまたは、降雨中でも再通知する閾値未満で再通知抑止期間も経過している場合
        if ( (! isRaining)
                || ((rainVal < RENOTIFICATION_RAIN_VAL_THRESHOLD)
                    && ((millis() - lastRainingNotificationTime) > RENOTIFICATION_INTERVAL))
           ) {
            isRaining = true;
            snprintf(msg, sizeof(msg), "雨が降り始めたよ！ (rainVal=%u)", rainVal);
            Serial.println(msg);
            sendToLine(msg);
            lastRainingNotificationTime = millis();
        }
        lastRainingTime = millis();
    } else {
        if (isRaining) {
            // センサが乾ききる直前は閾値の上下を行ったり来たりするので乾いた判定は遅延させる(即応性は不要)
            if ((millis() - lastRainingTime) > CONTINUOUS_DRY_TIME) {
                isRaining = false;
                snprintf(msg, sizeof(msg), "水滴が乾いたよ（雨はとっくに上がったよ）。 (rainVal=%u)", rainVal);
                Serial.println(msg);
                sendToLine(msg);
            }
        }
    }

    // M5Stack Grayではよく接続が切れてたので切れていたら再接続(Atom Liteでは切れるかは不明)
    connectWiFi();

    // Ambientは1チャネル当たりデータ登録数が1日3,000件という制限があるため30秒毎に送信
    if (lastSentTime == 0L || (millis() - lastSentTime) > 30000) {
        pressure = qmp6988.calcPressure();

        if (sht30.get() == 0) {
            temperature = sht30.cTemp;   
            humidity = sht30.humidity;
        } else {
            temperature = 0;
            humidity = 0;
        }

        ambient.set(1, isRaining);
        ambient.set(2, pressure);
        ambient.set(3, temperature);
        ambient.set(4, humidity);
        // 5番は元々rainVoltageを送っていたが使わなそうなので削除
        ambient.set(6, rainVal);
        ambient.send();
        lastSentTime = millis();
        snprintf(msg, sizeof(msg), "raining=%d, pressure=%f, temperature=%f, humidity=%f, rainVal=%u", isRaining, pressure, temperature, humidity, rainVal);
        Serial.println(msg);
    }
}
