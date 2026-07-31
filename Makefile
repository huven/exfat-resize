# SPDX-License-Identifier: MIT

CMAKE ?= cmake
CTEST ?= ctest

BUILD ?= build
SANITIZE_BUILD ?= $(BUILD)/sanitize
SANITIZE_CONSUMER_BUILD ?= $(BUILD)/sanitize-consumer
CMAKE_BUILD_TYPE ?= RelWithDebInfo
CMAKE_ARGS ?=
CMAKE_BUILD_ARGS ?= --parallel

.DEFAULT_GOAL := all

.PHONY: all clean cli-sanitize-test cli-test configure dist library-sanitize-test \
	library-test release-test sanitize-build sanitize-consumer-test sanitize-test test \
	test-build

all: configure
	$(CMAKE) --build "$(BUILD)" $(CMAKE_BUILD_ARGS) --target exfat-resize

configure:
	$(CMAKE) -S . -B "$(BUILD)" \
		-DCMAKE_BUILD_TYPE="$(CMAKE_BUILD_TYPE)" \
		-DEXFAT_RESIZE_BUILD_CLI=ON \
		-DEXFAT_RESIZE_BUILD_TESTS=ON $(CMAKE_ARGS)

test-build: configure
	$(CMAKE) --build "$(BUILD)" $(CMAKE_BUILD_ARGS)

test: test-build
	$(CTEST) --test-dir "$(BUILD)" --output-on-failure

library-test: test-build
	$(CTEST) --test-dir "$(BUILD)" -LE cli --output-on-failure

cli-test: test-build
	$(CTEST) --test-dir "$(BUILD)" -L cli --output-on-failure

sanitize-build:
	$(CMAKE) -S . -B "$(SANITIZE_BUILD)" \
		-DCMAKE_BUILD_TYPE=Debug \
		-DEXFAT_RESIZE_BUILD_CLI=ON \
		-DEXFAT_RESIZE_BUILD_TESTS=ON \
		-DEXFAT_RESIZE_ENABLE_SANITIZERS=ON $(CMAKE_ARGS)
	$(CMAKE) --build "$(SANITIZE_BUILD)" $(CMAKE_BUILD_ARGS)

sanitize-consumer-test: sanitize-build
	$(CMAKE) -S tests/package/consumer -B "$(SANITIZE_CONSUMER_BUILD)" \
		-DEXFAT_RESIZE_SOURCE_DIR="$(CURDIR)" \
		-DEXFAT_RESIZE_BUILD_CLI=OFF \
		-DEXFAT_RESIZE_BUILD_TESTS=OFF \
		-DEXFAT_RESIZE_ENABLE_SANITIZERS=ON
	$(CMAKE) --build "$(SANITIZE_CONSUMER_BUILD)" $(CMAKE_BUILD_ARGS)
	$(CTEST) --test-dir "$(SANITIZE_CONSUMER_BUILD)" --output-on-failure

sanitize-test: sanitize-build sanitize-consumer-test
	$(CTEST) --test-dir "$(SANITIZE_BUILD)" --output-on-failure

library-sanitize-test: sanitize-build sanitize-consumer-test
	$(CTEST) --test-dir "$(SANITIZE_BUILD)" -LE cli --output-on-failure

cli-sanitize-test: sanitize-build
	$(CTEST) --test-dir "$(SANITIZE_BUILD)" -L cli --output-on-failure

release-test:
	tests/package/release-check.sh
	tests/package/release-package.sh

dist: VERSION cmake/ExfatResizeVersion.cmake tools/make-dist.sh tools/version.cmake
	@CMAKE="$(CMAKE)" tools/make-dist.sh

clean:
	rm -rf "$(BUILD)" "$(SANITIZE_BUILD)" "$(SANITIZE_CONSUMER_BUILD)" dist
