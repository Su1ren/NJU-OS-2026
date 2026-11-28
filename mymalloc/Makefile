NAME := $(shell basename $(PWD))
export MODULE := M5

SRCS   := $(shell find tests/ -maxdepth 1 -name "*.c")
CFLAGS := -I.

all: $(NAME) check

check:
	gcc -DFREESTANDING -I. \
	    -ffreestanding -static -nostdlib -nostartfiles -no-pie \
	    -o malloc-check *.c && \
	        rm -f malloc-check

include ../oslab.mk
include ../.shadow/oslab.mk

# 本地 TestKit 接线：.shadow/oslab.mk 只取顶层 *.c，
# 既不含 tests/，也就没有 main（tests/main.c 才是 main 的载体）。
TEST_SRCS := tests/main.c $(shell find tests/ -maxdepth 1 -name 'tests-*.c')

.PHONY: test
test: $(SRCS) $(TEST_SRCS)
	$(CC) $(CFLAGS) $(SRCS) $(TEST_SRCS) ../testkit/testkit.c \
	      -o /tmp/$(NAME)-test -pthread
	TK_RUN=1 /tmp/$(NAME)-test
