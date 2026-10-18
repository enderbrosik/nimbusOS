# NimbusOS disk tools: open, copy files in and out of, and mount NimbusFS disk images.
#   make              build nfstool (and nimbusfuse when libfuse 2 / macFUSE is found)
#   make windows      cross-build nfstool.exe with MinGW (x86_64-w64-mingw32-gcc)
#   make test         run the mount test (Linux with libfuse 2)
CC      ?= cc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter
FUSE    := $(shell pkg-config --cflags --libs fuse 2>/dev/null)
BIN     := build

all: $(BIN)/nfstool $(BIN)/newfs_nimbusfs_fmt $(if $(FUSE),$(BIN)/nimbusfuse)

$(BIN)/nfstool: tools/nfstool.c tools/hostdev.h fs/nbfs.c fs/nbfs.h
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -I. tools/nfstool.c fs/nbfs.c -o $@

$(BIN)/newfs_nimbusfs_fmt: tools/newfs_nimbusfs.c fs/nbfs.c fs/nbfs.h
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -I. tools/newfs_nimbusfs.c fs/nbfs.c -o $@

$(BIN)/nimbusfuse: tools/nimbusfuse.c tools/hostdev.h fs/nbfs.c fs/nbfs.h
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -I. $(FUSE) tools/nimbusfuse.c fs/nbfs.c $(FUSE) -o $@

windows:
	@mkdir -p $(BIN)
	x86_64-w64-mingw32-gcc $(CFLAGS) -I. tools/nfstool.c fs/nbfs.c -o $(BIN)/nfstool.exe

test: all
	python3 tools/test_fuse.py

clean:
	rm -rf $(BIN)
.PHONY: all windows test clean
