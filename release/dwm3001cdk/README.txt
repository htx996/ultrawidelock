ultrawidelock @VERSION@   ·   DWM3001CDK

A credential lock on one board. Your iPhone carries the key in Wallet, and the lock
opens as you walk up to it, phone in your pocket. There is no app to install.


START HERE
──────────────────────────────────────────────────────────────────────────────

  Open FLASH.md in any text editor or on GitHub. That is the full guide, much
  easier to follow than this file.


THE SHORT VERSION
──────────────────────────────────────────────────────────────────────────────

  1.  Install nRF Util from nordicsemi.com, then run:  nrfutil install device
  2.  Plug the board into the USB port marked J-Link.
  3.  Run:  bash flash.sh
  4.  In Apple Home: +, Add Accessory, More options, Enter Code.

  SETUP CODE @SETUP_CODE@

  Then lock it in Home, walk five metres away, and walk back with the phone in
  your pocket. It opens without you touching anything.

  To add Home Assistant too, first import the Apple Thread credentials into
  Home Assistant and make that network preferred. Then use the Home Assistant
  app's Matter > Add device > already in use path to share from Apple Home.
  Do not create a second Thread network. FLASH.md has the exact steps.


WHAT IS IN HERE
──────────────────────────────────────────────────────────────────────────────

  merged.hex        the firmware: bootloader, credential reader, Matter node, UWB
  flash.sh          writes it to the board over the on-board debugger
  FLASH.md          the full guide                              <-- start here
  README.txt        this file
  VERSION.txt       what this build is, and your setup code
  SHA256SUMS.txt    checksums for every file above


CHECK IT REALLY CAME FROM US
──────────────────────────────────────────────────────────────────────────────

  SHA256SUMS.txt only proves your download is intact. It cannot prove who
  built it: anyone who could alter the firmware could alter the checksums in
  the same motion.

  This release is signed. To prove these bytes came out of ultrawidelock's CI,
  run this against the zip you downloaded:

    gh attestation verify ultrawidelock-dwm3001cdk.zip --repo ultrawidelock/ultrawidelock

  It checks a Sigstore signature created while the release was being built,
  and prints the commit and workflow that produced it. It needs the GitHub CLI
  (cli.github.com) and a few seconds. flash.sh runs it for you when it can.


BEFORE YOU TRUST IT WITH ANYTHING
──────────────────────────────────────────────────────────────────────────────

  This is a hobby project, not a product. Three things in particular:

    ·  The setup code above is public, and identical for everyone who flashes
       this release. Anyone in Bluetooth range while the lock is waiting to be
       added can add it to their home instead of yours.
    ·  Debug access is left open on purpose. Anyone holding the board can read
       its memory, including the lock's private key.
    ·  Whoever published this release holds the key that signs its updates.

  FLASH.md explains all three, and how to take the board over yourself with
  one USB reflash. Do not secure anything valuable with this.


                        https://github.com/ultrawidelock/ultrawidelock
