#include "esp_task_wdt.h"
#include "sx1280.h"
#include "libb64/cdecode.h"
#include "libb64/cencode.h"
#include "carousel.h"
#include "customize.h"
#include "settings.h"
#include "driver/gpio.h"

carousel data_carousel;				// File downloader for this stream
portMUX_TYPE sxMux;
extern bool sdCardPresent;
unsigned int filepacket, filepackets;
char filename[260] = "";
AsyncUDP udp;
int32_t offset;

// SX1280 variables
SX128XLT LT;
uint32_t Frequency;
int32_t _Offset = 0;                        //offset frequency for calibration purposes  
uint8_t Bandwidth;          //LoRa bandwidth
uint8_t SpreadingFactor;        //LoRa spreading factor
uint8_t CodeRate;            //LoRa coding rate

uint32_t packetsRX = 0;
uint16_t IRQStatus;

uint8_t RXPacketL;                               //stores length of packet received
int8_t  PacketRSSI;                              //stores RSSI of received packet
int8_t  PacketSNR;                               //stores signal to noise ratio (SNR) of received packet
extern xTaskHandle rxTaskHandle;
static uint16_t crc, header;
bool isFormatting;

//    bitrate variables
static uint8_t packetsCount;
static uint8_t lastPacket;
static uint8_t packets[100] = {0};
static uint8_t y = 0;

#define BLINK_PIN    GPIO_NUM_5

bool loraReady;                           // variable to display LoRa fault with led or on website

struct midi_data {
    uint8_t note;
    float   start;
    uint8_t velocity;
    float   duration;
};

struct midi_data midiarray[32];
uint8_t firstnote = 0;
char* txtarray;

/**
 * ISR function from SX1280
 */
IRAM_ATTR void rx1280ISR()
{
  xTaskNotify(rxTaskHandle, 0x0, eSetBits);
}

