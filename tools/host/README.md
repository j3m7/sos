# Host-side USB forwarding for the Pico

Automatically passes Raspberry Pi USB devices to the development VM through libvirt, so the
Pico follows its reboots between application mode (`2e8a:0009`) and BOOTSEL (`2e8a:000f`)
without SPICE redirection or a focused viewer window.

How it works: a udev rule fires when any vendor `2e8a` device is added or removed on the host
and runs `pico-vm-attach`, which issues `virsh attach-device` / `detach-device --live` with
the device's bus/device address. libvirt manages permissions and isolation for the device.

All of this runs on the **host**, not inside the VM.

## Install

`~/slop` is shared into the VM over virtiofs, so these paths are the same on the host:

```sh
sudo install -m 0755 ~/slop/sos/tools/host/pico-vm-attach /usr/local/bin/pico-vm-attach
sudo install -m 0644 ~/slop/sos/tools/host/90-pico-vm-attach.rules /etc/udev/rules.d/
sudo udevadm control --reload
```

The VM name defaults to `slopbox`. To change it:

```sh
echo 'PICO_VM_DOMAIN=othervm' | sudo tee /etc/default/pico-vm-attach
```

**Stop using SPICE redirection for the Pico**: start `virt-viewer` without the
`--spice-usbredir-*` options, otherwise the viewer and libvirt race for the device.

## Check it

Replug the Pico (or run the `flash` target in the VM), then on the host:

```sh
journalctl -t pico-vm-attach -f
```

You should see `attached 3:N to slopbox` lines. Inside the VM, `lsusb` / `picotool info`
should see the device.

The rule only acts on `add`/`remove` events, so a Pico that is already plugged in when you
install it is not attached. Replug it, or attach it once by hand:

```sh
sudo udevadm trigger --action=add --subsystem-match=usb --attr-match=idVendor=2e8a
```

## Uninstall

```sh
sudo rm /etc/udev/rules.d/90-pico-vm-attach.rules /usr/local/bin/pico-vm-attach
sudo udevadm control --reload
```
