# Build: DEVKITPRO=/opt/devkitpro (devkitA64 + libnx).  Never build under a path with spaces.
ifeq ($(strip $(DEVKITPRO)),)
$(error "Set DEVKITPRO")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules
TARGET   := skynet_sdl_nx
APP_TITLE:= SkyNET SDL
APP_AUTHOR:= skynet-sdl-nx (unofficial)
APP_VERSION:= 0.0.1
BUILD    := build
SOURCES  := source
INCLUDES := source
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS   := -g -Wall -O2 -ffunction-sections -fdata-sections $(ARCH) -D__SWITCH__ -D_GNU_SOURCE $(INCLUDE)
CXXFLAGS := $(CFLAGS)
ASFLAGS  := -g $(ARCH)
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) -Wl,--gc-sections
LIBS     := -lz -lnx -lm
LIBDIRS  := $(PORTLIBS) $(LIBNX)
ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export VPATH  := $(foreach d,$(SOURCES),$(CURDIR)/$(d))
export DEPSDIR:= $(CURDIR)/$(BUILD)
CFILES   := $(foreach d,$(SOURCES),$(notdir $(wildcard $(d)/*.c)))
export OFILES := $(CFILES:.c=.o)
export INCLUDE:= $(foreach d,$(INCLUDES),-iquote $(CURDIR)/$(d)) $(foreach d,$(LIBDIRS),-I$(d)/include)
export LIBPATHS:= $(foreach d,$(LIBDIRS),-L$(d)/lib)
export LD := $(CC)
.PHONY: $(BUILD) clean
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	rm -rf $(BUILD) $(TARGET).nro $(TARGET).elf
else
DEPENDS := $(OFILES:.o=.d)
$(OUTPUT).nro: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)
-include $(DEPENDS)
endif
