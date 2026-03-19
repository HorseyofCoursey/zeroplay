CC      = gcc
CFLAGS  = -Wall -Wextra -O2
CFLAGS += $(shell pkg-config --cflags libavformat libavcodec libavutil libswresample libswscale libdrm libcjson 2>/dev/null)
CFLAGS += -I/usr/include/libdrm

LIBS    = $(shell pkg-config --libs libavformat libavcodec libavutil libswresample libswscale libdrm libcjson 2>/dev/null)
LIBS   += -lasound -lpthread

# WebSocket remote control (opt-in: make WS=1)
ifdef WS
CFLAGS += -DHAVE_WEBSOCKET
CFLAGS += $(shell pkg-config --cflags libwebsockets 2>/dev/null)
LIBS   += $(shell pkg-config --libs libwebsockets 2>/dev/null)
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
