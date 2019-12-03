#include "flash.h"

#include "Adafruit_SPIFlash.h"


#define DEBUG 1
#if DEBUG
  #define LOG(x)    Serial.print(x)
  #define LOGLN(x)  Serial.println(x)
#else
  #define LOG(x)
  #define LOGLN(x)
#endif

namespace {

  #if defined(__SAMD51__) || defined(NRF52840_XXAA)
    Adafruit_FlashTransport_QSPI flashTransport(PIN_QSPI_SCK, PIN_QSPI_CS, PIN_QSPI_IO0, PIN_QSPI_IO1, PIN_QSPI_IO2, PIN_QSPI_IO3);
  #else
    #if (SPI_INTERFACES_COUNT == 1 || defined(ADAFRUIT_CIRCUITPLAYGROUND_M0))
      Adafruit_FlashTransport_SPI flashTransport(SS, &SPI);
    #else
      Adafruit_FlashTransport_SPI flashTransport(SS1, &SPI1);
    #endif
  #endif

  // Wrapper for Adafruit_SPIFlash::writeBuffer() that
  // correctly handles non-page aligned addresses
  uint32_t writeBuffer (
    Adafruit_SPIFlash& flash,
    uint32_t address, uint8_t const *buffer, uint32_t len)
  {
    uint32_t remain = len;

    // write one page at a time
    while(remain)
    {
      uint32_t const leftOnPage
        = SFLASH_PAGE_SIZE - (address & (SFLASH_PAGE_SIZE - 1));

      uint32_t const toWrite = min(remain, leftOnPage);

      if (flash.writeBuffer(address, buffer, toWrite) != toWrite) break;

      remain -= toWrite;
      buffer += toWrite;
      address += toWrite;
    }

    return len - remain;
  }

  Adafruit_SPIFlash flash(&flashTransport);
  bool flashBegun = false;

  uint32_t computeMagicValue(uint32_t a, uint32_t b, uint32_t c) {
    return (
      1176328692                // tracking number of my first Adafruit order
      ^ (a + 123) * 0x08040201  // spread out every 9 bits
      ^ (b + 456) * 0x20100401  // spread out every 10 bits
      ^ (c + 789) * 0x10204081  // spread out every 7 bits
      ) & ~0x1;                 // must have at least one zero
  }

/* Layout

  Each sector starts with a 4-byte magic number, which is used
  as a guard against reading random data as valid. And a serial number
  of the sector, so that after wrapping one can find which is the newest
  sector (since all will be valid).

  Then comes a bit map, where 0 means valid.

  Then comes n copies of the entry.
*/

  struct Header {
    uint32_t magic;
    uint32_t serial;
  };

  const uint32_t notFoundSerial = 0;    // must be smallest value
  const uint32_t firstSerial = 1;

  struct Layout {
    const uint32_t dataLength;
    const uint32_t entriesPerSector;
    const uint32_t bitmapLength;

    Layout(uint32_t dataLength);

    static const uint32_t maxDataLength
      = SFLASH_SECTOR_SIZE - sizeof(Header) - 1;

    static inline uint32_t headerAddress(uint32_t sector)
      { return sector * SFLASH_SECTOR_SIZE; }

    static inline uint32_t bitmapAddress(uint32_t sector)
      { return headerAddress(sector) + sizeof(Header); }

    inline uint32_t dataAddress(uint32_t sector, uint32_t index) const {
      { return bitmapAddress(sector) + bitmapLength + index * dataLength; }
    }
  };

  Layout::Layout(uint32_t dataLength)
    : dataLength(dataLength),
      entriesPerSector(
        (8 * (SFLASH_SECTOR_SIZE - sizeof(Header))
        / (8 * dataLength + 1))),
      bitmapLength((entriesPerSector + 7) / 8)
    { }

}


