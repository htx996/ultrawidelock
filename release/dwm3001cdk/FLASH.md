# ultrawidelock on the DWM3001CDK: flash guide

A credential lock on one board. Your iPhone carries the key in Wallet, and the lock
opens as you walk up to it, phone in your pocket. No app to install.

Everything is on the DWM3001CDK itself: the radio, the debugger, the lot. There
is nothing to wire and nothing to solder.

| File | What it is |
|---|---|
| `merged.hex` | the firmware: bootloader, credential reader, Matter node, UWB engine |
| `flash.sh` | writes it to the board over the on-board debugger |
| `VERSION.txt` | build details, **and your setup code** |
| `README.txt` | the short version of this guide, in plain text |
| `FLASH.md` | this guide |
| `SHA256SUMS.txt` | checksums for every file above |

## What you need

- A **Qorvo DWM3001CDK** board.
- A **USB cable** (the board has two ports; use the one marked **J-Link**).
- An **iPhone with UWB**, iPhone 11 or newer, on iOS 26 or later.
- A way onto your **Thread** network. The lock talks Thread, not Wi-Fi, so it
  needs one of:
  - An **Apple Home hub with Thread**: a HomePod, HomePod mini, or Apple TV 4K.
    This is the path we test, and the one to pick.
  - A **compatible third-party Thread border router**.
  - Possibly **nothing else at all**: since iOS 18, an iPhone 15 Pro or later can
    pair and control Thread accessories with no hub. We have not verified this
    lock against that path, so treat it as untested here.

If you live with anyone, you want the hub. Without one Apple Home cannot share
the home, and home keys reach other people only by being residents of your
household, so **you would be the only person who can unlock the door**. A hub is
also what gives you unlock notifications, remote lock and unlock, and automations
like Lock When Leaving Home. Going hub-less is a single-occupant setup.

## 1. Install one tool

