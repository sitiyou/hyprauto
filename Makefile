BUILD_DIR ?= build
PREFIX ?= $(HOME)/.local
CMAKE_ARGS ?=

all:
	cmake -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$(PREFIX)" $(CMAKE_ARGS)
	cmake --build "$(BUILD_DIR)" -j2

install: all
	cmake --install "$(BUILD_DIR)"

uninstall:
	cmake --build "$(BUILD_DIR)" --target uninstall

test:
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure --no-tests=error

clean:
	cmake --build "$(BUILD_DIR)" --target clean

.PHONY: all install uninstall test clean
