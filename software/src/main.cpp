#include "Arduino.h"

#include <Wire.h>
#include <Adafruit_INA219.h>

#include <filters.h>

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

#include "SPIFFS.h"

#include <ArduinoJson.h>

// Constants

#define SINGLE_SHOT_TIMEOUT 1500

#define PRE_SHOT_DELAY 100
#define POST_SHOT_DELAY 100

// Status

DynamicJsonDocument status(1024);
bool isMotorEnabled = false;
int numberOfShots = 0;
double delayBetweenShots = 0;

// Motor

#define MOTOR 25

void setMotorEnabled(bool);

// Network

const char* ssid = "***";
const char* password = "***";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

void onEvent(AsyncWebSocket *, AsyncWebSocketClient *, AwsEventType, void *, uint8_t *, size_t);

// Current sensing

Adafruit_INA219 ina219;

const float cutoff_freq   = 20.0;
const float sampling_time = 2.0f / 1000.0f;
IIR::ORDER  order = IIR::ORDER::OD2;

// Low-pass filter
Filter f(cutoff_freq, sampling_time, order);

void initFS() {
  if (!SPIFFS.begin()) {
    Serial.println("An error has occurred while mounting SPIFFS");
  }
  else{
   Serial.println("SPIFFS mounted successfully");
  }
}

void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi ..");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print('.');
    delay(1000);
  }
  Serial.println(WiFi.localIP());
}

void initWebSocket() {
  ws.onEvent(onEvent);
  server.addHandler(&ws);
}

void notifyAllClients();

void initIna219() {
  if (!ina219.begin()) {
    Serial.println("Failed to find INA219 chip");
    while (1) { delay(10); }
  }
}

void setup()
{
  Serial.begin(115200);
  while (!Serial) {
      // will pause Zero, Leonardo, etc until serial console opens
      delay(1);
  }

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(MOTOR, OUTPUT);
  status["isMotorEnabled"] = false;

  initFS();
  initWiFi();
  initWebSocket();
  initIna219();
  
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/index.html", "text/html");
  });
  server.serveStatic("/", SPIFFS, "/");

  server.begin();
  Serial.println("HTTP server started");
}

enum State {
  idle,
  start,
  executing,
  releasing,
  cooling,
};

State prevState = idle;
State state = idle;

double currentStateTimer = 0;

uint32_t sampleSize = 0;
double averagePower = 0;

uint32_t timer;

void loop()
{
  double dt = (double)(micros() - timer) / 1000000; // Calculate delta time
  timer = micros();

  if (prevState != state) {
    prevState = state;
    currentStateTimer = 0;
  }
  currentStateTimer += dt;

  if (state == idle) {
    setMotorEnabled(false);
  } else if (state == start) {
    if (numberOfShots == -1 || numberOfShots > 0) {
      if (numberOfShots > 0) numberOfShots --;
      setMotorEnabled(true);
      // Start listening for power peak
      sampleSize = 0;
      averagePower = 0;
      // Pre shot delay to make sure motor is on
      delay(PRE_SHOT_DELAY);
      // Update state to executing
      state = executing;
    } else {
      state = idle;
    }
  } else if (state == executing) {
    // Calculate average power
    float power = f.filterIn(ina219.getPower_mW());
    averagePower = (averagePower * sampleSize + power) / (sampleSize + 1);
    sampleSize += 1;
    // Wait for power peak
    if (power > averagePower * 1.5 || currentStateTimer > SINGLE_SHOT_TIMEOUT) {
      state = releasing;
    }
  } else if (state == releasing) {
    // Calculate average power
    float power = f.filterIn(ina219.getPower_mW());
    averagePower = (averagePower * sampleSize + power) / (sampleSize + 1);
    sampleSize += 1;
    // Wait for power drop
    if (power < averagePower * 1.5 || currentStateTimer > SINGLE_SHOT_TIMEOUT) {
      // Post shot delay to make sure spring is released
      delay(POST_SHOT_DELAY);
      setMotorEnabled(false);

      state = cooling;
    }
  } else if (state == cooling) {
    // Wait for timeout
    if (currentStateTimer > delayBetweenShots) {
      state = start;
      notifyAllClients();
    }
  }

  delay(2);
}

void setMotorEnabled(bool enabled) {
  if (isMotorEnabled == enabled) return;
  isMotorEnabled = enabled;
  digitalWrite(LED_BUILTIN, enabled ? HIGH : LOW);
  digitalWrite(MOTOR, enabled ? HIGH : LOW);
  notifyAllClients();
}

void syncStatus() {
  status["isMotorEnabled"] = isMotorEnabled;
  status["numberOfShots"] = numberOfShots;
  status["delayBetweenShots"] = delayBetweenShots;
}

void notifyAllClients() {
  syncStatus();
  String json = "";
  serializeJson(status, json);
  ws.textAll(json);
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      break;
    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
      break;
    case WS_EVT_DATA: {
      AwsFrameInfo *info = (AwsFrameInfo*)arg;
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        data[len] = 0;
        String message = (char*)data;
        if (message.indexOf("status") >= 0) {
          syncStatus();
          String json = "";
          serializeJson(status, json);
          client->text(json);
        }
        if (message.indexOf("{") >= 0) {
          DynamicJsonDocument patch(1024);
          deserializeJson(patch, message);
          if (patch.containsKey("numberOfShots")) {
            numberOfShots = patch["numberOfShots"];
          }
          if (patch.containsKey("delayBetweenShots")) {
            delayBetweenShots = patch["delayBetweenShots"];
          }
          if (patch.containsKey("state")) {
            String newState = patch["state"];
            if (newState.equals("start")) {
              state = start;
            } else if (newState.equals("idle")) {
              state = idle;
            }
          }
          notifyAllClients();
        }
        if (message.indexOf("on") >= 0) {
          state = start;
          notifyAllClients();
        }
        if (message.indexOf("off") >= 0) {
          state = idle;
          notifyAllClients();
        }
        if (message.indexOf("os") >= 0) {
          numberOfShots = 1;
          state = start;
          notifyAllClients();
        }
      }
      break;
    }
    case WS_EVT_PONG:
    case WS_EVT_ERROR:
      break;
  }
}