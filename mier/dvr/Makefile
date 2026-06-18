# Makefile for USB Camera DVR (Dashcam Video Recorder) + NPU Fusion
#
# 使用方法：
#   make                           - VM编译 (无NPU, 无libjpeg)
#   make HAS_LIBJPEG=1             - 含libjpeg的MJPEG解码
#   make CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ - 交叉编译
#   make board                     - 开发板编译 (含NPU+libjpeg, 需stai_mpu库)
#   make clean                     - 清理编译产物
#   make deploy                    - 部署到开发板 /xxl/dvr/
#
# 目标板: myd-ld25x (aarch64, OpenSTLinux)
# 开发板IP: 192.168.88.10

SRCDIR   := src
TARGET   := dvr

# C 源文件
C_SRCS   := $(SRCDIR)/dvr_main.c $(SRCDIR)/dvr_engine.c $(SRCDIR)/usb_camera.c \
            $(SRCDIR)/ring_buffer.c $(SRCDIR)/trigger_receiver.c \
            $(SRCDIR)/frame_decoder.c
C_OBJS   := $(C_SRCS:.c=.o)

# C++ 源文件 (NPU相关)
CXX_SRCS := $(SRCDIR)/npu_fusion.cpp $(SRCDIR)/npu_detector.cpp
CXX_OBJS := $(CXX_SRCS:.cpp=.o)

# 工具链
CC       ?= gcc
CXX      ?= g++
CFLAGS   := -Wall -Wextra -O2 -g -D_GNU_SOURCE
CXXFLAGS := -Wall -Wextra -O2 -std=c++14 -D_GNU_SOURCE
LDFLAGS  := -lpthread

# libjpeg支持 (开发板需要)
ifdef HAS_LIBJPEG
  CFLAGS   += -DHAS_LIBJPEG
  CXXFLAGS += -DHAS_LIBJPEG
  LDFLAGS  += -ljpeg
endif

# stai_mpu NPU库 (开发板)
STAI_MPU_CFLAGS  := $(shell pkg-config --cflags stai_mpu 2>/dev/null)
STAI_MPU_LDFLAGS := $(shell pkg-config --libs stai_mpu 2>/dev/null)

# 开发板信息
BOARD_IP    ?= 192.168.88.10
BOARD_USER  ?= root
BOARD_DIR   ?= /xxl/dvr
LOCAL_DIR   ?= /home/alientek/dvr_project/mier/dvr
BOARD_VIDEO_DIR ?= /run/media/mmcblk0p1/dvr

.PHONY: all board clean distclean deploy help

# VM默认编译: 无NPU
all: CFLAGS  += -DNO_NPU_SUPPORT
all: CXXFLAGS += -DNO_NPU_SUPPORT
all: $(TARGET)

# 开发板编译: 含NPU
board: CFLAGS  += $(STAI_MPU_CFLAGS)
board: CXXFLAGS += $(STAI_MPU_CFLAGS) -std=c++14
board: LDFLAGS += $(STAI_MPU_LDFLAGS)
board: $(TARGET)

$(TARGET): $(C_OBJS) $(CXX_OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)
	@echo "Build done: $(TARGET)"
	@file $(TARGET)

# C 编译规则
$(SRCDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/dvr_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

# C++ 编译规则
$(SRCDIR)/%.o: $(SRCDIR)/%.cpp $(SRCDIR)/dvr_types.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# 显式依赖
$(SRCDIR)/dvr_main.o: $(SRCDIR)/dvr_main.c $(SRCDIR)/dvr_engine.h $(SRCDIR)/dvr_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/dvr_engine.o: $(SRCDIR)/dvr_engine.c $(SRCDIR)/dvr_engine.h $(SRCDIR)/usb_camera.h \
                        $(SRCDIR)/ring_buffer.h $(SRCDIR)/trigger_receiver.h $(SRCDIR)/dvr_types.h \
                        $(SRCDIR)/frame_decoder.h $(SRCDIR)/npu_fusion.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/usb_camera.o: $(SRCDIR)/usb_camera.c $(SRCDIR)/usb_camera.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/ring_buffer.o: $(SRCDIR)/ring_buffer.c $(SRCDIR)/ring_buffer.h $(SRCDIR)/dvr_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/trigger_receiver.o: $(SRCDIR)/trigger_receiver.c $(SRCDIR)/trigger_receiver.h $(SRCDIR)/dvr_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/frame_decoder.o: $(SRCDIR)/frame_decoder.c $(SRCDIR)/frame_decoder.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/npu_fusion.o: $(SRCDIR)/npu_fusion.cpp $(SRCDIR)/npu_fusion.h $(SRCDIR)/npu_detector.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(SRCDIR)/npu_detector.o: $(SRCDIR)/npu_detector.cpp $(SRCDIR)/npu_detector.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(SRCDIR)/*.o

distclean: clean
	rm -f /tmp/emergency_*.mp4 /tmp/dvr_buffer.bin /tmp/dvr_tmp.mjpg /tmp/ffmpeg_stderr.log /tmp/dvr.log
	rm -f /usr/local/dvr/emergency_*.mp4 /usr/local/dvr/dvr_buffer.bin /usr/local/dvr/dvr_tmp.mjpg /usr/local/dvr/dvr.log
	rm -f /run/media/mmcblk0p1/dvr/emergency_*.mp4 /run/media/mmcblk0p1/dvr/dvr_buffer.bin \
	      /run/media/mmcblk0p1/dvr/dvr_tmp.mjpg /run/media/mmcblk0p1/dvr/dvr.log

deploy: $(TARGET)
	@echo "Deploying to $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)/"
	ssh $(BOARD_USER)@$(BOARD_IP) "mkdir -p $(BOARD_DIR)"
	scp $(TARGET) scripts/test_dvr.sh $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)/
	ssh $(BOARD_USER)@$(BOARD_IP) "chmod +x $(BOARD_DIR)/$(TARGET) $(BOARD_DIR)/test_dvr.sh"
	@echo "Deploy done."

fetch-video:
	@echo "Fetching videos from $(BOARD_USER)@$(BOARD_IP):$(BOARD_VIDEO_DIR)/"
	scp $(BOARD_USER)@$(BOARD_IP):$(BOARD_VIDEO_DIR)/emergency_*.mp4 $(LOCAL_DIR)/output/ 2>/dev/null || echo "No videos found"
	scp $(BOARD_USER)@$(BOARD_IP):$(BOARD_VIDEO_DIR)/dvr.log $(LOCAL_DIR)/output/ 2>/dev/null || echo "No log found"
	@echo "Fetch done."

help:
	@echo "Targets:"
	@echo "  all          - Build DVR (VM, no NPU)"
	@echo "  board        - Build DVR (board, with NPU)"
	@echo "  clean        - Remove object files and binary"
	@echo "  distclean    - Also clean temp output files"
	@echo "  deploy       - Deploy to board ($(BOARD_IP):$(BOARD_DIR)/)"
	@echo "  fetch-video  - Fetch recorded videos + log from board"
	@echo ""
	@echo "Cross-compile:"
	@echo "  make CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++"
	@echo "  make CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++ HAS_LIBJPEG=1"
	@echo ""
	@echo "Run on board:"
	@echo "  cd $(BOARD_DIR) && ./dvr -d /dev/video6 -s /run/media/mmcblk0p1/dvr \\"
	@echo "    --npu-model /usr/local/share/npu/xxx.nb \\"
	@echo "    --npu-labels /usr/local/share/npu/labels.txt"