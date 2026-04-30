/*
 * ESP32 Firebase IoT Cihaz
 * v2 - RTDB SSE Streaming (polling yok, anlik komut alma)
 *
 * Gerekli kutuphaneler:
 *   - ArduinoJson  (Benoit Blanchon)
 *   - WiFiClientSecure, HTTPClient (ESP32 core ile gelir)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// =====================================================
//  KONFIGURASYON
// =====================================================
const char* WIFI_SSID     = "RKTurknet";
const char* WIFI_PASSWORD = "22122018";

const char* FIREBASE_API_KEY    = "AIzaSyAK1NJ-dfAkrqESYN1ort95UnGJg-YWxAE";
const char* FIREBASE_PROJECT_ID = "website-gozupekteknoloji";
const char* RTDB_HOST           = "website-gozupekteknoloji-default-rtdb.firebaseio.com";
// =====================================================

Preferences      prefs;
String           serialCode   = "";
bool             lastCommand  = false;
bool             sseConnected = false;
unsigned long    lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 25000; // 25 sn -- online isaretini taze tut

// SSE icin kalici baglanti
WiFiClientSecure sseClient;

// SSE satirlarini biriktirmek icin tampon
String sseBuffer   = "";
String sseEvent    = "";
String sseData     = "";

// ------------------------------------------------------------------
// Seri kod uret
// ------------------------------------------------------------------
String generateSerialCode() {
    const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    String code = "ESP-";
    for (int i = 0; i < 6; i++) code += chars[random(0, 36)];
    return code;
}

// ------------------------------------------------------------------
// Firestore URL
// ------------------------------------------------------------------
String firestoreUrl(const String& id) {
    return "https://firestore.googleapis.com/v1/projects/" +
           String(FIREBASE_PROJECT_ID) +
           "/databases/(default)/documents/devices/" +
           id + "?key=" + String(FIREBASE_API_KEY);
}

// ------------------------------------------------------------------
// Firestore'a cihaz kaydet (sahiplik verisi burada tutuluyor)
// ------------------------------------------------------------------
bool registerInFirestore(const String& deviceId) {
    Serial.println("[Firestore] Kayit kontrol ediliyor...");
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    http.begin(client, firestoreUrl(deviceId));
    int code = http.GET();
    Serial.printf("[Firestore] HTTP %d\n", code);

    if (code == 200) {
        String body = http.getString();
        http.end();
        Serial.println(body.indexOf("ownerUid") >= 0
            ? "[Firestore] Cihaz bir hesaba bagli."
            : "[Firestore] Cihaz kayitli, henuz sahipsiz.");
        return true;
    }
    http.end();

    if (code == 404) {
        Serial.println("[Firestore] Ilk kayit yapiliyor...");
        String payload = "{\"fields\":{"
            "\"serialCode\":{\"stringValue\":\"" + deviceId + "\"},"
            "\"status\":{\"booleanValue\":false},"
            "\"claimed\":{\"booleanValue\":false}}}";

        WiFiClientSecure c2; c2.setInsecure();
        HTTPClient h2;
        h2.begin(c2, firestoreUrl(deviceId));
        h2.addHeader("Content-Type", "application/json");
        int pc = h2.PATCH(payload);
        h2.end();
        Serial.printf("[Firestore] PATCH HTTP %d\n", pc);
        return pc == 200 || pc == 201;
    }

    Serial.printf("[Firestore] Beklenmedik HTTP: %d\n", code);
    return false;
}

// ------------------------------------------------------------------
// RTDB'ye deger yaz  (PUT)
// ------------------------------------------------------------------
bool rtdbPut(const String& path, const String& value) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://" + String(RTDB_HOST) + path + ".json");
    http.addHeader("Content-Type", "application/json");
    int code = http.PUT(value);
    http.end();
    return code == 200;
}

// ------------------------------------------------------------------
// SSE baglantisini kur
// ------------------------------------------------------------------
bool connectSSE() {
    sseClient.stop();
    sseClient.setInsecure();
    sseBuffer = "";
    sseEvent  = "";
    sseData   = "";

    String path = "/devices/" + serialCode + ".json";

    Serial.println("[SSE] Sunucuya baglaniliyor...");
    if (!sseClient.connect(RTDB_HOST, 443)) {
        Serial.println("[SSE] Baglantı kurulamadi!");
        return false;
    }

    // HTTP GET -- SSE headers
    sseClient.print("GET " + path + " HTTP/1.1\r\n");
    sseClient.print("Host: " + String(RTDB_HOST) + "\r\n");
    sseClient.print("Accept: text/event-stream\r\n");
    sseClient.print("Cache-Control: no-cache\r\n");
    sseClient.print("Connection: keep-alive\r\n");
    sseClient.print("\r\n");

    // HTTP response header satirlarini oku ve atla
    unsigned long t = millis();
    while (sseClient.connected() && millis() - t < 5000) {
        String line = sseClient.readStringUntil('\n');
        if (line == "\r" || line.isEmpty()) break;
    }

    Serial.println("[SSE] Baglantı kuruldu! Komutlar aninda alinacak.");
    return true;
}

// ------------------------------------------------------------------
// Gelen SSE event'i isle
// ------------------------------------------------------------------
void processSSEEvent(const String& event, const String& data) {
    if (event != "put" && event != "patch") return;
    if (data.isEmpty() || data == "null")   return;

    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, data)) {
        Serial.println("[SSE] JSON parse hatasi");
        return;
    }

    const char* path = doc["path"] | "/";
    bool newCommand  = lastCommand;

    if (strcmp(path, "/") == 0) {
        // Ilk baglantiginda tam veri gelir
        newCommand = doc["data"]["command"] | false;
    } else if (strcmp(path, "/command") == 0) {
        // Sadece command alani degisti
        newCommand = doc["data"] | false;
    } else {
        return; // Baska bir alan degisti (online vs.), ilgilenmiyoruz
    }

    if (newCommand != lastCommand) {
        lastCommand = newCommand;
        Serial.println("==================================");
        Serial.print(">>> KOMUT: ");
        Serial.println(newCommand ? "ACIK" : "KAPALI");
        Serial.println("==================================");

        // Buraya role veya cikis kodu ekle:
        // digitalWrite(RELAY_PIN, newCommand ? HIGH : LOW);
    }
}

// ------------------------------------------------------------------
// SSE stream'den gelen karakterleri satirlara bolup isle
// ------------------------------------------------------------------
void readSSE() {
    if (!sseClient.connected()) {
        sseConnected = false;
        return;
    }

    while (sseClient.available()) {
        char c = sseClient.read();

        if (c == '\n') {
            String line = sseBuffer;
            sseBuffer = "";
            line.trim();

            if (line.startsWith("event:")) {
                sseEvent = line.substring(6);
                sseEvent.trim();
            } else if (line.startsWith("data:")) {
                sseData = line.substring(5);
                sseData.trim();
            } else if (line.isEmpty() && !sseEvent.isEmpty()) {
                processSSEEvent(sseEvent, sseData);
                sseEvent = "";
                sseData  = "";
            }
        } else {
            sseBuffer += c;
        }
    }
}

// ------------------------------------------------------------------
// setup
// ------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Brownout devre disi
    delay(500);

    Serial.println("\n=============================");
    Serial.println("  ESP32 IoT Cihaz  v2");
    Serial.println("=============================");

    // Seri kodu NVS'den oku veya uret
    prefs.begin("device", false);
    serialCode = prefs.getString("serial_code", "");
    if (serialCode.isEmpty()) {
        randomSeed(esp_random());
        serialCode = generateSerialCode();
        prefs.putString("serial_code", serialCode);
        Serial.println("Yeni seri kodu olusturuldu.");
    } else {
        Serial.println("Seri kodu NVS'den yuklendi.");
    }
    prefs.end();

    Serial.println("-----------------------------");
    Serial.print  ("SERI KOD : ");
    Serial.println(serialCode);
    Serial.println("-----------------------------");
    Serial.println("Bu kodu websitesinde 'Cihaz Ekle' alanina girin.\n");

    // WiFi
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("WiFi baglaniliyor: %s\n", WIFI_SSID);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        if (++attempts > 40) {
            Serial.println("\nWiFi basarisiz! Yeniden baslatiliyor...");
            ESP.restart();
        }
    }
    Serial.printf("\nWiFi baglandi. IP: %s\n\n", WiFi.localIP().toString().c_str());

    // Firestore kayit
    registerInFirestore(serialCode);

    // RTDB lastSeen yaz -- sunucu zamani ile
    Serial.println("[RTDB] lastSeen yaziliyor...");
    rtdbPut("/devices/" + serialCode + "/lastSeen", "{\".sv\":\"timestamp\"}");
    lastHeartbeat = millis();

    // SSE baglantisi
    sseConnected = connectSSE();
    if (!sseConnected) {
        Serial.println("SSE baglantisi kurulamadi, yeniden baslatiliyor...");
        delay(3000);
        ESP.restart();
    }

    Serial.println("\nHazir! Komutlar aninda alinacak.\n");
}

// ------------------------------------------------------------------
// loop
// ------------------------------------------------------------------
void loop() {
    // WiFi kontrolu
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi kesildi, yeniden baglaniliyor...");
        sseConnected = false;
        WiFi.reconnect();
        delay(5000);
        return;
    }

    // SSE baglantisi koptu mu?
    if (!sseConnected || !sseClient.connected()) {
        Serial.println("[SSE] Baglantı kesildi, yeniden baglaniliyor...");
        delay(2000);
        sseConnected = connectSSE();
        if (sseConnected) {
            rtdbPut("/devices/" + serialCode + "/lastSeen", "{\".sv\":\"timestamp\"}");
            lastHeartbeat = millis();
        }
        return;
    }

    // SSE oku (bloklamaz, sadece veri varsa okur)
    readSSE();

    // Heartbeat -- lastSeen'i taze tut
    if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL) {
        lastHeartbeat = millis();
        rtdbPut("/devices/" + serialCode + "/lastSeen", "{\".sv\":\"timestamp\"}");
    }
}
