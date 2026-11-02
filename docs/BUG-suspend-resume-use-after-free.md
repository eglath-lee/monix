# BUG: `monix-gui` crashes with GPF after every suspend/resume (use-after-free in `idctl_disconnect`)

**Status**: fixed on `fix/suspend-resume-uaf` (`driver: keep struct idctl_dev alive until the last
fd closes`, `gui: drop the stale device fd on ENODEV so reconnect works`) and **verified on
hardware** — see "Verification" at the end.
**Component**: `driver/audient_id.c` (kernel module)
**Severity**: high — the GUI daemon dies on every system resume; a stale userspace fd touching the freed
`idctl_dev` can hit arbitrary kernel memory.
**Affects**: Monix `driver/audient_id.c` as of `edc9ba1`-era tree (v0.1 DKMS module), Linux 7.2.x
**Reported by**: end user on MSI PRO Z690-A / Audient iD24, Arch Linux

---

## Summary

`monix-gui` keeps a long-lived fd on `/dev/audient_idN` and polls it with `IDCTL_IOC_XFER`.
Every time the USB device is suspended and re-enumerated (system suspend, hibernate, or a manual
`pm_test=devices`), the kernel calls `idctl_disconnect()`, which does `misc_deregister()` and then
`kfree(dev)` **while the fd is still open and still usable**. The next ioctl from `monix-gui`
dereferences the freed `struct idctl_dev` (including `dev->lock`) and the process dies with a
general protection fault.

## Environment

| | |
|---|---|
| Distro | Arch Linux, kernel `7.2.11-arch1-1` |
| Hardware | MSI PRO Z690-A WIFI DDR4, BIOS 1.M0 (2025-08-08) |
| Audio device | Audient iD24 (`2708:000d`), USB, internal Genesys hub chain |
| Driver | `audient_id` v0.1, DKMS `/usr/src/audient_id-0.1`, module tainted (`OE`) |
| Userspace | `monix-gui`, long-running, opens `/dev/audient_id0` once at startup |

## Steps to reproduce

The shortest reproducer does not require a real sleep — the kernel's `pm_test` facility is enough:

```bash
# device has been probed, monix-gui running and holding /dev/audient_id0
echo devices | sudo tee /sys/power/pm_test
echo mem | sudo tee /sys/power/state      # suspends all devices for 5 s, then resumes
# reset for normal operation afterwards
echo none | sudo tee /sys/power/pm_test
```

Result: `monix-gui` dies within ~0.2 s after `PM: suspend exit` and leaves a zombie
(`[monix-gui] <defunct>`). A real `systemctl suspend` / `systemctl hibernate` reproduces the same
thing on every resume.

## Kernel log

```
PM: suspend exit
Oops: general protection fault, probably for non-canonical address 0x666e69642e521162: 0000 [#1] SMP NOPTI
CPU: 4 UID: 1000 PID: 937 Comm: monix-gui Tainted: G           OE       7.2.11-arch1-1 #1 PREEMPT(full)
Hardware name: Micro-Star International Co., Ltd. MS-7D25/PRO Z690-A WIFI DDR4(MS-7D25), BIOS 1.M0 08/08/2025
RIP: 0010:osq_lock+0x5c/0x110
Code: ... 48 89 4a 08 <48> 89 11 ...
RAX: ffffffffffff8a60  RBX: 0000000000000001  RCX: 666e69642e521162
RDX: ffff8a681f534200  RSI: ffff8a60c38f7e64  RDI: ffff8a60c38f7e64
Call Trace:
 <TASK>
 __mutex_lock.constprop.0+0xbe/0x930
 idctl_do_xfer+0x3f/0x160 [audient_id]
 idctl_ioctl+0xa7/0x120 [audient_id]
 __x64_sys_ioctl+0xb9/0x100
 do_syscall_64+0xaa/0x660
 entry_SYSCALL_64_after_hwframe+0x76/0x7e

audient_id 1-1:1.4: Audient iD control relay removed (/dev/audient_id0)
...
note: monix-gui[937] exited with preempt_count 1
```

`RCX = 0x666e69642e521162` is ASCII-ish garbage (`"b.R.dinf"`), i.e. the `struct mutex` that
`osq_lock` is manipulating no longer contains a valid `owner`/`wait_list` — classic
use-after-free of the containing `struct idctl_dev`.

Note the ordering in the log: `idctl_disconnect()` runs during the device suspend, then
`PM: suspend exit`, and only *after* that does `monix-gui`'s next ioctl fault.

## Root cause

`driver/audient_id.c`:

```c
static void idctl_disconnect(struct usb_interface *intf)
{
	struct idctl_dev *dev = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);
	misc_deregister(&dev->misc);     /* (1) removes /dev/audient_idN */

	mutex_lock(&dev->lock);
	usb_put_dev(dev->udev);
	dev->udev = NULL;
	mutex_unlock(&dev->lock);

	ida_free(&idctl_ida, dev->id);
	dev_info(&intf->dev, "Audient iD control relay removed (/dev/%s)\n", dev->name);
	kfree(dev);                      /* (2) frees dev */
}
```

