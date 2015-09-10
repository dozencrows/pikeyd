TARGET:=pikeyd
PREFIX = $(DESTDIR)/usr/local
BINDIR = $(PREFIX)/bin

#comment out the line below when _NOT_ cross compiling
#include crosscomp.rules

ifneq ($(CROSS), yes)
CC := gcc
LDFLAGS :=
INCLUDE :=
endif

ifdef DEBUG
CFLAGS=-O0 -g3 -Wstrict-prototypes -Wmissing-prototypes $(INCLUDE)
LDFLAGS=-g3
else
CFLAGS=-O2 -Wstrict-prototypes -Wmissing-prototypes $(INCLUDE)
LDFLAGS=-s
endif

SRC := $(wildcard *.c)
OBJ := $(patsubst %.c,%.o,$(SRC))
LIBS=

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) *.o *~


install: release
	install -D $(TARGET) $(BINDIR)/$(TARGET)

uninstall:
	-rm $(BINDIR)/$(TARGET)

release: $(TARGET)
