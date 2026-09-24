#!/bin/sh
# Soft-reboot that looks like unplug to the USB host (ImmortalWrt/PC).
#
# Problem: reboot with cable in leaves host rndis_host in TX-watchdog zombie;
# static 66.1/66.x cannot ping because L2 URBs never complete.
#
# Fix: unbind UDC first → host sees disconnect → then reboot → boot binds
# gadget fresh (same as plug-in). Do NOT set need-usb-renum (that path does
# post-boot UDC thrash which can re-wedge ImmortalWrt).
#
# Usage: soft-reboot-safe.sh
#        called from device_reboot / scheduled reboot / manual ops

GADGET_UDC=/sys/kernel/config/usb_gadget/g1/UDC
MARKER_DIRTY=/mnt/data/need-usb-renum
MARKER_CLEAN=/mnt/data/usb-clean-reboot
LOG=/tmp/soft-reboot-safe.log

{
  echo "=== soft-reboot-safe $(date) ==="

  # 1) Force host-side disconnect (fake unplug)
  if [ -f "$GADGET_UDC" ]; then
    echo "UDC was: $(cat "$GADGET_UDC" 2>/dev/null)"
    echo none > "$GADGET_UDC" 2>/dev/null || true
    echo "UDC unbound"
  else
    echo "WARN: no $GADGET_UDC"
  fi

  # 2) Give ImmortalWrt time to tear down usb0 / rndis_host
  sleep 2

  # 3) Boot path = physical-plug style (no H9 multi-UDC)
  rm -f "$MARKER_DIRTY"
  touch "$MARKER_CLEAN" 2>/dev/null || true
  sync
  sleep 1
  sync

  echo "rebooting"
  reboot
} >>"$LOG" 2>&1
