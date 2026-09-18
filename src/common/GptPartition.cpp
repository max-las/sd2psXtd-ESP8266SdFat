/*
 * Copyright (c) 2026 sd2psX project
 *
 * MIT License
 */
#include "GptPartition.h"
#include "FsStructs.h"
#include "DebugMacros.h"
//------------------------------------------------------------------------------
void gptCrc32Update(uint32_t* crc, const uint8_t* data, uint32_t len) {
  uint32_t c = *crc;
  for (uint32_t i = 0; i < len; i++) {
    c ^= data[i];
    for (int j = 0; j < 8; j++) {
      c = (c >> 1) ^ (0xEDB88320 & (0 - (c & 1)));
    }
  }
  *crc = c;
}
//------------------------------------------------------------------------------
uint32_t gptCrc32(const uint8_t* data, uint32_t len) {
  uint32_t crc;
  gptCrc32Begin(&crc);
  gptCrc32Update(&crc, data, len);
  return gptCrc32End(crc);
}
//------------------------------------------------------------------------------
bool gptIsValidHeader(const uint8_t* sector,
                      uint32_t sectorSize,
                      uint64_t expectedCurrentLba,
                      uint32_t cardSectorCount,
                      const GptHeader_t** hdrOut) {
  if (!sector || sectorSize != 512) {
    DBG_LOG("GPT requires a 512-byte sector");
    return false;
  }
  const GptHeader_t* hdr = reinterpret_cast<const GptHeader_t*>(sector);
  if (memcmp(hdr->signature, GPT_SIGNATURE, 8) != 0) {
    DBG_LOG("GPT signature mismatch");
    return false;
  }
  uint32_t revision = getLe32(hdr->revision);
  if (revision != GPT_REVISION_1_0) {
    DBG_LOG("GPT revision mismatch");
    return false;
  }
  uint32_t headerSize = getLe32(hdr->headerSize);
  if (headerSize < GPT_HEADER_SIZE_MIN || headerSize > sectorSize) {
    DBG_LOG("GPT header size invalid");
    return false;
  }
  // CRC is computed with headerCrc32 field zeroed. Feed the three ranges
  // separately so validation does not need another sector-sized buffer.
  static const uint8_t zeroCrc[4] = {};
  uint32_t computedCrc;
  gptCrc32Begin(&computedCrc);
  gptCrc32Update(&computedCrc, sector, 16);
  gptCrc32Update(&computedCrc, zeroCrc, sizeof(zeroCrc));
  gptCrc32Update(&computedCrc, sector + 20, headerSize - 20);
  computedCrc = gptCrc32End(computedCrc);
  if (computedCrc != getLe32(hdr->headerCrc32)) {
    DBG_LOG("GPT header CRC mismatch");
    return false;
  }
  uint64_t currentLba = getLe64(hdr->currentLba);
  uint64_t backupLba = getLe64(hdr->backupLba);
  uint64_t firstUsable = getLe64(hdr->firstUsableLba);
  uint64_t lastUsable = getLe64(hdr->lastUsableLba);
  uint64_t entryLba = getLe64(hdr->partitionEntryLba);
  uint32_t numEntries = getLe32(hdr->numberOfPartitionEntries);
  uint32_t entrySize = getLe32(hdr->sizeOfPartitionEntry);

  if (getLe32(hdr->reserved) != 0) {
    DBG_LOG("GPT reserved field is nonzero");
    return false;
  }
  if (currentLba != expectedCurrentLba) {
    DBG_LOG("GPT currentLBA mismatch");
    return false;
  }
  if (currentLba >= cardSectorCount || backupLba >= cardSectorCount ||
      currentLba == backupLba) {
    DBG_LOG("GPT header LBAs out of range");
    return false;
  }
  if ((expectedCurrentLba == 1 && backupLba != cardSectorCount - 1) ||
      (expectedCurrentLba == cardSectorCount - 1 && backupLba != 1)) {
    DBG_LOG("GPT alternate header LBA invalid");
    return false;
  }
  if (firstUsable > lastUsable || firstUsable >= cardSectorCount) {
    DBG_LOG("GPT usable range invalid");
    return false;
  }
  if (lastUsable >= cardSectorCount) {
    DBG_LOG("GPT lastUsableLBA out of range");
    return false;
  }
  if (entryLba >= cardSectorCount) {
    DBG_LOG("GPT partitionEntryLBA out of range");
    return false;
  }
  if (numEntries == 0) {
    DBG_LOG("GPT numberOfPartitionEntries invalid");
    return false;
  }
  if (entrySize < 128 || (entrySize % 128) != 0) {
    DBG_LOG("GPT sizeOfPartitionEntry invalid");
    return false;
  }
  uint64_t arraySize = (uint64_t)numEntries * entrySize;
  uint64_t arraySectors = (arraySize + 511) / 512;
  if (arraySectors == 0 || entryLba >= cardSectorCount ||
      arraySectors > (uint64_t)cardSectorCount - entryLba) {
    DBG_LOG("GPT partition entry array out of range");
    return false;
  }
  uint64_t arrayEnd = entryLba + arraySectors;
  uint64_t usableEnd = lastUsable + 1;
  if (arraySectors > (uint64_t)cardSectorCount - 2 ||
      firstUsable < 2 + arraySectors ||
      lastUsable >= (uint64_t)cardSectorCount - 1 - arraySectors ||
      (firstUsable <= currentLba && currentLba < usableEnd) ||
      (firstUsable <= backupLba && backupLba < usableEnd) ||
      (entryLba < usableEnd && arrayEnd > firstUsable) ||
      (entryLba <= currentLba && currentLba < arrayEnd) ||
      (entryLba <= backupLba && backupLba < arrayEnd)) {
    DBG_LOG("GPT metadata overlaps usable space");
    return false;
  }
  if (hdrOut) {
    *hdrOut = hdr;
  }
  return true;
}
//------------------------------------------------------------------------------
