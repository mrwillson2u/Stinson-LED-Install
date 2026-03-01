#include <Arduino.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <ESPmDNS.h>
#include <ArtnetWifi.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <esp_task_wdt.h>

#include <dhtnew.h>

DHTNEW mySensor(27); //  ESP 16    UNO 5    MKR1010 5
// select which pin will trigger the configuration portal when set to LOW
#define TRIGGER_PIN 0

// put function declarations here:
void writeToBusA(const byte *dataPacket);
void writeToBusB(const byte *dataPacket);
int calculateChecksum(const byte *dataBuf, int len);
void doWiFiManager();
void onDmxFrame(uint16_t universe, uint16_t length, uint8_t sequence, uint8_t* data);
void readFromDHT();
void reconnectMQTT();
void publishColorState();

// Setting default values for dynamic environment variable
#ifndef HOSTNAME
#define HOSTNAME "outdoor-led-control"
#endif

#ifndef MQTT_IP
#define MQTT_IP " "
#endif

#ifndef MQTT_USER
#define MQTT_USER "user"
#endif

#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD "password"
#endif

#ifndef MQTT_ENABLED
#define MQTT_ENABLED false
#endif

bool mqttIsEnabled() {
#ifdef MQTT_ENABLED
  return (strcmp(MQTT_ENABLED, "true") == 0 || strcmp(MQTT_ENABLED, "True") == 0);
#else
  return false;
#endif
}




#ifndef WIFI_SSID
#define WIFI_SSID "undefined"
#endif

#ifndef STATIC_IP
#define STATIC_IP ""
#endif

#ifndef GATEWAY
#define GATEWAY ""
#endif

#ifndef SUBNET
#define SUBNET ""
#endif

#define TX_PIN_A 17
#define RX_PIN_A 16
#define ENABLE_PIN_A 21

#define TX_PIN_B 19
#define RX_PIN_B 18
#define ENABLE_PIN_B 23

#define VERBOSE_OUTPUT false

// Idle serial refresh rate (ms) — tune up if LEDs drop out, set lower for faster refresh
#define IDLE_REFRESH_MS 500

// Max output value sent to LED controllers (clamp to avoid 100% blink/flicker)
#define MAX_OUTPUT_VALUE 245

// How long to keep full-rate serial output after the last Art-Net frame (ms)
// Covers the entire fade transition before dropping to idle refresh
#define ACTIVE_HOLDOFF_MS 5000

