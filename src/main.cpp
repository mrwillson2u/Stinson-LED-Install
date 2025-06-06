#include <Arduino.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <ESPmDNS.h>
#include <ArtnetWifi.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>


#include <dhtnew.h>

DHTNEW mySensor(27);   //  ESP 16    UNO 5    MKR1010 5
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


#define TX_PIN_A 17
#define RX_PIN_A 16
#define ENABLE_PIN_A 21

#define TX_PIN_B 19
#define RX_PIN_B 18
#define ENABLE_PIN_B 23

#define VERBOS_OUTPUT true

byte dataPrefix[] = {149, 1, 250, 0};
byte outputDataA[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
byte outputDataB[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

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
const int DHTReadDelay = 300000; // 5 miutes
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

  digitalWrite(ENABLE_PIN_A, HIGH);
  digitalWrite(ENABLE_PIN_B, HIGH);

  wm.setHostname("outdoor-led-control");
  // wm.setEnableConfigPortal(false);
  // wm.setConfigPortalBlocking(false);
  wm.autoConnect();

  // OTA Configiration and Enable OTA
  Serial.println("\nEnabling OTA Feature");
  ArduinoOTA.setPassword("lonelybinary");
  ArduinoOTA.begin();

  // this will be called for each packet received
  artnet.setArtDmxCallback(onDmxFrame);
  artnet.begin();

  mqttClient.setServer("10.0.1.154", 1883); // Replace with your MQTT broker IP

}

void loop() {
  ArduinoOTA.handle();
  doWiFiManager();
  if (!mqttClient.connected()) reconnectMQTT();
  mqttClient.loop();

  // we call the read function inside the loop
  // OTA Handle
  

  artnet.read();
  // int checksum = calculateChecksum(data, 12);
  byte outputA[17] = {149, 1, 250, 0};
  for(int i = 0; i < 12; i++) {
    outputA[i + 4] = outputDataA[i];
  }
  outputA[16] = calculateChecksum(outputDataA, 12);

  byte outputB[17] = {149, 1, 250, 0};
  for(int i = 0; i < 12; i++) {
    outputB[i + 4] = outputDataB[i];
  }
  outputB[16] = calculateChecksum(outputDataB, 12);
  
  // Non blocking delayMicroseconds(575);
  static unsigned long serialTimer = 0;
  if(micros() - serialTimer >= 575) {
    writeToBusA(outputA);
    writeToBusB(outputB);
    serialTimer = millis();
  }
  
  /*
  Check from the DHT sensor only at the set frequency and 30 seconds after the last artnet command was received
  so that we are pretty sure artet commands are not going to be send during the DHT read delay (18ms).
  We also have another timer so that we are only reading from the humidity/temperature sensor every 5 minutes.
  */
  if( millis() - dhtReadTimestamp > DHTReadDelay && millis() - artnetLastRecieved > DHTAfterArtnetDelay) {
    readFromDHT();
    dhtReadTimestamp = millis();
  }

}



void writeToBusA(const byte *dataPacket) {
  // digitalWrite(ENABLE_PIN_A, HIGH);
  // delay(1);
  Serial1.write(dataPacket, 17);
  Serial1.flush();
  artnetLastRecieved = millis();
}

void writeToBusB(const byte *dataPacket) {
  // digitalWrite(ENABLE_PIN_B, HIGH);
  // delay(1);
  Serial2.write(dataPacket, 17);
  Serial2.flush();
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
  
  if(VERBOS_OUTPUT) {
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
    
    if( VERBOS_OUTPUT ) { Serial.print(data[i]); };

    if(i < 8) {
      if( VERBOS_OUTPUT ) { Serial.print(" A: "); };
      outputDataA[i] = data[i];
    }
    else {
      if( VERBOS_OUTPUT ) { Serial.print(" B: "); };
      outputDataB[i - 8] = data[i];
    }
    

    if( VERBOS_OUTPUT ) { Serial.print(" "); };
  }

  if (mqttClient.connected()) {
    publishColorState();
  }

  if( VERBOS_OUTPUT ) { 
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

  if (chk != DHTLIB_WAITING_FOR_READ) {
    switch (chk)
    {
      case DHTLIB_OK:
        Serial.print("OK,\t");
        break;
      case DHTLIB_ERROR_CHECKSUM:
        Serial.print("Checksum error,\t");
        break;
      case DHTLIB_ERROR_TIMEOUT_A:
        Serial.print("Time out A error,\t");
        break;
      case DHTLIB_ERROR_TIMEOUT_B:
        Serial.print("Time out B error,\t");
        break;
      case DHTLIB_ERROR_TIMEOUT_C:
        Serial.print("Time out C error,\t");
        break;
      case DHTLIB_ERROR_TIMEOUT_D:
        Serial.print("Time out D error,\t");
        break;
      case DHTLIB_ERROR_SENSOR_NOT_READY:
        Serial.print("Sensor not ready,\t");
        break;
      case DHTLIB_ERROR_BIT_SHIFT:
        Serial.print("Bit shift error,\t");
        break;
      default:
        Serial.print("Unknown: ");
        Serial.print(chk);
        Serial.print(",\t");
        break;
    }

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

    bool tempOk = mqttClient.publish("outdoor-led/temp", tempPayload);
    bool humOk = mqttClient.publish("outdoor-led/humidity", humPayload);

    if( VERBOS_OUTPUT ) { 
    Serial.printf("Temp publish: %s, Humidity publish: %s\n", tempOk ? "OK" : "FAIL", humOk ? "OK" : "FAIL");
    }
  }

  
}

void reconnectMQTT() {
  static unsigned long reconnectMQTTimer = 0;
  if (!mqttClient.connected() && millis() - reconnectMQTTimer > 5000 ) {
    Serial.print("Attempting MQTT connection...");
    if (mqttClient.connect("ESP32Client", "<user>", "<password>")) {
    // if (mqttClient.connect("ESP32Client", "colin", "AZoJQ$:%P5k@\\CgKkHCKjm^MbT.njfY$aL")) {
      Serial.println("connected");
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

  if( VERBOS_OUTPUT ) { 
    Serial.printf("Color publish: %s\n", pubOk ? "OK" : "FAIL");
  }
}

