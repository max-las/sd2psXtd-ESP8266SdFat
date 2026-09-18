/**
 * Copyright (c) 2011-2021 Bill Greiman
 * This file is part of the SdFat library for SD memory cards.
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#define DBG_FILE "ExFatPartition.cpp"
#include "../common/DebugMacros.h"
#include "ExFatLib.h"
//------------------------------------------------------------------------------
static uint32_t bootChecksumUpdate(uint32_t checksum, const uint8_t* sector,
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
static bool readBootRegion(BlockDevice* dev, uint32_t start,
                           uint8_t* firstSector, bool* ioError) {
  uint32_t checksum = 0;
  for (uint8_t i = 0; i < 12; i++) {
    if (!dev->readSector(start + i, firstSector)) {
      *ioError = true;
      return false;
    }
    if (i <= 8 && i != 0 && getLe16(firstSector + 510) != PBR_SIGNATURE) {
      return false;
    }
    if (i == 10) {
      for (uint16_t offset = 0; offset < 512; offset++) {
        if (firstSector[offset]) {
          return false;
        }
      }
    }
    if (i < 11) {
      checksum = bootChecksumUpdate(checksum, firstSector, i == 0);
    } else {
      for (uint16_t offset = 0; offset < 512; offset += 4) {
        if (getLe32(firstSector + offset) != checksum) {
          return false;
        }
      }
    }
  }
  return true;
}
//------------------------------------------------------------------------------
static bool isValidBootSector(const uint8_t* sector, uint32_t firstSector,
                              uint32_t sectorCount) {
  const ExFatPbs_t* pbs = reinterpret_cast<const ExFatPbs_t*>(sector);
  const BpbExFat_t* bpb = &pbs->bpb;
  static const uint8_t jump[3] = {0XEB, 0X76, 0X90};
  if (memcmp(pbs->jmpInstruction, jump, sizeof(jump)) != 0 ||
      memcmp(pbs->oemName, "EXFAT   ", 8) != 0 ||
      getLe16(pbs->signature) != PBR_SIGNATURE) {
    return false;
  }
  for (uint8_t i = 0; i < sizeof(bpb->mustBeZero); i++) {
    if (bpb->mustBeZero[i]) {
      return false;
    }
  }
  for (uint8_t i = 0; i < sizeof(bpb->reserved); i++) {
    if (bpb->reserved[i]) {
      return false;
    }
  }
  uint64_t partitionOffset = getLe64(bpb->partitionOffset);
  if (bpb->bytesPerSectorShift != 9 ||
      bpb->sectorsPerClusterShift > 16 || bpb->numberOfFats != 1 ||
      getLe16(bpb->fileSystemRevision) != 0X0100 ||
      (getLe16(bpb->volumeFlags) & 1) != 0 ||
      (partitionOffset != 0 && partitionOffset != firstSector)) {
    return false;
  }
  uint64_t volumeLength = getLe64(bpb->volumeLength);
  uint32_t fatOffset = getLe32(bpb->fatOffset);
  uint32_t fatLength = getLe32(bpb->fatLength);
  uint32_t clusterHeapOffset = getLe32(bpb->clusterHeapOffset);
  uint32_t clusterCount = getLe32(bpb->clusterCount);
  if (volumeLength < 24 || volumeLength > sectorCount || fatOffset < 24 ||
      fatLength == 0 || clusterCount == 0 || clusterCount > 0XFFFFFFF5) {
    return false;
  }
  uint64_t requiredFatSectors =
      (((uint64_t)clusterCount + 2) * 4 + 511) / 512;
  uint64_t fatRegionEnd = (uint64_t)fatOffset + fatLength;
  uint64_t clusterRegionSectors =
      (uint64_t)clusterCount << bpb->sectorsPerClusterShift;
  uint32_t rootCluster = getLe32(bpb->rootDirectoryCluster);
  return fatLength >= requiredFatSectors &&
         fatRegionEnd <= clusterHeapOffset &&
         clusterHeapOffset <= volumeLength &&
         clusterRegionSectors <= volumeLength - clusterHeapOffset &&
         rootCluster >= 2 && rootCluster <= clusterCount + 1;
}
//------------------------------------------------------------------------------
// return 0 if error, 1 if no space, else start cluster.
uint32_t ExFatPartition::bitmapFind(uint32_t cluster, uint32_t count) {
  uint32_t start = cluster ? cluster - 2 : m_bitmapStart;
  if (start >= m_clusterCount) {
    start = 0;
  }
  uint32_t endAlloc = start;
  uint32_t bgnAlloc = start;
  uint16_t sectorSize = 1 << m_bytesPerSectorShift;
  size_t i = (start >> 3) & (sectorSize - 1);
  uint8_t* cache;
  uint8_t mask = 1 << (start & 7);
  while (true) {
    uint32_t sector = m_clusterHeapStartSector +
                     (endAlloc >> (m_bytesPerSectorShift + 3));
    cache = bitmapCachePrepare(sector, FsCache::CACHE_FOR_READ);
    if (!cache) {
      return 0;
    }
    for (; i < sectorSize; i++) {
      for (; mask; mask <<= 1) {
        endAlloc++;
        if (!(mask & cache[i])) {
          if ((endAlloc - bgnAlloc) == count) {
            if (cluster == 0 && count == 1) {
              // Start at found sector.  bitmapModify may increase this.
              m_bitmapStart = bgnAlloc;
            }
            return bgnAlloc + 2;
          }
        } else {
          bgnAlloc = endAlloc;
        }
        if (endAlloc == start) {
          return 1;
        }
        if (endAlloc >= m_clusterCount) {
          endAlloc = bgnAlloc = 0;
          i = sectorSize;
          break;
        }
      }
      mask = 1;
    }
    i = 0;
  }
  return 0;
}
//------------------------------------------------------------------------------
bool ExFatPartition::bitmapModify(uint32_t cluster,
                                  uint32_t count, bool value) {
  uint32_t sector;
  uint32_t start = cluster - 2;
  size_t i;
  uint8_t* cache;
  uint8_t mask;
  cluster -= 2;
  if ((start + count) > m_clusterCount) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  if (value) {
    if (start  <= m_bitmapStart && m_bitmapStart < (start + count)) {
      m_bitmapStart = (start + count) < m_clusterCount ? start + count : 0;
    }
  } else {
    if (start < m_bitmapStart) {
      m_bitmapStart = start;
    }
  }
  mask = 1 << (start & 7);
  sector = m_clusterHeapStartSector +
                   (start >> (m_bytesPerSectorShift + 3));
  i = (start >> 3) & m_sectorMask;
  while (true) {
    cache = bitmapCachePrepare(sector++, FsCache::CACHE_FOR_WRITE);
    if (!cache) {
      DBG_FAIL_MACRO;
      goto fail;
    }
    for (; i < m_bytesPerSector; i++) {
      for (; mask; mask <<= 1) {
        if (value == static_cast<bool>(cache[i] & mask)) {
          DBG_FAIL_MACRO;
          goto fail;
        }
        cache[i] ^= mask;
        if (--count == 0) {
          return true;
        }
      }
      mask = 1;
    }
    i = 0;
  }

 fail:
  return false;
}
//------------------------------------------------------------------------------
uint32_t ExFatPartition::chainSize(uint32_t cluster) {
  uint32_t n = 0;
  int8_t status;
  do {
    status = fatGet(cluster, & cluster);
    if (status < 0) return 0;
    n++;
  } while (status);
  return n;
}
//------------------------------------------------------------------------------
uint8_t* ExFatPartition::dirCache(DirPos_t* pos, uint8_t options) {
  uint32_t sector = clusterStartSector(pos->cluster);
  sector += (m_clusterMask & pos->position) >> m_bytesPerSectorShift;
  uint8_t* cache = dataCachePrepare(sector, options);
  return cache ? cache + (pos->position & m_sectorMask) : nullptr;
}
//------------------------------------------------------------------------------
// return -1 error, 0 EOC, 1 OK
int8_t ExFatPartition::dirSeek(DirPos_t* pos, uint32_t offset) {
  int8_t status;
  uint32_t tmp = (m_clusterMask & pos->position) + offset;
  pos->position += offset;
  tmp >>= bytesPerClusterShift();
  while (tmp--) {
    if (pos->isContiguous) {
      pos->cluster++;
    } else {
      status = fatGet(pos->cluster, &pos->cluster);
      if (status != 1) {
        return status;
      }
    }
  }
  return 1;
}
//------------------------------------------------------------------------------
// return -1 error, 0 EOC, 1 OK
int8_t ExFatPartition::fatGet(uint32_t cluster, uint32_t* value) {
  uint8_t* cache;
  uint32_t next;
  uint32_t sector;

  if (cluster > (m_clusterCount + 1)) {
    DBG_FAIL_MACRO;
    return -1;
  }
  sector = m_fatStartSector + (cluster >> (m_bytesPerSectorShift - 2));

  cache = dataCachePrepare(sector, FsCache::CACHE_FOR_READ);
  if (!cache) {
    return -1;
  }
  next = getLe32(cache + ((cluster << 2) & m_sectorMask));
  if (next == EXFAT_EOC) {
    return 0;
  }
  *value = next;
  return 1;
}
//------------------------------------------------------------------------------
bool ExFatPartition::fatPut(uint32_t cluster, uint32_t value) {
  uint32_t sector;
  uint8_t* cache;
  if (cluster < 2 || cluster > (m_clusterCount + 1)) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  sector = m_fatStartSector + (cluster >> (m_bytesPerSectorShift - 2));
  cache = dataCachePrepare(sector, FsCache::CACHE_FOR_WRITE);
  if (!cache) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  setLe32(cache + ((cluster << 2) & m_sectorMask), value);
  return true;

 fail:
  return false;
}
//------------------------------------------------------------------------------
bool ExFatPartition::freeChain(uint32_t cluster) {
  uint32_t next;
  uint32_t start = cluster;
  int8_t status;
  do {
    status = fatGet(cluster, &next);
    if (status < 0) {
      DBG_FAIL_MACRO;
      goto fail;
    }
    if (!fatPut(cluster, 0)) {
      DBG_FAIL_MACRO;
      goto fail;
    }
    if (status == 0 || (cluster + 1) != next) {
      if (!bitmapModify(start, cluster - start + 1, 0)) {
        DBG_FAIL_MACRO;
        goto fail;
      }
      start = next;
    }
    cluster = next;
  } while (status);

  return true;

 fail:
  return false;
}
//------------------------------------------------------------------------------
uint32_t ExFatPartition::freeClusterCount() {
  uint32_t nc = 0;
  uint32_t sector = m_clusterHeapStartSector;
  uint32_t usedCount = 0;
  uint8_t* cache;

  while (true) {
    cache = dataCachePrepare(sector++, FsCache::CACHE_FOR_READ);
    if (!cache) {
      return 0;
    }
    for (size_t i = 0; i < m_bytesPerSector; i++) {
      if (cache[i] == 0XFF) {
        usedCount+= 8;
      } else if (cache[i]) {
        for (uint8_t mask = 1; mask ; mask <<=1) {
          if ((mask & cache[i])) {
            usedCount++;
          }
        }
      }
      nc += 8;
      if (nc >= m_clusterCount) {
        return m_clusterCount - usedCount;
      }
    }
  }
}
//------------------------------------------------------------------------------
bool ExFatPartition::init(BlockDevice* dev, uint8_t part) {
  m_fatType = 0;
  m_blockDev = dev;
  cacheInit(dev);
  if (!dev) {
    DBG_FAIL_MACRO;
    return false;
  }
  if (part == 0) {
    return initAt(dev, 0, dev->sectorCount());
  }
  if (part > 4) {
    DBG_FAIL_MACRO;
    return false;
  }
  uint8_t* mbrSector = dataCachePrepare(0, FsCache::CACHE_FOR_READ);
  if (!mbrSector) {
    DBG_FAIL_MACRO;
    return false;
  }
  MbrSector_t* mbr = reinterpret_cast<MbrSector_t*>(mbrSector);
  if (getLe16(mbr->signature) != MBR_SIGNATURE) {
    DBG_FAIL_MACRO;
    return false;
  }
  MbrPart_t mp = mbr->part[part - 1];
  if ((mp.boot != 0 && mp.boot != 0X80) || mp.type == 0) {
    DBG_FAIL_MACRO;
    return false;
  }
  uint32_t volStart = getLe32(mp.relativeSectors);
  uint32_t volSize = getLe32(mp.totalSectors);
  return initAt(dev, volStart, volSize);
}
//------------------------------------------------------------------------------
bool ExFatPartition::initAt(BlockDevice* dev,
                            uint32_t firstSector,
                            uint32_t sectorCount) {
  uint8_t bootSector[512];
  pbs_t* pbs;
  BpbExFat_t* bpb;
  uint32_t clusterCount;
  uint32_t clusterHeapOffset;
  uint32_t fatLength;
  uint32_t fatOffset;
  uint64_t clusterRegionSectors;
  uint64_t fatRegionEnd;
  uint64_t partitionOffset;
  uint64_t requiredFatSectors;
  uint64_t volumeLength;
  bool ioError = false;
  bool validBootSector = false;

  m_fatType = 0;
  m_blockDev = dev;
  cacheInit(m_blockDev);
  if (!dev || sectorCount < 24 || firstSector >= dev->sectorCount() ||
      sectorCount > dev->sectorCount() - firstSector) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  if (readBootRegion(dev, firstSector, bootSector, &ioError)) {
    // readBootRegion reuses this buffer, so reload the first sector for BPB
    // validation after the checksum sector has been consumed.
    if (dev->readSector(firstSector, bootSector)) {
      validBootSector = isValidBootSector(bootSector, firstSector, sectorCount);
    } else {
      ioError = true;
    }
  }
  if (!validBootSector && sectorCount >= 24 &&
      readBootRegion(dev, firstSector + 12, bootSector, &ioError)) {
    if (dev->readSector(firstSector + 12, bootSector)) {
      validBootSector = isValidBootSector(bootSector, firstSector, sectorCount);
    } else {
      ioError = true;
    }
  }
  if (!validBootSector) {
    if (ioError) {
      m_dataCache.markError();
    }
    DBG_FAIL_MACRO;
    goto fail;
  }
  pbs = reinterpret_cast<pbs_t*>(bootSector);
  bpb = reinterpret_cast<BpbExFat_t*>(pbs->bpb);
  for (uint8_t i = 0; i < sizeof(bpb->mustBeZero); i++) {
    if (bpb->mustBeZero[i]) {
      DBG_FAIL_MACRO;
      goto fail;
    }
  }
  for (uint8_t i = 0; i < sizeof(bpb->reserved); i++) {
    if (bpb->reserved[i]) {
      DBG_FAIL_MACRO;
      goto fail;
    }
  }
  partitionOffset = getLe64(bpb->partitionOffset);
  if (bpb->bytesPerSectorShift != m_bytesPerSectorShift ||
      bpb->sectorsPerClusterShift > 16 ||
      bpb->numberOfFats != 1 ||
      getLe16(bpb->fileSystemRevision) != 0X0100 ||
      (getLe16(bpb->volumeFlags) & 1) != 0 ||
      (partitionOffset != 0 && partitionOffset != firstSector)) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  volumeLength = getLe64(bpb->volumeLength);
  fatOffset = getLe32(bpb->fatOffset);
  fatLength = getLe32(bpb->fatLength);
  clusterHeapOffset = getLe32(bpb->clusterHeapOffset);
  clusterCount = getLe32(bpb->clusterCount);
  if (volumeLength < 24 || volumeLength > sectorCount || fatOffset < 24 ||
      fatLength == 0 || clusterCount == 0 || clusterCount > 0XFFFFFFF5) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  requiredFatSectors = (((uint64_t)clusterCount + 2) * 4 +
                        m_bytesPerSector - 1) / m_bytesPerSector;
  fatRegionEnd = (uint64_t)fatOffset + fatLength;
  clusterRegionSectors = (uint64_t)clusterCount <<
                         bpb->sectorsPerClusterShift;
  if (fatLength < requiredFatSectors || fatRegionEnd > clusterHeapOffset ||
      clusterHeapOffset > volumeLength ||
      clusterRegionSectors > volumeLength - clusterHeapOffset) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  m_rootDirectoryCluster = getLe32(bpb->rootDirectoryCluster);
  if (m_rootDirectoryCluster < 2 ||
      m_rootDirectoryCluster > clusterCount + 1) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  m_fatStartSector = firstSector + fatOffset;
  m_fatLength = fatLength;
  m_clusterHeapStartSector = firstSector + clusterHeapOffset;
  m_clusterCount = clusterCount;
  m_sectorsPerClusterShift = bpb->sectorsPerClusterShift;
  m_bytesPerCluster = 1UL << (m_bytesPerSectorShift + m_sectorsPerClusterShift);
  m_clusterMask = m_bytesPerCluster - 1;
  // Set m_bitmapStart to first free cluster.
  m_bitmapStart = 0;
  #if 0
  if (bitmapFind(0, 1) == 0) {
    DBG_FAIL_MACRO;
    goto fail;
  }
  #endif
  m_fatType = FAT_TYPE_EXFAT;
  return true;

 fail:
  return false;
}
//------------------------------------------------------------------------------
uint32_t ExFatPartition::rootLength() {
  uint32_t nc = chainSize(m_rootDirectoryCluster);
  return nc << bytesPerClusterShift();
}
