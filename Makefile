# AstraOS Makefile
# Freestanding x86-64 Operating System Foundation

PYTHON ?= python

.PHONY: all doctor iso run run-headless debug test test-host test-qemu test-vbox screenshot clean

all:
	$(PYTHON) scripts/build_kernel.py
	$(PYTHON) scripts/prepare_esp.py

iso: all
	$(PYTHON) scripts/build_iso.py

doctor:
	$(PYTHON) scripts/doctor.py

test-host:
	$(PYTHON) tests/test_pmm_host.py

test-qemu: all
	$(PYTHON) tests/test_runner.py --all

test-vbox: iso
	$(PYTHON) scripts/test_vbox.py

test: test-host test-qemu test-vbox

run: all
	tools/qemu/qemu-system-x86_64.exe \
		-drive if=pflash,format=raw,unit=0,file=tools/qemu/share/edk2-x86_64-code.fd,readonly=on \
		-m 512M \
		-drive file=fat:rw:build/esp,format=raw \
		-serial stdio

run-headless: all
	tools/qemu/qemu-system-x86_64.exe \
		-drive if=pflash,format=raw,unit=0,file=tools/qemu/share/edk2-x86_64-code.fd,readonly=on \
		-m 512M \
		-drive file=fat:rw:build/esp,format=raw \
		-serial stdio \
		-display none

debug: all
	@echo "Starting QEMU paused with GDB server on localhost:1234..."
	@echo "Connect with: gdb build/kernel.elf -ex 'target remote localhost:1234'"
	tools/qemu/qemu-system-x86_64.exe \
		-drive if=pflash,format=raw,unit=0,file=tools/qemu/share/edk2-x86_64-code.fd,readonly=on \
		-m 512M \
		-drive file=fat:rw:build/esp,format=raw \
		-serial stdio \
		-display none \
		-S -s

screenshot: all
	$(PYTHON) scripts/capture_screenshot.py

clean:
	@echo "Cleaning build artifacts..."
	$(PYTHON) -c "import shutil, os, glob; [os.remove(f) for f in glob.glob('build/*.o') + glob.glob('build/*.elf') + glob.glob('build/*.exe') + glob.glob('build/*.ppm') + glob.glob('build/*.bmp') + glob.glob('build/*.png') + glob.glob('build/*.iso') if os.path.exists(f)]; shutil.rmtree('build/esp', ignore_errors=True)"
	@echo "Clean completed."
