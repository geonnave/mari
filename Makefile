DOCKER_IMAGE ?= aabadie/dotbot:latest
DOCKER_TARGETS ?= all
PACKAGES_DIR_OPT ?=
SEGGER_DIR ?= /opt/segger
BUILD_CONFIG ?= Debug

.PHONY: all node gateway clean-gateway clean-node clean distclean docker

all: node gateway

node:
	@echo "\e[1mBuilding $@ application\e[0m"
	"$(SEGGER_DIR)/bin/emBuild" mira-node-nrf52840dk.emProject -project 03app_$@ -config $(BUILD_CONFIG) $(PACKAGES_DIR_OPT) -rebuild -verbose
	@echo "\e[1mOutput binary: app/03app_node/Output/nrf52840dk/$(BUILD_CONFIG)/Exe/03app_node-nrf52840dk.bin\e[0m"
	@echo "\e[1mDone\e[0m\n"

gateway:
	@echo "\e[1mBuilding $@ application\e[0m"
	"$(SEGGER_DIR)/bin/emBuild" mira-gateway-nrf52840dk.emProject -project 03app_$@ -config $(BUILD_CONFIG) $(PACKAGES_DIR_OPT) -rebuild -verbose
	@echo "\e[1mOutput binary: app/03app_gateway/Output/nrf52840dk/$(BUILD_CONFIG)/Exe/03app_gateway-nrf52840dk.bin\e[0m"
	@echo "\e[1mDone\e[0m\n"

clean-node:
	"$(SEGGER_DIR)/bin/emBuild" mira-node-nrf52840dk.emProject -config $(BUILD_CONFIG) -clean

clean-gateway:
	"$(SEGGER_DIR)/bin/emBuild" mira-gateway-nrf52840dk.emProject -config $(BUILD_CONFIG) -clean

clean: clean-node clean-gateway

distclean: clean

docker:
	docker run --rm -i \
		-e BUILD_CONFIG="$(BUILD_CONFIG)" \
		-e PACKAGES_DIR_OPT="-packagesdir $(SEGGER_DIR)/packages" \
		-e SEGGER_DIR="$(SEGGER_DIR)" \
		-v $(PWD):/dotbot $(DOCKER_IMAGE) \
		make $(DOCKER_TARGETS)
