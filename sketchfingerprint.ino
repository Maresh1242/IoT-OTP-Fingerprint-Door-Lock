
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <Keypad.h>
#include <GyverOLED.h>
#include <Adafruit_Fingerprint.h>
#include <Preferences.h>
#include <Wire.h>

// ---------- CONFIG (edit) ----------
const char* WIFI_SSID  = "wifi username";
const char* WIFI_PASS  = "password";
const char* BOT_TOKEN  = "8425229702:AAHvVLfUlng8w_A_MF0w4nlqTO_l6vRlGtI";

// Fingerprint UART (Serial2)
#define FINGER_RX_PIN 4    // sensor TX -> ESP32 RX2
#define FINGER_TX_PIN 13   // sensor RX <- ESP32 TX2
#define FINGER_BAUD   57600

// OLED I2C
#define OLED_SDA 21
#define OLED_SCL 22

// Keypad wiring
const byte ROWS = 4;
const byte COLS = 4;
char keysArr[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
// Avoid using pins 6..11, avoid 21/22 (I2C). These are safe choices:
byte rowPins[ROWS] = {32, 33, 25, 26};   // R1..R4
byte colPins[COLS] = {19, 18, 23, 5};    // C1..C4

// Relay and buzzer
const byte RELAY_PIN  = 17;
const byte BUZZER_PIN = 16;
const bool RELAY_ACTIVE_LOW = false; // set true if your relay module is active LOW

// OTP and timing
const int OTP_LEN = 6;
const unsigned long OTP_TIMEOUT_MS = 120000UL; // 2 minutes
const unsigned long TELEGRAM_POLL_MS = 2000UL; // poll interval

// Preferences namespace
const char* PREF_NS = "fpmap";

// ---------- Objects & Globals ----------
GyverOLED<SSH1106_128x64> oled;
Keypad keypad = Keypad(makeKeymap(keysArr), rowPins, colPins, ROWS, COLS);

HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger(&fingerSerial);

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

Preferences prefs;

String entered = "";
String currentOTP = "";
unsigned long otpStart = 0;
bool otpActive = false;
int expectedFingerID = -1;

unsigned long lastBotPoll = 0;

// ---------- UI helpers ----------
void showOLED(const char* a, const char* b = "", const char* c = "") {
  oled.clear();
  oled.setScale(2);
  oled.setCursorXY(0, 0);
  oled.print(a);
  if (b && strlen(b)) {
    oled.setScale(1);
    oled.setCursorXY(0, 20);
    oled.print(b);
  }
  if (c && strlen(c)) {
    oled.setScale(1);
    oled.setCursorXY(0, 34);
    oled.print(c);
  }
  oled.update();
}
void showSmallLines(const char* a, const char* b = "", const char* c = "") {
  oled.clear();
  oled.setScale(1);
  oled.setCursorXY(0,0); oled.print(a);
  if (b && strlen(b)) { oled.setCursorXY(0,10); oled.print(b); }
  if (c && strlen(c)) { oled.setCursorXY(0,20); oled.print(c); }
  oled.update();
}
void beep(int ms) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(ms);
  digitalWrite(BUZZER_PIN, LOW);
}

// ---------- Preferences mapping ----------
void saveMapping(int fid, const String &chatid) {
  if (fid <= 0) return;
  String key = String("f") + String(fid);
  prefs.putString(key.c_str(), chatid);
}
String loadMapping(int fid) {
  if (fid <= 0) return "";
  String key = String("f") + String(fid);
  return prefs.getString(key.c_str(), "");
}
void deleteMapping(int fid) {
  if (fid <= 0) return;
  String key = String("f") + String(fid);
  prefs.remove(key.c_str());
}
String listMappings() {
  String out="";
  for (int i=1;i<=200;i++){
    String v = loadMapping(i);
    if (v.length()) {
      out += "ID ";
      out += String(i);
      out += " -> ";
      out += v;
      out += "\n";
    }
  }
  if (out.length()==0) out = "No mappings stored.";
  return out;
}

// ---------- OTP & Telegram ----------
String generateOTP() {
  String s="";
  for (int i=0;i<OTP_LEN;i++) s += String(random(0,10));
  return s;
}
bool sendOtpToChat(const String &chatid, const String &otp) {
  if (String(BOT_TOKEN).length() < 10) {
    Serial.println("BOT_TOKEN empty - skipping Telegram send.");
    return false;
  }
  String text = "🔐 Your OTP: `" + otp + "`\nValid for " + String(OTP_TIMEOUT_MS/1000) + " seconds.";
  bool ok = bot.sendMessage(chatid, text, "Markdown");
  Serial.print("send OTP to "); Serial.print(chatid); Serial.print(" -> "); Serial.println(ok ? "OK" : "FAIL");
  return ok;
}

