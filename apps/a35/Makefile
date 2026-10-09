# STM32MP257 radar_fusion core project
#
# Targets:
#   make / make radar-fusion  - Build radar_fusion, HUD, and check Dashboard
#   make deploy-radar         - Deploy the complete runtime set to the board
#   make deploy-runtime       - Deploy, install current units, and restart A35
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
HUD_DIR      = ./hud

CPPFLAGS = -DCJSON_NESTING_LIMIT=32 -Iinclude -Iinclude/camera -Iinclude/vision -Iinclude/output \
           -Iinclude/navigation -isystem $(STAI_MPU_INC) -I$(HUD_DIR)
CFLAGS   = -Wall -Wextra -Wpedantic -O2 -std=c11 -D_DEFAULT_SOURCE -MMD -MP
CXXFLAGS = -Wall -Wextra -Wpedantic -O2 -std=c++17 -MMD -MP
LDFLAGS  = -L$(STAI_MPU_LIB) -Wl,-rpath,'$$ORIGIN/stai_mpu'
LDLIBS   = -lstai_mpu -ljpeg -lm -lpthread -ldl
HOST_CXX ?= c++
HOST_CC ?= cc
C_SOURCES := $(wildcard src/*/*.c) $(HUD_DIR)/cJSON.c $(HUD_DIR)/oled.c
CXX_SOURCES := $(wildcard src/*/*.cpp)
OBJECTS := $(addprefix build/,$(C_SOURCES:.c=.o) $(CXX_SOURCES:.cpp=.o))

MODEL_DIR  = ./models
MODEL_FILE = $(MODEL_DIR)/ssd_mobilenet_v2_fpnlite_10_256_int8_per_tensor.nb
LABEL_FILE = $(MODEL_DIR)/labels_coco_dataset_80.txt

.PHONY: all radar-fusion hud dashboard-check script-check ota-check ota-package \
	ota-test ota-clean deploy deploy-radar deploy-runtime deploy-ota-service \
	run run-radar clean

all: radar-fusion hud dashboard-check script-check

radar-fusion: $(TARGET)

hud:
	$(MAKE) -C $(HUD_DIR) CC=$(CC)

dashboard-check:
	python3 -m py_compile dashboard/radar_dashboard.py
	python3 -m py_compile scripts/dvr_encode_worker.py
	python3 -m unittest discover -s dashboard -p 'test_*.py'
	@if command -v gjs >/dev/null 2>&1; then \
		gjs -c 'const GLib=imports.gi.GLib; const ByteArray=imports.byteArray; const [ok,data]=GLib.file_get_contents("dashboard/static/app.js"); new Function(ByteArray.toString(data));'; \
	fi

script-check:
	bash -n start_dvr.sh
	sh -n scripts/start_m33_early.sh
	sh -n scripts/log_maintenance.sh
	sh -n scripts/collect_boot_metrics.sh
	sh -n scripts/collect_bootloader_audit.sh
	sh -n scripts/clear_runtime_logs.sh
	sh -n scripts/ble_led_test.sh
	sh -n scripts/verify_dvr_videos.sh
	sh -n scripts/tf_card_control.sh
	sh -n scripts/project_safe_stop.sh
	sh -n $(OTA_DIR)/install_ota_service.sh
	sh -n $(OTA_DIR)/helmet_ota_recover_if_needed.sh

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

$(TARGET): $(OBJECTS)
	$(CXX) -o $@ $^ $(LDFLAGS) $(LDLIBS)
	file $(TARGET)

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

-include $(OBJECTS:.o=.d)

.PHONY: core-check runtime-check lifecycle-check json-check oled-check encoder-check object-check
core-check:
	@mkdir -p build/tests
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pthread -Iinclude \
		tests/test_frame_pipeline.cpp -o build/tests/test_frame_pipeline
	./build/tests/test_frame_pipeline

runtime-check:
	@mkdir -p build/tests
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pthread -Iinclude \
		tests/test_runtime.cpp src/runtime/video_config.cpp src/vision/ssd_postprocess.cpp \
		-o build/tests/test_runtime
	./build/tests/test_runtime

lifecycle-check:
	@mkdir -p build/tests
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -pthread -Iinclude \
		tests/test_video_lifecycle.cpp src/runtime/video_pipeline.cpp src/runtime/video_config.cpp \
		-o build/tests/test_video_lifecycle
	./build/tests/test_video_lifecycle

json-check:
	@mkdir -p build/tests
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -DCJSON_NESTING_LIMIT=32 -Ihud \
		-c hud/cJSON.c -o build/tests/cJSON.o
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Ihud \
		-c hud/imu_message.c -o build/tests/imu_message.o
	$(HOST_CXX) -std=c++17 -Wall -Wextra -Werror -Ihud tests/test_json.cpp \
		build/tests/cJSON.o build/tests/imu_message.o -o build/tests/test_json
	./build/tests/test_json

