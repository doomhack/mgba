# Builds the profiler's timing test ROM with devkitARM and libgba. See main.c for what it does and
# the costs to expect. "make" builds timing-rom.gba, with timing-rom.elf for the symbols.

ifeq ($(strip $(DEVKITARM)),)
$(error Set DEVKITARM to the devkitARM installation)
endif

PREFIX := $(DEVKITARM)/bin/arm-none-eabi-
LIBGBA := $(DEVKITPRO)/libgba

CFLAGS := -O2 -mthumb -mthumb-interwork -mcpu=arm7tdmi -mtune=arm7tdmi -I$(LIBGBA)/include
LDFLAGS := -specs=gba.specs -L$(LIBGBA)/lib -lgba

all: timing-rom.gba

timing-rom.elf: main.c
	$(PREFIX)gcc $(CFLAGS) $< $(LDFLAGS) -o $@

timing-rom.gba: timing-rom.elf
	$(PREFIX)objcopy -O binary $< $@
	$(DEVKITPRO)/tools/bin/gbafix $@

clean:
	rm -f timing-rom.elf timing-rom.gba

.PHONY: all clean
