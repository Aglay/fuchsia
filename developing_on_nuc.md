# Developing with Fuchsia on a NUC

This document describes how to get a NUC up and running with Fuchsia.

[1. Get Parts](#parts)<br/>
[2. Create an Installable Fuchsia USB Drive](#usb)<br/>
[3. Prepare the NUC](#nuc)<br/>
[4. Install Fuchsia](#install)<br/>
[5. Update NUC BIOS to allow netbooting](#bios)<br/>

-----

## 1. Get Parts <a name="parts"/>

You’ll need the following:

- USB 3.0 Drive
- NUC
- RAM
- m.2 SSD
- Keyboard
- Mouse
- Monitor that supports HDMI
- HDMI cable
- ethernet cable
- Magnetic tip phillips head screwdriver.

This table shows what I bought from Amazon.

| Item | Link | Notes: |
| ---- | ---- | ------ |
| NUC | [B01MSZLO9P](https://www.amazon.com/gp/product/B01MSZLO9P) | Works fine. |
| RAM | [B01BIWKP58](https://www.amazon.com/gp/product/B01BIWKP58) | Works fine. |
| SSD (Only need one, | [B01IAGSDJ0](https://www.amazon.com/gp/product/B01IAGSDJ0) | Works fine. |
| I bought some of each) | [B00TGIVZTW](https://www.amazon.com/gp/product/B00TGIVZTW) | Works fine. |
| | [B01M9K0N8I](https://www.amazon.com/gp/product/B01M9K0N8I) | Works fine. |
| | | |
| **Optional:** | | |
| Keyboard and Mouse | [B00B7GV802](https://www.amazon.com/gp/product/B00B7GV802) | Works fine.  Next time I'd get a keyboard with a smaller foot print. |
| Monitor | [B015WCV70W](https://www.amazon.com/gp/product/B015WCV70W) | Works fine. |
| HDMI Cable | [B014I8SIJY](https://www.amazon.com/gp/product/B014I8SIJY) | Works fine. |
| USB 3.0 drive | [B01BGTG41W](https://www.amazon.com/gp/product/B01BGTG41W) | Works fine. |

-----

## 2. Create an Installable Fuchsia USB Drive <a name="usb"/>

Full description of what to do can be found [here](https://fuchsia.googlesource.com/install-fuchsia/+/master/README.md).

The concise version is as follows:
Build Fuchsia.
Run “create installer bootfs” script. (If you built release, use -r argument)
Copy the installer bootfs over the user bootfs.
Plug in USB drive.
Unmount the USB drive (via “Disks” if you are on gLinux.  If you unmount via the file browser it won’t work).
Run “create gigaboot bootable usb” script.  (If you built release, use -r argument)
Unplug drive.

-----

## 3. Prepare the NUC <a name="nuc"/>
NUCs don’t come with RAM or an SSD so you need to install them.
<br/><center><img width="50%" src="images/developing_on_nuc/parts.jpg"/></center><br/>

1. Remove the phillips screws in the bottom feet of the NUC.
<br/><center><img width="50%" src="images/developing_on_nuc/nuc_bottom.jpg"/></center>
<br/><center><img width="50%" src="images/developing_on_nuc/nuc_inside.jpg"/></center><br/><br/>
1. Install the RAM.
1. Remove the phillips screw that will hold the SSD in place (phillips screwdriver with magnetic tip is useful here).
1. Install the SSD.
1. Screw the SSD in place using screw from 3.
<br/><center><img width="50%" src="images/developing_on_nuc/parts_installed.jpg"/></center><br/><br/>
1. Replace bottom and screw feet back in.
1.(Optional) Apply fuchsia logo.
<br/><center><img width="50%" src="images/developing_on_nuc/nuc_fuchsia.jpg"/></center><br/><br/>
1. Plug power, ethernet, HDMI, keyboard, and mouse into NUC.

-----

## 4. Install Fuchsia <a name="install"/>

1. Plug in your installable fuchsia usb drive into NUC.
1. Turn on NUC.
1. Wait for NUC to boot into fuchsia.
1. Alt-tab to a terminal if you don’t boot into a terminal.
<br/><center><img width="50%" src="images/developing_on_nuc/terminal.jpg"/></center><br/><br/>
1. Run ‘lsblk’.  This should say there’s a ‘block’ device at 003.
<br/><center><img width="50%" src="images/developing_on_nuc/lsblk.jpg"/></center><br/><br/>
1. Run ‘gpt init /dev/class/block/003’.  Say ‘y’ to the warning.
1. Run ‘install-fuchsia’.
1. Run ‘dm reboot’.
1. Remove usb drive.

At this point the NUC should boot to fuchsia without the usb drive.  It’s using the internal SSD.  But it won’t work with netbooting.  Let’s fix that.

-----

## 5. Update NUC BIOS to allow netbooting <a name="bios"/>

1. Reboot NUC.
1. Press F2 while booting to enter BIOS.
1. In the Boot Order window on the left click the Legacy tab.
1. Uncheck ‘Legacy Boot’.
<br/><center><img width="50%" src="images/developing_on_nuc/bios.jpg"/></center><br/><br/>
1. Press the X in the top right to leave the BIOS.  Ensure you save before exiting.

-----


All done!
