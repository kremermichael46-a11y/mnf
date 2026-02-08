# Makefile for move-nested-files (mnf)
# Version: 1.0.0

# Configuration
VERSION ?= 1.0.0
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
MAN1DIR ?= $(MANDIR)/man1

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -pthread
CPPFLAGS?= -DMNF_VERSION=\"$(VERSION)\" -D_GNU_SOURCE
LDFLAGS ?=
TARGET   = mnf
SRC      = src/mnf.c
RELEASE_DIR ?= dist
UNAME_M := $(shell uname -m)
ARCH ?= $(if $(filter x86_64,$(UNAME_M)),amd64,$(if $(filter aarch64,$(UNAME_M)),arm64,$(UNAME_M)))
RELEASE_NAME ?= mnf-$(VERSION)-linux-$(ARCH)
RELEASE_STAGING := $(RELEASE_DIR)/$(RELEASE_NAME)

.PHONY: all build install uninstall clean dist release help

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

release: clean build
	@mkdir -p $(RELEASE_STAGING)
	@cp $(TARGET) $(RELEASE_STAGING)/$(TARGET)
	@cp man/mnf.1 $(RELEASE_STAGING)/mnf.1
	@if command -v strip >/dev/null 2>&1; then strip $(RELEASE_STAGING)/$(TARGET); fi
	@tar -C $(RELEASE_DIR) -czf $(RELEASE_DIR)/$(RELEASE_NAME).tar.gz $(RELEASE_NAME)
	@echo "Release archive created at $(RELEASE_DIR)/$(RELEASE_NAME).tar.gz"

help:
	@echo "Targets:"
	@echo "  build (default) - compile mnf"
	@echo "  install         - install to $(PREFIX)"
	@echo "  uninstall       - remove installed files"
	@echo "  clean           - remove build artifacts"
	@echo "  dist            - create a zip archive"
	@echo "  release         - create a linux tar.gz with binary + man page"
