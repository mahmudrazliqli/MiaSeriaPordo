ifeq ($(OS), Windows_NT)
include Windows.mk
else ifeq ($(shell uname -s), Linux)
include Linux.mk
else ifeq ($(shell uname -s), Darwin)
include Darwin.mk
endif
	
