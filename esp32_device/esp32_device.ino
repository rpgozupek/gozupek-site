/*
 * ESP32 Firebase IoT Cihaz
 * Gerekli kütüphaneler (Arduino Library Manager'dan yükle):
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
//  KONFİGÜRASYON — buraya kendi bilgilerini gir
// =====================================================
const char* WIFI_SSID     = "RKTurknet";
const char* WIFI_PASSWORD = "22122018";

const char* FIREBASE_API_KEY     = "AIzaSyAK1NJ-dfAkrqESYN1ort95UnGJg-YWxAE";
const char* FIREBASE_PROJECT_ID  = "website-gozupekteknoloji";
// =====================================================

Preferences prefs;
String serialCode  = "";
bool   lastStatus  = false;
bool   deviceReady = false;          // Firebase'e kayıt başarılı mı?

const unsigned long POLL_INTERVAL = 5000;  // ms
unsigned long lastPollMs = 0;

// ------------------------------------------------------------------
// Yardımcı: 6 haneli rastgele seri kodu üret  (örn. ESP-A3F2B1)
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
// Cihazı Firestore'a kaydet (henüz sahip yok)
// ------------------------------------------------------------------
bool registerDevice(const String& deviceId) {
    WiFiClientSecure client;
    client.setInsecure();          // Sertifika doğrulamasını atla (prototip)

    HTTPClient http;
    http.begin(client, firestoreUrl(deviceId));

    // Önce belgey var mı diye bak
    int getCode = http.GET();
    if (getCode == 200) {
        // Zaten kayıtlı — status alanı var mı kontrol et
        String body = http.getString();
        http.end();
        Serial.println("Cihaz Firebase'de zaten kayıtlı.");
        return true;
    }
    http.end();

    // 404 → yeni kayıt oluştur (PATCH ile otomatik create)
    http.begin(client, firestoreUrl(deviceId));
    http.addHeader("Content-Type", "application/json");

    // Firestore REST formatı: her alan tip etiketiyle sarılır
    String payload = "{\"fields\":{"
        "\"serialCode\":{\"stringValue\":\"" + deviceId + "\"},"
        "\"status\":{\"booleanValue\":false},"
        "\"claimed\":{\"booleanValue\":false}"
        "}}";

    int patchCode = http.PATCH(payload);
    http.end();

    if (patchCode == 200 || patchCode == 201) {
        Serial.println("Firebase kaydı başarılı.");
        return true;
    }
    Serial.printf("Firebase kayıt hatası, HTTP: %d\n", patchCode);
    return false;
}

// ------------------------------------------------------------------
// Firestore'dan status oku, değişince Serial'a yaz
// ------------------------------------------------------------------
void pollStatus(const String& deviceId) {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.begin(client, firestoreUrl(deviceId));
    int code = http.GET();

    if (code != 200) {
        Serial.printf("[Poll] HTTP hatası: %d\n", code);
        http.end();
        return;
    }

    String body = http.getString();
    http.end();

    StaticJsonDocument<1024> doc;
    if (deserializeJson(doc, body)) {
        Serial.println("[Poll] JSON parse hatası");
        return;
    }

    // Firestore boolean değeri: fields.status.booleanValue
    bool newStatus = doc["fields"]["status"]["booleanValue"] | false;

    if (newStatus != lastStatus) {
        lastStatus = newStatus;
        Serial.println("==================================");
        Serial.print(">>> DURUM DEĞİŞTİ: ");
        Serial.println(newStatus ? "AÇIK" : "KAPALI");
        Serial.println("==================================");

        // Buraya röle / LED kodu ekle:
        // digitalWrite(RELAY_PIN, newStatus ? HIGH : LOW);
    }
}

// ------------------------------------------------------------------
// setup
// ------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n=============================");
    Serial.println("  ESP32 IoT Cihaz Başlatılıyor");
    Serial.println("=============================");

    // Seri kodu NVS'den oku; yoksa üret ve kaydet
    prefs.begin("device", false);
    serialCode = prefs.getString("serial_code", "");
    if (serialCode.isEmpty()) {
        randomSeed(esp_random());
        serialCode = generateSerialCode();
        prefs.putString("serial_code", serialCode);
        Serial.println("Yeni seri kodu oluşturuldu.");
    }
    prefs.end();

    Serial.println("");
    Serial.println("┌─────────────────────────────────┐");
    Serial.print  ("│  CİHAZ SERİ KODU : ");
    Serial.print  (serialCode);
    Serial.println("  │");
    Serial.println("└─────────────────────────────────┘");
    Serial.println("Bu kodu websitesinde 'Cihaz Ekle' alanına girin.\n");

    // WiFi bağlantısı
    Serial.printf("WiFi'ye bağlanılıyor: %s\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        if (++attempts > 40) {
            Serial.println("\nWiFi bağlantısı başarısız! Cihaz yeniden başlatılıyor...");
            ESP.restart();
        }
    }
    Serial.printf("\nWiFi bağlandı. IP: %s\n\n", WiFi.localIP().toString().c_str());

    // Firebase kaydı
    deviceReady = registerDevice(serialCode);
    if (!deviceReady) {
        Serial.println("UYARI: Firebase kaydı başarısız. Polling yine de deneniyor...");
        deviceReady = true;  // yine de poll dene
    }

    Serial.println("Hazır! Websitesinden kontrol başladı.\n");
}

// ------------------------------------------------------------------
// loop
// ------------------------------------------------------------------
void loop() {
    if (millis() - lastPollMs >= POLL_INTERVAL) {
        lastPollMs = millis();

        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi bağlantısı kesildi, yeniden bağlanılıyor...");
            WiFi.reconnect();
            return;
        }

        if (deviceReady) {
            pollStatus(serialCode);
        }
    }
}
