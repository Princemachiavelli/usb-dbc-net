PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config

CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -pthread
LDFLAGS += -pthread

LIBUSB_CFLAGS = $(shell $(PKG_CONFIG) --cflags libusb-1.0)
LIBUSB_LIBS = $(shell $(PKG_CONFIG) --libs libusb-1.0)

PROGRAMS = dbc-impostor dbcnet-host

all: $(PROGRAMS)

dbc-impostor: dbc-impostor.o frame.o
	$(CC) $(LDFLAGS) -o $@ $^

dbcnet-host: dbcnet-host.o frame.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBUSB_LIBS)

dbcnet-host.o: CFLAGS += $(LIBUSB_CFLAGS)

frame-test: frame-test.o frame.o
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c frame.h
	$(CC) $(CFLAGS) -c -o $@ $<

check: frame-test
	./frame-test

install: $(PROGRAMS)
	install -Dm755 -t $(DESTDIR)$(PREFIX)/bin $(PROGRAMS)

clean:
	rm -f *.o $(PROGRAMS) frame-test

.PHONY: all check install clean
