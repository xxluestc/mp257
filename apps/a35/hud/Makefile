# 如需交叉编译，请设置CC环境变量，例如：
# export CC=arm-ostl-linux-gnueabi-gcc
CC ?= gcc
CFLAGS = -Wall -Wextra -Wpedantic -O2 -std=c11 -D_DEFAULT_SOURCE -DCJSON_NESTING_LIMIT=32 -MMD -MP -I.
LDFLAGS = -lm
OBJS = main.o udp.o imu_message.o cJSON.o
TARGET = hud
all: $(TARGET)
$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
-include $(OBJS:.o=.d)
clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET)
.PHONY: all clean