byte dataPrefix[] = {149, 1, 250, 0};
byte outputDataA[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
byte outputDataB[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// Pre-built output packets (rebuilt only when data changes)
byte outputA[17] = {149, 1, 250, 0};
byte outputB[17] = {149, 1, 250, 0};

// Rate-limiting: only send serial data at full rate when new Art-Net data arrives
bool dataChanged = false;
unsigned long lastArtNetFrame = 0; // timestamp of last Art-Net callback

// Bus enable timing: disable after transmission completes
// 17 bytes * 10 bits/byte / 38400 baud = ~4.43ms, round up to 5ms
const unsigned long BUS_TX_TIME_MS = 5;
unsigned long busAWriteTime = 0;
unsigned long busBWriteTime = 0;
bool busAEnabled = false;
bool busBEnabled = false;

// MQTT color publish throttling
bool colorChanged = false;
unsigned long lastColorPublish = 0;
const unsigned long COLOR_PUBLISH_INTERVAL = 1000; // 1 second

// OTA update in progress — pause all other processing
bool otaInProgress = false;

// WiFi disconnect tracking
unsigned long wifiDisconnectedSince = 0;
const unsigned long WIFI_REBOOT_TIMEOUT = 300000; // 5 minutes

WiFiManager wm;
WiFiClient espClient;
PubSubClient mqttClient(espClient);


unsigned int  timeout   = 120; // seconds to run for
unsigned int  startTime = millis();
bool portalRunning      = false;
bool startAP            = false; // start AP and webserver if true, else start only webserver

WiFiUDP UdpSend;
ArtnetWifi artnet;

// DHT dht;
const int DHTReadDelay = 300000; // 5 minutes
unsigned long dhtReadTimestamp = 0;

unsigned long artnetLastRecieved = 0;

const int DHTAfterArtnetDelay = 30000; // 30 sec

float enclosureTemp = 0;
float enclosureHumidity = 0;

void setup() {

  Serial.begin(115200);
  // Serial.setDebugOutput(true);
  Serial1.begin(38400, SERIAL_8N1, RX_PIN_A, TX_PIN_A);
  Serial2.begin(38400, SERIAL_8N1, RX_PIN_B, TX_PIN_B);



  Serial.printf("SSID: %s\n", WIFI_SSID);
  // WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP
  // it is a good practice to make sure your code sets wifi mode how you want it.

  // reset settings - wipe stored credentials for testing
  // these are stored by the esp library
  // wm.resetSettings();

  delay(1000);
  Serial.println("\n Starting");

  pinMode(TRIGGER_PIN, INPUT_PULLUP);
  pinMode(ENABLE_PIN_A, OUTPUT);
  pinMode(ENABLE_PIN_B, OUTPUT);

  digitalWrite(ENABLE_PIN_A, LOW);
  digitalWrite(ENABLE_PIN_B, LOW);

  // Configure static IP if set, otherwise use DHCP
  if (strlen(STATIC_IP) > 0) {
    IPAddress ip, gateway, subnet;
    ip.fromString(STATIC_IP);
    gateway.fromString(GATEWAY);
    subnet.fromString(SUBNET);
    wm.setSTAStaticIPConfig(ip, gateway, subnet);
    Serial.printf("Static IP: %s\n", STATIC_IP);
  }

  wm.setHostname(HOSTNAME);
  wm.setConnectTimeout(60);
  wm.setConfigPortalTimeout(120);
  if (!wm.autoConnect()) {
    Serial.println("WiFi connect failed, restarting...");
    ESP.restart();
  }

  // mDNS — reachable at outdoor-led-control.local
  if (MDNS.begin(HOSTNAME)) {
    Serial.printf("mDNS: %s.local\n", HOSTNAME);
  }

  // OTA Configiration and Enable OTA
  Serial.println("\nEnabling OTA Feature");
  ArduinoOTA.setPassword("lonelybinary");
  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    Serial.println("OTA update starting...");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    esp_task_wdt_reset();
    Serial.printf("OTA: %u%%\r", (progress * 100) / total);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA complete, rebooting...");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    Serial.printf("OTA error [%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();

  // this will be called for each packet received
  artnet.setArtDmxCallback(onDmxFrame);
  artnet.begin();

  if( mqttIsEnabled() ) {
    mqttClient.setServer(MQTT_IP, 1883);
  }

  // Initialize hardware watchdog (30 second timeout, panic/reboot on trigger)
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);
}

void loop() {
  // Feed the watchdog
  esp_task_wdt_reset();

  ArduinoOTA.handle();
  if (otaInProgress) return;

  doWiFiManager();

  // WiFi resilience: reconnect or reboot if down too long
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiDisconnectedSince == 0) {
      wifiDisconnectedSince = millis();
      Serial.println("WiFi disconnected, attempting reconnect...");
      WiFi.reconnect();
    } else if (millis() - wifiDisconnectedSince > WIFI_REBOOT_TIMEOUT) {
      Serial.println("WiFi down for 5 minutes, restarting...");
      ESP.restart();
    }
  } else {
    wifiDisconnectedSince = 0;
  }

  if( mqttIsEnabled() ) {
    if (!mqttClient.connected()) reconnectMQTT();
    mqttClient.loop();
  }

  // we call the read function inside the loop


  artnet.read();

  // Only rebuild packets when data changes
  if (dataChanged) {
    for(int i = 0; i < 12; i++) {
      outputA[i + 4] = outputDataA[i];
    }
    outputA[16] = calculateChecksum(outputDataA, 12);

    for(int i = 0; i < 12; i++) {
      outputB[i + 4] = outputDataB[i];
    }
    outputB[16] = calculateChecksum(outputDataB, 12);
  }

  // Rate-limited serial output with idle refresh
  static unsigned long serialTimer = 0;
  static unsigned long lastRefresh = 0;
  bool activeWindow = (millis() - lastArtNetFrame) < ACTIVE_HOLDOFF_MS;

  if (dataChanged || activeWindow) {
    // During fade or holdoff: send at full rate
    if(micros() - serialTimer >= 575) {
      writeToBusA(outputA);
      writeToBusB(outputB);
      serialTimer = micros();
      dataChanged = false;
      lastRefresh = millis();
    }
  } else {
    // Idle: send keep-alive refresh at IDLE_REFRESH_MS rate
    if(millis() - lastRefresh >= IDLE_REFRESH_MS) {
      writeToBusA(outputA);
      writeToBusB(outputB);
      serialTimer = micros();
      lastRefresh = millis();
    }
  }

  // Disable bus enable pins after transmission completes
  if (busAEnabled && millis() - busAWriteTime >= BUS_TX_TIME_MS) {
    digitalWrite(ENABLE_PIN_A, LOW);
    busAEnabled = false;
  }
  if (busBEnabled && millis() - busBWriteTime >= BUS_TX_TIME_MS) {
    digitalWrite(ENABLE_PIN_B, LOW);
    busBEnabled = false;
  }

  // Throttled MQTT color publish (only when data changed, max once per second)
  if (mqttIsEnabled() && mqttClient.connected() && colorChanged &&
      millis() - lastColorPublish >= COLOR_PUBLISH_INTERVAL) {
    publishColorState();
    colorChanged = false;
    lastColorPublish = millis();
  }

  /*
  Check from the DHT sensor only at the set frequency and 30 seconds after the last artnet command was received
  so that we are pretty sure artnet commands are not going to be send during the DHT read delay (18ms).
  We also have another timer so that we are only reading from the humidity/temperature sensor every 5 minutes.
  */
  if( millis() - dhtReadTimestamp > DHTReadDelay && millis() - artnetLastRecieved > DHTAfterArtnetDelay) {
    readFromDHT();
    dhtReadTimestamp = millis();
  }

}



