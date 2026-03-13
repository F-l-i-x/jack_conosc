CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pedantic -std=c11
LDFLAGS ?=
LDLIBS ?= -ljack -llo

TARGET := jack_conosc
SRC := jack_conosc.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
