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
# TLS: BearSSL (MIT licensed) is statically linked — no external DLL needed.
# BearSSL files compile individually (no amalgamation — static symbol collisions).
#
# IT NOTE: The -ml flag = large memory model (far data + far code pointers).
# Required for our 512-8192 byte buffers to not overflow segment limits.
# The -zW flag = Windows 3.x calling convention (Pascal register saves).
# -bt=windows = target 16-bit Windows (not DOS)
# -za99 = partial C99 mode (required for BearSSL's 'static inline' + stdint.h)

CC     = wcc
LINK   = wlink
CFLAGS = -ml -zW -bt=windows -d2 -W3 -wx -za99 -Ibearssl/inc -Ibearssl/src
TARGET = AEON16.EXE

# Application objects
APP_OBJS = aeon16.obj html4.obj bearssl_bridge.obj

all: $(TARGET)

$(TARGET): $(APP_OBJS) bearssl_lib
	$(LINK) NAME $(TARGET) &
	    SYSTEM windows &
	    OPTION HEAPSIZE=8192 &
	    OPTION STACKSIZE=8192 &
	    OPTION MAP &
	    LIBRARY libw.lib &
	    LIBRARY libcew.lib &
	    LIBRARY winsock.lib &
	    FILE { $(APP_OBJS) } &
	    LIBRARY bearssl.lib

aeon16.obj: aeon16.c html4.h bearssl_bridge.h
	$(CC) $(CFLAGS) aeon16.c

html4.obj: html4.c html4.h
	$(CC) $(CFLAGS) html4.c

bearssl_bridge.obj: bearssl_bridge.c bearssl_bridge.h trust_anchors.h bearssl/inc/bearssl.h
	$(CC) $(CFLAGS) bearssl_bridge.c

# BearSSL static library — compile each file individually to avoid
# static symbol collisions (K, Rcon, xorbuf, etc.)
bearssl_lib: .SYMBOLIC
	@if not exist bearssl_obj mkdir bearssl_obj
	for %f in (bearssl\src\codec\*.c) do $(CC) -ml -zW -bt=windows -za99 -W3 -Ibearssl/inc -Ibearssl/src -fo=bearssl_obj\ %f
	for %f in (bearssl\src\ec\*.c) do $(CC) -ml -zW -bt=windows -za99 -W3 -Ibearssl/inc -Ibearssl/src -fo=bearssl_obj\ %f
	for %f in (bearssl\src\hash\*.c) do $(CC) -ml -zW -bt=windows -za99 -W3 -Ibearssl/inc -Ibearssl/src -fo=bearssl_obj\ %f
	for %f in (bearssl\src\int\*.c) do $(CC) -ml -zW -bt=windows -za99 -W3 -Ibearssl/inc -Ibearssl/src -fo=bearssl_obj\ %f
	for %f in (bearssl\src\kdf\*.c) do $(CC) -ml -zW -bt=windows -za99 -W3 -Ibearssl/inc -Ibearssl/src -fo=bearssl_obj\ %f
	for %f in (bearssl\src\mac\*.c) do $(CC) -ml -zW -bt=windows -za99 -W3 -Ibearssl/inc -Ibearssl/src -fo=bearssl_obj\ %f
	for %f in (bearssl\src\rand\*.c) do $(CC) -ml -zW -bt=windows -za99 -W3 -Ibearssl/inc -Ibearssl/src -fo=bearssl_obj\ %f
	for %f in (bearssl\src\rsa\*.c) do $(CC) -ml -zW -bt=windows -za99 -W3 -Ibearssl/inc -Ibearssl/src -fo=bearssl_obj\ %f
	for %f in (bearssl\src\settings.c) do $(CC) -ml -zW -bt=windows -za99 -W3 -Ibearssl/inc -Ibearssl/src -fo=bearssl_obj\ %f
	for %f in (bearssl\src\ssl\*.c) do $(CC) -ml -zW -bt=windows -za99 -W3 -Ibearssl/inc -Ibearssl/src -fo=bearssl_obj\ %f
	for %f in (bearssl\src\symcipher\*.c) do $(CC) -ml -zW -bt=windows -za99 -W3 -Ibearssl/inc -Ibearssl/src -fo=bearssl_obj\ %f
	for %f in (bearssl\src\x509\*.c) do $(CC) -ml -zW -bt=windows -za99 -W3 -Ibearssl/inc -Ibearssl/src -fo=bearssl_obj\ %f
	wlib -n bearssl.lib bearssl_obj\*.obj

clean:
	del *.obj *.map *.lib $(TARGET) 2>NUL
	if exist bearssl_obj del bearssl_obj\*.obj 2>NUL
	if exist bearssl_obj rmdir bearssl_obj 2>NUL