void writeToBusA(const byte *dataPacket) {
  digitalWrite(ENABLE_PIN_A, HIGH);
  Serial1.write(dataPacket, 17);
  busAWriteTime = millis();
  busAEnabled = true;
  artnetLastRecieved = millis();
}

void writeToBusB(const byte *dataPacket) {
  digitalWrite(ENABLE_PIN_B, HIGH);
  Serial2.write(dataPacket, 17);
  busBWriteTime = millis();
  busBEnabled = true;
  artnetLastRecieved = millis();
}

int calculateChecksum(const byte *dataBuf, int len) {
  int sum = 400; // Sum of prefix
  for(int i = 0; i < len; i++) {
    sum += dataBuf[i];
  }
  return sum % 256;
}

void doWiFiManager() {
  // is auto timeout portal running
  if(portalRunning){
    wm.process(); // do processing

    // check for timeout
    if((millis()-startTime) > (timeout*1000)){
      Serial.println("portaltimeout");
      portalRunning = false;
      if(startAP){
        wm.stopConfigPortal();
      }
      else{
        wm.stopWebPortal();
      }
   }
  }

  // is configuration portal requested?
  if(digitalRead(TRIGGER_PIN) == LOW && (!portalRunning)) {
    if(startAP){
      Serial.println("Button Pressed, Starting Config Portal");
      wm.setConfigPortalBlocking(false);
      wm.startConfigPortal();
    }
    else{
      Serial.println("Button Pressed, Starting Web Portal");
      wm.startWebPortal();
    }
    portalRunning = true;
    startTime = millis();
  }
}

