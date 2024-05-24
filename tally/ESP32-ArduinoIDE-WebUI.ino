#include <WiFiManager.h>          // https://github.com/tzapu/WiFiManager
#include <Adafruit_NeoPixel.h>
#include <WiFiUdp.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>          // https://github.com/bblanchon/ArduinoJson

// Initial default values
#define DEFAULT_LED_PIN 16
#define DEFAULT_NUM_LEDS 16
#define RESET_BUTTON_PIN 0 // Define the pin for the reset button

// Configurable parameters
char deviceName[32] = "device-name";
char hubIp[16] = "10.4.20.41";
int hubPort = 7411;
int ledPin = DEFAULT_LED_PIN;
int ledCount = DEFAULT_NUM_LEDS;

// Initialize objects
Adafruit_NeoPixel strip(ledCount, ledPin, NEO_GRB + NEO_KHZ800);
WiFiUDP udp;
AsyncWebServer server(80);
unsigned long timeLastPackageReceived;
String lastStatus = "";

const char* index_html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Device Configuration</title>
  <style>
    body { font-family: Arial, sans-serif; }
    form { margin: 20px; }
    input { margin: 5px 0; padding: 8px; width: 100%; }
    input[type="submit"] { width: auto; background: #4CAF50; color: white; border: none; padding: 10px 20px; cursor: pointer; }
  </style>
</head>
<body>
  <h1>Device Configuration</h1>
  <form action="/get" method="get">
    Device Name: <input type="text" name="deviceName" value="%deviceName%"><br>
    Hub IP: <input type="text" name="hubIp" value="%hubIp%"><br>
    Hub Port: <input type="text" name="hubPort" value="%hubPort%"><br>
    LED Pin: <input type="text" name="ledPin" value="%ledPin%"><br>
    LED Count: <input type="text" name="ledCount" value="%ledCount%"><br>
    <input type="submit" value="Save">
  </form>
  <form action="/reset" method="post">
    <input type="submit" value="Reset WiFi">
  </form>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);

  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP); // Initialize the reset button pin

  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("An error has occurred while mounting SPIFFS");
    return;
  }

  // Load Config
  loadConfig();

  // Reinitialize the strip with the new configuration
  strip.updateLength(ledCount);
  strip.setPin(ledPin);
  strip.begin();
  strip.show();

  // Setup WiFi Manager
  WiFiManager wifiManager;

  // Custom Parameters
  WiFiManagerParameter custom_deviceName("deviceName", "Device Name", deviceName, 32);
  WiFiManagerParameter custom_hubIp("hubIp", "Hub IP", hubIp, 16);
  WiFiManagerParameter custom_hubPort("hubPort", "Hub Port", String(hubPort).c_str(), 6);
  WiFiManagerParameter custom_ledPin("ledPin", "LED Pin", String(ledPin).c_str(), 6);
  WiFiManagerParameter custom_ledCount("ledCount", "LED Count", String(ledCount).c_str(), 6);
  
  wifiManager.addParameter(&custom_deviceName);
  wifiManager.addParameter(&custom_hubIp);
  wifiManager.addParameter(&custom_hubPort);
  wifiManager.addParameter(&custom_ledPin);
  wifiManager.addParameter(&custom_ledCount);
  
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.autoConnect("AutoConnectAP");

  // Save custom parameters to global variables
  strcpy(deviceName, custom_deviceName.getValue());
  strcpy(hubIp, custom_hubIp.getValue());
  hubPort = atoi(custom_hubPort.getValue());
  ledPin = atoi(custom_ledPin.getValue());
  ledCount = atoi(custom_ledCount.getValue());

  // Reinitialize the strip with the updated configuration
  strip.updateLength(ledCount);
  strip.setPin(ledPin);
  strip.begin();
  strip.show();

  // Start web server for configuration
  startWebServer();

  // Connect to UDP
  udp.begin(7411);

  // Log info
  logMessage("INFO", "Setup complete");
}

void loop() {
  checkWiFiConnection();

  if (WiFi.status() == WL_CONNECTED) {
    int packetSize = udp.parsePacket();
    if (packetSize) {
      char incomingPacket[255];
      int len = udp.read(incomingPacket, 255);
      if (len > 0) {
        incomingPacket[len] = 0;
        handleReceivedMessage(incomingPacket);
      }
    }

    unsigned long currentTime = micros();
    if (diffMicroSeconds(timeLastPackageReceived) > 3000000) {
      updateLEDs(255, 0, 0); // Red for disconnected
      logMessageOnce("DISCONNECTED");
    } else {
      logMessageOnce("CONNECTED");
    }

    if (millis() % 1000 < 100) { // Send info every second
      sendTallyStatus("tally-ho \"%s\"", deviceName);
    }
  }

  if (digitalRead(RESET_BUTTON_PIN) == LOW) { // Check if the reset button is pressed
    Serial.println("Reset button pressed, entering config mode...");
    delay(1000); // Debounce delay
    WiFiManager wifiManager;
    wifiManager.resetSettings();
    ESP.restart();
  }
}

void checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    logMessageOnce("RECONNECTING TO WIFI");
    WiFi.reconnect();
    delay(1000);
  }
}

void saveConfigCallback() {
  Serial.println("Saving config");
  // Save config to file
  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("Failed to open config file for writing");
  }
  DynamicJsonDocument json(1024);
  json["deviceName"] = deviceName;
  json["hubIp"] = hubIp;
  json["hubPort"] = hubPort;
  json["ledPin"] = ledPin;
  json["ledCount"] = ledCount;

  serializeJson(json, configFile);
  configFile.close();

  // Reboot after saving the configuration
  ESP.restart();
}

void loadConfig() {
  if (SPIFFS.exists("/config.json")) {
    File configFile = SPIFFS.open("/config.json", "r");
    if (configFile) {
      DynamicJsonDocument json(1024);
      DeserializationError error = deserializeJson(json, configFile);
      if (!error) {
        strcpy(deviceName, json["deviceName"]);
        strcpy(hubIp, json["hubIp"]);
        hubPort = json["hubPort"];
        ledPin = json["ledPin"];
        ledCount = json["ledCount"];
      }
      configFile.close();
    }
  }
}

void startWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = String(index_html);
    html.replace("%deviceName%", deviceName);
    html.replace("%hubIp%", hubIp);
    html.replace("%hubPort%", String(hubPort));
    html.replace("%ledPin%", String(ledPin));
    html.replace("%ledCount%", String(ledCount));
    request->send(200, "text/html", html);
  });

  server.on("/get", HTTP_GET, [](AsyncWebServerRequest *request){
    String message;
    if (request->hasParam("deviceName")) {
      message = request->getParam("deviceName")->value();
      strcpy(deviceName, message.c_str());
    }
    if (request->hasParam("hubIp")) {
      message = request->getParam("hubIp")->value();
      strcpy(hubIp, message.c_str());
    }
    if (request->hasParam("hubPort")) {
      message = request->getParam("hubPort")->value();
      hubPort = message.toInt();
    }
    if (request->hasParam("ledPin")) {
      message = request->getParam("ledPin")->value();
      ledPin = message.toInt();
    }
    if (request->hasParam("ledCount")) {
      message = request->getParam("ledCount")->value();
      ledCount = message.toInt();
    }
    saveConfigCallback();
  });

  server.on("/reset", HTTP_POST, [](AsyncWebServerRequest *request){
    WiFiManager wifiManager;
    wifiManager.resetSettings();
    ESP.restart();
    request->send(200, "text/plain", "WiFi settings reset. Rebooting...");
  });

  server.begin();
}

void logMessage(const char *level, const char *message) {
  Serial.print(level);
  Serial.print(": ");
  Serial.println(message);
}

void logMessageOnce(const char *message) {
  if (lastStatus != message) {
    Serial.println(message);
    lastStatus = message;
  }
}

void updateLEDs(int r, int g, int b) {
  for (int i = 0; i < ledCount; i++) {
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  strip.show();
}

void handleReceivedMessage(const char *data) {
  timeLastPackageReceived = micros();
  // Parse the received data and update the LEDs accordingly
  int opR, opG, opB, stR, stG, stB;
  int patternNumber;
  int duration;
  char command[64];

  sscanf(data, "O%d/%d/%d S%d/%d/%d 0x%x %d", &opR, &opG, &opB, &stR, &stG, &stB, &patternNumber, &duration);
  if (strlen(data) > 25) {
    // Handle pattern logic if necessary
    updateLEDs(opR, opG, opB); // Update LEDs with the operator colors
    delay(duration * 1000);
  } else {
    updateLEDs(opR, opG, opB); // Update LEDs with the operator colors
  }
  logMessage("INFO", data); // Logging received message for debug
}

unsigned long diffMicroSeconds(unsigned long time) {
  unsigned long now = micros();
  if (now < time) return now + (4294967295 - time);
  else return now - time;
}

void sendTallyStatus(const char *status, const char *deviceName) {
  if (WiFi.status() == WL_CONNECTED) {
    udp.beginPacket(hubIp, hubPort);
    char buffer[128];
    snprintf(buffer, sizeof(buffer), status, deviceName);
    udp.write((const uint8_t *)buffer, strlen(buffer));
    udp.endPacket();
    logMessageOnce("Sent status to hub");
  } else {
    logMessageOnce("Failed to send status, not connected to WiFi");
  }
}
 
