
CROSS_COMPILE ?= 

CC := $(CROSS_COMPILE)gcc


CFLAGS ?= -Wall -Wextra -g

LDFLAGS ?=

TARGET := aesdsocket

SRCS := $(TARGET).c

default: all


all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Executable '$(TARGET)' created in $(CURDIR)"

clean:
	rm -f $(TARGET)
	@echo "Cleaned up $(TARGET)"
.PHONY: all default clean