void init_gpio(void) {
  gpio_reset_pin(BLINK_PIN);
  gpio_set_direction(BLINK_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(BLINK_PIN, 0);
}

void blinky(void *pvParameter)
{
  gpio_set_level(BLINK_PIN, 1);
  vTaskDelay(50 / portTICK_RATE_MS); // sleep 500ms
  gpio_set_level(BLINK_PIN, 0);
  vTaskDelete(NULL);
}

/**
 * Read data from SX1280
 */
uint8_t readbufferSX1280(uint8_t *rxbuffer, uint8_t size)
{
  uint8_t RXstart, _RXPacketL;
  uint16_t regdata;
  uint8_t buffer[2];

  regdata = LT.readIrqStatus();

  if ( (regdata & IRQ_HEADER_ERROR) | (regdata & IRQ_CRC_ERROR) | (regdata & IRQ_RX_TX_TIMEOUT ) ) //check if any of the preceding IRQs is set
  {
    return 0;
  }

  LT.readCommand(RADIO_GET_RXBUFFERSTATUS, buffer, 2);
  _RXPacketL = buffer[0];

  RXstart = buffer[1];

  LT.checkBusy();
  
  SPI.beginTransaction(SPISettings(LTspeedMaximum, LTdataOrder, LTdataMode));

  digitalWrite(LORA_NSS, LOW);
  SPI.transfer(RADIO_READ_BUFFER);
  SPI.transfer(RXstart);
  SPI.transfer(0xFF);

  SPI.transfer(rxbuffer, (uint32_t)_RXPacketL);

  digitalWrite(LORA_NSS, HIGH);

  SPI.endTransaction();

  return _RXPacketL;
}

/**
 * Helper function to feed website with stats
 */
extern "C" void getStats(uint16_t* _crc, uint16_t* _header)
{
  *_crc = crc;
  *_header = header;
}

extern "C" void getMidi(char* _txtarray)
{
  const char* txt_pre = "{\"type\":\"midi\",\"data\":[";
  const char* txt_suf = "]}";
  const char* txt_template = "[%d,%f,%d,%f],";
  int strlength = 0;
  strlength += sprintf(_txtarray+strlength, txt_pre);
  for (midi_data x : midiarray)
  {
    strlength += sprintf(_txtarray+strlength, txt_template, x.note, x.start, x.velocity, x.duration);
  }
  strlength += sprintf(_txtarray+strlength-1, txt_suf);
}

/**
 * Helper function to feed website with stats
 */
extern "C" void getPacketStats(int8_t* rssi, int8_t* snr)
{
  *rssi = PacketRSSI;
  *snr = PacketSNR;
}

class mycallback : public carousel::callback {
  void fileComplete( const std::string &path ) {
    // Serial.printf("new file path: %s\n", path.c_str());
    strcpy(filename, path.c_str());
  }
	void processFile(unsigned int index, unsigned int count) {
    // Serial.printf("file progress: %d of %d packets\n", index, count);
    filepacket = index;
    filepackets = count;
  }
};

/**
 * Read packet from SX1280, when IRQ is triggered
 */
void rxTaskSX1280(void* p)
{
  static uint32_t mask = 0;
  data_carousel.init("/files/tmp", new mycallback());
  while(1) {
    if (xTaskNotifyWait(0, 0, &mask, portMAX_DELAY))
    {
      IRQStatus = LT.readIrqStatus();

      if(IRQStatus & IRQ_RX_DONE) {
        PacketRSSI = LT.readPacketRSSI();              //read the recived RSSI value
        PacketSNR = LT.readPacketSNR();                //read the received SNR value
        offset = LT.getFrequencyErrorRegValue();

        uint8_t data[256];

        RXPacketL = readbufferSX1280(data, RXBUFFER_SIZE);

        xTaskCreate(&blinky, "blinky", 512,NULL,5,NULL);

        LT.setRx(0);

        packetsRX++;
        lastPacket = y%100;
        packets[lastPacket] = RXPacketL;
        packetsCount++;
        y++;
        // Check if we got a special Realtime Packet
        // 0x73 = Midi Stream
        if(data[2] == 0x73){
          udp.writeTo(data+4, RXPacketL-8, IPAddress(239,1,2,3), 8281);
          uint8_t midibuf[10] = {0};
          int chunks = floor((RXPacketL-8)/10);
          //loop at data in 10byte chunks to parse Data
          memset(midiarray, 0, sizeof(midiarray));
          for (int i = 0; i < chunks; i++)
          {  
            memcpy(midibuf, data+4+i*10, 10);
          
            struct midi_data tmpmidi;

            memcpy(&tmpmidi.note, midibuf, 1);
            memcpy(&tmpmidi.start, midibuf+1, 4);
            memcpy(&tmpmidi.velocity, midibuf+5, 1);
            memcpy(&tmpmidi.duration, midibuf+6, 4);
            midiarray[i] = tmpmidi;
          }
        }

        if(RXPacketL > 0) {
          if (!isFormatting) {            // stop consuming data during sd card formatting to not access card
            if (!sdCardPresent)
            {
              portENTER_CRITICAL(&sxMux);
                data_carousel.consume(data, RXPacketL);
              portEXIT_CRITICAL(&sxMux);
            } else {
              data_carousel.consume(data, RXPacketL);
            }
          }
          data[0] = RXPacketL;
          udp.writeTo(data, RXPacketL, IPAddress(239,1,2,3), 8280);
        }
      }

      if (IRQStatus & IRQ_CRC_ERROR) {
        crc++;
      } 
      if (IRQStatus & IRQ_HEADER_ERROR) {
        header++;
      }
    }
  }
}

/**
 * Init SX1280 LORA
 */
void initSX1280()
{
  init_gpio();
  
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);

  //setup hardware pins used by device, then check if device is found
  if (LT.begin(LORA_NSS, NRESET, RFBUSY, DIO1,LORA_DEVICE))
  {
    // TODO add notify to app
    Serial.println(F("LoRa Device found"));

    //***************************************************************************************************
    //Setup LoRa device
    //***************************************************************************************************
    LT.setMode(MODE_STDBY_RC);
    LT.setRegulatorMode(USE_LDO);
    LT.setPacketType(PACKET_TYPE_LORA);
    LT.setRfFrequency(Frequency, _Offset);
    LT.setBufferBaseAddress(0, 0);
    LT.setModulationParams(SpreadingFactor, Bandwidth, CodeRate);
    //for LoRa order is PreambleLength, HeaderType, PayloadLength, CRC, InvertIQ/chirp invert, not used, not used
    LT.setPacketParams(0x23, LORA_PACKET_VARIABLE_LENGTH, 255, LORA_CRC_ON, LORA_IQ_NORMAL, 0, 0);
    // LT.writeRegister(0x891, 0xC0);
    LT.writeRegister(0x925, 0x32);

    LT.setDioIrqParams(IRQ_RADIO_ALL, (IRQ_HEADER_ERROR + IRQ_CRC_ERROR + IRQ_RX_DONE + IRQ_TX_DONE + IRQ_RX_TX_TIMEOUT), 0, 0);
    //***************************************************************************************************

    Serial.println();
    LT.printModemSettings();                               //reads and prints the configured LoRa settings, useful check
    Serial.println();
    LT.printOperatingSettings();                           //reads and prints the configured operating settings, useful check
    Serial.println();

    Serial.print(F("SX1280 is ready"));
    Serial.println();

    pinMode(DIO1, INPUT_PULLUP);
    attachInterrupt(DIO1, rx1280ISR, RISING);

    delay(100);
    LT.setRx(0);
    loraReady = true;
  }
  else
  {
    Serial.println(F("No device responding"));
    loraReady = false;
  }
}

extern "C" void updateLoraSettings(uint32_t freq, uint8_t bw, uint8_t sf, uint8_t cr)
{
  LT.setMode(MODE_STDBY_RC);
  Frequency = freq;
  SpreadingFactor = sf;
  Bandwidth = bw;
  CodeRate = cr;
  LT.setRfFrequency(Frequency, _Offset);
  LT.setModulationParams(SpreadingFactor, Bandwidth, CodeRate);
  LT.setRx(0);
  storeLoraSettings();
}

/**
 * Simple function to count LoRa bitrate
 * @param update - time in millis from last update
 */
uint16_t countBitrate(uint16_t update)
{
  uint16_t bitrate = 0;
  int n = 0;

  do
  {
    n = lastPacket - packetsCount;
    if (n < 0)
    {
      n += 100;
    }
    
    bitrate += packets[n];
    packetsCount--;
  } while (packetsCount > 0);

  log_d("bitrate: %d bits/s\n", bitrate * 8 * 1000/update);

  return bitrate * 8 * 1000/update; // bitrate in bits/s
}
