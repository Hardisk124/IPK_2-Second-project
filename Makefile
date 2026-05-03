CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -pedantic -O2

SRC_DIR = src
SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(SRCS:.c=.o)
TARGET = ipk-rdt
BUILD_DIR ?= build

.PHONY: all clean NixDevShellName test test-clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(SRC_DIR)/*.o $(TARGET)
	rm -rf $(BUILD_DIR)

NixDevShellName:
	@echo c

test:
	cmake -S . -B $(BUILD_DIR)
	cmake --build $(BUILD_DIR)
	ctest --test-dir $(BUILD_DIR) --output-on-failure

