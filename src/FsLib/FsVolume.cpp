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
#define DBG_FILE "FsVolume.cpp"
#include "FsLib.h"
#include "../common/VolumeLocator.h"
#include "../common/DebugMacros.h"
FsVolume* FsVolume::m_cwv = nullptr;
//------------------------------------------------------------------------------
void FsVolume::end() {
  if (m_cwv == this) {
    m_cwv = nullptr;
  }
  if (m_fVol) {
    m_fVol->~FatVolume();
    m_fVol = nullptr;
  } else if (m_xVol) {
    m_xVol->~ExFatVolume();
    m_xVol = nullptr;
  }
  m_blockDev = nullptr;
  m_initError = FsInitError::NoSupportedFileSystem;
}
//------------------------------------------------------------------------------
bool FsVolume::begin(BlockDevice* blockDev) {
  end();
  if (!blockDev) {
    m_initError = FsInitError::CardError;
    return false;
  }
  m_blockDev = blockDev;
  m_initError = FsInitError::NoSupportedFileSystem;

  bool sawCandidate = false;
  bool sawCardError = false;
  uint64_t searchIndex = 0;
  while (true) {
    VolumeLocation loc;
    VolumeFindError err = VolumeFindError::None;
    if (!findMountableVolume(m_blockDev, &loc, &err, &searchIndex)) {
      if (err == VolumeFindError::CardError) {
        sawCardError = true;
      } else if (err == VolumeFindError::CorruptPartitionTable) {
        m_initError = FsInitError::CorruptPartitionTable;
        return false;
      }
      break;
    }
    sawCandidate = true;
    if (loc.type == VolumeFsType::ExFat) {
      m_xVol = new (m_volMem) ExFatVolume;
      if (m_xVol->beginAt(m_blockDev, loc.firstSector,
                          loc.sectorCount, false)) {
        DBG_LOG("exFAT volume mounted");
        goto done;
      }
      sawCardError |= m_xVol->hasError();
      m_xVol->~ExFatVolume();
      m_xVol = nullptr;
    } else {
      m_fVol = new (m_volMem) FatVolume;
      if (m_fVol->beginAt(m_blockDev, loc.firstSector,
                          loc.sectorCount, false)) {
        DBG_LOG("FAT volume mounted");
        goto done;
      }
      sawCardError |= m_fVol->hasError();
      m_fVol->~FatVolume();
      m_fVol = nullptr;
    }
  }
  m_initError = sawCardError ? FsInitError::CardError :
                sawCandidate ? FsInitError::CorruptPartitionTable :
                               FsInitError::NoSupportedFileSystem;
  return false;

 done:
  m_cwv = this;
  m_initError = FsInitError::OK;
  return true;
}
//------------------------------------------------------------------------------
bool FsVolume::ls(print_t* pr, const char* path, uint8_t flags) {
  FsBaseFile dir;
  return dir.open(this, path, O_RDONLY) && dir.ls(pr, flags);
}
//------------------------------------------------------------------------------
FsFile FsVolume::open(const char *path, oflag_t oflag) {
  FsFile tmpFile;
  tmpFile.open(this, path, oflag);
  return tmpFile;
}
#if ENABLE_ARDUINO_STRING
//------------------------------------------------------------------------------
FsFile FsVolume::open(const String &path, oflag_t oflag) {
  return open(path.c_str(), oflag );
}
#endif  // ENABLE_ARDUINO_STRING
