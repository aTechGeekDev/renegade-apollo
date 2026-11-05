# Windows on your Mi 10T — a beginner's guide

This is the plain-English version. If words like "fastboot" scare you, start here. There's a
[glossary](#glossary) at the bottom — every bold scary word is explained there.

---

## What is this, really?

The **Renegade Project** lets you run **real Windows 11** (the ARM version) on an Android phone,
instead of Android. Your Mi 10T (code-named **`apollo`**) is a supported phone. You don't delete
Windows-the-app or run it in a window — the phone boots straight into Windows, like a tiny PC.

This repo is where we're improving how the Mi 10T's hardware (touchscreen, Bluetooth, buttons…)
talks to Windows.

---

## ⚠️ Read this before you touch anything

Be honest with yourself about what you're getting into:

1. **This is a hobby experiment, not a finished product.** Think "cool weekend project," not
   "my new daily phone."
2. **It erases everything on your phone.** All apps, photos, messages — gone. Back up first.
3. **You can brick the phone** (turn it into a paperweight) if you flash the wrong thing. It's
   avoidable if you go slow and read carefully, but the risk is real.
4. **Lots of stuff won't work yet.** On the Mi 10T today, expect Windows to *boot and show the
   desktop*, but things like **touch, Wi-Fi, sound, and the camera may not work** until more
   development is done (that's literally what this project is doing). **There is no cellular/calling
   at all.** See [what works](#what-works-and-what-doesnt-right-now).
5. **If you only have one phone and you need it, don't do this on it.** Use a spare.

If that's all okay with you — welcome, this is fun. 🙂

---

## What you'll need

- 🔓 A Mi 10T / Mi 10T Pro with an **unlocked bootloader** (we unlock it in Step 2).
- 💻 A **Windows PC** (easiest for the Windows-imaging part).
- 🔌 A good **USB-C cable** and a bit of patience.
- 🧰 Free tools: **`adb` and `fastboot`** (the "Platform Tools"), and **7-Zip**.
- 💾 Roughly **20 GB** free on the phone and some space on the PC.
- ☕ An hour or three. First boots are slow.

---

## The big picture (5 phases)

1. **Back up** your phone so you can undo everything.
2. **Unlock** the bootloader (lets you install non-Xiaomi software).
3. **Get the files**: the Renegade UEFI for `apollo`, Windows 11 ARM64, and the driver pack.
4. **Assemble** a Windows install using a deployment tool on your PC.
5. **Boot** Windows on the phone and go through setup.

---

## Step by step

### Step 1 — Back up everything
- Copy your photos/files off the phone (to your PC or cloud).
- In a custom recovery (**OrangeFox** works on apollo), make a backup of these partitions and
  save them on your PC: `persist`, `modemst1`, `modemst2`, `fsg`, `fsc`, `boot`, `dtbo`,
  `vbmeta`. These are your "get back to normal" insurance. **Don't skip this.**

### Step 2 — Unlock the bootloader
- This is Xiaomi's official unlock. Search *"unlock bootloader Mi 10T"* and use Xiaomi's Mi
  Unlock tool. Xiaomi makes you **wait (often 7 days)** after requesting — that's normal.
- Unlocking **wipes the phone**. (You backed up in Step 1, right?)

### Step 3 — Get the Renegade files for apollo
- Go to the official project: the **Renegade Project** site and its wiki, and the **apollo**
  thread on **XDA-Developers**. Download their latest **UEFI / boot image for `apollo`**.
- Double-check your phone's code-name is **`apollo`** (Settings → About, or ask in the thread).
  There's a trap: some sites use `apollo` for the *Mi 10T Lite*, which is a **different phone** —
  this guide is only for the Mi 10T / 10T Pro.
- *(Advanced, optional:* this repo can build the UEFI and improve the hardware support — see
  [`install-runbook.md`](install-runbook.md). Beginners should use the ready-made download.)

### Step 4 — Get Windows 11 ARM64 + the driver pack
- You need the **ARM64** version of Windows 11 (not the normal x64 one). The usual beginner way
  is a tool like **UUP dump** that downloads an official ARM64 image for you.
- You need the **driver pack** for this phone's chip (Snapdragon 865 / "Kona"):
  [`WOA-Project/windows_silicon_qcom_kona`](https://github.com/WOA-Project/windows_silicon_qcom_kona).
  Download it as a ZIP.

### Step 5 — Put Windows together (on the PC)
- The community provides a **deployment tool** (follow the current Renegade/WOA guide for the
  exact one) that takes your Windows ARM64 image + the driver pack and writes a ready-to-boot
  Windows onto the phone's storage. Let it inject the drivers for you — don't do it by hand.

### Step 6 — Boot Windows
- Put the phone in **fastboot mode** (power off, then hold **Volume Down + Power** until you see
  "fastboot").
- On the PC, run:
  ```bash
  fastboot boot uefi.img
  ```
  (use the real file name you downloaded). **Nothing permanent happens** with `fastboot boot` —
  it's a test boot. If it goes wrong, just reboot and you're back to normal.
- The phone should start the Windows boot. **The very first boot can take a long time** — leave
  it alone, don't panic, don't unplug.

### Step 7 — First-time Windows setup
- Windows setup appears. Because touch may not work yet, **plug a USB mouse/keyboard into the
  phone** (via a USB-C hub) to click through setup.
- If it reaches the desktop — congratulations, your phone is running Windows. 🎉

---

## What works and what doesn't (right now)

Honest status for the Mi 10T. This improves over time, so check the project for updates.

| Works / likely works | Partly / needs the latest build | Not yet / never |
| --- | --- | --- |
| Boots to the Windows desktop | **Touch** (needs a Novatek driver — in progress here) | **Calls / SMS / mobile data** (no cellular) |
| Screen (display) | **Bluetooth, buttons** (ACPI added here; needs testing) | Cameras |
| USB (mouse, keyboard, drives) | Wi-Fi, sound, charger info (being worked on) | Fingerprint |
| Storage | | Fast/stable "daily driver" experience |

➡️ Short version: **great for showing off and tinkering; not a replacement for your phone.**

---

## If something goes wrong

- **Phone won't boot / stuck:** hold **Power** ~10-15s to force it off, then boot to recovery
  (**Volume Up + Power**) and restore the backup from Step 1.
- **Back to Android:** reflash the stock Xiaomi ROM with Xiaomi's flash tool (MiFlash). This is
  your ultimate undo button — keep a stock ROM downloaded *before* you start.
- **"fastboot" can't see the phone:** install the USB drivers, try another cable/port, make sure
  the phone actually says *fastboot* on screen.
- **Don't permanently flash anything** until a plain `fastboot boot` has worked for you at least
  once.

---

## Glossary

- **Bootloader** — the first program that runs when the phone turns on; decides what OS to load.
- **Unlock the bootloader** — Xiaomi's permission to install non-official software. Wipes the phone.
- **fastboot** — a mode (and PC command) for sending images to the phone over USB.
- **`fastboot boot file.img`** — runs an image *once* without installing it. Safe for testing.
- **Recovery (OrangeFox)** — a mini-tool you boot into to back up/restore/flash things.
- **UEFI** — the "firmware" Renegade installs that lets Windows boot on the phone.
- **ARM64** — the chip type in phones; Windows must be the ARM64 edition to run here.
- **Driver** — software that lets Windows talk to a piece of hardware (screen, touch, Wi-Fi…).
- **ACPI** — tables that *describe* the phone's hardware to Windows so drivers can find it. The
  `.asl` files in this repo are exactly that.
- **codename / `apollo`** — the Mi 10T's internal name; downloads are labeled by codename.
- **brick** — a phone broken so badly it won't turn on. Backups + going slow prevent it.

---

## Where to get help
- The **Renegade Project** website/wiki and its community chat (the official download + the
  up-to-date deployment steps live there).
- The **`apollo` thread on XDA-Developers** (people running this on the same phone).
- This repo's [`install-runbook.md`](install-runbook.md) for the technical version, and
  [`README.md`](../README.md) for what hardware support is in progress.

> Go slow, read twice, back up first. Have fun. 🚀
