#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <SPI.h>
#include <stdint.h>
#include <WebSocketsClient.h>
#include <Hash.h>
#include <ArduinoJson.h>
#include "FS.h"
#include "MFRC522.h"

/* wiring the MFRC522 to ESP8266 (ESP-12)
RST     = GPIO5
SDA(SS) = GPIO4
MOSI    = GPIO13
MISO    = GPIO12
SCK     = GPIO14
GND     = GND
3.3V    = 3.3V
*/

#define RST_PIN                 5  // RST-PIN für RC522 - RFID - SPI - Modul GPIO5
#define SS_PIN                  16  // SDA-PIN für RC522 - RFID - SPI - Modul GPIO4

#define DATA_PIN                4
#define CLK_PIN                 15
#define LATCH_PIN               2

#define BTN_PIN                 0

#define LED_UPDATE_INTERVAL     5 // 5 ms
#define CARD_SCAN_INTERLVAL     100 // 0,5 s
#define ONE_MINUTE              1200 //60000 // 60k ms
#define WASH_TIME               35 // 35 min
#define SEND_BROADCAST_CMD      1000
#define SEND_PING_CMD           1000

#define UDP_PORT                6789
#define TCP_PORT                8001

//String tagID= "A1 32 71 8B";
String tagID= "20 12 F1 19";

const char *ssid;     // change according to your Network - cannot be longer than 32 characters!
const char *pass; // change according to your Network

char packetBuffer[255];          // buffer to hold incoming packet

//const uint8_t number[10] = {0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F}; // katot chung
const uint8_t number[10] = {0xC0,0xF9,0xA4,0xB0,0x99,0x92,0x82,0xF8,0x80,0x90}; // anot chung

bool isStarted = false;
bool isWifiConnected = false;
bool isServerConnected = false;
bool isGotIpServer = false;
bool isNewCardFound = false;
bool isSameCard = false;
bool state_role = false;

volatile byte state_button = false;

byte card_uid[4] = {0xA1, 0x32, 0x71, 0x8B};

uint8_t cardNotFoundCount = 0;
uint8_t cardFoundCount  = 0;

uint32_t prevLedUpdateTime = 0;
uint32_t prevCardScanTime = 0;
uint32_t prevMinuteTime = 0;
uint32_t prevSendBroadcastCmdTime = 0;
uint32_t prevSendPingTime = 0;
uint32_t operateTime = 0;
uint32_t pingSendCount = 0;
uint32_t washTime = 0;
uint32_t deviceID = 0;

uint8_t ledData = 0;
uint8_t ledCtrlData = 0;
uint8_t ledPos = 0;

MFRC522 mfrc522(SS_PIN, RST_PIN); // Create MFRC522 instance

WiFiUDP udpClient;
WebSocketsClient webSocket;
IPAddress broadcastIp;
IPAddress remoteIp;

String socketCmd = "";

void setup() {
    size_t size;

    Serial.begin(9600);    // Initialize serial communications
    delay(100);

    Serial.println("\r\nInitialize....");

    SPI.begin();           // Init SPI bus
    mfrc522.PCD_Init();    // Init MFRC522

    // Init pin
    pinMode(DATA_PIN, OUTPUT);
    pinMode(CLK_PIN, OUTPUT);
    pinMode(LATCH_PIN, OUTPUT);
    pinMode(BTN_PIN, INPUT_PULLUP);
    //attachInterrupt(digitalPinToInterrupt(BTN_PIN), btnHandler, FALLING );

    digitalWrite(DATA_PIN, LOW);
    digitalWrite(CLK_PIN, LOW);
    digitalWrite(LATCH_PIN, LOW);

    if (!SPIFFS.begin()) {
        Serial.println("Failed to mount file system");
    }

    // load config info
    File configFile = SPIFFS.open("/config.json", "r");
    if (!configFile) {
        Serial.println("Failed to open config file");
    }
    else
    {
        size = configFile.size();
        if (size > 1024) {
            Serial.println("Config file size is too large");
        }
        else
        {
            // Allocate a buffer to store contents of the file.
            std::unique_ptr<char[]> buf(new char[size]);

            configFile.readBytes(buf.get(), size);

            StaticJsonBuffer<200> jsonBuffer;
            JsonObject& json = jsonBuffer.parseObject(buf.get());

            if (!json.success()) {
                Serial.println("Failed to parse config file");
            }

            ssid = json["ssid"];
            pass = json["pass"];
            deviceID = json["deviceID"];

            Serial.println(String(ssid));
            Serial.println(String(pass));
            Serial.println(deviceID);
        }
    }

    WiFi.begin(ssid, pass);

    udpClient.begin(UDP_PORT);

    Serial.println(F("Ready!"));
}

