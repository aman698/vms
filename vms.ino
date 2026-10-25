#include <SPI.h>
#include <EEPROM.h>
#include <Ethernet.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#include "DMD_RGB.h"

#pragma GCC diagnostic ignored "-Wnarrowing"
#pragma GCC diagnostic ignored "-Woverflow"
#pragma GCC diagnostic warning "-Wnarrowing"
#pragma GCC diagnostic warning "-Woverflow"

/* =================== *
-     RP2040 pins      *
-   =================== */
#define DMD_PIN_A 6
#define DMD_PIN_B 7
#define DMD_PIN_C 8
#define DMD_PIN_D 9

uint8_t mux_list[] = { DMD_PIN_A, DMD_PIN_B, DMD_PIN_C, DMD_PIN_D };

#define DMD_PIN_nOE 10
#define DMD_PIN_SCLK 12

uint8_t custom_rgbpins[] = { 11, 0, 1, 2, 3, 4, 5 }; // CLK, R0, G0, B0, R1, G1, B1

// Panel Specifications
#define DISPLAYS_ACROSS 1                     
#define DISPLAYS_DOWN 1
#define ENABLE_DUAL_BUFFER false

// Hardware Pins & Ethernet Settings
#define ETHERNET_SS_PIN 17
#define ETHERNET_RST_PIN 24
#define HeartBeatLed 25
#define HARDRST 27
#define LDR -1