bool FlashMemoryLog::begin(uint32_t startSector, uint32_t sectorCount)
{
  regionStartSector = startSector;
  regionEndSector = startSector + sectorCount;
  regionMagicValue = computeMagicValue(dataLength, startSector, sectorCount);
  currentSerial = notFoundSerial;
  currentSector = 0;
  currentIndex = 0;

  if (!flashBegun) {
    if (!flash.begin()) {
      LOGLN("flash begin failure");
      return false;
    }
    LOG("flash length in sectors: "); LOGLN(flash.size() / SFLASH_SECTOR_SIZE);
    flashBegun = true;
  }


  if (dataLength > Layout::maxDataLength) {
    LOGLN("data length too big");
    return false;
  }

  if ((regionEndSector - regionStartSector) < 2) {
    LOGLN("must have at least 2 sectors in region");
    return false;
  }

  uint32_t numberOfSectors = flash.size() / SFLASH_SECTOR_SIZE;

  if (regionStartSector >= numberOfSectors) {
    LOGLN("start sector beyond flash capacity");
    return false;
  }

  if (regionEndSector > numberOfSectors) {
    LOGLN("region extends past end of flash");
    return false;
  }

  auto layout = Layout(dataLength);

  currentSerial = notFoundSerial;
  currentSector = 0;

  for (uint32_t sector = regionStartSector;
       sector < regionEndSector;
       ++sector)
  {
    Header header;
    if (flash.readBuffer(layout.headerAddress(sector),
          reinterpret_cast<uint8_t*>(&header),
          sizeof(header))
        != sizeof(header))
    {
      LOG("flash read failure at sector ");
      LOGLN(sector);
      return false;
    }

    if (header.magic != regionMagicValue)
      // can't be our data from here on after
      break;
    if (header.serial < currentSerial)
      // older sector, as will be all that follow
      break;

    currentSerial = header.serial;
    currentSector = sector;
  }

  if (currentSerial != notFoundSerial) {
    currentIndex = 0;

    uint8_t bitMap[layout.bitmapLength];
    if (flash.readBuffer(layout.bitmapAddress(currentSector),
          bitMap, layout.bitmapLength)
        != layout.bitmapLength)
    {
      LOG("flash read failure of bitmap ");
      LOGLN(currentSector);
      return false;
    }

    for (uint32_t mapIndex = 0; mapIndex < layout.bitmapLength; mapIndex++)
    {
      switch (bitMap[mapIndex]) {
        case 0x00:  currentIndex = mapIndex * 8 + 7; break;
        case 0x80:  currentIndex = mapIndex * 8 + 6; goto found;
        case 0xc0:  currentIndex = mapIndex * 8 + 5; goto found;
        case 0xe0:  currentIndex = mapIndex * 8 + 4; goto found;
        case 0xf0:  currentIndex = mapIndex * 8 + 3; goto found;
        case 0xf8:  currentIndex = mapIndex * 8 + 2; goto found;
        case 0xfc:  currentIndex = mapIndex * 8 + 1; goto found;
        case 0xfe:  currentIndex = mapIndex * 8    ; goto found;
        case 0xff:  goto found;
        default:
          // something is very amiss
          // reset region
          LOG("bitmap was ill formed in sector  ");
          LOGLN(currentSector);
          currentSector = notFoundSerial;
          goto found;
      }
    }
found:    // sometimes, a goto is just the thing!
    ;
  }

  LOGLN("FlashMemoryLog begin:");
  LOG("   data length        = "); LOGLN(layout.dataLength);
  LOG("   entries per sector = "); LOGLN(layout.entriesPerSector);
  LOG("   bitmap length      = "); LOGLN(layout.bitmapLength);
  LOG("   current serial     = "); LOGLN(currentSerial);
  LOG("   current sector     = "); LOGLN(currentSector);
  LOG("   current index      = "); LOGLN(currentIndex);
  return true;
}


bool FlashMemoryLog::readCurrent(uint8_t* buf)
{
  if (currentSerial == notFoundSerial)
    return false;

  auto layout = Layout(dataLength);

  return
    flash.readBuffer(layout.dataAddress(currentSector, currentIndex),
      buf, dataLength)
    == dataLength;
}

bool FlashMemoryLog::writeNext(const uint8_t* buf)
{
  auto layout = Layout(dataLength);

  uint32_t nextSerial = currentSerial;
  uint32_t nextSector;
  uint32_t nextIndex;

  if (nextSerial == notFoundSerial) {
    nextSerial = firstSerial;
    nextSector = regionStartSector;
    nextIndex = 0;
  } else {
    nextSector = currentSector;
    nextIndex = currentIndex + 1;

    if (nextIndex >= layout.entriesPerSector) {
      nextSerial = nextSerial + 1;
      nextSector = nextSector + 1;
      nextIndex = 0;

      if (nextSector >= regionEndSector) {
        nextSector = regionStartSector;   // wrap
      }
    }
  }

  if (nextIndex == 0) {
    if (!flash.eraseSector(nextSector)) {
      LOG("sector failed to erase: ");
      LOGLN(nextSector);
      return false;
    }

    Header header;
    header.magic = regionMagicValue;
    header.serial = nextSerial;

    if (writeBuffer(flash, layout.headerAddress(nextSector),
          reinterpret_cast<uint8_t*>(&header), sizeof(Header))
      != sizeof(Header))
    {
      LOG("header write of failed to address: ");
      LOGLN(layout.headerAddress(nextSector));
      return false;
    }
  }

  if (writeBuffer(flash, layout.dataAddress(nextSector, nextIndex),
        buf, dataLength)
      != dataLength)
  {
    LOG("data write of failed to address: ");
    LOGLN(layout.dataAddress(nextSector, nextIndex));
    return false;
  }

  uint8_t bitmapByte = 0xff << ((nextIndex & 0x07) + 1);

  if (writeBuffer(flash, layout.bitmapAddress(nextSector) + nextIndex / 8,
      &bitmapByte, 1)
      != 1)
  {
    LOG("bitmap write failed to address: ");
    LOGLN(layout.bitmapAddress(nextSector) + nextIndex / 8);
    return false;
  }

  currentSerial = nextSerial;
  currentSector = nextSector;
  currentIndex = nextIndex;

  LOGLN("FlashMemoryLog wrote:");
  LOG("   current serial     = "); LOGLN(currentSerial);
  LOG("   current sector     = "); LOGLN(currentSector);
  LOG("   current index      = "); LOGLN(currentIndex);
  return true;
}

