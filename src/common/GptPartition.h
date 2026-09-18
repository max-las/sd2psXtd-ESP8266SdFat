/*
 * Copyright (c) 2026 sd2psX project
 *
 * MIT License
 */
#ifndef GptPartition_h
#define GptPartition_h
#include <stdint.h>
#include "GptStructs.h"
//------------------------------------------------------------------------------
/** Initialize CRC32 state. */
inline void gptCrc32Begin(uint32_t* crc) {
  *crc = 0xFFFFFFFF;
}

/** Update CRC32 state with data block. */
void gptCrc32Update(uint32_t* crc, const uint8_t* data, uint32_t len);

/** Finalize CRC32 state. */
inline uint32_t gptCrc32End(uint32_t crc) {
  return crc ^ 0xFFFFFFFF;
}

/** One-shot CRC32 over data block. */
uint32_t gptCrc32(const uint8_t* data, uint32_t len);

/** Validate GPT header in a 512-byte sector buffer.
 *
 *  \param[in] sector Sector buffer.
 *  \param[in] sectorSize Size of sector buffer. Must be 512.
 *  \param[in] expectedCurrentLba LBA this header was read from (1 for primary,
 *            backupLba for backup).
 *  \param[in] cardSectorCount Number of sectors on the card.
 *  \param[out] hdrOut Optional pointer to header struct inside sector.
 *
 *  \return true if header is valid, false otherwise.
 */
bool gptIsValidHeader(const uint8_t* sector,
                      uint32_t sectorSize,
                      uint64_t expectedCurrentLba,
                      uint32_t cardSectorCount,
                      const GptHeader_t** hdrOut = nullptr);

#endif  // GptPartition_h
