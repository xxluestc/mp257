# 雷达 + DVR 联调 Makefile
# 支持 VM 编译和 aarch64 交叉编译
#
# 架构说明:
#   radar_link: 纯雷达程序，通过UART读取雷达数据，发送事件到DVR命名管道
#   DVR (dvr/): 行车记录仪，接收管道事件，可选NPU融合验证
#   NPU融合: 已集成到DVR进程中，避免摄像头冲突

# 交叉编译器
CROSS_COMPILE ?= aarch64-none-linux-gnu-
CROSS_GCC := $(shell which $(CROSS_COMPILE)gcc 2>/dev/null)
ifneq ($(CROSS_GCC),)
  CC  = $(CROSS_COMPILE)gcc
else
  CC  = gcc
endif

# 开发板IP
BOARD_IP ?= 192.168.88.10

# 编译标志
CFLAGS   = -Wall -O2 -D_GNU_SOURCE
LDFLAGS  = -lpthread

# 目标
RADAR_SRC = radar_link.c
RADAR_TGT = radar_link

.PHONY: all clean deploy help

all: $(RADAR_TGT)

$(RADAR_TGT): $(RADAR_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(RADAR_TGT) *.o

deploy: $(RADAR_TGT)
	scp $(RADAR_TGT) root@$(BOARD_IP):/xxl/pro/
	scp start_pro.sh root@$(BOARD_IP):/xxl/pro/
	ssh root@$(BOARD_IP) 'chmod +x /xxl/pro/$(RADAR_TGT) /xxl/pro/start_pro.sh'

help:
	@echo "=== 编译目标 ==="
	@echo "  make              - 编译雷达程序 radar_link"
	@echo "  make clean        - 清理"
	@echo "  make deploy       - 部署 radar_link 到开发板"
	@echo ""
	@echo "注意: NPU融合已集成到DVR中，请到 dvr/ 目录编译DVR"