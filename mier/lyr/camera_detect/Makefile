# STM32MP257 radar_fusion core project
#
# Targets:
#   make / make radar-fusion  - Build radar_fusion, HUD, and check Dashboard
#   make deploy-radar         - Deploy the complete runtime set to the board
#   make clean                - Remove local build artifacts

TARGET       = radar_fusion
BOARD_IP     = 192.168.88.10
BOARD_DIR    = /xxl/camera_detect
HUD_REMOTE   = /home/root/hud

# Cross compiler. Override on the board with: make CC=gcc CXX=g++
CC  = aarch64-linux-gnu-gcc
CXX = aarch64-linux-gnu-g++

STAI_MPU_DIR = ./stai_mpu
STAI_MPU_INC = $(STAI_MPU_DIR)/include
STAI_MPU_LIB = $(STAI_MPU_DIR)
HUD_DIR      = ./hud_project

CFLAGS   = -Wall -O2 -I. -I$(STAI_MPU_INC) -I$(HUD_DIR)
CXXFLAGS = -Wall -O2 -std=c++17 -I. -I$(STAI_MPU_INC) -I$(HUD_DIR)
LDFLAGS  = -L$(STAI_MPU_LIB) -lstai_mpu -ljpeg -lstdc++ -lm -lpthread -ldl

COMMON_OBJS = camera.o npu_detect.o nav_tts.o cJSON.o oled.o

MODEL_DIR  = ./models
MODEL_FILE = $(MODEL_DIR)/ssd_mobilenet_v2_fpnlite_10_256_int8_per_tensor.nb
LABEL_FILE = $(MODEL_DIR)/labels_coco_dataset_80.txt

.PHONY: all radar-fusion hud dashboard-check clean deploy deploy-radar run run-radar

all: radar-fusion hud dashboard-check

radar-fusion: $(TARGET)

hud:
	$(MAKE) -C $(HUD_DIR) CC=$(CC)

dashboard-check:
	python3 -m py_compile dashboard/radar_dashboard.py

cJSON.o: $(HUD_DIR)/cJSON.c
	$(CC) $(CFLAGS) -c -o $@ $<

oled.o: $(HUD_DIR)/oled.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET): $(COMMON_OBJS) radar_fusion.o
	$(CXX) -o $@ $^ $(LDFLAGS)
	file $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

deploy: deploy-radar

deploy-radar: $(TARGET) hud dashboard-check
	@echo "Deploying $(TARGET), HUD, Dashboard, models and local audio..."
	ssh root@$(BOARD_IP) 'mkdir -p $(BOARD_DIR)/models $(BOARD_DIR)/scripts $(BOARD_DIR)/sounds $(BOARD_DIR)/nav_tts_cache $(BOARD_DIR)/dashboard/static /home/root'
	scp $(TARGET) root@$(BOARD_IP):$(BOARD_DIR)/
	scp $(HUD_DIR)/hud root@$(BOARD_IP):$(HUD_REMOTE)
	scp $(MODEL_FILE) $(LABEL_FILE) root@$(BOARD_IP):$(BOARD_DIR)/models/
	scp start_dvr.sh dvr.service.example dvr-m33.service.example radar_config.example root@$(BOARD_IP):$(BOARD_DIR)/
	scp scripts/*.sh root@$(BOARD_IP):$(BOARD_DIR)/scripts/
	scp sounds/*.wav root@$(BOARD_IP):$(BOARD_DIR)/sounds/
	scp nav_tts_cache/*.wav root@$(BOARD_IP):$(BOARD_DIR)/nav_tts_cache/
	scp dashboard/radar_dashboard.py root@$(BOARD_IP):$(BOARD_DIR)/dashboard/
	scp dashboard/static/index.html dashboard/static/styles.css dashboard/static/app.js root@$(BOARD_IP):$(BOARD_DIR)/dashboard/static/
	ssh root@$(BOARD_IP) 'test -f $(BOARD_DIR)/radar_config || cp $(BOARD_DIR)/radar_config.example $(BOARD_DIR)/radar_config; chmod +x $(BOARD_DIR)/$(TARGET) $(BOARD_DIR)/start_dvr.sh $(BOARD_DIR)/scripts/*.sh $(BOARD_DIR)/dashboard/radar_dashboard.py $(HUD_REMOTE)'
	@echo "Deploy complete."

run: run-radar

run-radar:
	ssh -t root@$(BOARD_IP) 'cd $(BOARD_DIR) && LD_LIBRARY_PATH=/usr/lib:/vendor/lib ./$(TARGET)'

clean:
	rm -f $(TARGET) *.o
	$(MAKE) -C $(HUD_DIR) clean
