/*
 * ORIGINAL SOURCE: Klusjesman, supersjimmie, Arjen Hiemstra
 * Project: https://github.com/arjenhiemstra/ithowifi
 * ESPHome port: John Halfords, with debugging and rework by Claude (Anthropic)
 * The original copyright and license remain applicable.
 * Port-specific edits are marked 'ESPHOME PORT CHANGE'.
 */
/*
   Author: Klusjesman, modified bij supersjimmie for Arduino/ESP8266
*/

#include "CC1101.h"

// default constructor
CC1101::CC1101()
{
  // ESPHOME PORT CHANGE: SPI and CS are initialized in setup, not in a global constructor.

} // CC1101

// default destructor
CC1101::~CC1101()
{
} //~CC1101

/***********************/
// SPI helper functions select() and deselect()
inline void CC1101::select(void)
{
  digitalWrite(csPin, LOW);
  // ESPHome port fix: wait until the CC1101 reports "ready" (MISO low) after CS goes low, as the datasheet
  // requires. After power-down (SPWD) the crystal needs ~150-250 us to start; without this wait, commands
  // and register writes sent right after wake-up can be lost.
  uint32_t t0 = micros();
  while (digitalRead(misoPin))
  {
    if (micros() - t0 > 2000)
    {
      misoTimeouts++;
      break;
    }
  }
}

inline void CC1101::deselect(void)
{
  digitalWrite(csPin, HIGH);
}

void CC1101::init()
{
  reset();
}

void CC1101::reset()
{
  deselect();
  delayMicroseconds(5);
  select();
  delayMicroseconds(40);
  deselect();
  delayMicroseconds(10);

  writeCommand(CC1101_SRES);
}

uint8_t CC1101::writeCommand(uint8_t command)
{
  uint8_t result;

  select();

  SPI.beginTransaction(SPISettings());
  result = SPI.transfer(command);
  SPI.endTransaction();

  deselect();

  return result;
}

void CC1101::writeRegister(uint8_t address, uint8_t data)
{
  select();

  SPI.beginTransaction(SPISettings());
  SPI.transfer(address);
  SPI.transfer(data);
  SPI.endTransaction();

  deselect();
}

uint8_t CC1101::readRegister(uint8_t address)
{
  uint8_t val;

  select();

  SPI.beginTransaction(SPISettings());
  SPI.transfer(address);
  val = SPI.transfer(0);
  SPI.endTransaction();

  deselect();

  return val;
}

uint8_t CC1101::readRegisterMedian3(uint8_t address)
{
  uint8_t val, val1, val2, val3;

  val1 = readRegister(address);
  val2 = readRegister(address);
  val3 = readRegister(address);

  // reverse sort (largest in val1) because this is te expected order for TX_BUFFER
  if (val3 > val2)
  {
    val = val3; // Swap(val3,val2)
    val3 = val2;
    val2 = val;
  }
  if (val2 > val1)
  {
    val = val2; // Swap(val2,val1)
    val2 = val1, val1 = val;
  }
  if (val3 > val2)
  {
    val = val3; // Swap(val3,val2)
    val3 = val2, val2 = val;
  }

  return val2;
}

/* Known SPI/26MHz synchronization bug (see CC1101 errata)
  This issue affects the following registers: SPI status byte (fields STATE and FIFO_BYTES_AVAILABLE),
  FREQEST or RSSI while the receiver is active, MARCSTATE at any time other than an IDLE radio state,
  RXBYTES when receiving or TXBYTES when transmitting, and WORTIME1/WORTIME0 at any time.*/
// uint8_t CC1101::readRegisterWithSyncProblem(uint8_t address, uint8_t registerType)
uint8_t /* ICACHE_RAM_ATTR */ CC1101::readRegisterWithSyncProblem(uint8_t address, uint8_t registerType)
{
  uint8_t value1, value2;
  value1 = readRegister(address | registerType);
  value2 = readRegister(address | registerType);

  if (value1 == value2)
    return value1;

  // if two consecutive reads gives us the same result then we know we are ok
  do
  {
    value2 = value1;
    value1 = readRegister(address | registerType);
  } while (value1 != value2);

  return value1;
}

// registerType = CC1101_CONFIG_REGISTER or CC1101_STATUS_REGISTER
uint8_t CC1101::readRegister(uint8_t address, uint8_t registerType)
{
  switch (address)
  {
  case CC1101_FREQEST:
  case CC1101_MARCSTATE:
  case CC1101_RXBYTES:
  case CC1101_TXBYTES:
  case CC1101_WORTIME1:
  case CC1101_WORTIME0:
    return readRegisterWithSyncProblem(address, registerType);

  default:
    return readRegister(address | registerType);
  }
}

void CC1101::writeBurstRegister(const uint8_t address, const uint8_t *data, const uint8_t length)
{
  uint8_t i;

  select();

  SPI.beginTransaction(SPISettings());
  SPI.transfer(address | CC1101_WRITE_BURST);
  for (i = 0; i < length; i++)
  {
    SPI.transfer(data[i]);
  }
  SPI.endTransaction();

  deselect();
}

void CC1101::readBurstRegister(uint8_t *buffer, const uint8_t address, const uint8_t length)
{
  uint8_t i;

  select();

  SPI.beginTransaction(SPISettings());
  SPI.transfer(address | CC1101_READ_BURST);

  for (i = 0; i < length; i++)
  {
    buffer[i] = SPI.transfer(0x00);
  }
  SPI.endTransaction();

  deselect();
}