// ---------- Telegram handler ----------
void handleTelegram() {
  int num = bot.getUpdates(bot.last_message_received + 1);
  if (num <= 0) return;
  for (int i = 0; i < num; ++i) {
    String chat_id = String(bot.messages[i].chat_id);
    String text = bot.messages[i].text;
    text.trim();
    Serial.print("TELE msg from "); Serial.print(chat_id); Serial.print(": "); Serial.println(text);

    if (text.startsWith("/setme")) {
      int sp = text.indexOf(' ');
      if (sp > 0) {
        String sid = text.substring(sp+1); sid.trim();
        int fid = sid.toInt();
        if (fid > 0 && fid <= 200) {
          saveMapping(fid, chat_id);
          bot.sendMessage(chat_id, "Registered this chat for fingerprint ID " + String(fid), "");
          Serial.println("Saved mapping: " + String(fid) + " -> " + chat_id);
        } else bot.sendMessage(chat_id, "Usage: /setme <finger_id> (1..200)", "");
      } else bot.sendMessage(chat_id, "Usage: /setme <finger_id> (1..200)", "");
    }
    else if (text.startsWith("/unset")) {
      int sp = text.indexOf(' ');
      if (sp > 0) {
        String sid = text.substring(sp+1); sid.trim();
        int fid = sid.toInt();
        if (fid > 0 && fid <= 200) {
          deleteMapping(fid);
          bot.sendMessage(chat_id, "Mapping for fingerprint ID " + String(fid) + " removed.", "");
          Serial.println("Removed mapping ID " + String(fid));
        } else bot.sendMessage(chat_id, "Usage: /unset <finger_id>", "");
      } else bot.sendMessage(chat_id, "Usage: /unset <finger_id>", "");
    }
    else if (text == "/whoami") {
      bot.sendMessage(chat_id, "Your chat id: " + chat_id, "");
    }
    else if (text == "/list") {
      String lm = listMappings();
      bot.sendMessage(chat_id, lm, "");
    }
    else {
      bot.sendMessage(chat_id, "Commands:\n/setme <finger_id>\n/unset <finger_id>\n/whoami\n/list", "");
    }
  }
}

// ---------- Fingerprint blocking helpers ----------
uint8_t waitForImageBlocking(uint16_t timeout_ms = 15000) {
  uint32_t start = millis();
  while (millis() - start < timeout_ms) {
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_OK) {
      Serial.println(F("Image captured"));
      return FINGERPRINT_OK;
    } else if (p == FINGERPRINT_NOFINGER) {
      // keep waiting
    } else {
      Serial.print(F("getImage error code: "));
      Serial.println(p);
      return p;
    }
    delay(120);
  }
  Serial.println(F("Timeout waiting for finger"));
  return 0xFF;
}

int processFingerprintBlockingAndReturnID() {
  showOLED("Scan fingerprint", "Place finger now");
  beep(60); // start beep
  uint8_t r = waitForImageBlocking(15000);
  if (r != FINGERPRINT_OK) {
    Serial.println("No image or error");
    return -1;
  }
  beep(30); // captured
  if (finger.image2Tz(1) != FINGERPRINT_OK) {
    Serial.println("image2Tz failed");
    return -1;
  }
  uint8_t res = finger.fingerFastSearch();
  if (res == FINGERPRINT_OK) {
    int fid = finger.fingerID;
    Serial.print("Fingerprint matched id="); Serial.println(fid);
    return fid;
  } else {
    Serial.println("No match");
    return -1;
  }
}

// ---------- Unlock ----------
void triggerUnlock() {
  if (!RELAY_ACTIVE_LOW) digitalWrite(RELAY_PIN, HIGH);
  else digitalWrite(RELAY_PIN, LOW);
  beep(90);
  showOLED("UNLOCKED", "Door open");
  delay(10000);
  if (!RELAY_ACTIVE_LOW) digitalWrite(RELAY_PIN, LOW);
  else digitalWrite(RELAY_PIN, HIGH);
  showOLED("Door Locked", "Ready");
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(200);
  randomSeed(analogRead(34) ^ millis());

  // Wire + OLED init
  Wire.begin(OLED_SDA, OLED_SCL);
  oled.init();
  oled.clear();
  oled.update();

  // Fingerprint init (Serial2)
  fingerSerial.begin(FINGER_BAUD, SERIAL_8N1, FINGER_RX_PIN, FINGER_TX_PIN);
  delay(100);
  finger.begin(FINGER_BAUD);
  if (!finger.verifyPassword()) {
    Serial.println("Fingerprint sensor not found (check wiring/power).");
    showOLED("Fingerprint", "Sensor NOT found");
  } else {
    Serial.println("Fingerprint sensor OK");
    showOLED("Fingerprint OK", "Ready");
  }

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  showOLED("WiFi", "Connecting...");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 30) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK");
    showOLED("WiFi OK", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi FAIL");
    showOLED("WiFi FAIL", "Check creds");
  }

  // Telegram
  secured_client.setInsecure();
  bot.updateToken(BOT_TOKEN);

  // Preferences
  prefs.begin(PREF_NS, false);

  // pins
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // keypad tuning
  keypad.setDebounceTime(10);
  keypad.setHoldTime(500);

  showOLED("System Ready", "Press A to scan");
  Serial.println("Setup complete.");
}

