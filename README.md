# brightnessd #

_brightnessd_ regulates screen brightness depending on user input (in)activity: When inactive for a certain period of time, _brightnessd_ darkens the screen in two consecutive stages, without freezing screen content. Upon user input activity, the screen's previous brightness is restored.

_brightnessd_ is most useful in combination with window managers such as [awesome](http://awesome.naquadah.org/) since Desktop Environments mostly provide such a functionality via their power management utilities.

## Features ##

- dims the screen brightness in two stages
- supports [XRandR](http://www.x.org/wiki/Projects/XRandR/) as well as [sysfs](https://www.kernel.org/doc/Documentation/filesystems/sysfs.txt) backend
    - when using the XRandR backend for brightness adjustment, no (root) write permissions to `/sys/class/backlight/<backlight>/*` are required
    - if XrandR is not supported by the video card driver, the sysfs backend can be enabled via compile-time switch
- uses the [X11 Screen Saver Extension](http://www.x.org/releases/X11R7.7/doc/scrnsaverproto/saver.html) to determine user (in)activity, i.e., no polling of input devices or idle times
- no screen content freeze when dimmed, i.e., you can continue watching videos -- albeit a bit darkened

## Installation & Configuration ##

First, install the following libraries and their development headers:

* [libxcb](http://xcb.freedesktop.org/)
* [libxau](http://xorg.freedesktop.org/)
* [libxdmcp](http://xorg.freedesktop.org/)

Then checkout, make, and run _brightnessd_
```bash
git clone https://github.com/stormc/brightnessd.git
cd ./brightnessd
make
./brightnessd
# press CTRL+C to abort
```

To install _brightnessd_ to `/usr/bin/brightnessd`, run as root
```bash
make PREFIX=/usr install
```


The default compiler is set to [`clang`](http://clang.llvm.org/). If [`gcc`](https://gcc.gnu.org/) should be used instead, provide the `CC=gcc` option to `make`, e.g.,
```bash
make CC=gcc
```

The `make` target `sysfs` uses the sysfs backend while the default `xrandr` target uses the XRandR backend.
When using the sysfs backend, the path to the directory containing the files `brightness`, `max_brightness`, and `actual_brightness` should be specified via the `SYSFS_BACKLIGHT_PATH="/sys/class/backlight/<directory>"` option to `make`. It defaults to `/sys/class/backlight/intel_backlight/`.
```bash
make sysfs CC=gcc SYSFS_BACKLIGHT_PATH="/sys/class/backlight/intel_backlight"
```

_brightnessd_ dims the screen in two stages corresponding to [X11 Screen Saver Extension](http://www.x.org/releases/X11R7.7/doc/scrnsaverproto/saver.html)'s `timeout` and `cycle` values. Upon `timeout` seconds of user input inactivity, it dims the screen to `DIM_PERCENT_TIMEOUT`% of its maximal brightness. Upon further inactivity for `cycle` seconds, it dims the screen to `DIM_PERCENT_INTERVAL`% of its maximal brightness. Both values can be defined by providing `DIM_PERCENT_TIMEOUT=<value>` and `DIM_PERCENT_INTERVAL=<value>` options to `make`, e.g,

```bash
make sysfs CC=gcc SYSFS_BACKLIGHT_PATH="/sys/class/backlight/intel_backlight" DIM_PERCENT_TIMEOUT=40 DIM_PERCENT_INTERVAL=20
```
`DIM_PERCENT_TIMEOUT` defaults to 40% and `DIM_PERCENT_INTERVAL` defaults to 20% of the maximal screen brightness.

Use `xset s 240 60` to set `timeout` to 240 seconds and `cycle` to 60 seconds, respectively. See `man 1 xset` for further options to set with respect to the screensaver.


## Q&A ##

#### Come on, yet another brightness daemon? There are [brightd](http://www.pberndt.com/Programme/Linux/brightd/index.html), ... ####

True. _brightnessd_ was done just for fun and to code some application in C using [xcb](http://xcb.freedesktop.org/).

#### Why is most configuration possible at compile-time only? ####

I didn't feel the need to implement, e.g., [getopt](http://www.gnu.org/software/libc/manual/html_node/Getopt.html). However, if you do, Pull Requests are very welcome :)

#### brightnessd behaves strangely, what can I do? ####

Please recompile _brightnessd_ with the `debug` or `debug_sysfs` `make` target, respectively, to get more information on what's going on while _brightnessd_ runs. The resulting log is usually helpful in identifying problems or bugs.

#### I found a bug! I'm missing a feature! ####

Pull Requests are very welcome, feel encouraged to provide a patch!


## License ##

Copyright © 2015 Christian Storm

Released under the [MIT License](http://opensource.org/licenses/MIT), see LICENSE for details.