uint8_t CC1101::getChipVersion()
{
  return readRegister(CC1101_VERSION, CC1101_STATUS_REGISTER);
}

size_t CC1101::readData(CC1101Packet *packet, size_t maxLen)
{
  packet->length = 0;
  size_t length = maxLen;

  // Measure frequency offset and signal strength while the transmitter is still active.
  lastFreqEst = (int8_t)readRegister(CC1101_FREQEST, CC1101_STATUS_REGISTER);
  {
    uint8_t r = readRegister(CC1101_RSSI, CC1101_STATUS_REGISTER);
    lastRssiDbm = (r >= 128) ? ((int)r - 256) / 2 - 74 : (int)r / 2 - 74;
  }
  uint8_t bytesInFIFO = 0;

  bytesInFIFO = readRegisterWithSyncProblem(CC1101_RXBYTES, CC1101_READ_BURST) & CC1101_BITS_RX_BYTES_IN_FIFO;

  size_t readBytes = 0;
  uint32_t lastFIFORead = millis();

  // keep reading from FIFO until we get all the packet.
  while (readBytes < length)
  {
    if (bytesInFIFO == 0)
    {
      if (millis() - lastFIFORead > 5)
      {
        break;
      }
      else
      {
        bytesInFIFO = readRegisterWithSyncProblem(CC1101_RXBYTES, CC1101_READ_BURST) & CC1101_BITS_RX_BYTES_IN_FIFO;
        continue;
      }
    }

    uint8_t bytesToRead = min((uint8_t)(length - readBytes), bytesInFIFO); // only read from bytesInFIFO if there is buffer length available
    if (bytesToRead > 1)
    { // CC1101 errata: RX FIFO pointer is sometimes not properly updated and the last read byte is duplicated, leave on byte in buffer until only one byte is left
      bytesToRead -= 1;
    }
    readBurstRegister(&(packet->data[readBytes]), CC1101_RXFIFO, bytesToRead);
    readBytes += bytesToRead;
    packet->length = readBytes;
    lastFIFORead = millis();

    // Get how many bytes are left in FIFO.
    bytesInFIFO = readRegisterWithSyncProblem(CC1101_RXBYTES, CC1101_READ_BURST) & CC1101_BITS_RX_BYTES_IN_FIFO;
  }

  writeCommand(CC1101_SIDLE); // idle
  writeCommand(CC1101_SFRX);  // flush RX buffer
  writeCommand(CC1101_SRX);   // switch to RX state

  return readBytes;
}

// This function is able to send packets bigger then the FIFO size.
// ESPHome port: all wait loops have a time-out, and the result of each transmission is recorded for diagnostics.
void CC1101::sendData(CC1101Packet *packet)
{
  uint8_t index = 0;
  uint8_t txStatus, MarcState = 0xFF;
  uint8_t length;
  bool aborted = false;

  txStreamed = false;
  txAborted = false;
  txEndState = 0xFF;

  writeCommand(CC1101_SIDLE); // idle

  // Always flush the TX FIFO first (SFTX is allowed in IDLE).
  writeCommand(CC1101_SFTX);
  writeCommand(CC1101_SIDLE);

  // determine how many bytes to send
  length = (packet->length <= CC1101_DATA_LEN ? packet->length : CC1101_DATA_LEN);

  writeBurstRegister(CC1101_TXFIFO, packet->data, length);

  writeCommand(CC1101_SIDLE);
  // start sending packet
  writeCommand(CC1101_STX);

  // continue sending when packet is bigger than FIFO
  if (packet->length > CC1101_DATA_LEN)
  {
    txStreamed = true;
    index += length;
    // loop until all bytes are transmitted
    while (index < packet->length && !aborted)
    {
      // check if there is free space in the fifo (time-out 50 ms)
      uint32_t t0 = millis();
      while ((txStatus = (readRegisterMedian3(CC1101_TXBYTES | CC1101_STATUS_REGISTER) & CC1101_BITS_RX_BYTES_IN_FIFO)) > (CC1101_DATA_LEN - 2))
      {
        if (millis() - t0 > 50)
        {
          txWaitTimeouts++;
          aborted = true;
          break;
        }
      }
      if (aborted)
        break;

      // calculate how many bytes we can send
      length = (CC1101_DATA_LEN - txStatus);
      length = ((packet->length - index) < length ? (packet->length - index) : length);

      // send some more bytes
      for (int i = 0; i < length; i++)
        writeRegister(CC1101_TXFIFO, packet->data[index + i]);

      index += length;
    }
  }

  // wait until transmission is finished (TXOFF_MODE is expected to be set to 0/IDLE or TXFIFO_UNDERFLOW), time-out 300 ms
  uint32_t tEnd = millis();
  do
  {
    MarcState = (readRegisterWithSyncProblem(CC1101_MARCSTATE, CC1101_STATUS_REGISTER) & CC1101_BITS_MARCSTATE);
    if (millis() - tEnd > 300)
    {
      txWaitTimeouts++;
      aborted = true;
      break;
    }
  } while ((MarcState != CC1101_MARCSTATE_IDLE) && (MarcState != CC1101_MARCSTATE_TXFIFO_UNDERFLOW));

  if (MarcState == CC1101_MARCSTATE_TXFIFO_UNDERFLOW)
    txUnderflows++;
  if (aborted)
  {
    writeCommand(CC1101_SIDLE);
    writeCommand(CC1101_SFTX);
  }
  txEndState = MarcState;
  txAborted = aborted;
}
