#!/usr/bin/env sh
set -eu
arkos_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
arkos_iso="$arkos_root/arkos-0.1.0.iso"
if [ ! -f "$arkos_iso" ]; then arkos_iso="$arkos_root/build/arkos-0.1.0.iso"; fi
if [ ! -f "$arkos_iso" ]; then echo 'Build first: make'; exit 1; fi
exec qemu-system-x86_64 -machine pc -m 256M -vga std -cdrom "$arkos_iso" -boot d -serial stdio -nic none "$@"
