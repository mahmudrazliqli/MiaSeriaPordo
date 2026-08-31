# MiaSeriaPordo - GTK3 serial port monitor with libconfig settings

NAME     := miaseriapordo
VERSION  := 1.0.0

CC       ?= gcc
PKGS     := gtk+-3.0 libconfig

CFLAGS   += -Wall -Wextra $(shell pkg-config --cflags $(PKGS)) \
            -DMIASERIAPORDO_DATA_DIR='"/usr/share/$(NAME)"'
LDLIBS   := $(shell pkg-config --libs $(PKGS))

TARGET   := $(NAME)
SRCS     := main.c

PREFIX   := /usr
DEBARCH  := $(shell dpkg --print-architecture 2>/dev/null || echo amd64)
STAGE    := .deb/$(NAME)-$(VERSION)
DEB      := $(NAME)_$(VERSION)_$(DEBARCH).deb

.PHONY: all clean install deb run

all: clean $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

# ---------------------------------------------------------------------
# System install (also used by the .deb): make install [DESTDIR=...]

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(NAME)
	install -d $(DESTDIR)$(PREFIX)/share/$(NAME)
	install -m 0644 windows1.glade $(DESTDIR)$(PREFIX)/share/$(NAME)/
	install -d $(DESTDIR)$(PREFIX)/share/applications
	install -m 0644 debian/$(NAME).desktop \
		$(DESTDIR)$(PREFIX)/share/applications/
	install -d $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps
	install -m 0644 debian/$(NAME).svg \
		$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/$(NAME).svg
	install -d $(DESTDIR)$(PREFIX)/share/doc/$(NAME)
	install -m 0644 debian/copyright \
		$(DESTDIR)$(PREFIX)/share/doc/$(NAME)/copyright

# ---------------------------------------------------------------------
# Build a .deb package: make deb -> miaseriapordo_<version>_<arch>.deb
# Runtime library packages are detected from this build host.

.PHONY: deb

deb: all
	rm -rf $(STAGE)
	mkdir -p $(STAGE)/DEBIAN
	$(MAKE) DESTDIR=$(CURDIR)/$(STAGE) install
	sed -e 's/@VERSION@/$(VERSION)/g' \
	    -e 's/@ARCH@/$(DEBARCH)/g' debian/control.in > $(STAGE)/DEBIAN/control
	gtkpkg=$$(dpkg -S "$$(ldd $(TARGET) | awk '/libgtk-3\.so/{print $$3}')" 2>/dev/null | cut -d: -f1 | head -1); \
	cfgpkg=$$(dpkg -S "$$(ldd $(TARGET) | awk '/libconfig\.so/{print $$3}')" 2>/dev/null | cut -d: -f1 | head -1); \
	glibpkg=$$(dpkg -S "$$(ldd $(TARGET) | awk '/libglib-2\.0\.so/{print $$3}')" 2>/dev/null | cut -d: -f1 | head -1); \
	sed -i -e "s/@GTK@/$${gtkpkg:-libgtk-3-0}/" \
	       -e "s/@CONFIG@/$${cfgpkg:-libconfig11}/" \
	       -e "s/@GLIB@/$${glibpkg:-libglib2.0-0}/" $(STAGE)/DEBIAN/control; \
	if grep -qE '@(GTK|GLIB|CONFIG|ARCH|VERSION)@' $(STAGE)/DEBIAN/control; then \
	    echo "ERROR: unresolved placeholder in control"; exit 1; fi
	dpkg-deb --root-owner-group --build $(STAGE) $(DEB)
	rm -rf $(STAGE)
	@echo "== Built $(DEB) =="
