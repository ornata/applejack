CC       := clang
CFLAGS   := -std=c11 -Wall -Wextra -Wpedantic $(shell pkg-config --cflags libusb-1.0)
LDFLAGS  := -framework IOKit -framework CoreFoundation -framework ApplicationServices $(shell pkg-config --libs libusb-1.0)

CONFIG ?= debug

ifeq ($(CONFIG),debug)
CFLAGS += -O0 -g
else ifeq ($(CONFIG),release)
CFLAGS += -O2 -DNDEBUG
else ifeq ($(CONFIG),san)
CFLAGS += -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
LDFLAGS += -fsanitize=address,undefined
else
$(error unknown CONFIG '$(CONFIG)': use debug, release, or san)
endif

BUILD := build/$(CONFIG)
TARGET := $(BUILD)/hidprobe

.PHONY: all debug release san clean

all: $(TARGET)

debug release san:
	$(MAKE) CONFIG=$@

$(TARGET): main.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf build
