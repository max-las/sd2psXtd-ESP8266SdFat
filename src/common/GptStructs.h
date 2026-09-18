/*
 * Copyright (c) 2026 sd2psX project
 *
 * MIT License
 */
#ifndef GptStructs_h
#define GptStructs_h
#include <stdint.h>
#include <string.h>
//------------------------------------------------------------------------------
/** GPT header signature "EFI PART" */
static const uint8_t GPT_SIGNATURE[8] = {
  0x45, 0x46, 0x49, 0x20, 0x50, 0x41, 0x52, 0x54
};

/** Microsoft Basic Data GUID (EBD0A0A2-B9E5-4433-87C0-68B6B72699C7) */
static const uint8_t GPT_MS_BASIC_DATA_GUID[16] = {
  0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
  0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};

/** GPT header size must be at least 92 bytes */
static const uint32_t GPT_HEADER_SIZE_MIN = 92;

/** GPT revision 1.0 */
static const uint32_t GPT_REVISION_1_0 = 0x00010000;
//------------------------------------------------------------------------------
/** GPT GUID helper */
struct GptGuid_t {
  uint8_t data[16];

  bool isZero() const {
    for (int i = 0; i < 16; i++) {
      if (data[i]) {
        return false;
      }
    }
    return true;
  }

  bool equals(const uint8_t* other) const {
    return memcmp(data, other, 16) == 0;
  }

  bool equals(const GptGuid_t& other) const {
    return memcmp(data, other.data, 16) == 0;
  }
};
//------------------------------------------------------------------------------
/** GPT Header (92 bytes minimum) */
typedef struct gptHeader {
  uint8_t  signature[8];
  uint8_t  revision[4];
  uint8_t  headerSize[4];
  uint8_t  headerCrc32[4];
  uint8_t  reserved[4];
  uint8_t  currentLba[8];
  uint8_t  backupLba[8];
  uint8_t  firstUsableLba[8];
  uint8_t  lastUsableLba[8];
  uint8_t  diskGuid[16];
  uint8_t  partitionEntryLba[8];
  uint8_t  numberOfPartitionEntries[4];
  uint8_t  sizeOfPartitionEntry[4];
  uint8_t  partitionEntryArrayCrc32[4];
} GptHeader_t;
//------------------------------------------------------------------------------
/** GPT Partition Entry (128 bytes minimum) */
typedef struct gptPartitionEntry {
  uint8_t  typeGuid[16];
  uint8_t  uniqueGuid[16];
  uint8_t  firstLba[8];
  uint8_t  lastLba[8];
  uint8_t  attributes[8];
  uint8_t  name[72];
} GptPartitionEntry_t;
//------------------------------------------------------------------------------
#endif  // GptStructs_h
