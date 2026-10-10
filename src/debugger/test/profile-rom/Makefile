# Builds the profiler's test ROM with devkitARM and libgba, and embeds it for the unit tests.
# The built files are committed, so running the tests doesn't need devkitARM. Rebuild with
# "make" after changing main.c.

ifeq ($(strip $(DEVKITARM)),)
$(error Set DEVKITARM to the devkitARM installation)
endif

PREFIX := $(DEVKITARM)/bin/arm-none-eabi-
LIBGBA := $(DEVKITPRO)/libgba

CFLAGS := -O2 -mthumb -mthumb-interwork -mcpu=arm7tdmi -mtune=arm7tdmi -I$(LIBGBA)/include
LDFLAGS := -specs=gba.specs -L$(LIBGBA)/lib -lgba

all: profile-rom.h

profile-rom.elf: main.c
	$(PREFIX)gcc $(CFLAGS) $< $(LDFLAGS) -o $@
	$(PREFIX)strip --strip-debug $@

profile-rom.gba: profile-rom.elf
	$(PREFIX)objcopy -O binary $< $@
	$(DEVKITPRO)/tools/bin/gbafix $@

profile-rom.h: profile-rom.gba profile-rom.elf
	xxd -i profile-rom.gba > $@
	xxd -i profile-rom.elf >> $@

clean:
	rm -f profile-rom.elf profile-rom.gba profile-rom.h

.PHONY: all clean
