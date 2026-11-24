# Host-side USB forwarding for the Pico

Automatically passes Raspberry Pi USB devices to the development VM through libvirt, so the
Pico follows its reboots between application mode (`2e8a:0009`) and BOOTSEL (`2e8a:000f`)
without SPICE redirection or a focused viewer window.

How it works: a udev rule fires when any vendor `2e8a` device is added or removed on the host
and runs `pico-vm-attach`, which issues `virsh attach-device` / `detach-device --live` with
the device's bus/device address. libvirt manages permissions and isolation for the device.

All of this runs on the **host**, not inside the VM.

## Install

On the host, from the repository checkout (for example the folder shared into the VM):

```sh
sudo install -m 0755 tools/host/pico-vm-attach /usr/local/bin/pico-vm-attach
sudo install -m 0644 tools/host/90-pico-vm-attach.rules /etc/udev/rules.d/
echo 'PICO_VM_DOMAIN=<vm-name>' | sudo tee /etc/default/pico-vm-attach   # as listed by `virsh list --all`
sudo udevadm control --reload
```

`/etc/default/pico-vm-attach` can also set `PICO_VM_URI` (default `qemu:///system`).

**Stop using SPICE redirection for the Pico**: start `virt-viewer` without the
`--spice-usbredir-*` options, otherwise the viewer and libvirt race for the device.

## Check it

Replug the Pico (or run the `flash` target in the VM), then on the host:

```sh
journalctl -t pico-vm-attach -f
```

You should see `attached <bus>:<device> to <vm-name>` lines. Inside the VM, `lsusb` / `picotool info`
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
