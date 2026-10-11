$ErrorActionPreference = 'Stop'
$ArkRoot = Split-Path -Parent $PSScriptRoot
$ArkIso = Join-Path $ArkRoot 'arkos-0.1.0.iso'
if (-not (Test-Path $ArkIso)) { $ArkIso = Join-Path $ArkRoot 'build/arkos-0.1.0.iso' }
if (-not (Test-Path $ArkIso)) { throw 'Build the ISO first with make in Linux or WSL.' }
& qemu-system-x86_64.exe -machine pc -m 256M -vga std -cdrom $ArkIso -boot d -serial stdio -nic none