void onDmxFrame(uint16_t universe, uint16_t length, uint8_t sequence, uint8_t* data) {
  bool tail = false;

  if(VERBOSE_OUTPUT) {
    Serial.print("DMX: Univ: ");
    Serial.print(universe, DEC);
    Serial.print(", Seq: ");
    Serial.print(sequence, DEC);
    Serial.print(", Data (");
    Serial.print(length, DEC);
    Serial.print("): ");
  }


  if (length > 16) {
    length = 16;
    tail = true;
  }
  // send out the buffer
  for (uint16_t i = 0; i < length; i++)
  {

    if( VERBOSE_OUTPUT ) { Serial.print(data[i]); };

    if(i < 8) {
      if( VERBOSE_OUTPUT ) { Serial.print(" A: "); };
      outputDataA[i] = min(data[i], (uint8_t)MAX_OUTPUT_VALUE);
    }
    else {
      if( VERBOSE_OUTPUT ) { Serial.print(" B: "); };
      outputDataB[i - 8] = min(data[i], (uint8_t)MAX_OUTPUT_VALUE);
    }

    if( VERBOSE_OUTPUT ) { Serial.print(" "); };
  }

  // Signal that new data is available (processed in loop)
  dataChanged = true;
  colorChanged = true;
  lastArtNetFrame = millis();

  if( VERBOSE_OUTPUT ) {
    if (tail) {
      Serial.print("...");
    }
    Serial.println();
  }
}

void readFromDHT() {

  //  READ DATA
  uint32_t start = micros();
  int chk = mySensor.read();
  uint32_t stop = micros();

  switch (chk)
  {
    case DHTLIB_OK:
      Serial.print("OK,\t");
      break;
    case DHTLIB_WAITING_FOR_READ:
      return;
    case DHTLIB_ERROR_CHECKSUM:
      Serial.println("Checksum error");
      return;
    case DHTLIB_ERROR_TIMEOUT_A:
      Serial.println("Time out A error");
      return;
    case DHTLIB_ERROR_TIMEOUT_B:
      Serial.println("Time out B error");
      return;
    case DHTLIB_ERROR_TIMEOUT_C:
      Serial.println("Time out C error");
      return;
    case DHTLIB_ERROR_TIMEOUT_D:
      Serial.println("Time out D error");
      return;
    case DHTLIB_ERROR_SENSOR_NOT_READY:
      Serial.println("Sensor not ready");
      return;
    case DHTLIB_ERROR_BIT_SHIFT:
      Serial.println("Bit shift error");
      return;
    default:
      Serial.print("Unknown: ");
      Serial.println(chk);
      return;
  }

  // Only reached on DHTLIB_OK
  enclosureTemp = mySensor.getTemperature();
  enclosureHumidity = mySensor.getHumidity();

  //  DISPLAY DATA
  Serial.print(enclosureHumidity, 1);
  Serial.print(",\t");
  Serial.println(enclosureTemp, 1);

  char tempPayload[16];
  char humPayload[16];
  dtostrf(enclosureTemp, 5, 2, tempPayload);
  dtostrf(enclosureHumidity, 5, 2, humPayload);

  if( mqttIsEnabled() ) {
    bool tempOk = mqttClient.publish("outdoor-led/temp", tempPayload);
    bool humOk = mqttClient.publish("outdoor-led/humidity", humPayload);

    if( VERBOSE_OUTPUT ) {
      Serial.printf("Temp publish: %s, Humidity publish: %s\n", tempOk ? "OK" : "FAIL", humOk ? "OK" : "FAIL");
    }
  }
}

void reconnectMQTT() {
  static unsigned long reconnectMQTTimer = 0;
  if (!mqttClient.connected() && millis() - reconnectMQTTimer > 5000 ) {
    Serial.print("Attempting MQTT connection...");
    if (mqttClient.connect("ESP32Client", MQTT_USER, MQTT_PASSWORD,
                           "outdoor-led/availability", 0, true, "offline")) {
      Serial.println("connected");
      mqttClient.publish("outdoor-led/availability", "online", true);
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 5 seconds");

      // delay(5000);
    }
    reconnectMQTTimer = millis();
  }
}

void publishColorState() {
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%d,%d,%d,%d,%d,%d,%d,%d",
           outputDataA[0], outputDataA[1], outputDataA[2], outputDataA[3],
           outputDataB[0], outputDataB[1], outputDataB[2], outputDataB[3]);

  bool pubOk = mqttClient.publish("outdoor-led/color", buffer);

  if( VERBOSE_OUTPUT ) {
    Serial.printf("Color publish: %s\n", pubOk ? "OK" : "FAIL");
  }
}
