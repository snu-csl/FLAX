#!/bin/bash

ssd_name_alias="/dev/disk/by-id/nvme-CSL_Virt_MN_01_CSL_Virt_SN_01"
SSD="$( realpath "${ssd_name_alias}" )"

echo $SSD

sudo umount /mnt/nvme
sudo mkfs.ext4 -F $SSD
sudo mount $SSD /mnt/nvme
sudo chown -R "$USER": /mnt/nvme
