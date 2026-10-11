# 🛰️ Flashing Orbit

## One-file flash (the deliverable — trimmed image, ~7.7 MB, fast upload)
#   orbit_firmware.bin lives at the PROJECT ROOT (orbit-assistant/orbit_firmware.bin)
esptool.py --chip esp32s3 -b 921600 write_flash 0x0 orbit_firmware.bin
#   (first time after the old firmware: esptool.py erase_flash first)

## Or individual parts (all in this directory, except assets)
esptool.py --chip esp32s3 -b 921600 write_flash \
  0x0      bootloader.bin \
  0x8000   partition-table.bin \
  0xd000   ota_data_initial.bin \
  0x20000  orbit.bin \
  0x420000 ../prebuilt/assets.bin

## Monitor the boot log
idf.py -p /dev/ttyACM0 monitor
#   (or any serial monitor at 115200 baud)
