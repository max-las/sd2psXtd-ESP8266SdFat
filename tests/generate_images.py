#!/usr/bin/env python3
"""Generate test SD card images for ESP8266SdFat mount tests.

Images are written to tests/images/ and cover:
  - Superfloppy FAT12/16/32
  - Superfloppy exFAT
  - MBR with 4 primary partitions (FAT12/16/32/exFAT)
  - GPT with FAT16 and exFAT partitions
  - Negative cases (raw zeros, protective MBR without GPT, GPT with bad CRC,
    MBR with only Linux partitions).

Requires pyfatfs (install via tests/venv).
"""
import os
import shutil
import sys
import struct
import uuid
import argparse

# Optional: pyfatfs is used for FAT12/16/32 image generation.
try:
    from pyfatfs.PyFat import PyFat
    from pyfatfs.PyFatFS import PyFatFS
    from fs.opener import open_fs
    HAS_PYFATFS = True
except ImportError:
    HAS_PYFATFS = False


SECTOR_SIZE = 512

# GPT GUIDs in mixed-endian byte order (as stored on disk).
GPT_MS_BASIC_DATA_GUID = uuid.UUID(
    "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
).bytes_le
GPT_LINUX_FS_GUID = uuid.UUID(
    "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
).bytes_le


def crc32_byte(data: bytes) -> int:
    """Standard CRC-32 (PKZIP) as used by GPT."""
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if (crc & 1) else 0)
    return crc ^ 0xFFFFFFFF


def clear_image(path: str, size: int) -> None:
    """Create a zero-filled image file."""
    with open(path, "wb") as f:
        f.write(bytearray(size))


def patch_fat_hidden_sectors(img_path: str, start_lba: int) -> None:
    """Fix the BPB hiddenSectors field so it matches the partition start."""
    with open(img_path, "r+b") as f:
        f.seek(start_lba * SECTOR_SIZE + 28)
        f.write(struct.pack("<I", start_lba))


def fat_create(img_path: str, start_lba: int, size_bytes: int, fat_type: int,
               label: str = "NO NAME") -> None:
    """Create a FAT12/16/32 filesystem at the given LBA.

    pyfatfs' mkfs truncates the target file, so the filesystem is built in a
    temporary image of the exact partition size and then copied into the
    main image without changing its size.
    """
    if not HAS_PYFATFS:
        raise RuntimeError("pyfatfs is required for FAT image generation")
    temp_path = img_path + ".fatpart.tmp"
    clear_image(temp_path, size_bytes)
    try:
        pf = PyFat(offset=0)
        pf.mkfs(temp_path, fat_type, size=size_bytes, sector_size=SECTOR_SIZE,
                label=label)
        pf.close()
        with open(temp_path, "rb") as src:
            with open(img_path, "r+b") as dst:
                dst.seek(start_lba * SECTOR_SIZE)
                dst.write(src.read(size_bytes))
        patch_fat_hidden_sectors(img_path, start_lba)
    finally:
        if os.path.exists(temp_path):
            os.remove(temp_path)


def fat_add_file(img_path: str, start_lba: int, filename: str,
                 content: bytes) -> None:
    """Add a file to a FAT filesystem at the given LBA."""
    abs_path = os.path.abspath(img_path)
    with open_fs("fat:///{path}?offset={off}".format(
            path=abs_path, off=start_lba * SECTOR_SIZE)) as fat:
        fat.writebytes(filename, content)


def exfat_name_hash(name: str) -> int:
    """exFAT name hash (ASCII path, upper-cased).

    Matches the uint16_t implementation in SdFat: the intermediate value is
    truncated to 16 bits after the first addition, before the second rotate.
    """
    h = 0
    for c in name:
        u = ord(c.upper())
        h = (((h << 15) | (h >> 1)) + u) & 0xFFFF
        h = ((h << 15) | (h >> 1)) & 0xFFFF
    return h


def exfat_dir_checksum(data: bytes, checksum: int = 0) -> int:
    """exFAT directory entry set checksum.

    Skips bytes 2 and 3 of the File Directory Entry (0x85) because those
    hold the SetChecksum field itself.
    """
    assert len(data) == 32
    skip = data[0] == 0x85
    i = 0
    while i < 32:
        checksum = ((checksum << 15) | (checksum >> 1)) + data[i]
        checksum &= 0xFFFF
        if i == 1 and skip:
            i += 3
        else:
            i += 1
    return checksum


