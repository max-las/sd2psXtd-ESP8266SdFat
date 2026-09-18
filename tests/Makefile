# Host test harness for ESP8266SdFat auto-mount implementation.
# Builds the subset of SdFat that is required to mount/examine FAT/exFAT
# volumes using the HOST_MOCK DedicatedSpiCard.

SDFAT_DIR = ../src
ATTIC_DIR = ../extras/attic

CXX = g++
BLOCK_DEVICE_INTERFACE ?= 0
CXXFLAGS = -std=c++11 -Wall -Wextra -O0 -g \
    -DHOST_MOCK \
    -DHOST_MOCK_READ_FAILURE \
    -DHAS_SDIO_CLASS=0 \
    -DUSE_BLOCK_DEVICE_INTERFACE=$(BLOCK_DEVICE_INTERFACE) \
    -DUSE_SD_CRC=0 \
    -DENABLE_DEDICATED_SPI=1 \
    -DSPI_DRIVER_SELECT=3 \
    -DSDFAT_FILE_TYPE=3 \
    -DUSE_LONG_FILE_NAMES=1 \
    -DUSE_LFN_HASH=1 \
    -DFAT12_SUPPORT=1 \
    -DENABLE_ARDUINO_FEATURES=0 \
    -DENABLE_ARDUINO_SERIAL=0 \
    -DENABLE_ARDUINO_STRING=0 \
    -I$(SDFAT_DIR) \
    -I$(ATTIC_DIR)

LIB_SRC = \
    $(SDFAT_DIR)/common/FmtNumber.cpp \
    $(SDFAT_DIR)/common/FsCache.cpp \
    $(SDFAT_DIR)/common/FsDateTime.cpp \
    $(SDFAT_DIR)/common/FsName.cpp \
    $(SDFAT_DIR)/common/FsStructs.cpp \
    $(SDFAT_DIR)/common/GptPartition.cpp \
    $(SDFAT_DIR)/common/VolumeLocator.cpp \
    $(SDFAT_DIR)/common/FsUtf.cpp \
    $(SDFAT_DIR)/common/upcase.cpp \
    $(SDFAT_DIR)/ExFatLib/ExFatDbg.cpp \
    $(SDFAT_DIR)/ExFatLib/ExFatFile.cpp \
    $(SDFAT_DIR)/ExFatLib/ExFatFilePrint.cpp \
    $(SDFAT_DIR)/ExFatLib/ExFatFileWrite.cpp \
    $(SDFAT_DIR)/ExFatLib/ExFatFormatter.cpp \
    $(SDFAT_DIR)/ExFatLib/ExFatName.cpp \
    $(SDFAT_DIR)/ExFatLib/ExFatPartition.cpp \
    $(SDFAT_DIR)/ExFatLib/ExFatVolume.cpp \
    $(SDFAT_DIR)/FatLib/FatDbg.cpp \
    $(SDFAT_DIR)/FatLib/FatFile.cpp \
    $(SDFAT_DIR)/FatLib/FatFileLFN.cpp \
    $(SDFAT_DIR)/FatLib/FatFilePrint.cpp \
    $(SDFAT_DIR)/FatLib/FatFileSFN.cpp \
    $(SDFAT_DIR)/FatLib/FatFormatter.cpp \
    $(SDFAT_DIR)/FatLib/FatName.cpp \
    $(SDFAT_DIR)/FatLib/FatPartition.cpp \
    $(SDFAT_DIR)/FatLib/FatVolume.cpp \
    $(SDFAT_DIR)/FreeStack.cpp \
    $(SDFAT_DIR)/FsLib/FsFile.cpp \
    $(SDFAT_DIR)/FsLib/FsNew.cpp \
    $(SDFAT_DIR)/FsLib/FsVolume.cpp \
    $(SDFAT_DIR)/MinimumSerial.cpp \
    $(SDFAT_DIR)/SdCard/SdCardInfo.cpp \
    $(ATTIC_DIR)/PrintBasic.cpp

LIB_OBJ = $(LIB_SRC:.cpp=.o)

TARGET = host_test

.PHONY: all clean generate run

all: $(TARGET)

generate:
	./venv/bin/python generate_images.py --out-dir images

$(TARGET): $(LIB_OBJ) host_test.o
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# The host_test.cpp source is in this directory.
host_test.o: host_test.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(LIB_OBJ) $(LIB_SRC:.cpp=.d) host_test.o host_test.d $(TARGET)

-include $(LIB_SRC:.cpp=.d)
-include host_test.d
