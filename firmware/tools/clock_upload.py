"""Flash only the app image of the clock (ota_1, offset from board_upload.offset_address).

PlatformIO's esptool upload also writes bootloader, partition table and boot_app0.bin
(the latter resets the boot selection to ota_0). The clock must not touch these: after
`pio run -e slate -t upload` has been done once, they are already in place.
"""
Import("env")  # noqa: F821  (PlatformIO SCons)

flags = list(env.get("UPLOADERFLAGS", []))  # noqa: F821
if "--flash_size" in flags:
    # ... "--flash_size", "<size>", <offset1>, <file1>, <offset2>, <file2>, ...  -> drop the extra images
    cut = flags.index("--flash_size") + 2
    env.Replace(UPLOADERFLAGS=flags[:cut])  # noqa: F821
