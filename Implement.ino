#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <time.h>

// WiFi Credentials
const char* ssid = "xxxxxxxx";
const char* password = "xxxxxxxxxx";

// OCR API
const char* ocrApiUrl = "api.ocr.space";
const char* serverPath = "/parse/image";
const int serverPort = 80;
const char* apiKey = "xxxxxxxxxxxxx";

// LCD Display
LiquidCrystal_I2C lcd(0x27, 16, 2); // Change address if needed

// Web Server & WebSocket
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

// Pins
#define triggerButton 13
#define flashLight 4

WiFiClient client;

// Camera GPIO
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head><title>IoT Based Load Cell Counting System</title></head>
<body>
<h2>IoT Based Load Cell Counting System</h2>
<div id="ocrResult" style="font-family:monospace;"></div>
<script>
  var ws = new WebSocket("ws://" + window.location.hostname + ":81/");
  ws.onmessage = function(event) {
    document.getElementById("ocrResult").innerHTML = event.data;
  };
</script>
</body>
</html>
)rawliteral";

void displayText(String text) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("OCR:");
  lcd.setCursor(0, 1);
  lcd.print(text.substring(0, 16)); // Only display first 16 characters
}

int sendPhoto();  // Function prototype

struct SerialEntry {
  String serial;
  time_t timestamp;
};
std::vector<SerialEntry> allSerialEntries;

struct TextGroup {
  time_t startTime;
  time_t endTime;
  std::vector<SerialEntry> entries;
};


std::vector<TextGroup> groupedTexts;
std::vector<String> sentSerials;  // Stores already sent serial numbers
bool firstSerialSkipped = false; // Add
std::vector<String> usedLast3Digits; // Track only unique last 3 digits

const int GROUP_DURATION = 3600; // seconds

void addTextToGroup(time_t timestamp, String text) {
  bool added = false;
  for (auto& group : groupedTexts) {
    if (timestamp >= group.startTime && timestamp <= group.endTime) {
      group.entries.push_back({text, timestamp});
      added = true;
      break;
    }
  }

  if (!added) {
    TextGroup newGroup;
    newGroup.startTime = timestamp;
    newGroup.endTime = timestamp + GROUP_DURATION;
    newGroup.entries.push_back({text, timestamp});
    groupedTexts.push_back(newGroup);
  }
}


String formatShortTime(time_t rawTime) {
  struct tm* timeinfo = localtime(&rawTime);
  char buffer[10];
  strftime(buffer, sizeof(buffer), "%H:%M:%S", timeinfo);
  return String(buffer);
}

int countPer2Seconds(const std::vector<SerialEntry>& entries) {
  if (entries.empty()) return 0;

  time_t start = entries.front().timestamp;
  time_t end = entries.back().timestamp;
  int intervalCount = ((end - start) / 2) + 1;
  return intervalCount;
}


void sendGroupedOCRResults(time_t timestamp, String recognizedText) {
  String message;
  message += "<b>ESP32 OCR Recognition</b><br>";
  message += "Time: " + formatShortTime(timestamp) + "<br>";

  // Count total number of serials across all groups
  int totalSerials = 0;
  for (auto& group : groupedTexts) totalSerials += group.entries.size();
  
  message += "All Total Count - " + String(totalSerials) + "<br>";
  message += "Load Cell Serial Number: " + recognizedText + "<br><br>";
  message += "<b>Time Range and Serial Numbers</b><br><br>";

  for (auto& group : groupedTexts) {
    if (group.entries.size() <= 1) continue;

    message += "(" + formatShortTime(group.startTime) + " - " + formatShortTime(group.endTime) + ")<br>";
    for (const auto& entry : group.entries) {
      message += "Serial Number: " + entry.serial + " - Time: " + formatShortTime(entry.timestamp) + "<br>";
    }
    message += "<b>Total Count: " + String(group.entries.size()) + "</b><br><br>";
  }

  webSocket.broadcastTXT(message);
}


