/*
 * Copyright (c) 2026 sd2psX project
 *
 * MIT License
 */
#define DBG_FILE "VolumeLocator.cpp"
#include "VolumeLocator.h"
#include "DebugMacros.h"
#include "FsStructs.h"
#include "GptPartition.h"
#include "SdFatConfig.h"
#include <string.h>
//------------------------------------------------------------------------------
enum class ProbeResult {
  Invalid,
  Fat,
  ExFat,
  CardError
};
static const uint64_t SEARCH_SAW_CARD_ERROR = UINT64_C(1) << 63;
static const uint64_t SEARCH_INDEX_MASK = ~SEARCH_SAW_CARD_ERROR;
//------------------------------------------------------------------------------
static bool setError(VolumeFindError* error, VolumeFindError value) {
  if (error) {
    *error = value;
  }
  return false;
}
//------------------------------------------------------------------------------
static bool hasPbrSignature(const uint8_t* sector) {
  return getLe16(sector + 510) == PBR_SIGNATURE;
}
//------------------------------------------------------------------------------
static bool isPowerOfTwo(uint8_t value) {
  return value && (value & (value - 1)) == 0;
}
//------------------------------------------------------------------------------
static uint32_t exFatBootChecksumUpdate(uint32_t checksum,
                                        const uint8_t* sector,
                                        bool isBootSector) {
  for (uint16_t i = 0; i < 512; i++) {
    if (isBootSector && (i == 106 || i == 107 || i == 112)) {
      continue;
    }
    checksum = ((checksum << 31) | (checksum >> 1)) + sector[i];
  }
  return checksum;
}
//------------------------------------------------------------------------------
static bool isValidFatBootSector(const uint8_t* sector,
                                 uint32_t volumeStart,
                                 uint32_t volumeSectorCount) {
  if (!hasPbrSignature(sector)) {
    return false;
  }
  const PbsFat_t* pbs = reinterpret_cast<const PbsFat_t*>(sector);
  const BpbFat32_t* bpb = &pbs->bpb.bpb32;
  uint16_t bytesPerSector = getLe16(bpb->bytesPerSector);
  uint8_t sectorsPerCluster = bpb->sectorsPerCluster;
  uint16_t reservedSectorCount = getLe16(bpb->reservedSectorCount);
  if (bytesPerSector != 512 || !isPowerOfTwo(sectorsPerCluster) ||
      sectorsPerCluster > 128 || bpb->fatCount != 2 ||
      reservedSectorCount == 0 ||
      getLe32(bpb->hidddenSectors) != volumeStart) {
    return false;
  }

  uint32_t totalSectors = getLe16(bpb->totalSectors16);
  if (totalSectors == 0) {
    totalSectors = getLe32(bpb->totalSectors32);
  }
  uint32_t sectorsPerFat = getLe16(bpb->sectorsPerFat16);
  if (sectorsPerFat == 0) {
    sectorsPerFat = getLe32(bpb->sectorsPerFat32);
  }
  if (totalSectors == 0 || totalSectors > volumeSectorCount ||
      sectorsPerFat == 0) {
    return false;
  }

  uint16_t rootDirEntryCount = getLe16(bpb->rootDirEntryCount);
  uint64_t rootDirSectors =
      ((uint64_t)FS_DIR_SIZE * rootDirEntryCount + bytesPerSector - 1) /
      bytesPerSector;
  uint64_t dataStart = (uint64_t)reservedSectorCount +
                       (uint64_t)bpb->fatCount * sectorsPerFat +
                       rootDirSectors;
  if (dataStart >= totalSectors) {
    return false;
  }

  uint32_t clusterCount =
      (totalSectors - (uint32_t)dataStart) / sectorsPerCluster;
  if (clusterCount < 2 || clusterCount > 0X0FFFFFF6) {
    return false;
  }
  uint64_t entries = (uint64_t)clusterCount + 2;
  uint64_t requiredFatBytes;
  if (clusterCount < 4085) {
    if (!FAT12_SUPPORT) {
      return false;
    }
    requiredFatBytes = (entries * 3 + 1) / 2;
  } else if (clusterCount < 65525) {
    requiredFatBytes = entries * 2;
  } else {
    requiredFatBytes = entries * 4;
  }
  if ((uint64_t)sectorsPerFat * bytesPerSector < requiredFatBytes ||
      (clusterCount >= 65525 && rootDirEntryCount != 0) ||
      (clusterCount < 65525 && rootDirEntryCount == 0)) {
    return false;
  }
  if (clusterCount >= 65525) {
    uint32_t rootCluster = getLe32(bpb->fat32RootCluster);
    if (rootCluster < 2 || rootCluster > clusterCount + 1) {
      return false;
    }
  }
  return true;
}
//------------------------------------------------------------------------------
static ProbeResult probeExFatBootRegion(BlockDevice* dev,
                                        const uint8_t* sector,
                                        uint32_t bootRegionStart,
                                        uint32_t volumeStart,
                                        uint32_t volumeSectorCount) {
  if (!hasPbrSignature(sector)) {
    return ProbeResult::Invalid;
  }
  const ExFatPbs_t* pbs = reinterpret_cast<const ExFatPbs_t*>(sector);
  static const uint8_t jump[3] = {0XEB, 0X76, 0X90};
  if (memcmp(pbs->jmpInstruction, jump, sizeof(jump)) != 0 ||
      memcmp(pbs->oemName, "EXFAT   ", 8) != 0) {
    return ProbeResult::Invalid;
  }
  const BpbExFat_t* bpb = &pbs->bpb;
  for (uint8_t i = 0; i < sizeof(bpb->mustBeZero); i++) {
    if (bpb->mustBeZero[i]) {
      return ProbeResult::Invalid;
    }
  }
  for (uint8_t i = 0; i < sizeof(bpb->reserved); i++) {
    if (bpb->reserved[i]) {
      return ProbeResult::Invalid;
    }
  }
  if (bpb->bytesPerSectorShift != 9 ||
      bpb->sectorsPerClusterShift > 16 ||
      bpb->numberOfFats != 1 ||
      getLe16(bpb->fileSystemRevision) != 0X0100 ||
      (getLe16(bpb->volumeFlags) & 1) != 0 ||
      getLe64(bpb->partitionOffset) != volumeStart) {
    return ProbeResult::Invalid;
  }

  uint64_t volumeLength = getLe64(bpb->volumeLength);
  uint32_t fatOffset = getLe32(bpb->fatOffset);
  uint32_t fatLength = getLe32(bpb->fatLength);
  uint32_t clusterHeapOffset = getLe32(bpb->clusterHeapOffset);
  uint32_t clusterCount = getLe32(bpb->clusterCount);
  if (volumeLength < 24 || volumeLength > volumeSectorCount ||
      fatOffset < 24 || fatLength == 0 || clusterCount == 0) {
    return ProbeResult::Invalid;
  }
  uint64_t requiredFatSectors =
      (((uint64_t)clusterCount + 2) * 4 + 511) / 512;
  uint64_t fatRegionEnd = (uint64_t)fatOffset + fatLength;
  uint64_t clusterRegionSectors =
      (uint64_t)clusterCount << bpb->sectorsPerClusterShift;
  if (fatLength < requiredFatSectors || fatRegionEnd > clusterHeapOffset ||
      clusterHeapOffset > volumeLength ||
      clusterRegionSectors > volumeLength - clusterHeapOffset) {
    return ProbeResult::Invalid;
  }
  uint32_t rootCluster = getLe32(bpb->rootDirectoryCluster);
  if (rootCluster < 2 || rootCluster > clusterCount + 1 ||
      volumeSectorCount < 24) {
    return ProbeResult::Invalid;
  }

  uint32_t checksum = exFatBootChecksumUpdate(0, sector, true);
  uint8_t bootSector[512];
  for (uint32_t i = 1; i < 11; i++) {
    if (!dev->readSector(bootRegionStart + i, bootSector)) {
      return ProbeResult::CardError;
    }
    if (i <= 8 && !hasPbrSignature(bootSector)) {
      return ProbeResult::Invalid;
    }
    if (i == 10) {
      for (uint16_t offset = 0; offset < sizeof(bootSector); offset++) {
        if (bootSector[offset]) {
          return ProbeResult::Invalid;
        }
      }
    }
    checksum = exFatBootChecksumUpdate(checksum, bootSector, false);
  }
  if (!dev->readSector(bootRegionStart + 11, bootSector)) {
    return ProbeResult::CardError;
  }
  for (uint16_t i = 0; i < 512; i += 4) {
    if (getLe32(bootSector + i) != checksum) {
      return ProbeResult::Invalid;
    }
  }
  return ProbeResult::ExFat;
}
//------------------------------------------------------------------------------
static ProbeResult probeExFatBootSector(BlockDevice* dev,
                                        const uint8_t* sector,
                                        uint32_t volumeStart,
                                        uint32_t volumeSectorCount) {
  const ExFatPbs_t* pbs = reinterpret_cast<const ExFatPbs_t*>(sector);
  static const uint8_t jump[3] = {0XEB, 0X76, 0X90};
  if (memcmp(pbs->oemName, "EXFAT   ", 8) != 0 &&
      memcmp(pbs->jmpInstruction, jump, sizeof(jump)) != 0) {
    return ProbeResult::Invalid;
  }
  if (volumeSectorCount < 24) {
    return ProbeResult::Invalid;
  }
  ProbeResult mainResult = probeExFatBootRegion(dev, sector, volumeStart,
                                                volumeStart,
                                                volumeSectorCount);
  if (mainResult == ProbeResult::ExFat || volumeSectorCount < 24) {
    return mainResult;
  }
  uint8_t backupSector[512];
  if (!dev->readSector(volumeStart + 12, backupSector)) {
    return ProbeResult::CardError;
  }
  ProbeResult backupResult = probeExFatBootRegion(
      dev, backupSector, volumeStart + 12, volumeStart, volumeSectorCount);
  if (backupResult == ProbeResult::ExFat) {
    return backupResult;
  }
  return mainResult == ProbeResult::CardError ||
         backupResult == ProbeResult::CardError
             ? ProbeResult::CardError : ProbeResult::Invalid;
}
//------------------------------------------------------------------------------
static ProbeResult probeBootSector(BlockDevice* dev,
                                   const uint8_t* sector,
                                   uint32_t volumeStart,
                                   uint32_t volumeSectorCount) {
  ProbeResult result = probeExFatBootSector(dev, sector, volumeStart,
                                            volumeSectorCount);
  if (result != ProbeResult::Invalid) {
    return result;
  }
  return isValidFatBootSector(sector, volumeStart, volumeSectorCount)
             ? ProbeResult::Fat : ProbeResult::Invalid;
}
//------------------------------------------------------------------------------
static bool isValidGptEntryArray(BlockDevice* dev,
                                 const GptHeader_t* hdr,
                                 VolumeFindError* error) {
  uint32_t numEntries = getLe32(hdr->numberOfPartitionEntries);
  uint32_t entrySize = getLe32(hdr->sizeOfPartitionEntry);
  uint64_t arrayLba = getLe64(hdr->partitionEntryLba);
  uint64_t remaining = (uint64_t)numEntries * entrySize;
  uint32_t crc;
  gptCrc32Begin(&crc);
  while (remaining) {
    uint8_t sector[512];
    if (!dev->readSector((uint32_t)arrayLba++, sector)) {
      return setError(error, VolumeFindError::CardError);
    }
    uint32_t count = remaining < sizeof(sector)
                         ? (uint32_t)remaining : sizeof(sector);
    gptCrc32Update(&crc, sector, count);
    remaining -= count;
  }
  if (gptCrc32End(crc) != getLe32(hdr->partitionEntryArrayCrc32)) {
    return setError(error, VolumeFindError::CorruptPartitionTable);
  }
  if (error) {
    *error = VolumeFindError::None;
  }
  return true;
}
//------------------------------------------------------------------------------
static bool gptHeadersMatch(const GptHeader_t* primary,
                            const GptHeader_t* backup) {
  return getLe64(primary->currentLba) == getLe64(backup->backupLba) &&
         getLe64(primary->backupLba) == getLe64(backup->currentLba) &&
         getLe64(primary->firstUsableLba) ==
             getLe64(backup->firstUsableLba) &&
         getLe64(primary->lastUsableLba) ==
             getLe64(backup->lastUsableLba) &&
         memcmp(primary->diskGuid, backup->diskGuid, 16) == 0 &&
         getLe32(primary->numberOfPartitionEntries) ==
             getLe32(backup->numberOfPartitionEntries) &&
         getLe32(primary->sizeOfPartitionEntry) ==
             getLe32(backup->sizeOfPartitionEntry) &&
         getLe32(primary->partitionEntryArrayCrc32) ==
          getLe32(backup->partitionEntryArrayCrc32);
}
//------------------------------------------------------------------------------
static ProbeResult checkGptEntryOverlap(BlockDevice* dev,
                                        const GptHeader_t* hdr,
                                        uint64_t candidateIndex,
                                        uint64_t candidateFirst,
                                        uint64_t candidateLast) {
  uint32_t numEntries = getLe32(hdr->numberOfPartitionEntries);
  uint32_t entrySize = getLe32(hdr->sizeOfPartitionEntry);
  uint64_t arrayStart = getLe64(hdr->partitionEntryLba);
  uint64_t firstUsable = getLe64(hdr->firstUsableLba);
  uint64_t lastUsable = getLe64(hdr->lastUsableLba);
  uint64_t cachedLba = UINT64_MAX;
  uint8_t sector[512];
  static const uint8_t zeroGuid[16] = {};
  for (uint64_t index = 0; index < numEntries; index++) {
    if (index == candidateIndex) {
      continue;
    }
    uint64_t byteOffset = (uint64_t)index * entrySize;
    uint64_t entryLba = arrayStart + byteOffset / 512;
    uint16_t offset = byteOffset % 512;
    if (entryLba != cachedLba) {
      if (!dev->readSector((uint32_t)entryLba, sector)) {
        return ProbeResult::CardError;
      }
      cachedLba = entryLba;
    }
    const uint8_t* entry = sector + offset;
    if (memcmp(entry, zeroGuid, sizeof(zeroGuid)) == 0) {
      continue;
    }
    uint64_t first = getLe64(entry + 32);
    uint64_t last = getLe64(entry + 40);
    if (first > last || first < firstUsable || last > lastUsable) {
      return ProbeResult::Invalid;
    }
    if (first <= candidateLast && candidateFirst <= last) {
      return ProbeResult::Invalid;
    }
  }
  return ProbeResult::Fat;
}
//------------------------------------------------------------------------------
static bool selectGptHeader(BlockDevice* dev,
                            uint32_t cardSectorCount,
                            GptHeader_t* selected,
                            VolumeFindError* error) {
  uint8_t sector[512];
  const GptHeader_t* header;
  GptHeader_t primary;
  GptHeader_t backup;
  bool primaryRead = dev->readSector(1, sector);
  bool primaryValid = primaryRead &&
      gptIsValidHeader(sector, 512, 1, cardSectorCount, &header);
  if (primaryValid) {
    memcpy(&primary, header, sizeof(primary));
  }
  uint64_t backupLba = primaryValid ? getLe64(primary.backupLba)
                                     : (uint64_t)cardSectorCount - 1;
  bool backupRead = dev->readSector((uint32_t)backupLba, sector);
  bool backupValid = backupRead &&
      gptIsValidHeader(sector, 512, backupLba, cardSectorCount, &header);
  if (backupValid) {
    memcpy(&backup, header, sizeof(backup));
  }

  if (primaryValid) {
    VolumeFindError primaryError;
    if (isValidGptEntryArray(dev, &primary, &primaryError)) {
      *selected = primary;
      return true;
    }
    VolumeFindError backupError = VolumeFindError::CorruptPartitionTable;
    if (backupValid && gptHeadersMatch(&primary, &backup) &&
        isValidGptEntryArray(dev, &backup, &backupError)) {
      *selected = backup;
      return true;
    }
    if (primaryError == VolumeFindError::CardError ||
        backupError == VolumeFindError::CardError || !backupRead) {
      return setError(error, VolumeFindError::CardError);
    }
    return setError(error, VolumeFindError::CorruptPartitionTable);
  }

  if (backupValid) {
    VolumeFindError backupError;
    if (isValidGptEntryArray(dev, &backup, &backupError)) {
      *selected = backup;
      return true;
    }
    return setError(error, backupError);
  }
  return setError(error, (!primaryRead || !backupRead)
                             ? VolumeFindError::CardError
                             : VolumeFindError::CorruptPartitionTable);
}
//------------------------------------------------------------------------------
static bool findGptVolume(BlockDevice* dev,
                          uint32_t cardSectorCount,
                          VolumeLocation* loc,
                          VolumeFindError* error,
                          uint64_t* searchIndex) {
  if (cardSectorCount < 2) {
    return setError(error, VolumeFindError::CorruptPartitionTable);
  }
  GptHeader_t header;
  if (!selectGptHeader(dev, cardSectorCount, &header, error)) {
    return false;
  }
  const GptHeader_t* hdr = &header;

  uint32_t numEntries = getLe32(hdr->numberOfPartitionEntries);
  uint32_t entrySize = getLe32(hdr->sizeOfPartitionEntry);
  uint64_t arrayStart = getLe64(hdr->partitionEntryLba);
  uint64_t firstUsable = getLe64(hdr->firstUsableLba);
  uint64_t lastUsable = getLe64(hdr->lastUsableLba);
  bool sawCardError = searchIndex && (*searchIndex & SEARCH_SAW_CARD_ERROR);
  uint64_t next = searchIndex ? *searchIndex & SEARCH_INDEX_MASK : 0;
  uint64_t totalSlots = (uint64_t)numEntries * 2;
  bool sawInvalidEntry = false;
  if (next >= totalSlots) {
    return setError(error, VolumeFindError::NoSupportedFileSystem);
  }

  // Prefer Microsoft Basic Data entries while preserving table order.
  uint8_t startPass = (uint8_t)(next / numEntries);
  for (uint8_t pass = startPass; pass < 2; pass++) {
    uint64_t cachedLba = UINT64_MAX;
    uint8_t entrySector[512];
    uint64_t firstIndex = pass == startPass ? next % numEntries : 0;
    for (uint64_t index = firstIndex; index < numEntries; index++) {
      uint64_t slot = (uint64_t)pass * numEntries + index;
      if (searchIndex) {
        *searchIndex = (slot + 1) |
                       (sawCardError ? SEARCH_SAW_CARD_ERROR : 0);
      }
      uint64_t byteOffset = (uint64_t)index * entrySize;
      uint64_t entryLba = arrayStart + byteOffset / 512;
      uint16_t offset = byteOffset % 512;
      if (entryLba != cachedLba) {
        if (!dev->readSector((uint32_t)entryLba, entrySector)) {
          sawCardError = true;
          if (searchIndex) {
            *searchIndex |= SEARCH_SAW_CARD_ERROR;
          }
          cachedLba = UINT64_MAX;
          continue;
        }
        cachedLba = entryLba;
      }
      const uint8_t* entry = entrySector + offset;
      static const uint8_t zeroGuid[16] = {};
      if (memcmp(entry, zeroGuid, sizeof(zeroGuid)) == 0) {
        continue;
      }
      bool isMs = memcmp(entry, GPT_MS_BASIC_DATA_GUID, 16) == 0;
      if (isMs != (pass == 0)) {
        continue;
      }
      uint64_t firstLba = getLe64(entry + 32);
      uint64_t lastLba = getLe64(entry + 40);
      if (firstLba > lastLba || firstLba < firstUsable ||
          lastLba > lastUsable || lastLba >= cardSectorCount ||
          firstLba > UINT32_MAX || lastLba > UINT32_MAX) {
        sawInvalidEntry = true;
        continue;
      }
      uint64_t size64 = lastLba - firstLba + 1;
      if (size64 == 0 || size64 > UINT32_MAX) {
        sawInvalidEntry = true;
        continue;
      }
      ProbeResult overlap = checkGptEntryOverlap(
          dev, hdr, index, firstLba, lastLba);
      if (overlap == ProbeResult::CardError) {
        sawCardError = true;
        if (searchIndex) {
          *searchIndex |= SEARCH_SAW_CARD_ERROR;
        }
        continue;
      }
      if (overlap == ProbeResult::Invalid) {
        sawInvalidEntry = true;
        continue;
      }
      uint32_t start = (uint32_t)firstLba;
      uint32_t size = (uint32_t)size64;
      uint8_t pbs[512];
      if (!dev->readSector(start, pbs)) {
        sawCardError = true;
        if (searchIndex) {
          *searchIndex |= SEARCH_SAW_CARD_ERROR;
        }
        continue;
      }
      ProbeResult probe = probeBootSector(dev, pbs, start, size);
      if (probe == ProbeResult::CardError) {
        sawCardError = true;
        if (searchIndex) {
          *searchIndex |= SEARCH_SAW_CARD_ERROR;
        }
        continue;
      }
      if (probe != ProbeResult::Fat && probe != ProbeResult::ExFat) {
        continue;
      }
      loc->firstSector = start;
      loc->sectorCount = size;
      loc->type = probe == ProbeResult::ExFat
                      ? VolumeFsType::ExFat : VolumeFsType::Fat;
      if (error) {
        *error = VolumeFindError::None;
      }
      return true;
    }
  }
  if (searchIndex) {
    *searchIndex = totalSlots |
                   (sawCardError ? SEARCH_SAW_CARD_ERROR : 0);
  }
  return setError(error, sawCardError ? VolumeFindError::CardError :
                         sawInvalidEntry
                             ? VolumeFindError::CorruptPartitionTable
                             : VolumeFindError::NoSupportedFileSystem);
}
//------------------------------------------------------------------------------
static bool isProtectiveMbr(const MbrSector_t* mbr,
                            uint32_t cardSectorCount) {
  for (uint8_t i = 0; i < 4; i++) {
    const MbrPart_t* part = &mbr->part[i];
    uint32_t size = getLe32(part->totalSectors);
    if (part->type == 0XEE && (part->boot == 0 || part->boot == 0X80) &&
        getLe32(part->relativeSectors) == 1 && size != 0 &&
        size == cardSectorCount - 1) {
      return true;
    }
  }
  return false;
}
//------------------------------------------------------------------------------
static bool findMbrVolume(BlockDevice* dev,
                          const MbrSector_t* mbr,
                          uint32_t cardSectorCount,
                          VolumeLocation* loc,
                          VolumeFindError* error,
                          uint64_t* searchIndex) {
  if (isProtectiveMbr(mbr, cardSectorCount)) {
    return findGptVolume(dev, cardSectorCount, loc, error, searchIndex);
  }
  bool sawCardError = searchIndex && (*searchIndex & SEARCH_SAW_CARD_ERROR);
  uint64_t next = searchIndex ? *searchIndex & SEARCH_INDEX_MASK : 0;
  for (uint8_t i = next < 4 ? (uint8_t)next : 4; i < 4; i++) {
    if (searchIndex) {
      *searchIndex = (i + 1) |
                     (sawCardError ? SEARCH_SAW_CARD_ERROR : 0);
    }
    const MbrPart_t* part = &mbr->part[i];
    if (part->type == 0 || (part->boot != 0 && part->boot != 0X80)) {
      continue;
    }
    uint32_t start = getLe32(part->relativeSectors);
    uint32_t size = getLe32(part->totalSectors);
    if (start == 0 || size == 0 || start >= cardSectorCount ||
        size > cardSectorCount - start) {
      continue;
    }
    uint8_t pbs[512];
    if (!dev->readSector(start, pbs)) {
      sawCardError = true;
      if (searchIndex) {
        *searchIndex |= SEARCH_SAW_CARD_ERROR;
      }
      continue;
    }
    ProbeResult probe = probeBootSector(dev, pbs, start, size);
    if (probe == ProbeResult::CardError) {
      sawCardError = true;
      if (searchIndex) {
        *searchIndex |= SEARCH_SAW_CARD_ERROR;
      }
      continue;
    }
    if (probe != ProbeResult::Fat && probe != ProbeResult::ExFat) {
      continue;
    }
    loc->firstSector = start;
    loc->sectorCount = size;
    loc->type = probe == ProbeResult::ExFat
                    ? VolumeFsType::ExFat : VolumeFsType::Fat;
    if (error) {
      *error = VolumeFindError::None;
    }
    return true;
  }
  return setError(error, sawCardError ? VolumeFindError::CardError
                                      : VolumeFindError::NoSupportedFileSystem);
}
//------------------------------------------------------------------------------
bool findMountableVolume(BlockDevice* dev,
                         VolumeLocation* loc,
                         VolumeFindError* error,
                         uint64_t* searchIndex) {
  if (!dev || !loc) {
    return setError(error, VolumeFindError::NoSupportedFileSystem);
  }
  uint32_t cardSectorCount = dev->sectorCount();
  if (cardSectorCount == 0) {
    return setError(error, VolumeFindError::NoSupportedFileSystem);
  }
  uint8_t sector[512];
  if (!dev->readSector(0, sector)) {
    return setError(error, VolumeFindError::CardError);
  }
  ProbeResult probe = probeBootSector(dev, sector, 0, cardSectorCount);
  if (probe == ProbeResult::CardError) {
    return setError(error, VolumeFindError::CardError);
  }
  if (probe == ProbeResult::Fat || probe == ProbeResult::ExFat) {
    if (searchIndex && (*searchIndex & SEARCH_INDEX_MASK) != 0) {
      return setError(error, VolumeFindError::NoSupportedFileSystem);
    }
    if (searchIndex) {
      *searchIndex = 1;
    }
    loc->firstSector = 0;
    loc->sectorCount = cardSectorCount;
    loc->type = probe == ProbeResult::ExFat
                    ? VolumeFsType::ExFat : VolumeFsType::Fat;
    if (error) {
      *error = VolumeFindError::None;
    }
    return true;
  }
  const MbrSector_t* mbr = reinterpret_cast<const MbrSector_t*>(sector);
  if (getLe16(mbr->signature) != MBR_SIGNATURE) {
    return setError(error, VolumeFindError::NoSupportedFileSystem);
  }
  return findMbrVolume(dev, mbr, cardSectorCount, loc, error,
                       searchIndex);
}
//------------------------------------------------------------------------------
