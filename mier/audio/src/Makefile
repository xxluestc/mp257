CROSS_COMPILE ?= aarch64-linux-gnu-
CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -O2
LDFLAGS = -lasound

TARGET = alsa_player
SRC = alsa_player.c

.PHONY: all clean deploy

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

deploy: $(TARGET)
	scp $(TARGET) root@192.168.88.10:/xxl/audio/bin/
