# Makefile for camera_detect / fusion_detect / radar_fusion
#
# Targets:
#   make                - Build camera_detect (standalone detection)
#   make fusion         - Build fusion_detect (pipe-based radar + camera)
#   make radar-fusion   - Build radar_fusion (single-process radar + camera + NPU)
#   make deploy-XXX     - Deploy to board
#   make clean          - Clean all

TARGET         = camera_detect
TARGET_FUSION  = fusion_detect
TARGET_RADAR   = radar_fusion
BOARD_IP       = 192.168.88.10
BOARD_DIR      = /xxl/camera_detect

# --- Cross-compile toolchain ---
# 强制使用交叉编译器；如要在开发板本地编译，可覆盖: make CC=gcc CXX=g++
CC  = aarch64-linux-gnu-gcc
CXX = aarch64-linux-gnu-g++

# --- stai_mpu library paths ---
STAI_MPU_DIR = ./stai_mpu
STAI_MPU_INC = $(STAI_MPU_DIR)/include
STAI_MPU_LIB = $(STAI_MPU_DIR)

# --- HUD project (cJSON) ---
HUD_DIR = ../hud_project

# --- Flags ---
CFLAGS   = -Wall -O2 -I. -I$(STAI_MPU_INC) -I$(HUD_DIR)
CXXFLAGS = -Wall -O2 -std=c++17 -I. -I$(STAI_MPU_INC) -I$(HUD_DIR)
LDFLAGS  = -L$(STAI_MPU_LIB) -lstai_mpu -ljpeg -lstdc++ -lm -lpthread -ldl

# --- Common objects ---
COMMON_OBJS = camera.o npu_detect.o nav_tts.o cJSON.o oled.o

cJSON.o: $(HUD_DIR)/cJSON.c
	$(CC) $(CFLAGS) -c -o $@ $<

oled.o: $(HUD_DIR)/oled.c
	$(CC) $(CFLAGS) -c -o $@ $<

# --- Build ---
.PHONY: all fusion radar-fusion clean deploy deploy-fusion deploy-radar run run-fusion run-radar

all: $(TARGET)

fusion: $(TARGET_FUSION)

radar-fusion: $(TARGET_RADAR)

$(TARGET): $(COMMON_OBJS) jpeg_decoder.o main.o
	$(CXX) -o $@ $^ $(LDFLAGS)
	file $(TARGET)

$(TARGET_FUSION): $(COMMON_OBJS) jpeg_decoder.o fusion_main.o
	$(CXX) -o $@ $^ $(LDFLAGS)
	file $(TARGET_FUSION)

$(TARGET_RADAR): $(COMMON_OBJS) radar_fusion.o
	$(CXX) -o $@ $^ $(LDFLAGS)
	file $(TARGET_RADAR)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# --- Deploy ---
MODEL_DIR  = ./models
MODEL_FILE = $(MODEL_DIR)/ssd_mobilenet_v2_fpnlite_10_256_int8_per_tensor.nb
LABEL_FILE = $(MODEL_DIR)/labels_coco_dataset_80.txt

deploy: $(TARGET)
	@echo "Deploying $(TARGET)..."
	ssh root@$(BOARD_IP) 'mkdir -p $(BOARD_DIR)/models'
	scp $(TARGET) root@$(BOARD_IP):$(BOARD_DIR)/
	scp $(MODEL_FILE) root@$(BOARD_IP):$(BOARD_DIR)/models/
	scp $(LABEL_FILE) root@$(BOARD_IP):$(BOARD_DIR)/models/
	ssh root@$(BOARD_IP) 'chmod +x $(BOARD_DIR)/$(TARGET)'
	@echo "Done."

deploy-fusion: $(TARGET_FUSION)
	@echo "Deploying $(TARGET_FUSION)..."
	ssh root@$(BOARD_IP) 'mkdir -p $(BOARD_DIR)/models'
	scp $(TARGET_FUSION) root@$(BOARD_IP):$(BOARD_DIR)/
	scp $(MODEL_FILE) root@$(BOARD_IP):$(BOARD_DIR)/models/
	scp $(LABEL_FILE) root@$(BOARD_IP):$(BOARD_DIR)/models/
	ssh root@$(BOARD_IP) 'chmod +x $(BOARD_DIR)/$(TARGET_FUSION)'
	@echo "Done."

deploy-radar: $(TARGET_RADAR)
	@echo "Deploying $(TARGET_RADAR)..."
	ssh root@$(BOARD_IP) 'mkdir -p $(BOARD_DIR)/models $(BOARD_DIR)/scripts $(BOARD_DIR)/nav_tts_cache'
	scp $(TARGET_RADAR) root@$(BOARD_IP):$(BOARD_DIR)/
	scp $(MODEL_FILE) root@$(BOARD_IP):$(BOARD_DIR)/models/
	scp $(LABEL_FILE) root@$(BOARD_IP):$(BOARD_DIR)/models/
	scp scripts/gen_nav_tts.sh root@$(BOARD_IP):$(BOARD_DIR)/scripts/
	scp scripts/gen_danger_sounds.sh root@$(BOARD_IP):$(BOARD_DIR)/scripts/
	scp nav_tts_cache/base_*.wav nav_tts_cache/nav_前方*.wav nav_tts_cache/alert_*.wav nav_tts_cache/danger_*.wav root@$(BOARD_IP):$(BOARD_DIR)/nav_tts_cache/ 2>/dev/null || true
	ssh root@$(BOARD_IP) 'chmod +x $(BOARD_DIR)/$(TARGET_RADAR) $(BOARD_DIR)/scripts/gen_nav_tts.sh $(BOARD_DIR)/scripts/gen_danger_sounds.sh'
	@echo "Done."

# --- Run on board ---
run:
	ssh -t root@$(BOARD_IP) 'cd $(BOARD_DIR) && LD_LIBRARY_PATH=/usr/lib:/vendor/lib ./$(TARGET) -n 50'

run-fusion:
	ssh -t root@$(BOARD_IP) 'cd $(BOARD_DIR) && LD_LIBRARY_PATH=/usr/lib:/vendor/lib ./$(TARGET_FUSION)'

run-radar:
	ssh -t root@$(BOARD_IP) 'cd $(BOARD_DIR) && LD_LIBRARY_PATH=/usr/lib:/vendor/lib ./$(TARGET_RADAR)'

# --- Clean ---
clean:
	rm -f $(TARGET) $(TARGET_FUSION) $(TARGET_RADAR) *.o