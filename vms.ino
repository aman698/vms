#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include "pico/multicore.h"
#include "hardware/sync.h"
#include "DMD_RGB.h"

#define DISPLAYS_ACROSS 2   // 2 x 2 panels of 32x32 = 64x64
#define DISPLAYS_DOWN 2
#define ETHERNET_RST_PIN 24
#define IMAGE_W (DISPLAYS_ACROSS * 32)
#define IMAGE_H (DISPLAYS_DOWN * 32)
#define IMAGE_PIXELS (IMAGE_W * IMAGE_H)
#define FRAME_BYTES (IMAGE_PIXELS * 2u)   // RGB565, low byte first

// UDP packet: [frame id: u16 LE][byte offset in frame: u16 LE][pixel data]
// Packets may arrive in any order; a frame is shown once all its bytes are in.
#define HDR_BYTES 4

#define PANELTYPE RGB32x32_S8_SM16169
#define PANELCOLORMODE COLOR_4BITS

#define PIN_A 6
#define PIN_B 7
#define PIN_C 8
#define PIN_D 9
#define PIN_nOE 10
#define PIN_SCLK 12

uint8_t RGBPinsList[] = {11, 0, 1, 2, 3, 4, 5};
uint8_t addressMuxList[] = {PIN_A, PIN_B, PIN_C, PIN_D};

#define SERVER_PORT 1000
byte MAC[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x01};
IPAddress ip(192, 168, 0, 121);

// byte MAC[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x02};
// IPAddress ip(192, 168, 2, 122);

EthernetUDP Udp;

uint16_t frameA[IMAGE_PIXELS];
uint16_t frameB[IMAGE_PIXELS];
uint16_t *volatile front = frameA;   // core0 owns it while frameReady == true
uint16_t *volatile back = frameB;    // only core1 touches it

volatile bool frameReady = false;


DMD_RGB<PANELTYPE, PANELCOLORMODE> PANEL(
  addressMuxList,
  PIN_nOE,
  PIN_SCLK,
  RGBPinsList,
  DISPLAYS_ACROSS,
  DISPLAYS_DOWN,
  false  // SM16169 keeps the frame in its own memory; library double buffer must stay off
);


void core1_loop() {
  static uint8_t pkt[1500];
  uint16_t frameId = 0;
  uint32_t got = 0;   // bytes received so far for frameId

  while (true) {
    if (Udp.parsePacket() <= 0) continue;
    int len = Udp.read(pkt, sizeof(pkt));
    if (len <= HDR_BYTES) continue;

    uint16_t id = pkt[0] | (pkt[1] << 8);
    uint32_t off = pkt[2] | (pkt[3] << 8);
    uint32_t dataLen = len - HDR_BYTES;
    if (off + dataLen > FRAME_BYTES) continue;   // malformed packet

    // A new frame id starts a new frame; an incomplete older frame is dropped.
    if (id != frameId) {
      frameId = id;
      got = 0;
    } else if (got >= FRAME_BYTES) {
      continue;   // this frame is already complete, ignore duplicates
    }

    memcpy(((uint8_t*)back) + off, pkt + HDR_BYTES, dataLen);
    got += dataLen;

    // Hand the frame over only if core0 is done with the previous one,
    // otherwise drop it so we never overwrite a buffer being drawn.
    if (got >= FRAME_BYTES && !frameReady) {
      uint16_t *tmp = front;
      front = back;
      back = tmp;
      __dmb();
      frameReady = true;
    }
  }
}


void setup() {
  Serial.begin(115200);
  if (ETHERNET_RST_PIN != -1) {
    pinMode(ETHERNET_RST_PIN, OUTPUT);
    digitalWrite(ETHERNET_RST_PIN, LOW);
    delay(5);
    digitalWrite(ETHERNET_RST_PIN, HIGH);
    delay(150);
  }
  Ethernet.init(17);
  Ethernet.begin(MAC, ip);
  Udp.begin(SERVER_PORT);

  PANEL.init();
  PANEL.setBrightness(100);  //8 255
  // PANEL.setConnectScheme(CONNECT_ZIGZAG);  // uncomment if the bottom row of panels is mounted upside down

  multicore_launch_core1(core1_loop);
}


void loop() {
  if (!frameReady) return;
  __dmb();

  uint16_t *buf = front;
  for (int y = 0; y < IMAGE_H; y++) {
    for (int x = 0; x < IMAGE_W; x++) {
      PANEL.drawPixel(x, y, buf[y * IMAGE_W + x]);  // library does the 565 -> 4-bit conversion
    }
  }
  PANEL.swapBuffers(false);  // SM16169: uploads the drawn frame into the panel's memory

  __dmb();
  frameReady = false;  // give 'front' back to core1
}