void btnHandler(void)
{
    static byte i = false;
    if(state_role == true) state_role = false;
    else state_role = true;

    setRelayEnable(bool(state_role));
    state_button=true;
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            {
                Serial.printf("[WSc] Disconnected!\n");
                isServerConnected = false;
            }
            break;
        case WStype_CONNECTED:
            {
                Serial.printf("[WSc] Connected to url: %s\n",  payload);
                isServerConnected = true;
            }
            break;
        case WStype_TEXT:
            {
                pingSendCount = 0;
                Serial.printf("[WSc] get text: %s\n", payload);
            }
            break;
        case WStype_BIN:
            break;
    }
}

void loop() {

    uint32_t curTime = millis();
    uint32_t packetSize = 0;

    if (WiFi.status() == WL_CONNECTED && isWifiConnected == false)
    {
        if (!isWifiConnected)
        {
            broadcastIp = ~WiFi.subnetMask() | WiFi.gatewayIP();
            isWifiConnected = true;
        }
        Serial.println("Connected");
    }
    else if (WiFi.status() != WL_CONNECTED && isWifiConnected == true)
    {
        Serial.println("Disconnected");
        if (isServerConnected)
        {
            // disconnect
            isServerConnected = false;
        }
        isWifiConnected = false;
        isGotIpServer = false;
    }

    if (isWifiConnected && isGotIpServer)
    {
        webSocket.loop();
    }

    if (curTime - prevCardScanTime >= CARD_SCAN_INTERLVAL) // every 0.1s
    {
        prevCardScanTime = curTime;

        if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial())
        {
            //Show UID on serial monitor
            //Serial.print("UID tag :");
            String content= "";
            byte letter;
            for (byte i = 0; i < mfrc522.uid.size; i++)
            {
               Serial.print(mfrc522.uid.uidByte[i] < 0x10 ? " 0" : " ");
               Serial.print(mfrc522.uid.uidByte[i], HEX);
            }
            isNewCardFound = true;
            Serial.println();
        }
    }

    if (isWifiConnected)
    {
        if (!isGotIpServer)
        {
            if (curTime - prevSendBroadcastCmdTime >= SEND_BROADCAST_CMD)
            {
                prevSendBroadcastCmdTime = curTime;
                Serial.println("broadcast msg");
                // send broadcast cmd
                udpClient.beginPacket(broadcastIp, UDP_PORT);
                udpClient.write("{\"client\":\"hello server\"}");
                udpClient.endPacket();
            }
            else
            {
                packetSize = udpClient.parsePacket();

                if (packetSize)
                {
                    // read the packet into packetBufffer
                    int len = udpClient.read(packetBuffer, 255);
                    if (len > 0) {
                      packetBuffer[len] = 0;
                    }
                    remoteIp = udpClient.remoteIP();
                    isGotIpServer = true;
                    Serial.println(remoteIp);
                    // if got ip server
                    {
                        String ipAddr;
                        ipAddr += remoteIp[0];
                        ipAddr += '.';
                        ipAddr += remoteIp[1];
                        ipAddr += '.';
                        ipAddr += remoteIp[2];
                        ipAddr += '.';
                        ipAddr += remoteIp[3];
                        webSocket.beginSocketIO(ipAddr, TCP_PORT);
                        webSocket.onEvent(webSocketEvent);
                    }
                }
            }
        }
        else
        {
            // if (pingSendCount >= 10 && isServerConnected)
            // {
            //     pingSendCount = 0;
            //     webSocket.disconnect();
            //     isServerConnected = false;
            //     isGotIpServer = false;
            //     Serial.println("out of ping count");
            // }
            if (isServerConnected)
            {
                if (isNewCardFound)
                {
                    isNewCardFound = false;
                    //send card uid
                    socketCmd += "42[\"wm_ask_for_run\",";
                    socketCmd += "{";
                    socketCmd += "\"deviceID\":";
                    socketCmd += deviceID;
                    socketCmd += ",";
                    socketCmd += "\"cardID\":";
                    socketCmd += deviceID;
                    socketCmd += "}]";
                    // send ping
                    Serial.println(socketCmd);
                    webSocket.sendTXT(socketCmd);
                    socketCmd = "";
                }
                else
                {
                    if (curTime - prevSendPingTime >= SEND_PING_CMD)
                    {
                        prevSendPingTime = curTime;
                        // pingSendCount++;
                        // webSocket.sendTXT("42[\"messageType\",{\"greeting\":\"hello\"}]");
                        socketCmd += "42[\"wm_ping\",";
                        socketCmd += "{";
                        socketCmd += "\"deviceID\":";
                        socketCmd += deviceID;
                        socketCmd += "}]";
                        // send ping
                        // Serial.println(socketCmd);
                        webSocket.sendTXT(socketCmd);
                        socketCmd = "";
                    }
                }
            }
        }
    }
}

