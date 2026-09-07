# Makefile for MiaSeriaPordo
APP_NAME = miaseriapordo
# MiaSeriaPordo - GTK3 serial port monitor with libconfig settings

all: clean

	gcc main.c  `pkg-config --cflags --libs gtk+-3.0`  -c -o main.o
	windres -I.  -i windows/application/resource.rc  -o  windows/application/resource.o
	gcc  main.o windows/application/resource.o `pkg-config --cflags --libs gtk+-3.0` -mwindows   -lws2_32 -lshlwapi -lsetupapi  -lconfig   -lsqlite3  -Wl,--export-all-symbols -o  main
	rm -f   windows/application/resource.o
	rm -rf *.o
	cp main $(APP_NAME)
	

clean:
	@echo + OS = $(OS) 
	@echo - shell uname= $(shell uname -s)
	rm -f main *.o  *.exe $(APP_NAME)  windows/nsis/*.ico windows/nsis/*.o windows/nsis/*.exe windows/nsis/*.dll  windows/nsis/*.glade 

program:all
ifeq ($(OS), Windows_NT)	
	cp windows/Application/application.ico  windows/nsis/$(APP_NAME).ico
	cp main windows/nsis/$(APP_NAME)
	cp windows1.glade windows/nsis/windows1.glade
	7z x windows/nsis/dlls.7z -owindows/nsis -y;
	rm -f windows/nsis/uninst.exe
	cd windows/nsis && makensis installer.nsi
	rm -f windows/nsis/*.dll
	mv windows/nsis/$(APP_NAME)_Setup.exe .
	@echo "Installer built successfully: $(APP_NAME)_Setup.exe"
endif


	
#pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-pkg-config mingw-w64-x86_64-gtk3 mingw-w64-x86_64-libconfig
#pacman -S mingw-w64-x86_64-nsis p7zip
#in ubuntu
#sudo apt install libhidapi-dev 
