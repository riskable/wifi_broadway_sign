WiFi Broadway Sign - Riskable's Reddit Secret Santa Gift 2018
-------------------------------------------------------------
Merry Christmas!  This year I made my Reddit Secret Santa an Internet-connected Broadway marquee sign.  This project is the source code for the ESP32 board that controls the sign.  It is meant to be compiled using the esp-idf development framework which you can learn about here:

https://docs.espressif.com/projects/esp-idf/en/latest/

It was meant to work with WS2811 pixels hooked up to `GPIO16` with three touch pads (wires soldered to some copper tape) connected to `TOUCH0`, `TOUCH2`, and `TOUCH3` (why not `TOUCH1`?  I couldn't get that one to work!).  Having said that, the pins are configurable at the top of the code and you don't need to use the touch sensors at all since you can control it entirely with MQTT!  It uses open source code from the esp-idf examples and the following other projects:

* https://github.com/CalinRadoni/esp32_digitalLEDs
* https://github.com/tonyp7/esp32-wifi-manager

Quickstart
----------
Setup your build environment according to the instructions on the aforementioned website and run:

.. code-block:: shell

    make menuconfig

Make sure you select the "Light Configuration" submenu and enter your username, password, and MQTT broker URL.  For my Secret Santa I used Adafruit.io but any MQTT broker will work.  You'll probably also want to configure the other things in that submenu to your liking.

Once you've saved your config (it saves as `sdkconfig`) you can build the project (assuming you've got your esp-idf environment setup correctly):

.. code-block:: shell

    make

Then you can flash it to your ESP32 board (don't forget to plug it in) and watch the serial output for info/debugging information:

.. code-block:: shell

    make flash && make install

Provisioning
------------
The first time you run this firmware it will start up the wifi-manager access point using the name and password you gave it via `make menuconfig`.  You can then join to that access point from your phone (or whatever) and connect to `192.168.1.1` in your browser.  From there you can select (or enter manually) the wifi network you wish the ESP32 to join.  Once joined the AP (and web server) will shut down.

.. note:: You can reset the wifi settings by holding the power button down for 10 seconds (if using the touch sensors).

MQTT Control
------------
The device will subscribe to five MQTT topics (which are configurable via `make menuconfig`):

* CONFIG_MQTT_TOPIC_CONTROL (e.g. `lightcontrol`): `ON` or `OFF`
* CONFIG_MQTT_TOPIC_COLOR (e.g. `lightcolor`): Hex color values to set the color palette... `#000000` through `#FFFFFF`
* CONFIG_MQTT_TOPIC_MODE (e.g. `lightmode`): Controls the current mode/effect.  One of, `rainbow`, `color`, `enumerate`, `twinkle`, `marquee`, `rmarquee`.
* CONFIG_MQTT_TOPIC_SPEED (e.g. `lightspeed`): Integer value, 1-255
* CONFIG_MQTT_TOPIC_BRIGHTNESS (e.g. `lightbrightness`): Integer value, 1-255

Operating Modes
---------------
* Rainbow:  Blends one pixel to the next in a rainbow of colors.
* Color: A solid color (whatever was set via the `COLOR` topic).
* Enumerate: Blinks each pixel on and off in sequence, rotating between red, green, and blue.  This mode is only used when the wifi configuration is erased (long-press power).
* Twinkle: A twinkling lights effect...  Randomly light 25% of the lights.  Controlled via the `twinkly` global in `main.c`.
* Marquee: Blinks the lights in a forward pattern... Every 3rd pixel is lit according to the color set via `COLOR`.
* Rainbow Marquee: Rainbow version of the Marquee mode.

The Code is a Mess
------------------
I know it.  You know it.  But it works!  Here's the deal:  I suck at C.  My brain just wasn't made for it!  I much prefer Python and Rust.  If I could program an ESP32 board using Rust I would!

BTW: I didn't use micropython because the library support wasn't there (at the time I started) for all the things I wanted to do.