# Compile all source objects without linking the architecture-specific STAI runtime.
object-check: $(OBJECTS)

oled-check:
	@mkdir -p build/tests
	$(HOST_CC) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Ihud \
		tests/test_oled.c hud/oled.c -Wl,--wrap=open,--wrap=ioctl,--wrap=write,--wrap=close \
		-o build/tests/test_oled
	./build/tests/test_oled

encoder-check:
	python3 -m unittest discover -s tests -p 'test_*.py'

deploy: deploy-radar

deploy-radar: $(TARGET) hud dashboard-check script-check
	@echo "Deploying $(TARGET), HUD, Dashboard, models and local audio..."
	ssh root@$(BOARD_IP) 'mkdir -p $(BOARD_DIR)/models $(BOARD_DIR)/scripts $(BOARD_DIR)/sounds $(BOARD_DIR)/nav_tts_cache $(BOARD_DIR)/dashboard/static $(BOARD_DIR)/stai_mpu'
	scp $(TARGET) root@$(BOARD_IP):$(BOARD_DIR)/
	scp $(HUD_DIR)/hud root@$(BOARD_IP):$(HUD_REMOTE)
	scp $(STAI_MPU_DIR)/*.so* root@$(BOARD_IP):$(BOARD_DIR)/stai_mpu/
	scp $(MODEL_FILE) $(LABEL_FILE) root@$(BOARD_IP):$(BOARD_DIR)/models/
	scp start_dvr.sh dvr.service.example dvr-m33.service.example \
		radar-dashboard.service.example \
		dnsmasq.service.override.example radar_config.example root@$(BOARD_IP):$(BOARD_DIR)/
	scp scripts/*.sh scripts/*.py root@$(BOARD_IP):$(BOARD_DIR)/scripts/
	scp sounds/*.wav root@$(BOARD_IP):$(BOARD_DIR)/sounds/
	scp nav_tts_cache/*.wav root@$(BOARD_IP):$(BOARD_DIR)/nav_tts_cache/
	scp dashboard/radar_dashboard.py root@$(BOARD_IP):$(BOARD_DIR)/dashboard/
	scp dashboard/static/index.html dashboard/static/styles.css dashboard/static/app.js root@$(BOARD_IP):$(BOARD_DIR)/dashboard/static/
	ssh root@$(BOARD_IP) 'test -f $(BOARD_DIR)/radar_config || cp $(BOARD_DIR)/radar_config.example $(BOARD_DIR)/radar_config; chmod +x $(BOARD_DIR)/$(TARGET) $(BOARD_DIR)/start_dvr.sh $(BOARD_DIR)/scripts/*.sh $(BOARD_DIR)/scripts/*.py $(BOARD_DIR)/dashboard/radar_dashboard.py $(HUD_REMOTE)'
	@echo "Deploy complete."

# 完整更新运行态：部署文件、安装最新unit参数并重启A35业务。
# 不重启M33，不覆盖现场radar_config。
deploy-runtime: deploy-radar
	ssh root@$(BOARD_IP) 'cp $(BOARD_DIR)/dvr.service.example /etc/systemd/system/dvr.service && cp $(BOARD_DIR)/dvr-m33.service.example /etc/systemd/system/dvr-m33.service && cp $(BOARD_DIR)/radar-dashboard.service.example /etc/systemd/system/radar-dashboard.service && systemctl daemon-reload && systemctl enable dvr-m33.service radar-dashboard.service dvr.service && systemctl restart radar-dashboard.service dvr.service'
	@echo "Runtime services updated and restarted."

deploy-ota-service: ota-check
	ssh root@$(BOARD_IP) 'mkdir -p $(OTA_REMOTE)'
	scp \
		$(OTA_DIR)/helmet_ota_common.py \
		$(OTA_DIR)/helmet_ota_installer.py \
		$(OTA_DIR)/helmet_ota_server.py \
		$(OTA_DIR)/helmet_ota_recover_if_needed.sh \
		$(OTA_DIR)/helmet-ota.service.example \
		$(OTA_DIR)/install_ota_service.sh \
		root@$(BOARD_IP):$(OTA_REMOTE)/
	ssh root@$(BOARD_IP) 'chmod +x $(OTA_REMOTE)/install_ota_service.sh && $(OTA_REMOTE)/install_ota_service.sh'

run: run-radar

run-radar:
	ssh -t root@$(BOARD_IP) 'cd $(BOARD_DIR) && LD_LIBRARY_PATH=/usr/lib:/vendor/lib:$(BOARD_DIR)/stai_mpu ./$(TARGET)'

clean:
	rm -f $(TARGET) $(OBJECTS) $(OBJECTS:.o=.d) build/tests/test_frame_pipeline build/tests/test_runtime \
		build/tests/test_json build/tests/cJSON.o build/tests/imu_message.o
	rm -f build/tests/test_video_lifecycle build/tests/test_oled
	$(MAKE) -C $(HUD_DIR) clean
