# Makefile for powernowd.. -very- simple.
#

all: powernow

powernow:
	gcc -O2 -Wall -o powernowd powernowd.c

install:
	install -m 755 powernowd /usr/sbin

clean:
	rm -rf powernowd