// ---------- Loop ----------
void loop() {
  // Telegram poll
  if (millis() - lastBotPoll > TELEGRAM_POLL_MS) {
    if (String(BOT_TOKEN).length() > 8) handleTelegram();
    lastBotPoll = millis();
  }

  // Keypad handling (A triggers fingerprint)
  char k = keypad.getKey();
  if (k) {
    Serial.print("Key: "); Serial.println(k);

    if (k == 'A') {
      // User asked to scan fingerprint now
      int fid = processFingerprintBlockingAndReturnID();
      if (fid > 0) {
        // matched
        String chatid = loadMapping(fid);
        if (chatid.length() == 0) {
          showOLED("Finger matched", ("ID: " + String(fid)).c_str());
          delay(900);
          showOLED("Not registered", "Use /setme <id>");
          Serial.println("No mapping for ID " + String(fid));
          beep(120);
        } else {
          currentOTP = generateOTP();
          otpStart = millis();
          otpActive = true;
          expectedFingerID = fid;
          bool ok = sendOtpToChat(chatid, currentOTP);
          if (ok) {
            showOLED("OTP Sent", ("To ID " + String(fid)).c_str());
            Serial.println("OTP sent: " + currentOTP + " for fid " + String(fid));
            beep(80);
          } else {
            showOLED("OTP Send Failed", "Check bot/token");
            Serial.println("OTP send fail for " + chatid);
            beep(180);
          }
        }
      } else {
        showOLED("No match", "Try again");
        beep(120);
      }
    }

    // CANCEL (stop current OTP)
    else if (k == 'B') {
      if (otpActive) {
        otpActive = false;
        currentOTP = "";
        entered = "";
        expectedFingerID = -1;
        showOLED("OTP Cancelled", "Ready");
      } else {
        showOLED("No OTP active", "Ready");
      }
      beep(80);
    }

    // CLEAR ALL
    else if (k == '*') {
      entered = "";
      showOLED("Cleared", "Enter OTP");
      beep(60);
    }

    // BACKSPACE
    else if (k == 'C') {
      if (entered.length() > 0) {
        entered.remove(entered.length()-1, 1);
        String mask = "";
        for (int i=0;i<entered.length();i++) mask += "*";
        showSmallLines("Entering OTP:", mask.c_str());
        beep(60);
      } else {
        showOLED("Empty", "Nothing to delete");
        beep(120);
      }
    }

    // SUBMIT (D or #)
    else if (k == 'D' || k == '#') {
      if (!otpActive) {
        showOLED("No OTP active", "Press A to start");
        beep(120);
      } else if (millis() - otpStart > OTP_TIMEOUT_MS) {
        showOLED("OTP EXPIRED", "Scan again");
        otpActive = false; currentOTP=""; entered=""; expectedFingerID = -1;
        beep(200);
      } else {
        Serial.print("Entered: "); Serial.println(entered);
        Serial.print("Expected: "); Serial.println(currentOTP);
        if (entered == currentOTP) {
          showOLED("OTP OK", "Unlocking...");
          triggerUnlock();
          otpActive = false; currentOTP=""; entered=""; expectedFingerID = -1;
        } else {
          showOLED("WRONG OTP", "Try again");
          entered = "";
          beep(300);
        }
      }
    }

    // DIGITS
    else if (k >= '0' && k <= '9') {
      if (entered.length() < OTP_LEN) {
        entered += k;
        String mask = "";
        for (int i=0;i<entered.length();i++) mask += "*";
        showSmallLines("Entering OTP:", mask.c_str());
        beep(40);
      } else {
        showOLED("Max digits", "Press * to clear");
        beep(80);
      }
    }
  }

  // OTP timeout auto-clear
  if (otpActive && (millis() - otpStart) > OTP_TIMEOUT_MS) {
    otpActive = false; currentOTP=""; entered=""; expectedFingerID=-1;
    showOLED("OTP timed out", "Press A to scan");
  }

  delay(8);
}
