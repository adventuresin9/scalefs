# Scalefs

A Plan 9 sytle filesystem for USB HID POS scales.  Tested on a ELANE 510 Stamps.com scale.

## Install

Run 'mk' to just produce a .out file.  Run 'mk install' and it will install 'scalefs' into /usr/$user/bin/$objtype.

## How to use

Kill any kb driver that is trying to read the scale.  If the scale was just plugged in, it will be the last 2 instances of kb running.  run 'kill kb' to get a list of kb's that can be killed.

Next, run 'usbtree' to see which endpoint the scale is on.  The run 'scalefs -u N' where N is the endpoit number.

ex. scalefs -u 11

By default, it will mount to /mnt/scalefs.  There will be a 'ctl' and a 'scale' file.  The scale file can be read to get the weight on the scale.  Ctl can be read to get back some info on the scale, and 'tare' can be writen to it to tare the scale.  The tare function is not to the USB spec, so see the code if you need to change it.

https://usb.org/sites/default/files/pos1_03.pdf
