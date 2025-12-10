# Makefile for move-nested-files (mnf)
# Version: 1.0.0

# Configuration
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
MAN1DIR ?= $(MANDIR)/man1

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -pthread
CPPFLAGS?= -DMNF_VERSION=\"1.0.0\" -D_GNU_SOURCE
LDFLAGS ?=
TARGET   = mnf
SRC      = src/mnf.c

.PHONY: all build install uninstall clean dist help

all: build
build: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: build
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -d $(DESTDIR)$(MAN1DIR)
	gzip -c man/mnf.1 > $(DESTDIR)$(MAN1DIR)/mnf.1.gz

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(MAN1DIR)/mnf.1.gz

clean:
	rm -f $(TARGET)

dist: clean
	zip -r move-nested-files-1.0.0.zip .

help:
	@echo "Targets:"
	@echo "  build (default) - compile mnf"
	@echo "  install         - install to $(PREFIX)"
	@echo "  uninstall       - remove installed files"
	@echo "  clean           - remove build artifacts"
	@echo "  dist            - create a zip archive"
