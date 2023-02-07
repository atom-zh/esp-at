#!/bin/sh

mv build/bootloader/bootloader.bin build/bootloader/raw_bootloader.bin && espsecure.py encrypt_flash_data \
            --keyfile my_flash_encryption_key.bin --address 0x1000 --output build/bootloader/bootloader.bin build/bootloader/raw_bootloader.bin
mv build/partition_table/partition-table.bin build/partition_table/raw_partition-table.bin && espsecure.py encrypt_flash_data \
            --keyfile my_flash_encryption_key.bin --address 0x8000 --output build/partition_table/partition-table.bin build/partition_table/raw_partition-table.bin
mv build/esp-at.bin build/raw_esp-at.bin && espsecure.py encrypt_flash_data \
            --keyfile my_flash_encryption_key.bin --address 0x100000 --output build/esp-at.bin build/raw_esp-at.bin
mv build/ota_data_initial.bin build/raw_ota_data_initial.bin && espsecure.py encrypt_flash_data \
	    --keyfile my_flash_encryption_key.bin --address 0x10000 --output build/ota_data_initial.bin build/raw_ota_data_initial.bin
