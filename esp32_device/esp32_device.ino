/*
 * ESP32 Firebase IoT Cihaz
 * Gerekli kutuphaneler (Arduino Library Manager'dan yukle):
 *   - ArduinoJson  (Benoit Blanchon)
 *   - WiFiClientSecure (ESP32 core ile birlikte gelir)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// =====================================================
//  KONFIGURASYON -- buraya kendi bilgilerini gir
// =====================================================
const char* WIFI_SSID     = "WIFI_ADINIZI_GIRIN";
const char* WIFI_PASSWORD = "WIFI_SIFRENIZI_GIRIN";

const char* FIREBASE_API_KEY     = "AIzaSyAK1NJ-dfAkrqESYN1ort95UnGJg-YWxAE";
const char* FIREBASE_PROJECT_ID  = "website-gozupekteknoloji";
// =====================================================

Preferences prefs;
String serialCode  = "";
bool   lastStatus  = false;
bool   deviceReady = false;

// Gecikmeyi azaltmak icin poll araligini 2 sn yaptik
const unsigned long POLL_INTERVAL = 2000;
unsigned long lastPollMs = 0;

// TLS istemcisini global tut -- her seferinde yeniden olusturmak
// zaman alir (handshake ~1-2 sn). Boylece baglanti daha hizli.
WiFiClientSecure tlsClient;
HTTPClient       http;

// ------------------------------------------------------------------
// 6 haneli rastgele seri kodu uret  (ornek: ESP-A3F2B1)
// ------------------------------------------------------------------
String generateSerialCode() {
    const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    String code = "ESP-";
    for (int i = 0; i < 6; i++) code += chars[random(0, 36)];
    return code;
}

// ------------------------------------------------------------------
// Firestore REST URL
// ------------------------------------------------------------------
String firestoreUrl(const String& deviceId) {
    return "https://firestore.googleapis.com/v1/projects/" +
           String(FIREBASE_PROJECT_ID) +
           "/databases/(default)/documents/devices/" +
           deviceId + "?key=" + String(FIREBASE_API_KEY);
}

// ------------------------------------------------------------------
// Cihazi Firestore'a kaydet
// ------------------------------------------------------------------
bool registerDevice(const String& deviceId) {
    Serial.println("[Kayit] Firestore kontrol ediliyor...");

    http.begin(tlsClient, firestoreUrl(deviceId));
    int getCode = http.GET();

    Serial.printf("[Kayit] GET yaniti: HTTP %d\n", getCode);

    if (getCode == 200) {
        // Belge var -- claimed mi kontrol et
        String body = http.getString();
        http.end();

        StaticJsonDocument<512> doc;
        DeserializationError err = deserializeJson(doc, body);
        if (!err) {
            bool claimed = doc["fields"]["claimed"]["booleanValue"] | false;
            if (claimed) {
                Serial.println("[Kayit] Bu cihaz zaten bir hesaba bagli.");
            } else {
                Serial.println("[Kayit] Cihaz Firebase'de kayitli, henuz sahipsiz. Hazir.");
            }
        } else {
            Serial.println("[Kayit] JSON parse hatasi ama belge var, devam ediliyor.");
        }
        return true;
    }

    http.end();

    if (getCode == 404) {
        // Belge yok -- ilk kez kayit
        Serial.println("[Kayit] Cihaz yok, ilk kayit yapiliyor...");

        String payload = "{\"fields\":{"
            "\"serialCode\":{\"stringValue\":\"" + deviceId + "\"},"
            "\"status\":{\"booleanValue\":false},"
            "\"claimed\":{\"booleanValue\":false}"
            "}}";

        http.begin(tlsClient, firestoreUrl(deviceId));
        http.addHeader("Content-Type", "application/json");
        int patchCode = http.PATCH(payload);
        http.end();

        Serial.printf("[Kayit] PATCH yaniti: HTTP %d\n", patchCode);

        if (patchCode == 200 || patchCode == 201) {
            Serial.println("[Kayit] Firebase kaydi basarili!");
            return true;
        }
        Serial.printf("[Kayit] HATA: Kayit basarisiz (HTTP %d)\n", patchCode);
        return false;
    }

    // 403, 400 vs. -- kurallarda sorun olabilir
    Serial.printf("[Kayit] Beklenmedik HTTP kodu: %d -- Firestore kurallari kontrol edilmeli\n", getCode);
    return false;
}

// ------------------------------------------------------------------
// Firestore'dan status oku, degisince Serial'a yaz
// ------------------------------------------------------------------
void pollStatus(const String& deviceId) {
    http.begin(tlsClient, firestoreUrl(deviceId));
    int code = http.GET();

    if (code != 200) {
        Serial.printf("[Poll] HTTP hatasi: %d\n", code);
        http.end();
        return;
    }

    String body = http.getString();
    http.end();

    StaticJsonDocument<1024> doc;
    if (deserializeJson(doc, body)) {
        Serial.println("[Poll] JSON parse hatasi");
        return;
    }

    bool newStatus = doc["fields"]["status"]["booleanValue"] | false;

    if (newStatus != lastStatus) {
        lastStatus = newStatus;
        // Sadece ASCII karakterler kullaniyoruz -- bozuk yazi olmaz
        Serial.println("==================================");
        Serial.print(">>> DURUM: ");
        Serial.println(newStatus ? "ACIK" : "KAPALI");
        Serial.println("==================================");

        // Buraya role / LED kodu ekle:
        // digitalWrite(RELAY_PIN, newStatus ? HIGH : LOW);
    }
}

// ------------------------------------------------------------------
// setup
// ------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(500);

    // Turkce ve kutu karakterleri Serial'da bozuluyor
    // bu yuzden tamamen ASCII kullaniyoruz
    Serial.println("\n=============================");
    Serial.println("  ESP32 IoT Cihaz");
    Serial.println("=============================");

    // Seri kodu NVS'den oku; yoksa uret ve kaydet
    prefs.begin("device", false);
    serialCode = prefs.getString("serial_code", "");
    if (serialCode.isEmpty()) {
        randomSeed(esp_random());
        serialCode = generateSerialCode();
        prefs.putString("serial_code", serialCode);
        Serial.println("Yeni seri kodu olusturuldu.");
    } else {
        Serial.println("Mevcut seri kodu NVS'den yuklendi.");
    }
    prefs.end();

    Serial.println("-----------------------------");
    Serial.print("SERI KOD : ");
    Serial.println(serialCode);
    Serial.println("-----------------------------");
    Serial.println("Bu kodu websitesinde 'Cihaz Ekle' alanina girin.");
    Serial.println();

    // TLS -- sertifika dogrulamasi yok (prototip)
    tlsClient.setInsecure();

    // WiFi baglantisi
    Serial.printf("WiFi'ye baglaniliyor: %s\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        if (++attempts > 40) {
            Serial.println("\nWiFi baglantisi basarisiz! Yeniden baslatiliyor...");
            ESP.restart();
        }
    }
    Serial.printf("\nWiFi baglandi. IP: %s\n\n", WiFi.localIP().toString().c_str());

    // Firebase kaydi
    deviceReady = registerDevice(serialCode);
    if (!deviceReady) {
        Serial.println("UYARI: Firebase kaydi basarisiz. Polling yine de deneniyor...");
        deviceReady = true;
    }

    Serial.println("\nHazir! Websitesinden kontrol basliyor...");
    Serial.println();
}

// ------------------------------------------------------------------
// loop
// ------------------------------------------------------------------
void loop() {
    if (millis() - lastPollMs >= POLL_INTERVAL) {
        lastPollMs = millis();

        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi kesildi, yeniden baglaniliyor...");
            WiFi.reconnect();
            return;
        }

        if (deviceReady) {
            pollStatus(serialCode);
        }
    }
}