def exfat_boot_checksum(data: bytes) -> int:
    """exFAT main boot-region checksum (sectors 0..10)."""
    c = 0
    for i, b in enumerate(data):
        # VolumeFlags and PercentInUse are excluded from the checksum.
        if i in (106, 107, 112):
            continue
        c = ((c << 31) | (c >> 1)) + b
        c &= 0xFFFFFFFF
    return c


def exfat_checksum(data: bytes) -> int:
    """exFAT rotate-right/add checksum without excluded offsets."""
    checksum = 0
    for b in data:
        checksum = ((checksum << 31) | (checksum >> 1)) + b
        checksum &= 0xFFFFFFFF
    return checksum


def refresh_exfat_boot_checksum(img_path: str, start_lba: int = 0) -> None:
    """Recalculate and mirror both exFAT boot regions after BPB mutation."""
    with open(img_path, "r+b") as f:
        f.seek(start_lba * SECTOR_SIZE)
        data = f.read(11 * SECTOR_SIZE)
        checksum = exfat_boot_checksum(data)
        f.seek((start_lba + 11) * SECTOR_SIZE)
        f.write(struct.pack("<I", checksum) * 128)
        f.seek(start_lba * SECTOR_SIZE)
        boot_region = f.read(12 * SECTOR_SIZE)
        f.seek((start_lba + 12) * SECTOR_SIZE)
        f.write(boot_region)


