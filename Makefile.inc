# Add syscall and irqstubs to build
C_SOURCES += syscalls.c irqstubs.S

# Ensure irqstubs.S assembled as text
AS_SOURCES += irqstubs.S

# Add rule to build a small user program for testing exec_elf
USERPROG_DIR := userprog
USERPROG := $(USERPROG_DIR)/hello

.PHONY: userprog
userprog:
	# build 32-bit PIC ET_DYN object and link as a shared object (PIE-like)
	gcc -m32 -fPIC -nostdlib -ffreestanding -fno-builtin -Os -Wall -Wextra \
	    -o $(USERPROG).o -c $(USERPROG_DIR)/hello.c
	ld -m elf_i386 -shared -o $(USERPROG).so $(USERPROG).o
	# Strip to reduce size
	strip --strip-all $(USERPROG).so || true
	@echo "Built user program: $(USERPROG).so"

# helper to copy userprog into kernel build area if desired
install-userprog: userprog
	mkdir -p out
	cp $(USERPROG).so out/hello.so || true

# Hook to embed the built userprog into C source used by kernel
.PHONY: embed-userprog
embed-userprog: userprog
	sh tools/embed_hello.sh
	@echo "Generated userprog_blob.c"

# Ensure userprog_blob.c compiled into kernel if present
ifneq (,$(wildcard userprog_blob.c))
C_SOURCES += userprog_blob.c userprog_blob.o userprog_blob_inc.c userprog_hello.h userprog_blob.c
endif

# USB experimental sources (phase A stubs). Enable by creating usb_stub_enable file.
ifneq (,$(wildcard usb/usb_host.c))
C_SOURCES += usb/usb_host.c usb/nic_stub.c
endif

ifneq (,$(wildcard usb/usb_core.c))
C_SOURCES += usb/usb_core.c usb/usb_rndis.c
endif

ifneq (,$(wildcard net/eth.c))
C_SOURCES += net/eth.c net/dhcp.c
endif

# Add kernel menu target to copy ELF to iso (if you want filesystem instead)