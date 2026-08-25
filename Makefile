# Copyright (c) 2026 Christiaan (chris@boreddev.nl)
# BoredOS Installer Standalone Makefile

CC = x86_64-boredos-gcc

DESTDIR ?= $(abspath build/dist)

CFLAGS  = -Wall -Wextra -std=gnu11 -O2 -fno-stack-protector \
          -fno-stack-check -m64 -march=x86-64

LDFLAGS = -Wl,-z,max-page-size=0x1000 -Wl,-dynamic-linker,/usr/lib/ld.so -Wl,-rpath,/usr/lib:/lib

APPS    = boredos_install.elf

all: $(APPS)

boredos_install.elf: obj/boredos_install.o
	$(CC) $< $(LDFLAGS) -o $@

obj/%.o: src/%.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -c $< -o $@

install: all
	mkdir -p $(DESTDIR)/bin
	cp $(APPS) $(DESTDIR)/bin/

clean:
	rm -rf obj build $(APPS)
