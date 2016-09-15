
PWD := $(shell pwd)

INSTALL := install

KERNEL_MOD_DIR := adc7k

obj-m := adc7k.o adc7k-cpci3u.o adc7k-pseudo.o
adc7k-objs := adc7k-base.o
adc7k-cpci3u-objs:= adc7k-cpci3u-base.o
adc7k-pseudo-objs:= adc7k-pseudo-base.o

KERNEL_VERSION := `uname -r`
KERNEL_SRC_DIR := /lib/modules/$(KERNEL_VERSION)/build
KERNEL_STG_DIR := /

all: modules

modules:
	@make -C $(KERNEL_SRC_DIR) M=$(PWD) modules

modules_install: install_modules

install: install_modules install_headers install_modprobe_conf

install_modules:
	@make -C $(KERNEL_SRC_DIR) M=$(PWD) INSTALL_MOD_PATH=$(KERNEL_STG_DIR) INSTALL_MOD_DIR=$(KERNEL_MOD_DIR) modules_install

install_headers:
	$(INSTALL) -m 755 -d "$(DESTDIR)/usr/include/adc7k"
	for header in adc7k/*.h ; do \
		$(INSTALL) -m 644 $$header "$(DESTDIR)/usr/include/adc7k" ; \
	done

install_udev_rules:
	$(INSTALL) -m 644 adc7k-udev.rules "$(DESTDIR)/etc/udev/rules.d/adc7k.rules"

install_modprobe_conf:
	$(INSTALL) -m 644 adc7k-modprobe.conf "$(DESTDIR)/etc/modprobe.d/adc7k.conf"

uninstall: uninstall_modules uninstall_headers uninstall_udev_rules uninstall_modprobe_conf

uninstall_modules:
	rm -rvf "$(DESTDIR)/lib/modules/$(KERNEL_VERSION)/$(KERNEL_MOD_DIR)"
	depmod

uninstall_headers:
	rm -rvf "$(DESTDIR)/usr/include/adc7k"

uninstall_udev_rules:
	rm -fv $(DESTDIR)/etc/udev/rules.d/adc7k.rules

uninstall_modprobe_conf:
	rm -fv $(DESTDIR)/etc/modprobe.d/adc7k.conf

clean:
	@make -C $(KERNEL_SRC_DIR) M=$(PWD) clean
	@rm -f *~ adc7k/*~