[nRF Util](https://www.nordicsemi.com/Products/Development-tools/nRF-Util) is a
single executable from Nordic.

**macOS or Linux**

```bash
chmod +x nrfutil
sudo mv nrfutil /usr/local/bin/
nrfutil install device
```

If macOS blocks it, allow it under System Settings, Privacy & Security.

**Windows:** run `nrfutil.exe` from its folder, then `nrfutil install device`.

Check it sees the board: plug in the **J-Link** USB port, then run
`nrfutil device list`. One device should appear.

## 2. Flash it

```bash
bash flash.sh
```

That is the whole step. It takes about twenty seconds and prints your setup code
at the end.

Before it writes anything, `flash.sh` checks this bundle against its own
`SHA256SUMS.txt` and refuses to continue if a file has changed. If you have the
[GitHub CLI](https://cli.github.com) installed it also checks who built the
firmware. See "Check it really came from us" below for what that proves.

Windows without bash: `nrfutil device program --firmware merged.hex --core application --options chip_erase_mode=ERASE_ALL,verify=VERIFY_READ,reset=RESET_SYSTEM`

## 3. Add it to Apple Home

There is no QR sticker on this board, so you type the code instead.

1. Open **Home**, tap **+**, then **Add Accessory**.
2. Tap **More options…**, then **Enter Code**.
3. Type the setup code from **`VERSION.txt`** (also printed by `flash.sh`).
4. Accept the "uncertified accessory" warning. It appears because this is not a
   commercially certified product, which it is not pretending to be.
5. Wait for Home to finish. It joins your Thread network and appears as a lock.

Apple issues the key to your iPhone's Wallet automatically once the lock is added.

## 4. Optional: share it with Home Assistant

The lock holds five Matter fabrics. Apple Home normally uses two and Home
Assistant one. Before sharing, make Home Assistant use the Apple Thread network:

1. In the Home Assistant iOS Companion app, open **Settings > Devices & services
   > Thread > Configure**, send the phone's Apple Thread credentials to Home
   Assistant, refresh, and make that network preferred.
2. If Home Assistant has its own OpenThread Border Router, join it to those
   credentials. Do not create a second Thread network for the lock.
3. Open **Settings > Matter > Add device**, answer **Yes, it's already in use**,
   select Apple Home, and finish the sharing dialog.
4. Operate the lock in both apps, power-cycle it, and try both again.

Home Assistant keeps the current UI instructions in its
[Matter](https://www.home-assistant.io/integrations/matter/) and
[Thread](https://www.home-assistant.io/integrations/thread/) guides.

## 5. Walk up to it

Lock it in Home, walk away about five metres, then walk back with the phone in
your pocket. The Wallet animation plays and the lock opens. You do not need to
take the phone out, wake it, or unlock it.

## Updating the firmware later

No cable needed after the first flash.

1. In Apple Home, open the lock's settings and tap **Turn On Pairing Mode**.
   (Pressing **SW2** on the board does the same thing.)
2. **D10, the blue LED, blinks** while the update window is open. It lasts five
   minutes.
3. Open **nRF Device Manager** on the phone, connect to the board, use the
   **Images** tab, pick the update file, and upload.

Use the **Images** tab, not the guided firmware-upgrade wizard: that flow waits
for a reconnect that this board's update outlasts.

## Running your own firmware

Builds you make yourself will **not** install over the air onto a board flashed
from this release, and that is the signature doing its job rather than a bug.
This board only accepts firmware signed by the key that published this release,
which is exactly what stops a stranger pushing their own.

To own the board yourself, reflash it once over USB. That replaces the
bootloader and the signing key together, so from then on your builds install and
this release's no longer apply:

```bash
git clone https://github.com/ultrawidelock/ultrawidelock.git
cd ultrawidelock
make dfu-key      # your own signing key, once
make bootstrap    # toolchain and SDK, once, takes a while
make build
make flash        # over the board's USB
```

**A current-schema Apple Home pairing survives this.** `make flash` leaves the
current settings area alone, so a board already running the `mf2` image keeps
its fabrics, identity, and keys. Two deliberate exceptions, each costing one
re-pair. Upgrading a v0.3 image is a clean break because the settings layout
moved and the old fabric format is not trusted. And moving to a build carrying
the fabric label field is another: the fabric record grew, and a record whose
length does not match the one expected is dropped at load rather than
half-read. Either way, remove the old controller records and pair Apple Home
and Home Assistant again. You can also set your own setup code at this point, which is
one line of configuration.

It is a one-way switch by design: the board belongs to whoever last flashed it
with a cable.

## Check it really came from us

`SHA256SUMS.txt` proves your download arrived intact. It cannot prove who built
it: it travels in the same zip as the files it lists, so whoever could alter the
firmware could alter the checksums in the same motion.

Every file published with this release is separately signed at build time, by
the workflow that built it, using [Sigstore](https://www.sigstore.dev). That
signature is what proves origin, and anyone can check it without trusting the
release page or whoever handed them the zip:

```bash
gh attestation verify merged.hex --repo ultrawidelock/ultrawidelock
```

It prints the repository, the commit and the workflow run that produced the
file. It needs the [GitHub CLI](https://cli.github.com) and takes a few seconds.
`flash.sh` runs the same check for you when the CLI is installed, and says so
either way.

You can check the zip itself the same way, before unzipping it:

```bash
gh attestation verify ultrawidelock-dwm3001cdk.zip --repo ultrawidelock/ultrawidelock
```

## Read this before you trust it with anything

This is a hobby project, not a product. Three specific things, plainly:

- **The setup code above is public.** It is printed in this guide's folder and it
  is the same for everyone who flashes this release, because nothing on the board
  can display a code of its own. Anyone within Bluetooth range while the lock is
  waiting to be added, or while you have a pairing window open, can add it to
  *their* home instead of yours. Commission it promptly and do not leave pairing
  mode on. If that is not good enough for you, build from source and set your own
  code: it is one line of configuration.
- **Whoever published this release holds the update key.** The board only runs
  firmware signed by that key, which is what stops a stranger installing their
  own. It also means you are trusting the publisher. See "Running your own
  firmware" below, which takes one command.
- **Debug access is left open on purpose.** Anyone who can hold the board in
  their hands can read its memory, including the lock's private key. Locking that
  down is a one-way door that would also destroy the key if it were ever needed,
  so it is deliberately not locked. Treat physical access to the board as
  equivalent to having a key.

Do not secure anything valuable with this.

## If something goes wrong

| What you see | What to do |
|---|---|
| `nrfutil device list` shows nothing | Use the **J-Link** USB port, not the other one. Try another cable. |
| Flashing fails to connect | Unplug, wait five seconds, plug back in. On macOS, quit any SEGGER or nRF Connect app first. |
| Home never finds the accessory | Check your Thread hub or border router is reachable, then power-cycle the board and retry. |
| Apple works but Home Assistant cannot find it | Import the Apple Thread credentials into Home Assistant, make them preferred, join the Home Assistant border router to them, then share the existing Apple accessory again. |
| Home sits on "Adding to Home" | Give it two minutes. If it fails, let the Matter fail-safe expire before retrying; a failed attempt does not erase a working controller. |
| A controller was removed from its app but still occupies the lock | From a controller that still reaches it, use Home Assistant's **Manage fabrics** action to remove only the stale fabric. |
| It pairs but never unlocks on approach | Confirm the key is in Wallet (Wallet app, look for the lock), and that the phone has UWB. |
| No controller can reach it and you must start over | Hold **SW2** and tap **RESET**. That clears every Matter and Home Key identity so you can add it again. |

More detail, and the build-it-yourself route, at
<https://github.com/ultrawidelock/ultrawidelock>.
