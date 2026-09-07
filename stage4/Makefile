# mini-lua 构建文件
# 用法:
#   make          编译生成 mini-lua 可执行文件
#   make run      编译并运行 REPL
#   make test     运行测试用例
#   make bench    运行性能基准
#   make clean    清理构建产物

CC      := gcc
CFLAGS  := -std=c99 -Wall -Wextra -g -O2 -fno-strict-aliasing
LDFLAGS := -lm

SRC_DIR := src
SOURCES := $(wildcard $(SRC_DIR)/*.c)
OBJECTS := $(SOURCES:.c=.o)
TARGET  := mini-lua

UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)
ifeq ($(UNAME_S),Darwin)
    TARGET := mini-lua
endif

.PHONY: all run test bench clean docs

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

test: $(TARGET)
	@echo "==> Running tests..."
	@for f in tests/*.lua; do \
		echo "  $$f"; \
		./$(TARGET) "$$f" || exit 1; \
	done
	@echo "==> All tests passed."

bench: $(TARGET)
	@echo "==> Running benchmarks..."
	@for f in benchmarks/*.lua; do \
		echo "--- $$f ---"; \
		./$(TARGET) "$$f"; \
	done

docs:
	cd docs && npm install && npm run build

clean:
	rm -f $(SRC_DIR)/*.o $(TARGET)
	rm -rf docs/.vitepress/dist docs/node_modules