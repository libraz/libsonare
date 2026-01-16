.PHONY: all build test clean rebuild format wasm coverage coverage-build coverage-clean

BUILD_DIR := build

all: build

build:
	cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(BUILD_DIR) -j

release:
	cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR) -j

wasm:
	emcmake cmake -B build-wasm -DBUILD_WASM=ON -DCMAKE_BUILD_TYPE=Release
	cmake --build build-wasm -j

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean build

format:
	find src tests -name '*.cpp' -o -name '*.h' | xargs clang-format -i

# Coverage targets
coverage-build:
	cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON
	cmake --build $(BUILD_DIR) -j

coverage: coverage-build
	@mkdir -p $(BUILD_DIR)/coverage
	cd $(BUILD_DIR) && lcov --directory . --zerocounters
	-cd $(BUILD_DIR) && ctest --output-on-failure --parallel
	cd $(BUILD_DIR) && lcov --directory . --capture --output-file coverage/coverage.info
	cd $(BUILD_DIR) && lcov --extract coverage/coverage.info '$(CURDIR)/src/*' --output-file coverage/coverage_filtered.info
	cd $(BUILD_DIR) && genhtml coverage/coverage_filtered.info --output-directory coverage/html
	@echo "Coverage report: $(BUILD_DIR)/coverage/html/index.html"

coverage-clean:
	find $(BUILD_DIR) -name '*.gcda' -delete 2>/dev/null || true
	rm -rf $(BUILD_DIR)/coverage
