CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 \
           $(shell pkg-config --cflags libusb-1.0)
LIBS    = $(shell pkg-config --libs libusb-1.0)
TARGET  = adb_loopback_test

$(TARGET): adb_loopback_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: clean