bool compareCardUID(byte *buffer, byte bufferSize)
{
    byte i = 0;
    bool retVal = true;

    for(i = 0; i < bufferSize; i++)
    {
        if (buffer[i] != card_uid[i])
        {
            retVal = false;
            break;
        }
    }

    return retVal;
}

void setRelayEnable(bool enable)
{
  if (enable)
  {
    ledCtrlData = (ledCtrlData & 0x07) | (0x03 << 3);
  }
  else
  {
    ledCtrlData = ledCtrlData & (~(0x03 << 3));
  }
}

void ledUpdate(void)
{

  switch(ledPos){
    case 0: ledData = number[washTime / 100]; break;
    case 1: ledData = number[(washTime / 10 ) % 10]; break;
    //case 2: ledData = number[washTime % 10]; break;
  }

  ledCtrlData = (1 << ledPos) | (ledCtrlData & 0xF8);

  if (++ledPos >= 3) ledPos = 0;

  // push out data
  digitalWrite(LATCH_PIN, LOW);
  shiftOut(DATA_PIN, CLK_PIN, ledCtrlData);
  shiftOut(DATA_PIN, CLK_PIN, ledData);
  digitalWrite(LATCH_PIN, HIGH);
}

void shiftOut(int myDataPin, int myClockPin, byte myDataOut) {
  // This shifts 8 bits out MSB first,
  //on the rising edge of the clock,
  //clock idles low

  int i=0;
  int pinState;

  pinMode(myClockPin, OUTPUT);
  pinMode(myDataPin, OUTPUT);

  digitalWrite(myDataPin, 0);
  digitalWrite(myClockPin, 0);

  for (i=7; i>=0; i--)  {
    digitalWrite(myClockPin, 0);

    if ( myDataOut & (1<<i) ) {
      pinState= 1;
    }
    else {
      pinState= 0;
    }

    //Sets the pin to HIGH or LOW depending on pinState
    digitalWrite(myDataPin, pinState);
    //register shifts bits on upstroke of clock pin
    digitalWrite(myClockPin, 1);
    //zero the data pin after shift to prevent bleed through
    digitalWrite(myDataPin, 0);
  }

  //stop shifting
  digitalWrite(myClockPin, 0);
}