def exfat_create(img_path: str, start_lba: int, size_bytes: int,
                 filename: str = "TEST.TXT", file_content: bytes = b"EXFAT test") -> None:
    """Create a minimal mountable exFAT volume at the given LBA."""
    total_sectors = size_bytes // SECTOR_SIZE
    # Layout relative to the volume start:
    #   LBA 0       boot sector
    #   LBA 1-8     extended boot sectors
    #   LBA 9       OEM parameters (zeros)
    #   LBA 10      reserved
    #   LBA 11      boot checksum
    #   LBA 12-23   backup boot region
    #   LBA 24      FAT
    #   LBA 25      cluster heap start (bitmap cluster)
    #   cluster 2   allocation bitmap
    #   cluster 3   upcase table
    #   cluster 4   root directory
    #   cluster 5   file data
    fat_offset = 24
    fat_length = 1
    cluster_heap_offset = fat_offset + fat_length
    cluster_count = 64
    sectors_per_cluster_shift = 4
    cluster_size = SECTOR_SIZE << sectors_per_cluster_shift
    bytes_per_sector_shift = 9
    root_dir_cluster = 4
    bitmap_cluster = 2
    upcase_cluster = 3
    file_cluster = 5

    # Build the main boot region (12 sectors) in a buffer.
    boot_region = bytearray(12 * SECTOR_SIZE)
    bs = boot_region

    # Jump + OEM
    bs[0:3] = bytes([0xEB, 0x76, 0x90])
    bs[3:11] = b"EXFAT   "
    # mustBeZero (53 bytes) already zero
    struct.pack_into("<Q", bs, 64, start_lba)          # partitionOffset
    struct.pack_into("<Q", bs, 72, total_sectors)       # volumeLength
    struct.pack_into("<I", bs, 80, fat_offset)          # fatOffset
    struct.pack_into("<I", bs, 84, fat_length)          # fatLength
    struct.pack_into("<I", bs, 88, cluster_heap_offset) # clusterHeapOffset
    struct.pack_into("<I", bs, 92, cluster_count)       # clusterCount
    struct.pack_into("<I", bs, 96, root_dir_cluster)   # firstClusterOfRootDir
    struct.pack_into("<I", bs, 100, 0x12345678)         # volumeSerial
    struct.pack_into("<H", bs, 104, 0x0100)             # fileSystemRevision
    struct.pack_into("<H", bs, 106, 0)                  # volumeFlags
    bs[108] = bytes_per_sector_shift
    bs[109] = sectors_per_cluster_shift
    bs[110] = 1                                          # numberOfFats
    bs[111] = 0x80                                       # driveSelect
    bs[112] = 0xFF                                       # percentInUse
    # boot code (390 bytes) + signature
    bs[510:512] = b"\x55\xAA"
    for sector in range(1, 9):
        offset = sector * SECTOR_SIZE
        bs[offset + 510:offset + 512] = b"\x55\xAA"

    # Boot checksum sector (LBA 11) filled with repeated checksum.
    checksum = exfat_boot_checksum(bs[:11 * SECTOR_SIZE])
    for i in range(128):
        struct.pack_into("<I", bs, 11 * SECTOR_SIZE + i * 4, checksum)

    # Write matching main and backup boot regions.
    with open(img_path, "r+b") as f:
        f.seek(start_lba * SECTOR_SIZE)
        f.write(boot_region)
        f.write(boot_region)

    # FAT (one sector)
    fat = bytearray(SECTOR_SIZE)
    struct.pack_into("<I", fat, 0, 0xFFFFFFF8)          # media/reserved
    struct.pack_into("<I", fat, 4, 0xFFFFFFFF)          # reserved
    struct.pack_into("<I", fat, bitmap_cluster * 4, 0xFFFFFFFF)
    struct.pack_into("<I", fat, upcase_cluster * 4, 0xFFFFFFFF)
    struct.pack_into("<I", fat, root_dir_cluster * 4, 0xFFFFFFFF)
    struct.pack_into("<I", fat, file_cluster * 4, 0xFFFFFFFF)
    with open(img_path, "r+b") as f:
        f.seek(start_lba * SECTOR_SIZE + fat_offset * SECTOR_SIZE)
        f.write(fat)

    # Allocation bitmap (cluster 2)
    bitmap = bytearray(SECTOR_SIZE)
    # clusters 2,3,4,5 are allocated
    bitmap[0] = 0x0F
    with open(img_path, "r+b") as f:
        f.seek(start_lba * SECTOR_SIZE + cluster_heap_offset * SECTOR_SIZE
               + (bitmap_cluster - 2) * SECTOR_SIZE)
        f.write(bitmap)

    # A valid custom upcase table: ASCII lowercase maps to uppercase and all
    # remaining code points are identity-mapped through a compressed skip.
    upcase_values = list(range(0x61))
    upcase_values.extend(range(0x41, 0x5B))
    upcase_values.extend((0xFFFF, 0x10000 - 0x7B))
    upcase = struct.pack("<{}H".format(len(upcase_values)), *upcase_values)
    upcase_sector = bytearray(cluster_size)
    upcase_sector[:len(upcase)] = upcase
    with open(img_path, "r+b") as f:
        f.seek(start_lba * SECTOR_SIZE + cluster_heap_offset * SECTOR_SIZE
               + (upcase_cluster - 2) * cluster_size)
        f.write(upcase_sector)

    # Root directory (cluster 4)
    root = bytearray(SECTOR_SIZE)
    bitmap_entry = bytearray(32)
    bitmap_entry[0] = 0x81
    struct.pack_into("<I", bitmap_entry, 20, bitmap_cluster)
    struct.pack_into("<Q", bitmap_entry, 24, (cluster_count + 7) // 8)
    root[0:32] = bitmap_entry

    upcase_entry = bytearray(32)
    upcase_entry[0] = 0x82
    struct.pack_into("<I", upcase_entry, 4, exfat_checksum(upcase))
    struct.pack_into("<I", upcase_entry, 20, upcase_cluster)
    struct.pack_into("<Q", upcase_entry, 24, len(upcase))
    root[32:64] = upcase_entry

    # Build secondary entries first to calculate set checksum.
    name_utf16 = filename.encode("utf-16-le")[:30]
    name_utf16 = name_utf16 + b"\x00" * (30 - len(name_utf16))
    name_len = len(filename)
    name_hash = exfat_name_hash(filename)

    stream = bytearray(32)
    stream[0] = 0xC0
    stream[1] = 0x03                       # AllocationPossible | NoFatChain
    stream[3] = name_len
    struct.pack_into("<H", stream, 4, name_hash)
    struct.pack_into("<Q", stream, 8, len(file_content))  # validDataLength
    struct.pack_into("<I", stream, 20, file_cluster)     # firstCluster
    struct.pack_into("<Q", stream, 24, len(file_content))  # dataLength

    fname_entry = bytearray(32)
    fname_entry[0] = 0xC1
    fname_entry[2:32] = name_utf16

    file_entry = bytearray(32)
    file_entry[0] = 0x85
    file_entry[1] = 2                      # secondary count
    file_entry[4] = 0x20                   # archive
    checksum = exfat_dir_checksum(file_entry)
    checksum = exfat_dir_checksum(stream, checksum)
    checksum = exfat_dir_checksum(fname_entry, checksum)
    struct.pack_into("<H", file_entry, 2, checksum)

    root[64:96] = file_entry
    root[96:128] = stream
    root[128:160] = fname_entry
    with open(img_path, "r+b") as f:
        f.seek(start_lba * SECTOR_SIZE + cluster_heap_offset * SECTOR_SIZE
               + (root_dir_cluster - 2) * cluster_size)
        f.write(root)

    # File data cluster
    data = bytearray(SECTOR_SIZE)
    data[:len(file_content)] = file_content
    with open(img_path, "r+b") as f:
        f.seek(start_lba * SECTOR_SIZE + cluster_heap_offset * SECTOR_SIZE
               + (file_cluster - 2) * cluster_size)
        f.write(data)


def write_mbr(img_path: str, partitions: list) -> None:
    """Write a classic MBR partition table.

    partitions is a list of up to 4 dicts with keys:
      type (int), start (int sectors), size (int sectors), boot (bool)
    """
    assert len(partitions) <= 4
    with open(img_path, "r+b") as f:
        f.seek(446)
        for p in partitions:
            entry = bytearray(16)
            entry[0] = 0x80 if p.get("boot", False) else 0x00
            # CHS start/end ignored (use LBA)
            entry[1:4] = b"\x00\x00\x00"
            entry[4] = p["type"] & 0xFF
            entry[5:8] = b"\x00\x00\x00"
            struct.pack_into("<I", entry, 8, p["start"])
            struct.pack_into("<I", entry, 12, p["size"])
            f.write(entry)
        f.seek(510)
        f.write(b"\x55\xAA")


def write_gpt(img_path: str, partitions: list, entry_size: int = 128,
              num_entries: int = None) -> None:
    """Write a protective MBR + GPT with the given partitions.

    partitions is a list of dicts with keys:
      type (bytes), start (int sectors), size (int sectors), name (str)
    """
    with open(img_path, "r+b") as f:
        f.seek(0, os.SEEK_END)
        total_size = f.tell()
    total_sectors = total_size // SECTOR_SIZE
    if total_sectors < 34:
        raise ValueError("image too small for GPT")

    # Protective MBR.
    with open(img_path, "r+b") as f:
        f.seek(446)
        entry = bytearray(16)
        entry[0] = 0x00
        entry[4] = 0xEE
        struct.pack_into("<I", entry, 8, 1)
        struct.pack_into("<I", entry, 12, total_sectors - 1)
        f.write(entry)
        f.seek(510)
        f.write(b"\x55\xAA")

    # GPT parameters
    num_entries = max(len(partitions), 4) if num_entries is None else num_entries
    if num_entries < len(partitions):
        raise ValueError("num_entries is smaller than the partition list")
    array_size = num_entries * entry_size
    array_sectors = (array_size + SECTOR_SIZE - 1) // SECTOR_SIZE
    primary_array_lba = 2
    backup_array_lba = total_sectors - 1 - array_sectors
    backup_header_lba = total_sectors - 1
    first_usable = primary_array_lba + array_sectors
    last_usable = backup_array_lba - 1

    # Build entry array. Each entry is exactly 128 bytes; unused bytes are 0.
    entry_array = bytearray(array_size)
    for i, p in enumerate(partitions):
        off = i * entry_size
        entry_array[off:off+16] = p["type"]
        entry_array[off+16:off+32] = uuid.uuid4().bytes_le
        struct.pack_into("<Q", entry_array, off+32, p["start"])
        struct.pack_into("<Q", entry_array, off+40, p["start"] + p["size"] - 1)
        name = p["name"].encode("utf-16-le")[:80]
        entry_array[off+48:off+48+len(name)] = name

    disk_guid = uuid.uuid4().bytes_le

    def build_header(my_lba, alternate_lba, array_lba, entries_crc):
        hdr = bytearray(SECTOR_SIZE)
        hdr[0:8] = b"EFI PART"
        struct.pack_into("<I", hdr, 8, 0x00010000)
        struct.pack_into("<I", hdr, 12, 92)
        struct.pack_into("<I", hdr, 16, 0)          # CRC placeholder
        struct.pack_into("<I", hdr, 20, 0)
        struct.pack_into("<Q", hdr, 24, my_lba)
        struct.pack_into("<Q", hdr, 32, alternate_lba)
        struct.pack_into("<Q", hdr, 40, first_usable)
        struct.pack_into("<Q", hdr, 48, last_usable)
        hdr[56:72] = disk_guid
        struct.pack_into("<Q", hdr, 72, array_lba)
        struct.pack_into("<I", hdr, 80, num_entries)
        struct.pack_into("<I", hdr, 84, entry_size)
        struct.pack_into("<I", hdr, 88, entries_crc)
        # rest is zero
        crc = crc32_byte(hdr[:92])
        struct.pack_into("<I", hdr, 16, crc)
        return hdr

    entries_crc = crc32_byte(entry_array)
    primary_header = build_header(1, backup_header_lba, primary_array_lba,
                                  entries_crc)
    backup_header = build_header(backup_header_lba, 1, backup_array_lba,
                                 entries_crc)

    with open(img_path, "r+b") as f:
        f.seek(SECTOR_SIZE)
        f.write(primary_header)
        f.seek(primary_array_lba * SECTOR_SIZE)
        f.write(entry_array)
        f.seek(backup_array_lba * SECTOR_SIZE)
        f.write(entry_array)
        f.seek(backup_header_lba * SECTOR_SIZE)
        f.write(backup_header)


def main():
    parser = argparse.ArgumentParser(description="Generate SdFat test images")
    parser.add_argument("--out-dir", default="tests/images",
                        help="output directory")
    parser.add_argument("--skip-fat", action="store_true",
                        help="skip FAT images (requires pyfatfs)")
    args = parser.parse_args()

    out_dir = args.out_dir
    os.makedirs(out_dir, exist_ok=True)

    if not HAS_PYFATFS and not args.skip_fat:
        print("ERROR: pyfatfs is not installed. Use tests/venv or --skip-fat.",
              file=sys.stderr)
        sys.exit(1)

    print(f"Generating images in {out_dir}...")

    # Superfloppy images
    if not args.skip_fat:
        sf = os.path.join(out_dir, "superfloppy_fat12.img")
        clear_image(sf, 8 * 1024 * 1024)
        fat_create(sf, 0, 8 * 1024 * 1024, 12)
        fat_add_file(sf, 0, "hello.txt", b"FAT12 superfloppy")
        print("  superfloppy FAT12")

        sf = os.path.join(out_dir, "superfloppy_fat16.img")
        clear_image(sf, 16 * 1024 * 1024)
        fat_create(sf, 0, 16 * 1024 * 1024, 16)
        fat_add_file(sf, 0, "hello.txt", b"FAT16 superfloppy")
        print("  superfloppy FAT16")

        sf = os.path.join(out_dir, "superfloppy_fat32.img")
        clear_image(sf, 64 * 1024 * 1024)
        fat_create(sf, 0, 64 * 1024 * 1024, 32)
        fat_add_file(sf, 0, "hello.txt", b"FAT32 superfloppy")
        print("  superfloppy FAT32")

    sf = os.path.join(out_dir, "superfloppy_exfat.img")
    clear_image(sf, 8 * 1024 * 1024)
    exfat_create(sf, 0, 8 * 1024 * 1024, "hello.txt", b"EXFAT superfloppy")
    print("  superfloppy exFAT")

    # MBR with 4 primary partitions
    if not args.skip_fat:
        mbr = os.path.join(out_dir, "mbr_4primary.img")
        clear_image(mbr, 128 * 1024 * 1024)
        # Partition 1: FAT12
        fat_create(mbr, 2048, 8 * 1024 * 1024, 12)
        fat_add_file(mbr, 2048, "hello.txt", b"FAT12 MBR")
        # Partition 2: FAT16
        fat_create(mbr, 18432, 16 * 1024 * 1024, 16)
        fat_add_file(mbr, 18432, "hello.txt", b"FAT16 MBR")
        # Partition 3: FAT32
        fat_create(mbr, 51200, 64 * 1024 * 1024, 32)
        fat_add_file(mbr, 51200, "hello.txt", b"FAT32 MBR")
        # Partition 4: exFAT
        exfat_create(mbr, 184320, 8 * 1024 * 1024, "hello.txt", b"EXFAT MBR")
        write_mbr(mbr, [
            {"type": 0x01, "start": 2048, "size": 8 * 1024 * 1024 // 512},
            {"type": 0x06, "start": 18432, "size": 16 * 1024 * 1024 // 512},
            {"type": 0x0C, "start": 51200, "size": 64 * 1024 * 1024 // 512},
            {"type": 0x07, "start": 184320, "size": 8 * 1024 * 1024 // 512},
        ])
        print("  MBR 4 primary")

    # GPT with FAT16 and exFAT partitions
    gpt = os.path.join(out_dir, "gpt_fat16_exfat.img")
    clear_image(gpt, 80 * 1024 * 1024)
    if not args.skip_fat:
        fat_create(gpt, 2048, 16 * 1024 * 1024, 16)
        fat_add_file(gpt, 2048, "hello.txt", b"FAT16 GPT")
    exfat_create(gpt, 34816, 8 * 1024 * 1024, "hello.txt", b"EXFAT GPT")
    write_gpt(gpt, [
        {"type": GPT_MS_BASIC_DATA_GUID, "start": 2048, "size": 16 * 1024 * 1024 // 512,
         "name": "FAT16"},
        {"type": GPT_MS_BASIC_DATA_GUID, "start": 34816, "size": 8 * 1024 * 1024 // 512,
         "name": "EXFAT"},
    ])
    print("  GPT FAT16 + exFAT")

    # GPT where first partition is Linux, second is FAT -> tests fallback
    if not args.skip_fat:
        gpt2 = os.path.join(out_dir, "gpt_linux_fat12.img")
        clear_image(gpt2, 32 * 1024 * 1024)
        fat_create(gpt2, 34816, 8 * 1024 * 1024, 12)
        fat_add_file(gpt2, 34816, "hello.txt", b"FAT12 GPT fallback")
        write_gpt(gpt2, [
            {"type": GPT_LINUX_FS_GUID, "start": 2048, "size": 16 * 1024 * 1024 // 512,
             "name": "Linux"},
            {"type": GPT_MS_BASIC_DATA_GUID, "start": 34816, "size": 8 * 1024 * 1024 // 512,
             "name": "FAT12"},
        ])
        print("  GPT Linux + FAT12")

    # Negative images
    raw = os.path.join(out_dir, "raw_zeros.img")
    clear_image(raw, 8 * 1024 * 1024)
    print("  raw zeros")

    prot = os.path.join(out_dir, "protective_mbr_no_gpt.img")
    clear_image(prot, 8 * 1024 * 1024)
    write_mbr(prot, [{"type": 0xEE, "start": 1, "size": 8 * 1024 * 1024 // 512 - 1}])
    print("  protective MBR without GPT")

    mbr_linux = os.path.join(out_dir, "mbr_linux_only.img")
    clear_image(mbr_linux, 8 * 1024 * 1024)
    write_mbr(mbr_linux, [{"type": 0x82, "start": 2048, "size": 7 * 1024 * 1024 // 512}])
    print("  MBR Linux only")

    gpt_bad = os.path.join(out_dir, "gpt_bad_crc.img")
    clear_image(gpt_bad, 16 * 1024 * 1024)
    if not args.skip_fat:
        fat_create(gpt_bad, 2048, 8 * 1024 * 1024, 12)
    write_gpt(gpt_bad, [
        {"type": GPT_MS_BASIC_DATA_GUID, "start": 2048, "size": 8 * 1024 * 1024 // 512,
         "name": "FAT12"},
    ])
    # Corrupt the partition entry array by flipping one byte.
    with open(gpt_bad, "r+b") as f:
        f.seek(2 * SECTOR_SIZE)
        f.write(b"\x42")
    print("  GPT with corrupt primary array")

    gpt_bad_size = os.path.join(out_dir, "gpt_bad_entry_size.img")
    clear_image(gpt_bad_size, 16 * 1024 * 1024)
    write_gpt(gpt_bad_size, [
        {"type": GPT_MS_BASIC_DATA_GUID, "start": 2048,
         "size": 8 * 1024 * 1024 // 512, "name": "FAT12"},
    ])
    # Both headers advertise an entry size that is not a multiple of 128.
    with open(gpt_bad_size, "r+b") as f:
        for lba in (1, (16 * 1024 * 1024 // SECTOR_SIZE) - 1):
            f.seek(lba * SECTOR_SIZE)
            hdr = bytearray(f.read(SECTOR_SIZE))
            struct.pack_into("<I", hdr, 84, 136)
            struct.pack_into("<I", hdr, 16, 0)
            struct.pack_into("<I", hdr, 16, crc32_byte(hdr[:92]))
            f.seek(lba * SECTOR_SIZE)
            f.write(hdr)
    print("  GPT with invalid entry size")

    if not args.skip_fat:
        gpt_384 = os.path.join(out_dir, "gpt_entry_size_384.img")
        clear_image(gpt_384, 16 * 1024 * 1024)
        fat_create(gpt_384, 2048, 8 * 1024 * 1024, 12)
        fat_add_file(gpt_384, 2048, "hello.txt", b"GPT 384-byte entry")
        write_gpt(gpt_384, [
            {"type": GPT_MS_BASIC_DATA_GUID, "start": 2048,
             "size": 8 * 1024 * 1024 // 512, "name": "FAT12"},
        ], entry_size=384)
        print("  GPT with 384-byte entries")

        gpt_many = os.path.join(out_dir, "gpt_many_entries.img")
        clear_image(gpt_many, 16 * 1024 * 1024)
        fat_create(gpt_many, 2048, 8 * 1024 * 1024, 12)
        fat_add_file(gpt_many, 2048, "hello.txt", b"GPT large table")
        write_gpt(gpt_many, [
            {"type": GPT_MS_BASIC_DATA_GUID, "start": 2048,
             "size": 8 * 1024 * 1024 // 512, "name": "FAT12"},
        ], num_entries=300)
        print("  GPT with more than 256 entries")

        gpt_mismatch = os.path.join(out_dir, "gpt_mismatched_backup.img")
        clear_image(gpt_mismatch, 16 * 1024 * 1024)
        fat_create(gpt_mismatch, 2048, 8 * 1024 * 1024, 12)
        write_gpt(gpt_mismatch, [
            {"type": GPT_MS_BASIC_DATA_GUID, "start": 2048,
             "size": 8 * 1024 * 1024 // 512, "name": "FAT12"},
        ])
        with open(gpt_mismatch, "r+b") as f:
            f.seek(2 * SECTOR_SIZE)
            f.write(b"\x42")
            f.seek(-SECTOR_SIZE, os.SEEK_END)
            hdr = bytearray(f.read(SECTOR_SIZE))
            hdr[56] ^= 0xFF
            struct.pack_into("<I", hdr, 16, 0)
            struct.pack_into("<I", hdr, 16, crc32_byte(hdr[:92]))
            f.seek(-SECTOR_SIZE, os.SEEK_END)
            f.write(hdr)
        print("  GPT with mismatched backup metadata")

    gpt_overlap = os.path.join(out_dir, "gpt_metadata_overlap.img")
    clear_image(gpt_overlap, 16 * 1024 * 1024)
    write_gpt(gpt_overlap, [
        {"type": GPT_MS_BASIC_DATA_GUID, "start": 2048,
         "size": 8 * 1024 * 1024 // 512, "name": "FAT12"},
    ])
    with open(gpt_overlap, "r+b") as f:
        for lba in (1, (16 * 1024 * 1024 // SECTOR_SIZE) - 1):
            f.seek(lba * SECTOR_SIZE)
            hdr = bytearray(f.read(SECTOR_SIZE))
            struct.pack_into("<Q", hdr, 40, 2)
            struct.pack_into("<I", hdr, 16, 0)
            struct.pack_into("<I", hdr, 16, crc32_byte(hdr[:92]))
            f.seek(lba * SECTOR_SIZE)
            f.write(hdr)
    print("  GPT metadata overlapping usable space")

    exfat_bad_checksum = os.path.join(out_dir, "superfloppy_exfat_bad_checksum.img")
    clear_image(exfat_bad_checksum, 8 * 1024 * 1024)
    exfat_create(exfat_bad_checksum, 0, 8 * 1024 * 1024)
    with open(exfat_bad_checksum, "r+b") as f:
        f.seek(11 * SECTOR_SIZE)
        f.write(b"\x00")
        f.seek(23 * SECTOR_SIZE)
        f.write(b"\x00")
    print("  exFAT with bad boot checksum")

    exfat_backup = os.path.join(out_dir, "superfloppy_exfat_backup.img")
    shutil.copyfile(sf, exfat_backup)
    with open(exfat_backup, "r+b") as f:
        f.seek(11 * SECTOR_SIZE)
        f.write(b"\x00")
    print("  exFAT recovered from backup boot region")

    exfat_backup_bpb = os.path.join(
        out_dir, "superfloppy_exfat_backup_bpb.img")
    shutil.copyfile(sf, exfat_backup_bpb)
    with open(exfat_backup_bpb, "r+b") as f:
        # Keep the main checksum valid but make its OEM identity invalid.
        f.seek(3)
        f.write(b"BROKEN  ")
        f.seek(0)
        main = bytearray(f.read(11 * SECTOR_SIZE))
        checksum = exfat_boot_checksum(main)
        f.seek(11 * SECTOR_SIZE)
        f.write(struct.pack("<I", checksum) * 128)
    print("  exFAT recovered from backup after invalid main BPB")

    exfat_two_fats = os.path.join(out_dir, "superfloppy_exfat_two_fats.img")
    shutil.copyfile(sf, exfat_two_fats)
    with open(exfat_two_fats, "r+b") as f:
        f.seek(110)
        f.write(b"\x02")
    refresh_exfat_boot_checksum(exfat_two_fats)
    print("  exFAT with unsupported TexFAT layout")

    exfat_small_fat = os.path.join(out_dir, "superfloppy_exfat_small_fat.img")
    shutil.copyfile(sf, exfat_small_fat)
    with open(exfat_small_fat, "r+b") as f:
        f.seek(92)
        f.write(struct.pack("<I", 200))
    refresh_exfat_boot_checksum(exfat_small_fat)
    print("  exFAT with undersized FAT")

    if not args.skip_fat:
        fat16_source = os.path.join(out_dir, "superfloppy_fat16.img")
        fat_small = os.path.join(out_dir, "superfloppy_fat16_small_fat.img")
        shutil.copyfile(fat16_source, fat_small)
        with open(fat_small, "r+b") as f:
            f.seek(22)
            f.write(struct.pack("<H", 1))
        print("  FAT16 with undersized FAT")

        fat_wrap = os.path.join(out_dir, "superfloppy_fat16_bad_layout.img")
        shutil.copyfile(fat16_source, fat_wrap)
        with open(fat_wrap, "r+b") as f:
            f.seek(22)
            f.write(struct.pack("<H", 0xFFFF))
        print("  FAT16 with out-of-range layout")

        malformed_ee = os.path.join(out_dir, "mbr_malformed_ee.img")
        clear_image(malformed_ee, 16 * 1024 * 1024)
        fat_create(malformed_ee, 2048, 8 * 1024 * 1024, 12)
        fat_add_file(malformed_ee, 2048, "hello.txt", b"Malformed EE fallback")
        write_mbr(malformed_ee, [
            {"type": 0xEE, "start": 1, "size": 0},
            {"type": 0x01, "start": 2048, "size": 8 * 1024 * 1024 // 512},
        ])
        print("  malformed protective entry with valid MBR partition")

    print("Done.")


if __name__ == "__main__":
    main()
