.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver corsair-hxi-psu
==========================

Supported devices:

  * Corsair HXi ATX power supplies (HX750i, HX850i, HX1000i and HX1200i)

Author: Jack Doan

Description
-----------

This driver provides a sysfs interface for the Corsair HXi series of ATX
power supplies.

Usage Notes
-----------

Since it is a USB device, hot-swapping is possible. The device is auto-detected.

Sysfs entries
-------------

* in0_input / in0_label    Voltage on ATX_12V
* in1_input / in1_label    Voltage on ATX_5V
* in2_input / in2_label    Voltage on ATX_3V
* in3_input / in3_label    Input AC voltage

* curr0_input / curr0_label    Current on ATX_12V
* curr1_input / curr1_label    Current on ATX_5V
* curr2_input / curr2_label    Current on ATX_3V

* power1_input / power1_label    Power on ATX_12V
* power2_input / power2_label    Power on ATX_5V
* power3_input / power3_label    Power on ATX_3V
* power4_input / power4_label    Total AC Power

* temp1_input   Temperature before PSU fan
* temp2_input   Temperature after PSU fan

Future work
------------

* Adding support for monitoring and control of the fan
* Getting and setting the overcurrent-protection mode
* Testing on other lines of Corsair PSUs (RMi, AXi)
* Broadening support to other "smart" ATX PSUs (NZXT, Seasonic)
* Potentially pulling this into the PMBus code
