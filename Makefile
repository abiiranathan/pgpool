# -------- Configuration --------

BUILD_DIR ?= build
BUILD_TYPE ?= Release
PREFIX ?= /usr/local

CMAKE ?= cmake

# -------- Targets --------

.PHONY: all configure build install clean distclean

all: build

configure:
	@mkdir -p $(BUILD_DIR)
	$(CMAKE) -S . -B $(BUILD_DIR) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_INSTALL_PREFIX=$(PREFIX)

build: configure
	$(CMAKE) --build $(BUILD_DIR)

install: build
	$(CMAKE) --install $(BUILD_DIR)

clean:
	$(CMAKE) --build $(BUILD_DIR) --target clean || true

distclean:
	rm -rf $(BUILD_DIR)

# -------- Convenience --------

rebuild: distclean all
