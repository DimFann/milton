# Fork Info

This fork adds some quality of life features I've been wanting for years, as well as some rendering & functionality improvements.

### Notable additions
* Rebindable hotkeys
* Reorganized UI, with dark mode
* Pressure response tuning, and pressure smoothing
* From 8 to 16 bit values for smoother stroke appearances
* Better precision for stroke placement
* Allow for deeper zoom, start canvas zoom further out for more in/out by default.
* Variable harness for brush and eraser + other options & tilt support
* Rectangular brushes
* Lasso Functions
    * Arm with S
    * D to delete selected strokes
    * Ctrl+T to transform selected strokes
* Common art program shortcuts
    * Hold W and drag to rotate, can be rebound
    * Hold Ctrl+Space and drag to zoom
    * Hold Alt+RMB + scrub to resize brush, alternatively Ctrl+Alt+Drag (PaintToolSai)
    * Hold Alt for eyedropper
* Improved loading times, and responsiveness during loading for stroke-heavy files

### Current Issues
* Layer optimizer function doesn't catch all stray/orphaned strokes
* Cut eraser doesn't always cut strokes out
* Merge down function
    * Eraser strokes get applied to the layer below, or don't get cut out precisely if using stroke conversion feature.
* Untested with Wacom/Huion/XP-Pen manufacturer-provided drivers
* Wintab may not work yet, untested
* Only tested with OpenTabletDriver, using WindowsInk plugin by Kuuuube (Absolute).

### Notes, caution, from DimFann
* I'm not committed to actively maintaining this fork.
* I'm not committed ensuring that every commit I push maintains compatibility with files either by source repo Milton, or previous commits of this fork.
* The vast majority of changes I'll be making and be pushing will by implemented by Copilot (AI). At this point I don't think there's a clean path for a merge back into Milton. While that repo seems inactive, I wouldn't want to burden anyone with a huge pile of vibe code if they're by any chance still watching pull requests.
* I take no credit for the hard work done by Sergio and all other original contributors, or by Copilot for that matter. I'm just someone who draws and really, really, really, misses Mischief.

Preserving the readme below for posterity.

----

![MiltonLogo](http://i.imgur.com/ADgRZUB.png)

[Milton](https://github.com/serge-rgb/milton) is an open source application that lets you Just Paint.

There are no pixels, you can paint with (almost) infinite detail. It feels raster-based but it works with vectors.
It is not an image editor. It is not a vector graphics editor. It is a program that lets you draw, sketch and paint.
There is no save button, your work is persistent with unlimited undo.

### [Latest release](https://github.com/serge-rgb/milton/releases/)

![Milton Paint ss](http://i.imgur.com/4pdHeeI.png)

![zoooom](http://i.imgur.com/fqOhPlr.gif)


What Milton is not:
-------------------

Milton is not an image editor or a vector graphics editor. It's a program that
lets you draw, sketch and paint.

User Manual
===========

If the GUI makes something not-obvious, please create a github issue!

It's very helpful to drag the mouse (or pen) while pressing `space` to pan the
canvas.  Also, switching between the brush and the eraser with `b` and `e`.
You can change the brush size with `[` and `]` and control the transparency
with the number keys.

Here is the  [latest video tutorial](https://www.youtube.com/watch?v=g27gHio2Ohk)

Check out the [patreon page](https://www.patreon.com/serge_rgb?ty=h) if you would like to help out. :)

While on Windows there are binaries available, for Milton on Linux or OSX you will have to compile from source. There are some basic build instructions below. They will probably build, but please be prepared to do a bit of debugging on your end if you run into trouble, since these are not the primary development platforms.

How to Compile
==============

Windows
-------

Milton currently supports Visual Studio 2019.

Other versions of Visual Studio might not work.

To build, install Visual Studio 2019 with the C++ desktop development tools and a Windows SDK. From VS Code, open the Milton folder and run **Terminal: Run Build Task** (Ctrl+Shift+B). The task runs `build.bat`, which locates and initializes the installed Visual Studio C++ tools if they are not already available in the environment.

You can also run the build manually from an x64 Developer Command Prompt:

```
build.bat
```

Milton will be compiled to `build\Milton.exe`


This repo provides a binary SDL.lib that was compiled by running
`build_deps.bat` in the `third_party` directory.


Linux and macOS
---------------

As of 2018-10-24, linux and mac are not officially supported. I (Sergio) would like to support them again but my efforts are currently going into producing a new release for Windows. You can try and compile with the included scripts, but things will likely not work!

On 2021-02-27 a successful build for Linux can be done with these steps:

While in the milton top directory
```
cd third_party/SDL2-2.0.8
mkdir build
cd build
cmake -DVIDEO_WAYLAND=OFF -DCMAKE_INSTALL_PREFIX=linux64 -DCMAKE_BUILD_TYPE=Debug ../
make
make install
```

and then in the milton top directory, so ```cd ../../../```,
```
mkdir build
cd build
cmake ../
make
```

And if successful, you should have an executable called "Milton" that runs.

I did not make this work automatically with CMake, because I don't know CMake.

Versioning scheme
=================

Milton uses a MAJOR.MINOR.PATCH versioning scheme, where MAJOR keeps track of very significant changes, such as a UI overhaul. MINOR keeps track of binary file format compatibility. PATCH is incremented for new releases that do not break file format compatibility. PATCH version gets reset to 0 when the MINOR version increases.

For example, Milton version 1.3.1 can read mlt files produced any previous version, but it can't read files produced by 1.4.0


License
=======

    Milton

    Copyright (C) 2015 - 2018 Sergio Gonzalez

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

Thanks
======

Milton is made with love by Sergio Gonzalez with the help of [awesome
people](https://github.com/serge-rgb/milton/blob/master/CREDITS.md).

