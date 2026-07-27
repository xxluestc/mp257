# STM32MP257 radar_fusion core project
#
# Targets:
#   make / make radar-fusion  - Build radar_fusion, HUD, and check Dashboard
#   make deploy-radar         - Deploy the complete runtime set to the board
#   make ota-package VERSION=1.0.1 - Build a versioned runtime-only OTA package
#   make deploy-ota-service   - Install/update the independent board OTA service
#   make clean                - Remove local build artifacts

TARGET       = radar_fusion
BOARD_IP     = 192.168.88.10
BOARD_DIR    = /xxl/camera_detect
HUD_REMOTE   = $(BOARD_DIR)/hud
DIST_DIR     = dist
OTA_DIR      = ota
OTA_REMOTE   = /tmp/helmet-ota-deploy

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

.PHONY: all radar-fusion hud dashboard-check script-check ota-check ota-package \
	ota-test ota-clean deploy deploy-radar deploy-ota-service run run-radar clean

all: radar-fusion hud dashboard-check script-check

radar-fusion: $(TARGET)

hud:
	$(MAKE) -C $(HUD_DIR) CC=$(CC)

dashboard-check:
	python3 -m py_compile dashboard/radar_dashboard.py

script-check:
	bash -n start_dvr.sh
	sh -n scripts/start_m33_early.sh
	sh -n scripts/log_maintenance.sh
	sh -n scripts/clear_runtime_logs.sh
	sh -n $(OTA_DIR)/install_ota_service.sh

ota-check:
	python3 -m py_compile \
		$(OTA_DIR)/helmet_ota_common.py \
		$(OTA_DIR)/helmet_ota_installer.py \
		$(OTA_DIR)/helmet_ota_server.py \
		$(OTA_DIR)/build_ota_package.py \
		$(OTA_DIR)/test_ota.py

ota-package: all ota-check
	@test -n "$(VERSION)" || { echo "VERSION is required, e.g. make ota-package VERSION=1.0.1" >&2; exit 2; }
	python3 $(OTA_DIR)/build_ota_package.py --version "$(VERSION)" --dist "$(DIST_DIR)"

ota-test: ota-check
	python3 $(OTA_DIR)/test_ota.py --dist "$(DIST_DIR)"

ota-clean:
	rm -rf "$(DIST_DIR)"

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
	ssh root@$(BOARD_IP) 'mkdir -p $(BOARD_DIR)/models $(BOARD_DIR)/scripts $(BOARD_DIR)/sounds $(BOARD_DIR)/nav_tts_cache $(BOARD_DIR)/dashboard/static'
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

deploy-ota-service: ota-check
	ssh root@$(BOARD_IP) 'mkdir -p $(OTA_REMOTE)'
	scp \
		$(OTA_DIR)/helmet_ota_common.py \
		$(OTA_DIR)/helmet_ota_installer.py \
		$(OTA_DIR)/helmet_ota_server.py \
		$(OTA_DIR)/helmet-ota.service.example \
		$(OTA_DIR)/install_ota_service.sh \
		root@$(BOARD_IP):$(OTA_REMOTE)/
	ssh root@$(BOARD_IP) 'chmod +x $(OTA_REMOTE)/install_ota_service.sh && $(OTA_REMOTE)/install_ota_service.sh'

run: run-radar

run-radar:
	ssh -t root@$(BOARD_IP) 'cd $(BOARD_DIR) && LD_LIBRARY_PATH=/usr/lib:/vendor/lib ./$(TARGET)'

clean:
	rm -f $(TARGET) *.o
	$(MAKE) -C $(HUD_DIR) clean
