/*
 * Copyright (c) 2026 sd2psX project
 *
 * MIT License
 */
#ifndef VolumeLocator_h
#define VolumeLocator_h
#include <stdint.h>
#include "BlockDevice.h"
//------------------------------------------------------------------------------
/** Location of a mountable volume on the block device. */
enum class VolumeFsType {
  Fat,
  ExFat
};

struct VolumeLocation {
  /** First sector of the filesystem (partition or superfloppy start). */
  uint32_t firstSector;
  /** Number of sectors in the filesystem. */
  uint32_t sectorCount;
  /** Filesystem type identified by boot-sector validation. */
  VolumeFsType type;
};
//------------------------------------------------------------------------------
/** Reason why findMountableVolume failed. */
enum class VolumeFindError {
  /** No error / success. */
  None,
  /** Card read error. */
  CardError,
  /** No supported FAT/exFAT filesystem found. */
  NoSupportedFileSystem,
  /** Partition table is present but corrupt or invalid. */
  CorruptPartitionTable
};
//------------------------------------------------------------------------------
/** Find a mountable FAT12/16/32 or exFAT volume.
 *
 *  Tries Superfloppy, then MBR (4 primary partitions), then GPT.
 *  If a protective MBR (type 0xEE) is found, the GPT path is taken.
 *
 *  \param[in] dev Block device.
 *  \param[out] location Mountable volume location.
 *  \param[out] error Optional failure reason.
 *  \param[in,out] searchIndex Optional persistent scan cursor. Initialize it
 *  to zero and reuse it to enumerate candidates without rescanning earlier
 *  partition entries. If null, only the first candidate is returned.
 *
 *  \return true if a mountable volume was found.
 */
bool findMountableVolume(BlockDevice* dev,
                         VolumeLocation* location,
                         VolumeFindError* error = nullptr,
                         uint64_t* searchIndex = nullptr);

#endif  // VolumeLocator_h
