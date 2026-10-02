# MiaHIDPordo - GTK3 GTK3  monitor
TARGET     := miaseriapordo
VERSION  := 5.0
TITLE	 := MiaSeriaPordo
####################################################################################################################
CFLAGS   += -Wall -Wextra `pkg-config --cflags gtk+-3.0 libconfig` 
CFLAGS   += -DWINTITLE='"$(TITLE) $(VERSION)"' -DTARGET='"$(TARGET)"'
LDLIBS   := `pkg-config --libs gtk+-3.0 libconfig`

ifeq ($(OS), Windows_NT)
LDLIBS   +=  -mwindows -lshlwapi -lsetupapi
else ifeq ($(shell uname -s), Linux)
LDLIBS   += 
endif
####################################################################################################################
all:clean info gresource
	gcc	-c main.c		-o main.o		$(CFLAGS)
	gcc	-c resources.c	-o resources.o	$(CFLAGS)
	gcc	*.o -o $(TARGET) $(LDLIBS)
	@rm -rf main *.o resources.c
	
run:all
	./$(TARGET)
####################################################################################################################
info: $(RES_SRC)
	@echo + OS = $(OS) 
	@echo - shell uname= $(shell uname -s)	
	cp resources/icon.svg debian/$(TARGET).svg
ifeq ($(OS), Windows_NT)
	rsvg-convert -w 256 -h 256 resources/icon.svg -o /tmp/icon.png && icotool -c -o windows/Application/Application.ico /tmp/icon.png && rm -f /tmp/icon.png
	windres -I. -i windows/application/resource.rc -o winresource.o
endif
####################################################################################################################
gresource:
	rm -f resources/resources.gresource.xml
	@mkdir -p resources
	@printf '%s\n' \
	  '<?xml version="1.0" encoding="UTF-8"?>' \
	  '<gresources>' \
	  '  <gresource prefix="/org/$(TARGET)">' \
	  '    <file>window1.glade</file>' \
	  '    <file>style.css</file>' \
	  '  </gresource>' \
	  '</gresources>' \
	  > resources/resources.gresource.xml
	@echo "Generated resources/resources.gresource.xml"
	@echo -  GResource :
	glib-compile-resources resources/resources.gresource.xml --sourcedir=resources --generate-source --target=resources.c --c-name=$(TARGET)
####################################################################################################################
clean:
	rm -rf main *.o  *.exe $(TARGET)  
	@rm -rf resources.c
	@rm -rf windows/nsis/*.ico windows/nsis/*.exe windows/nsis/*.dll  windows/nsis/*.glade 
	@rm -rf $(TARGET) *.o main .deb *.deb
	@rm -f resources/resources.gresource.xml
####################################################################################################################
ifeq ($(shell uname -s),Linux)
  ifneq ($(wildcard /etc/debian_version),)
program: deb
PREFIX   := /usr
DEBARCH  := $(shell dpkg --print-architecture 2>/dev/null || echo amd64)
STAGE    := .deb/$(TARGET)-$(VERSION)
DEBFILE  := $(TARGET)_$(VERSION)_$(DEBARCH).deb
deb:all
	@echo "######################   DEB BASED LINUX   ##########################"
	@mkdir -p $(STAGE)/DEBIAN $(STAGE)$(PREFIX)/bin $(STAGE)$(PREFIX)/share/$(TARGET) $(STAGE)$(PREFIX)/share/icons/hicolor/scalable/apps $(STAGE)$(PREFIX)/share/doc/$(TARGET) $(STAGE)$(PREFIX)/share/applications
	@install -m 0755 $(TARGET) $(STAGE)$(PREFIX)/bin/$(TARGET)
	@install -m 0644 debian/$(TARGET).desktop $(STAGE)$(PREFIX)/share/applications/
	@install -m 0644 debian/$(TARGET).svg $(STAGE)$(PREFIX)/share/icons/hicolor/scalable/apps/$(TARGET).svg
	@install -m 0644 debian/copyright $(STAGE)$(PREFIX)/share/doc/$(TARGET)/copyright
	@DEPS=$$(dpkg-shlibdeps -O $(TARGET) 2>/dev/null | sed -n 's/^shlibs:Depends=//p'); sed -e 's/@VERSION@/$(VERSION)/g' -e 's/@ARCH@/$(DEBARCH)/g' -e "s|@DEPS@|$$DEPS|g" debian/control.in > $(STAGE)/DEBIAN/control
	@dpkg-deb --root-owner-group --build $(STAGE) $(DEBFILE)
	@rm -rf .deb
	@echo "############### $(DEBFILE) IS READY ###################"
  else
	@echo "deb: not a Debian-based system" >&2
	@exit 1
  endif
endif
#sudo apt install libhidapi-dev librsvg2-bin icoutils
####################################################################################################################
ifeq ($(OS), Windows_NT)	
program:all
	@echo "######################   WINDOWS SETUP   ##########################"
	cp windows/Application/application.ico  windows/nsis/$(TARGET).ico
	cp $(TARGET) windows/nsis/$(TARGET)
	7z x windows/nsis/dlls.7z -owindows/nsis -y;
	rm -f windows/nsis/uninst.exe
	cd windows/nsis && makensis installer.nsi
	mv windows/nsis/$(TARGET)_Setup.exe ./$(TITLE)_$(VERSION)_Setup.exe
	@rm -f windows/nsis/*.ico windows/nsis/*.exe windows/nsis/*.dll .deb
	@echo "#############   $(TITLE)_$(VERSION)_Setup.exe  IS READY  ####################"
endif
#pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-pkg-config mingw-w64-x86_64-gtk3 mingw-w64-x86_64-libconfig mingw-w64-x86_64-nsis p7zip

####################################################################################################################