`misc_deregister()` only unregisters the device node — it does **not** wait for already-open file
descriptors to be closed, and `file_operations` has no `.release` callback here. Therefore after
`(2)`:

* `file->private_data` of every still-open fd points into freed memory;
* `idctl_do_xfer()` does `mutex_lock_interruptible(&dev->lock)` on freed memory → GPF above;
* even without the fault, the `if (!dev->udev)` check and the `dev_dbg(&dev->udev->dev, ...)` error
  path can dereference a stale `udev` (the `udev` NULL-ing and the check are only safe while
  `dev` itself is alive).

The same window also exists in the opposite order: `idctl_open()` can race a concurrent
`idctl_disconnect()` and obtain a `dev` that is about to be freed.

## Suggested fix

Keep `struct idctl_dev` alive until both sides are done with it. Minimal shape:

1. Add a refcount (or at least an open counter) to `struct idctl_dev`.
2. `idctl_open()`: take a reference and store `dev` in `file->private_data`.
3. `idctl_disconnect()`: `usb_set_intfdata(intf, NULL)`, `misc_deregister()`, then under
   `dev->lock` do `usb_put_dev(dev->udev); dev->udev = NULL;` — **do not `kfree()`**.
4. Add `.release`: drop the reference taken in `open()`; when it reaches zero (i.e. no open fds and
   the device has been disconnected), `ida_free()` + `kfree(dev)`.
5. Guard the `dev_dbg()` in the error path with a local copy / the lock, so `udev` is read only
   under `dev->lock` (or with an extra `usb_get_dev()` reference held for the duration of the
   transfer).

A ready-to-adapt sketch:

```c
struct idctl_dev {
	struct usb_device   *udev;
	struct miscdevice    misc;
	struct mutex         lock;
	char                 name[32];
	int                  id;
	struct kref          ref;
	bool                 disconnected;
};

static int idctl_open(struct inode *inode, struct file *file)
{
	struct idctl_dev *dev = container_of(file->private_data, struct idctl_dev, misc);
	kref_get(&dev->ref);
	file->private_data = dev;
	return 0;
}

static int idctl_release(struct inode *inode, struct file *file)
{
	struct idctl_dev *dev = file->private_data;
	kref_put(&dev->ref, idctl_free);   /* free only when disconnected && no fds */
	return 0;
}

static void idctl_disconnect(struct usb_interface *intf)
{
	struct idctl_dev *dev = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);
	misc_deregister(&dev->misc);

	mutex_lock(&dev->lock);
	usb_put_dev(dev->udev);
	dev->udev = NULL;
	dev->disconnected = true;
	mutex_unlock(&dev->lock);

	kref_put(&dev->ref, idctl_free);   /* module's own reference */
}
```

(`idctl_free()` = `ida_free(&idctl_ida, dev->id); kfree(dev);`, only called from `kref_put`.)

A cheaper stop-gap, if a refcount rewrite is not desired: keep `dev` allocated forever (never
`kfree` in `disconnect`), i.e. leak one small structure per device probe. That is ugly but removes
the UAF; it should not be merged as the final fix.

## Impact / user-visible behaviour

* `monix-gui` is a long-running tray/scratchpad app and is re-launched by the user's session
  autostart; after every sleep it silently dies, so the iD24 mixer control is dead until the user
  restarts it manually.
* Because the fault happens inside the kernel on a stale mutex, the crash is not contained to the
  process in principle (arbitrary write through `osq_lock` on attacker-influenced-ish memory); it is
  a kernel memory-safety bug, not just an app crash.

## Notes

* `snd-usb-audio` and the rest of the audio path are unaffected; the fault is contained to the
  `audient_id` DFU-interface relay.
* Confirmed on this machine that a plain `systemctl hibernate` (S4) and `systemctl suspend` (S3)
  both trigger it; no special userspace activity is required beyond `monix-gui` holding the fd.
* This report is written from a downstream fork (`eglath-lee/monix`), please upstream it
  (`sKuhLight/monix`) as an issue/PR.

## Verification (2026-11-02)

Same machine and device as the report (Arch Linux, kernel `7.2.11-arch1-1`, iD24 `2708:000d`),
using the `pm_test` reproducer above, three times, including runs before and after a reboot:

* `monix-gui` survives the resume (before the fix it was killed by the GPF ~0.2 s after
  `PM: suspend exit`);
* `dmesg` contains exactly `... relay removed (/dev/audient_id0)` → `... relay ready at
  /dev/audient_id0` → `PM: suspend exit`, with no `Oops`/GPF;
* the GUI's fd moves to the re-created node (`21->/dev/audient_id0 inode=1544` before,
  `21->/dev/audient_id0 inode=1546` after) and `monixctl status` answers again — the mixer
  recovers on its own instead of needing an app restart;
* `/sys/module/audient_id/refcnt` is 1 while the GUI holds the fd and returns to 0 after it
  exits, i.e. the reference accounting balances.

One side effect worth knowing: after an `Oops` from the old (buggy) module, a module reference
is leaked, so `modprobe -r audient_id` keeps failing with `Module audient_id is in use` even
with nothing holding the fd; only a reboot clears it.

The report's cheap stop-gap (never `kfree()` in `disconnect()`, leaking one struct per probe)
was not used.
