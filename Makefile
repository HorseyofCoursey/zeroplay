CC      = gcc
CFLAGS  = -Wall -Wextra -O2
CFLAGS += $(shell pkg-config --cflags libavformat libavcodec libavutil libswresample libswscale libdrm 2>/dev/null)
CFLAGS += -I/usr/include/libdrm

LIBS    = $(shell pkg-config --libs libavformat libavcodec libavutil libswresample libswscale libdrm 2>/dev/null)
LIBS   += -lasound -lpthread

# FreeType2 subtitle rendering (auto-detected)
FREETYPE_CFLAGS := $(shell pkg-config --cflags freetype2 2>/dev/null)
FREETYPE_LIBS   := $(shell pkg-config --libs   freetype2 2>/dev/null)
ifneq ($(FREETYPE_CFLAGS),)
    CFLAGS += -DHAVE_FREETYPE $(FREETYPE_CFLAGS)
    LIBS   += $(FREETYPE_LIBS)
    $(info subtitle: FreeType2 found — smooth font rendering enabled)
else
    $(info subtitle: FreeType2 not found — using bitmap font fallback)
endif

# WebSocket remote control (opt-in: make WS=1)
ifdef WS
CFLAGS += -DHAVE_WEBSOCKET
CFLAGS += $(shell pkg-config --cflags libwebsockets libcjson 2>/dev/null)
LIBS   += $(shell pkg-config --libs libwebsockets libcjson 2>/dev/null)
WS_SRC  = $(SRCDIR)/ws.c
endif

TARGET  = zeroplay
SRCDIR  = src
SRCS    = $(SRCDIR)/main.c     \
          $(SRCDIR)/queue.c    \
          $(SRCDIR)/demux.c    \
          $(SRCDIR)/audio.c    \
          $(SRCDIR)/vdec.c     \
          $(SRCDIR)/drm.c      \
          $(SRCDIR)/playlist.c \
          $(SRCDIR)/image.c    \
          $(SRCDIR)/subtitle.c    \
          $(WS_SRC)

PREFIX  = /usr/local

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LIBS)

install: $(TARGET)
	install -m 755 $(TARGET) $(PREFIX)/bin/$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all install clean
