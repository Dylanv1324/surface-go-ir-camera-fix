# Surface Go 1 - IR Camera Fix for Linux

Get the OV7251 IR camera working on the Microsoft Surface Go 1 under Linux for biometric face unlock via [Howdy](https://github.com/boltgolt/howdy).

## The Problem

On the Surface Go 1 running Linux kernel 6.19+, the IR camera (OV7251) doesn't work due to multiple issues:

1. **Kernel 6.19 regression** ([intel/ipu6-drivers#405](https://github.com/intel/ipu6-drivers/issues/405)): Commit `d7cdbbc93c5` broke software node property handling, causing the OV7251 driver to crash during probe.

2. **CIO2 async notifier deadlock**: The IPU3 CIO2 driver registers async entries for all 3 sensors (OV5693, OV8865, OV7251) and waits for **all** to bind before activating **any**. The OV5693 and OV8865 fail to bind due to fwnode matching issues, blocking all cameras.

3. **IPU3 packed format**: The camera outputs IPU3-packed 10-bit greyscale (`ip3y`) which no userspace tool (ffmpeg, GStreamer, OpenCV) can read.

4. **IR emitter not exposed**: The BIOS doesn't expose the IR flood emitter GPIO (DSC2 INT3472 returns `_STA=0` with 0 GPIOs).

## The Solution

| Component | Purpose |
|-----------|---------|
| `patches/ipu-bridge-ir-only.patch` | Remove non-binding sensors from the bridge so the CIO2 notifier completes |
| `src/ir-bridge.c` | Userspace converter: IPU3 Y10 packed -> 8-bit greyscale via v4l2loopback |
| `src/screen_flash.py` | Flash screen white during auth to illuminate face (compensates for missing IR emitter) |
| `systemd/ir-camera.service` | Auto-start everything on boot |

## Architecture

```
OV7251 sensor (Y10_1X10/640x480)
    |
    v
IPU3 CSI-2 receiver
    |
    v
IPU3 CIO2 DMA (/dev/video2, ip3y format)
    |
    v
ir-bridge (unpacks IPU3 Y10 --> 8-bit grey)
    |
    v
v4l2loopback (/dev/video20, GREY format)
    |
    v
Howdy face recognition --> PAM --> screen unlock
```

## Prerequisites

- Arch Linux with `linux-surface` kernel
- Required packages:

```bash
sudo pacman -S base-devel linux-surface-headers v4l-utils v4l2loopback-dkms libgpiod acpica
paru -S howdy-git  # from AUR - uses native PAM module, no pam-python needed
```

## Installation

### 1. Build and install patched ipu-bridge

```bash
# Download upstream source for your kernel version
KVER=$(uname -r | grep -oP '^\d+\.\d+')
curl -sL "https://raw.githubusercontent.com/torvalds/linux/v${KVER}/drivers/media/pci/intel/ipu-bridge.c" \
    -o ipu-bridge-build/ipu-bridge.c

# Apply patch
cd ipu-bridge-build
patch -p1 < ../patches/ipu-bridge-ir-only.patch

# Build
make

# Backup and install
sudo cp "/lib/modules/$(uname -r)/kernel/drivers/media/pci/intel/ipu-bridge.ko.zst" \
        "/lib/modules/$(uname -r)/kernel/drivers/media/pci/intel/ipu-bridge.ko.zst.bak"
zstd ipu-bridge.ko -o ipu-bridge.ko.zst
sudo cp ipu-bridge.ko.zst "/lib/modules/$(uname -r)/kernel/drivers/media/pci/intel/ipu-bridge.ko.zst"
sudo depmod -a
```

### 2. Build and install ir-bridge

```bash
gcc -O2 -o ir-bridge src/ir-bridge.c
sudo cp ir-bridge /usr/local/bin/
```

### 3. Install screen flash module

```bash
sudo cp src/screen_flash.py /usr/lib/howdy/screen_flash.py
```

Then add to Howdy's `compare.py`:

```python
# At the top, add:
import screen_flash

# In the exit() function, add:
screen_flash.stop()

# Before the main recognition loop (after "Identifying you..."), add:
screen_flash.start()
```

### 4. Install systemd service

```bash
sudo cp systemd/ir-camera.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now ir-camera.service
```

### 5. Configure Howdy

```bash
sudo sed -i 's|device_path = .*|device_path = /dev/video20|' /etc/howdy/config.ini
sudo sed -i 's/^timeout = .*/timeout = 10/' /etc/howdy/config.ini
sudo sed -i 's/^certainty = .*/certainty = 4.5/' /etc/howdy/config.ini
sudo sed -i 's/^dark_threshold = .*/dark_threshold = 90/' /etc/howdy/config.ini
```

### 6. Configure PAM

Add to `/etc/pam.d/system-auth` after the `pam_faillock.so preauth` line:

```
auth       sufficient   /usr/lib/security/pam_howdy.so
```

### 7. Enroll and test

```bash
sudo howdy add      # enroll your face
sudo howdy test     # verify detection
```

### 8. Reboot

```bash
sudo reboot
```

## Sensor Tuning

The OV7251 sensor controls are available via `/dev/v4l-subdev6`:

```bash
# View current settings
v4l2-ctl -d /dev/v4l-subdev6 --list-ctrls

# Adjust (included in systemd service)
v4l2-ctl -d /dev/v4l-subdev6 --set-ctrl=exposure=1450,analogue_gain=650
```

| Parameter | Range | Recommended | Notes |
|-----------|-------|-------------|-------|
| exposure | 1-1704 | 1450 | Higher = brighter but more motion blur |
| analogue_gain | 16-1023 | 650 | Higher = brighter but more noise |

The privacy LED (gpio-98) provides weak IR illumination and is enabled by the systemd service.

## IPU3 Y10 Packed Format

The Intel IPU3 uses a non-standard 10-bit packing:

- **25 pixels** packed into **32 bytes** per group
- Bytes 0-24: high 8 bits (MSBs [9:2]) of each pixel
- Bytes 25-31: low 2 bits (LSBs [1:0]) packed 4 per byte, with 6 bits unused
- Bytes per line: `ceil(width / 25) * 32` (832 for 640px width)

For 8-bit conversion, simply extract bytes 0-24 from each 32-byte group.

## Known Limitations

- Only the IR camera (OV7251) works; RGB cameras (OV5693, OV8865) are disabled by the bridge patch
- The IR flood emitter is not software-controllable (BIOS limitation)
- Screen flash provides illumination but is not as effective as a dedicated IR emitter
- High sensor gain introduces noise
- Kernel 6.20+ should fix the upstream regression and potentially enable all cameras

## Troubleshooting

**Camera not detected after reboot:**
```bash
sudo dmesg | grep -i "ipu\|ov7251\|Connected"
# Should show: "Found supported sensor INT347E:00" and "Connected 1 cameras"
```

**ir-bridge fails with STREAMON error:**
```bash
# Ensure media pipeline is configured
media-ctl -d /dev/media0 --set-v4l2 '"ov7251 3-0060":0[fmt:Y10_1X10/640x480]'
sudo systemctl restart ir-camera.service
```

**Howdy can't read frames:**
```bash
# Check v4l2loopback is loaded
v4l2-ctl --list-devices | grep "IR Camera"
# Check ir-bridge is running
systemctl status ir-camera.service
```

**Face not detected (too dark):**
- Increase exposure/gain
- Ensure privacy LED is on: `cat /sys/kernel/debug/gpio | grep privacy`
- Enable screen flash in compare.py

## References

- [linux-surface/linux-surface](https://github.com/linux-surface/linux-surface)
- [boltgolt/howdy](https://github.com/boltgolt/howdy)
- [intel/ipu6-drivers#405](https://github.com/intel/ipu6-drivers/issues/405) - Kernel 6.19 regression
- [linux-surface Camera Support Wiki](https://github.com/linux-surface/linux-surface/wiki/Camera-Support)
- [Sakari Ailus fix patch](https://lkml.org/lkml/2025/12/19/405) - Upstream software_node fix

## License

MIT