#define SERVER_PORT 1000
byte SERVER_MAC[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
const int ip_array[4] = {192, 168, 0, 125};
const int gateway_array[4] = {192, 168, 0, 1};
const int subnet_array[4] = {255, 255, 255, 0};
const int dns_array[4] = {192, 168, 0, 1};

// ==========================================
// EEPROM LAYOUT & SEPARATE PARTITION FLAGS
// ==========================================
#define EEPROM_IP_ADDR                 0    // Bytes 0-3
#define EEPROM_ADDR_IP_FLAG            4    // Byte 4
#define EEPROM_PORT_ADDR               5    // Bytes 5-6
#define EEPROM_ADDR_PORT_FLAG          7    // Byte 7

// Alag partition for Panel Type to avoid any conflict
#define EEPROM_PANEL_ADDR              8    // Byte 8 (Stores 1 or 2)
#define EEPROM_ADDR_PANEL_FLAG         9    // Byte 9 (Dedicated Flag for Panel)

#define EEPROM_FLAG_SET                0xA5 // Flag for IP/Port
#define EEPROM_PANEL_FLAG_SET          0x5A // Unique separate Flag for Panel Type

// Serial Configs
#define SERIAL_BAUDRATE 115200
#define SERIAL_INTERFACE Serial

// ==========================================
// GLOBALS & OBJECTS
// ==========================================

EthernetClient client;
EthernetServer* server = nullptr;

// We use pointers because the panel type is selected dynamically at runtime from EEPROM
DMD_RGB<RGB32x16_s4_miralay001, COLOR_4BITS>* dmd_s4 = nullptr;
DMD_RGB<RGB32x32_S8_SM16169, COLOR_4BITS>* dmd_s8 = nullptr;
bool activeIsS8 = false; // true if S8, false if S4

uint16_t bg = 0;  // background - black
uint16_t fg = 0;  // foreground

// Helper wrappers to handle whichever panel object is active
int getPanelHeight() {
  return activeIsS8 ? dmd_s8->height() : dmd_s4->height();
}
int getPanelWidth() {
  return activeIsS8 ? dmd_s8->width() : dmd_s4->width();
}
void drawPixel(int x, int y, uint16_t color) {
  if (activeIsS8) dmd_s8->drawPixel(x, y, color);
  else dmd_s4->drawPixel(x, y, color);
}
void swapBuffers(bool val) {
  if (activeIsS8) dmd_s8->swapBuffers(val);
  else dmd_s4->swapBuffers(val);
}
void fillScreen(uint16_t color) {
  if (activeIsS8) dmd_s8->fillScreen(color);
  else dmd_s4->fillScreen(color);
}

// ==========================================
// FUNCTIONS
// ==========================================

void checkHardReset() {
  static unsigned long lowSince = 0;
  static bool wasLow = false;
  static bool triggered = false;

  bool isLow = (digitalRead(HARDRST) == LOW);

  if (!isLow) {
    wasLow = false;
    triggered = false;
    return;
  }

  if (!wasLow) {
    wasLow = true;
    lowSince = millis();
    return;
  }

  if (triggered) return;

  if (millis() - lowSince > 10000) {
    triggered = true;
    // Clear all EEPROM configuration flags so it falls back to defaults on reboot
    EEPROM.write(EEPROM_ADDR_IP_FLAG, 0xFF);
    EEPROM.write(EEPROM_ADDR_PANEL_FLAG, 0xFF);
    EEPROM.commit();
    SERIAL_INTERFACE.println("HARD RESET: EEPROM configuration cleared. Please reboot.");
  }
}

String getMessageEthernet() {
  static char buf[128];
  size_t len = 0;

  client = server->available();
  if (!client) return "";

  unsigned long lastByteMillis = millis();
  while (len < sizeof(buf) - 1) {
    if (client.available() > 0) {
      buf[len++] = client.read();
      lastByteMillis = millis();
    } else if (millis() - lastByteMillis > 5) {
      break;
    }
  }
  buf[len] = '\0';

  return String(buf);
}

int parseCommand(String str) {
  if (str == "") return -1;

  int commaIndex1 = str.indexOf(',');
  String firstValue = str.substring(0, commaIndex1);

  if (firstValue == "SET") {
    // Command format: SET,<ip1>,<ip2>,<ip3>,<ip4>
    int secondCommaIndex1 = str.indexOf(',', commaIndex1 + 1);
    int thirdCommaIndex1 = str.indexOf(',', secondCommaIndex1 + 1);
    int fourthCommaIndex1 = str.indexOf(',', thirdCommaIndex1 + 1);
    if (secondCommaIndex1 == -1 || thirdCommaIndex1 == -1 || fourthCommaIndex1 == -1) return -1;

    String ip1 = str.substring(commaIndex1 + 1, secondCommaIndex1);
    String ip2 = str.substring(secondCommaIndex1 + 1, thirdCommaIndex1);
    String ip3 = str.substring(thirdCommaIndex1 + 1, fourthCommaIndex1);
    String ip4 = str.substring(fourthCommaIndex1 + 1);

    EEPROM.write(EEPROM_IP_ADDR + 0, ip1.toInt());
    EEPROM.write(EEPROM_IP_ADDR + 1, ip2.toInt());
    EEPROM.write(EEPROM_IP_ADDR + 2, ip3.toInt());
    EEPROM.write(EEPROM_IP_ADDR + 3, ip4.toInt());
    EEPROM.write(EEPROM_ADDR_IP_FLAG, EEPROM_FLAG_SET);
    EEPROM.commit();
    
    server->println("IP SAVED SUCCESSFULLY. Please reboot the device.");
    SERIAL_INTERFACE.println("New IP saved to EEPROM via network command.");
  }
  else if (firstValue == "PANEL") {
    // Command format: PANEL,1 (for RGB32x16_s4_miralay001) or PANEL,2 (for RGB32x32_S8_SM16169)
    int secondCommaIndex1 = str.indexOf(',', commaIndex1 + 1);
    String panelValStr;
    if (secondCommaIndex1 == -1) {
      panelValStr = str.substring(commaIndex1 + 1);
    } else {
      panelValStr = str.substring(commaIndex1 + 1, secondCommaIndex1);
    }
    
    int panelChoice = panelValStr.toInt();

    if (panelChoice == 1 || panelChoice == 2) {
      EEPROM.write(EEPROM_PANEL_ADDR, panelChoice);
      EEPROM.write(EEPROM_ADDR_PANEL_FLAG, EEPROM_PANEL_FLAG_SET); // Uses separate partition flag 0x5A
      EEPROM.commit();
      server->println("PANEL TYPE SAVED SUCCESSFULLY. Please reboot the device.");
      SERIAL_INTERFACE.println("New Panel Type saved to EEPROM. Please reboot.");
    } else {
      server->println("INVALID PANEL CHOICE. Use PANEL,1 or PANEL,2");
    }
  }

  return -1;
}

void setup() {
  SERIAL_INTERFACE.begin(SERIAL_BAUDRATE);
  EEPROM.begin(64);

  pinMode(HeartBeatLed, OUTPUT);
  pinMode(HARDRST, INPUT_PULLUP);

  // Read Panel Type from dedicated EEPROM partition (Default to Choice 1 if not set)
  int panelChoice = 1;
  if (EEPROM.read(EEPROM_ADDR_PANEL_FLAG) == EEPROM_PANEL_FLAG_SET) {
    panelChoice = EEPROM.read(EEPROM_PANEL_ADDR);
  }

  // Initialize corresponding panel object based on EEPROM choice
  if (panelChoice == 2) {
    activeIsS8 = true;
    dmd_s8 = new DMD_RGB<RGB32x32_S8_SM16169, COLOR_4BITS>(mux_list, DMD_PIN_nOE, DMD_PIN_SCLK, custom_rgbpins, DISPLAYS_ACROSS, DISPLAYS_DOWN, ENABLE_DUAL_BUFFER);
    dmd_s8->init();
    dmd_s8->setBrightness(50);
    fg = dmd_s8->Color888(255, 0, 0); // RED
    SERIAL_INTERFACE.println("Initialized Panel Type: RGB32x32_S8_SM16169 (Choice 2)");
  } else {
    activeIsS8 = false;
    dmd_s4 = new DMD_RGB<RGB32x16_s4_miralay001, COLOR_4BITS>(mux_list, DMD_PIN_nOE, DMD_PIN_SCLK, custom_rgbpins, DISPLAYS_ACROSS, DISPLAYS_DOWN, ENABLE_DUAL_BUFFER);
    dmd_s4->init();
    dmd_s4->setBrightness(50);
    fg = dmd_s4->Color888(255, 0, 0); // RED
    SERIAL_INTERFACE.println("Initialized Panel Type: RGB32x16_s4_miralay001 (Choice 1)");
  }

  // Pulse W5500 Reset line low -> high for stable startup
  if (ETHERNET_RST_PIN != -1) {
    pinMode(ETHERNET_RST_PIN, OUTPUT);
    digitalWrite(ETHERNET_RST_PIN, LOW);
    delay(5);
    digitalWrite(ETHERNET_RST_PIN, HIGH);
    delay(150);
  }

  Ethernet.init(ETHERNET_SS_PIN);

  int port = SERVER_PORT;
  if (EEPROM.read(EEPROM_ADDR_PORT_FLAG) == EEPROM_FLAG_SET) {
    port = EEPROM.read(EEPROM_PORT_ADDR) | (EEPROM.read(EEPROM_PORT_ADDR + 1) << 8);
  }
  server = new EthernetServer(port);

  if (EEPROM.read(EEPROM_ADDR_IP_FLAG) == EEPROM_FLAG_SET) {
    IPAddress SERVER_IP(EEPROM.read(EEPROM_IP_ADDR), EEPROM.read(EEPROM_IP_ADDR + 1), EEPROM.read(EEPROM_IP_ADDR + 2), EEPROM.read(EEPROM_IP_ADDR + 3));
    Ethernet.begin(SERVER_MAC, SERVER_IP);
    SERIAL_INTERFACE.println("Ethernet initialized with EEPROM-saved IP.");
  } else {
    IPAddress SERVER_IP(ip_array[0], ip_array[1], ip_array[2], ip_array[3]);
    Ethernet.begin(SERVER_MAC, SERVER_IP);
    SERIAL_INTERFACE.println("Ethernet initialized with default ip_array IP.");
  }
  
  server->begin();
  SERIAL_INTERFACE.print("Server listening on IP: ");
  SERIAL_INTERFACE.println(Ethernet.localIP());

  fillScreen(bg);
  swapBuffers(true);
}

void loop() {
  digitalWrite(HeartBeatLed, HIGH);
  delay(50);
  digitalWrite(HeartBeatLed, LOW);
  delay(50);

  String str = getMessageEthernet();
  if (str != "") {
    server->println(str);
    parseCommand(str);
  }

  // Fill the matrix row by row (Red pattern test)
  for (int i = 0; i < getPanelHeight(); i++) {
      for (int j = 0; j < getPanelWidth(); j++) {
          drawPixel(j, i, fg);
          swapBuffers(true);
          delay(10);
      }
  }
  
  fillScreen(bg);
  swapBuffers(true);

  checkHardReset();
}