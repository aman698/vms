#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include "pico/multicore.h" 
#include "hardware/pio.h"
#include "DMD_RGB.h"
#include "hardware/dma.h"

#define IMAGE_W 32      
#define IMAGE_H 32      
#define IMAGE_PIXELS (IMAGE_W * IMAGE_H)

#define DISPLAYS_ACROSS 1      
#define DISPLAYS_DOWN 1      
#define PANELTYPE RGB32x32_S8_maxmurugan
#define PANELCOLORMODE COLOR_4BITS 

#define PIN_A 6
#define PIN_B 7
#define PIN_C 8
#define PIN_D 9
#define PIN_nOE 10
#define PIN_SCLK 12

#define DATA_PIN 2  
#define CLK_PIN 3  
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
uint16_t *front = frameA;
uint16_t *back = frameB;

volatile bool frameReady = false;
volatile bool receiving = false;
volatile uint32_t offset = 0;


PIO pio = pio0;
int sm;
int dma_chan;


DMD_RGB<PANELTYPE, PANELCOLORMODE> PANEL(
  addressMuxList,
  PIN_nOE,
  PIN_SCLK,
  RGBPinsList,
  DISPLAYS_ACROSS,
  DISPLAYS_DOWN,
  false 
);


inline void renderFrame(uint16_t *buf) {
  for (int y = 0; y < IMAGE_H; y++) {
    uint32_t rowOffset = y * IMAGE_W;
    for (int x = 0; x < IMAGE_W; x++) {
      uint16_t c = buf[rowOffset + x];
      uint8_t r = (c >> 12) & 0x0F;
      uint8_t g = (c >> 7) & 0x0F;
      uint8_t b = (c >> 1) & 0x0F;
      PANEL.drawPixel(x, y, (r << 12) | (g << 7) | (b << 1));
    }
  }
}


void core1_loop() {
  static uint8_t pkt[1500];
  while (true) {
    int size = Udp.parsePacket();
    if (size > 0) {
      int len = Udp.read(pkt, sizeof(pkt));

      if (len == 5 && pkt[0] == 'S') {
        receiving = true;
        offset = 0;
        continue;
      }

      if (len == 3 && pkt[0] == 'E') {
        if (offset >= IMAGE_PIXELS * 2) {
          frameReady = true;
          uint16_t *tmp = front;
          front = back;
          back = tmp;
        }
        receiving = false;
        continue;
      }

      if (receiving) {
        uint32_t copyLen = len;
        if (offset + copyLen > IMAGE_PIXELS * 2) copyLen = IMAGE_PIXELS * 2 - offset;
        memcpy(((uint8_t*)back) + offset, pkt, copyLen);
        offset += copyLen;
      }
    }
  }
}


static const uint16_t rgb_stream_program_instructions[] = {

  0x6000,   
  0x6401,   
};

static const pio_program_t rgb_stream_program = {
  .instructions = rgb_stream_program_instructions,
  .length = sizeof(rgb_stream_program_instructions) / sizeof(uint16_t),
  .origin = 0
};


void pio_dma_setup() {
  uint offset_pio = pio_add_program(pio, &rgb_stream_program); 

  sm = pio_claim_unused_sm(pio, true);  
  pio_sm_config sm_config = pio_get_default_sm_config();
  
  sm_config_set_out_pins(&sm_config, DATA_PIN, 1);  
  sm_config_set_out_pins(&sm_config, CLK_PIN, 1);   

  pio_sm_init(pio, sm, offset_pio, &sm_config);  

  dma_chan = dma_claim_unused_channel(true);
  dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
  channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
  channel_config_set_read_increment(&cfg, true);
  channel_config_set_write_increment(&cfg, false);


  dma_channel_configure(
    dma_chan,
    &cfg,
    &pio->txf[sm], 
    front,          
    IMAGE_PIXELS,  
    true            
  );
}


void setup() {
  Serial.begin(115200);

  Ethernet.init(17);
  Ethernet.begin(MAC, ip);
  Udp.begin(SERVER_PORT);

  pio_dma_setup();
  PANEL.init(); 
  PANEL.setBrightness(100);  //8 255

  multicore_launch_core1(core1_loop);  
}


void loop() {
  if (frameReady) {
    dma_channel_abort(dma_chan);
    dma_channel_set_read_addr(dma_chan, front, true);
    renderFrame(front);
    frameReady = false;
  }
}