# AeonBrowser retro — Open Watcom 16-bit Makefile
# DelgadoLogic
#
# BUILD REQUIREMENTS:
#   - Open Watcom 2.0 (https://github.com/open-watcom/open-watcom-v2)
#   - 16-bit Windows SDK headers (included in OW2)
#   - WINSOCK.H from WinSock 1.1 SDK
#
# USAGE:
#   wmake                → builds AEON16.EXE (16-bit Windows)
#   wmake clean          → removes build artifacts
#
# IT NOTE: The -ml flag = large memory model (far data + far code pointers).
# Required for our 512-8192 byte buffers to not overflow segment limits.
# The -zW flag = Windows 3.x calling convention (Pascal register saves).
# -bt=windows = target 16-bit Windows (not DOS)

CC     = wcc
LINK   = wlink
CFLAGS = -ml -zW -bt=windows -d2 -W3 -wx
SRCS   = aeon16.c html4.c wolfssl_bridge.c
OBJS   = aeon16.obj html4.obj wolfssl_bridge.obj
TARGET = AEON16.EXE

all: $(TARGET)

$(TARGET): $(OBJS)
	$(LINK) NAME $(TARGET) &
	    SYSTEM windows &
	    OPTION HEAPSIZE=8192 &
	    OPTION STACKSIZE=8192 &
	    OPTION MAP &
	    LIBRARY libw.lib &
	    LIBRARY libcew.lib &
	    LIBRARY winsock.lib &
	    FILE { $(OBJS) }

aeon16.obj: aeon16.c html4.h wolfssl_bridge.h
	$(CC) $(CFLAGS) aeon16.c

html4.obj: html4.c html4.h
	$(CC) $(CFLAGS) html4.c

wolfssl_bridge.obj: wolfssl_bridge.c wolfssl_bridge.h
	$(CC) $(CFLAGS) wolfssl_bridge.c

clean:
	del *.obj *.map $(TARGET) 2>NUL

.SUFFIXES: .c .obj