void setup() {
  Serial.begin(115200);
  pinMode(flashLight, OUTPUT);
  pinMode(triggerButton, INPUT);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());

  configTzTime("IST-5:30", "pool.ntp.org", "time.nist.gov");

  // LCD Init
  Wire.begin(15, 14);  // Set custom SDA and SCL pins
  lcd.init();
  lcd.backlight();
  displayText("System Ready");

  // Camera init
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  if (psramFound()) {
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_CIF;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }
  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Camera init failed");
    ESP.restart();
  }

  server.on("/", []() {
    server.send(200, "text/html", htmlPage);
  });
  server.begin();
  webSocket.begin();
  webSocket.onEvent([](uint8_t client_num, WStype_t type, uint8_t * payload, size_t length) {
    // Optional WebSocket command handling
  });

  displayText("Press Button");
}

void loop() {
  server.handleClient();
  webSocket.loop();

  if (digitalRead(triggerButton) == HIGH) {
    int result = sendPhoto();
    //if (result == -1) 
    if (result == -2) displayText("Conn Failed");
    delay(1000);
  }
}

bool isNewSerial(const String& serial) {
  for (const auto& s : sentSerials) {
    if (s == serial) return false; // Already sent
  }
  sentSerials.push_back(serial);
  return true;
}


int sendPhoto() {
  digitalWrite(flashLight, HIGH);
  delay(200);
  camera_fb_t* fb = esp_camera_fb_get();
  digitalWrite(flashLight, LOW);
  if (!fb) return -1;

  if (!client.connect(ocrApiUrl, serverPort)) {
    esp_camera_fb_return(fb);
    return -2;
  }

  String boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
  String head = "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"apikey\"\r\n\r\n" + apiKey + "\r\n" +
                "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"language\"\r\n\r\neng\r\n" +
                "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"file\"; filename=\"image.jpg\"\r\n"
                "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  client.println("POST " + String(serverPath) + " HTTP/1.1");
  client.println("Host: " + String(ocrApiUrl));
  client.println("Content-Type: multipart/form-data; boundary=" + boundary);
  client.println("Content-Length: " + String(head.length() + fb->len + tail.length()));
  client.println("Connection: close\r\n");
  client.print(head);

  size_t fbLen = fb->len;
  uint8_t* fbBuf = fb->buf;
  for (size_t n = 0; n < fbLen; n += 1024) {
    size_t chunkSize = (n + 1024 < fbLen) ? 1024 : fbLen - n;
    client.write(fbBuf, chunkSize);
    fbBuf += chunkSize;
  }

  client.print(tail);
  esp_camera_fb_return(fb);

  String response;
  long timeout = millis();
  while (client.connected() && millis() - timeout < 10000) {
    while (client.available()) {
      response += (char)client.read();
    }
  }
  client.stop();

  int start = response.indexOf("\"ParsedText\":\"");
  int end = response.indexOf("\"", start + 14);
  String recognizedText = "Not Found";
  time_t now = time(nullptr);


  if (start > 0 && end > start) {
    recognizedText = response.substring(start + 14, end);
    recognizedText.trim(); // Remove \r\n and other whitespace

    // Filter to include only digits
    String digitsOnly = "";
    for (char c : recognizedText) {
      if (isdigit(c)) digitsOnly += c;
      if (digitsOnly.length() == 8) break;
    }
    recognizedText = digitsOnly;
  }

  //Serial.println("Recognized: " + recognizedText);
  //displayText(recognizedText);


  /*  if (isNewSerial(recognizedText)) {
        addTextToGroup(now, recognizedText);
        sendGroupedOCRResults(now, recognizedText);
      } else {
        Serial.println("Duplicate serial number skipped: " + recognizedText);
      } */



    if (recognizedText.length() >= 3) {
      String last3 = recognizedText.substring(recognizedText.length() - 3);

      // Check if these last 3 digits are already used
      bool alreadyUsed = false;
      for (const auto& existing : usedLast3Digits) {
        if (existing == last3) {
          alreadyUsed = true;
          break;
        }     
      }

      if (!alreadyUsed) {
        // Add to used last 3 digits
        usedLast3Digits.push_back(last3);

        // Record full serial and group it
        SerialEntry newEntry = {recognizedText, now};
        allSerialEntries.push_back(newEntry);

        addTextToGroup(now, recognizedText);
        sendGroupedOCRResults(now, recognizedText);
        displayText(recognizedText);

        Serial.println("Accepted Serial Number: " + recognizedText);
      } else {
        Serial.println("Skipped duplicate last-3-digits: " + recognizedText);
        displayText("Press Again");
      }
    } else {
      Serial.println("Invalid Serial Number (too short): " + recognizedText);
      displayText("Press Again");
    }

 
  




 return 0;
}
