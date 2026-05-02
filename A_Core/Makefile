CC      ?= gcc
CFLAGS  += -Wall -Wextra -O2 -g
LDFLAGS += -lpthread -lrt

SRCDIR  = ..
OBJDIR  = build
TARGET  = dvr

SRCS  = $(SRCDIR)/A_Core/dvr_main.c
SRCS += $(SRCDIR)/common/ring_buffer.c
SRCS += $(SRCDIR)/camera/camera_v4l2.c
SRCS += $(SRCDIR)/display/display_lcd.c
SRCS += $(SRCDIR)/ipc/trigger_receiver.c
SRCS += $(SRCDIR)/ipc/rpmsg_channel.c
SRCS += $(SRCDIR)/recorder/dvr_engine.c

OBJS  = $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(SRCS))
DEPS  = $(OBJS:.o=.d)

INC   = -I$(SRCDIR)/common
INC  += -I$(SRCDIR)/camera
INC  += -I$(SRCDIR)/display
INC  += -I$(SRCDIR)/ipc
INC  += -I$(SRCDIR)/recorder

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build: $(TARGET)"

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) -MMD -c $< -o $@

-include $(DEPS)

clean:
	rm -rf $(OBJDIR) $(TARGET)
