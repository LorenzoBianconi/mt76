# SPDX-License-Identifier: GPL-2.0-only
ifneq ($(KERNELRELEASE),)
EXTRA_CFLAGS += -Werror -DCONFIG_MT76_LEDS

obj-m := mt76.o
obj-m += mt7603/
obj-m += mt76x02-lib.o
obj-m += mt76x02-usb.o
obj-m += mt76-usb.o
obj-m += mt76x2/
obj-m += mt76x0/
obj-m += mt7615/

mt76-y := \
	mmio.o util.o trace.o dma.o mac80211.o debugfs.o eeprom.o \
	tx.o agg-rx.o mcu.o airtime.o

mt76-m += pci.o

mt76-usb-y := usb.o usb_trace.o

CFLAGS_trace.o := -I$(src)
CFLAGS_usb_trace.o := -I$(src)
CFLAGS_mt76x02_trace.o := -I$(src)

mt76x02-lib-y := mt76x02_util.o mt76x02_mac.o mt76x02_mcu.o \
		 mt76x02_eeprom.o mt76x02_phy.o mt76x02_mmio.o \
		 mt76x02_txrx.o mt76x02_trace.o mt76x02_debugfs.o \
		 mt76x02_dfs.o mt76x02_beacon.o

mt76x02-usb-y := mt76x02_usb_mcu.o mt76x02_usb_core.o
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD  := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

endif
