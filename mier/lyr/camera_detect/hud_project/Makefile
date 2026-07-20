# 如需交叉编译，请设置CC环境变量，例如：
# export CC=arm-ostl-linux-gnueabi-gcc
CC ?= gcc
CFLAGS = -Wall -O2 -I.
LDFLAGS = -lm
OBJS = main.o udp.o oled.o cJSON.o
TARGET = hud
all: $(TARGET)
$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
%.o: %.c common.h
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	rm -f $(OBJS) $(TARGET)
.PHONY: all clean
