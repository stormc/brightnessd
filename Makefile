SYSFS_BACKLIGHT_PATH = /sys/class/backlight/intel_backlight/

DESTDIR =
PREFIX  = /usr/local

SOURCE = brightnessd.c
EXECUTABLE=$(SOURCE:.c=)

X11LIBS = -lxcb-screensaver -lxcb-dpms -lxcb-randr -lxcb
GCCLIBS = -lm
debug_CFLAGS = -O0 -g3 -gdwarf-4 -fno-omit-frame-pointer ## framepointers are needed by valgrind
base_CFLAGS  = -std=gnu11 -D_REENTRANT -Wall -Wextra  -pedantic -O2 -D_XOPEN_SOURCE=600 -DPROGNAME=\"${EXECUTABLE}\"
clang_CFLAGS = -Weverything -Wno-disabled-macro-expansion

CC = clang

ifeq ($(CC),clang)
	base_CFLAGS += ${clang_CFLAGS}
endif
ifeq ($(CC),gcc)
	base_CFLAGS += -Wno-unknown-pragmas
endif

.DEFAULT_GOAL := ${EXECUTABLE}

all: $(EXECUTABLE)

xrandr $(EXECUTABLE): $(SOURCE) clean
	$(CC) $(CFLAGS) ${X11LIBS} ${GCCLIBS} ${base_CFLAGS} ${define_FLAGS} $< -o ${EXECUTABLE}
debug: $(SOURCE) clean
	$(CC) $(CFLAGS) -DDEBUGLOG=1 -DTRACELOG=1 ${X11LIBS} ${GCCLIBS} ${base_CFLAGS} ${debug_CFLAGS} ${define_FLAGS} $< -o ${EXECUTABLE}


sysfs: $(SOURCE) clean
	$(CC) $(CFLAGS) -DUSE_SYSFS_BACKLIGHT_CONTROL=1 -DSYSFS_BACKLIGHT_PATH=\"${SYSFS_BACKLIGHT_PATH}\" ${X11LIBS} ${GCCLIBS} ${base_CFLAGS} ${define_FLAGS} $< -o ${EXECUTABLE}
debug_sysfs: clean
	$(CC) $(CFLAGS) -DDEBUGLOG=1 -DTRACELOG=1 -DUSE_SYSFS_BACKLIGHT_CONTROL=1 -DSYSFS_BACKLIGHT_PATH=\"${SYSFS_BACKLIGHT_PATH}\" ${X11LIBS} ${GCCLIBS} ${base_CFLAGS} ${debug_CFLAGS} ${define_FLAGS} $< -o ${EXECUTABLE}


install: $(EXECUTABLE)
	install -D --group=root --owner=root --mode=0755 --strip $(EXECUTABLE) $(DESTDIR)/$(PREFIX)/bin/$(EXECUTABLE)


.PHONY: clean
clean:
	@rm -f $(EXECUTABLE)
