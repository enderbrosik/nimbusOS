NimbusOS for the Raspberry Pi 4 / Pi 400 - stage 1
===================================================

What works: the Pi boots NimbusOS and shows the desktop on HDMI.
Not yet: keyboard and mouse (USB is the next stage), SD card storage for
your files (changes are kept in RAM only), network, sound.

Install
1. Use an SD card that already boots Raspberry Pi OS (Raspberry Pi Imager,
   any version).  Its boot partition ("bootfs") has the firmware NimbusOS
   needs (start4.elf, fixup4.dat).
2. Rename the card's config.txt to config-raspios.txt (to switch back later).
3. Copy nimbus8.img, config.txt and NIMBUS.LOG from this folder onto the
   boot partition.
4. Put the card in the Pi 400 and switch it on.

Every boot message is shown on screen while it starts, and written to
NIMBUS.LOG on the card.  If something goes wrong, take a photo of the
screen or send NIMBUS.LOG.

To go back to Raspberry Pi OS: delete config.txt and rename
config-raspios.txt back to config.txt.
