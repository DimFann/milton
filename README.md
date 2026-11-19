# Fork Info
This fork adds some quality of life features I've been wanting for years, as well as some rendering & functionality improvements.

----

### Downloading
[Releases](https://github.com/DimFann/milton/releases)
1) Download milton_YYYYMMDD_HHmm.zip from the latest release.
2) Extract to a folder of your choosing.
3) Run milton.exe

### Notable additions
* Rebindable hotkeys
* Reorganized UI, with dark mode
* Pressure response tuning, pressure and position smoothing
* Pen and eraser pressure-opacity options are saved separately, including when you quit with the eraser active. Opening a canvas keeps the current pressure-opacity option.
* From 8 to 16 bit color values for smoother stroke appearances
* The triangle color picker keeps the selected hue fully saturated along the edge from pure hue to black.
* Below the color wheel, H/S/V sliders preview the colors they produce. Type a hue in degrees or saturation and value as percentages, then press Enter. Ten recent color swatches sit below the sliders.
* Better precision for stroke placement
* Allow for deeper zoom, start canvas zoom further out for more in/out by default.
* Variable hardness for brush and eraser + other brush options & tilt support
* **Preserve opacity** in Pen tool options prevents new strokes from adding opacity over matching-color paint on the same layer, including strokes made before this feature or saved in previous sessions. Existing paint is unchanged; higher-pressure or higher-opacity strokes can raise the maximum. Other colors do not break this behavior, and erasing reduces the protected opacity so you can repaint erased areas. Press **A** to toggle it, or rebind **Toggle Preserve opacity** in Hotkeys. Layer optimization and erased-stroke cleanup remain unavailable on layers containing preserving strokes.
* Rectangular brushes
* **Blur brush (F)** softens existing paint on the current layer without adding the selected color. Select Blur in the left toolbar; size, strength, hardness, and pressure controls appear in Tool Options. Strength ramps up quickly: 75% is approximately the original full-strength effect; previously saved strokes retain their appearance. Each stroke is undoable and saved with the canvas; repeated strokes increase the blur. Layer optimization and erased-stroke cleanup are disabled on layers containing blur strokes.
    * Completed blur results and Preserve opacity history are reused across stroke starts and ends, so drawing does not reapply all previous blurs. Panning, zooming, and rotating temporarily show paint without blur for a responsive preview, including while navigation controls remain held without movement. Full quality returns once all navigation controls are released and the view has settled for 120 ms (or immediately when drawing starts after navigation ends). Export always uses full quality. Editing earlier strokes rebuilds the cache without changing the saved artwork.
* Lasso Functions
    * Arm with S
    * D to delete selected strokes
    * Ctrl+T to transform selected strokes
* Common art program shortcuts    
    * Hold W and drag to rotate, can be rebound
    * Hold Ctrl+Space and drag to zoom
    * Hold Alt+RMB + scrub to resize brush, alternatively Ctrl+Alt+Drag (PaintToolSai)
    * Hold Alt for eyedropper
    * Press V to flip canvas horizontally
* Improved loading times, and responsiveness during loading for stroke-heavy files

### Current Issues
* Layer optimizer function doesn't catch all stray/orphaned strokes
* Cut eraser doesn't always cut strokes out
* Merge Down is temporarily hidden from the Layers panel; the implementation is retained.
    * Eraser strokes get applied to the layer below, or don't get cut out precisely if using stroke conversion feature.
* Untested with Wacom/Huion/XP-Pen manufacturer-provided drivers
* Wintab may not work yet, untested
* Only tested with OpenTabletDriver, using WindowsInk plugin by Kuuuube (Absolute).

Milton reads Windows Ink pen position and pressure history before each native message expires. A
lower display refresh rate can delay the on-screen preview, but it no longer discards the pen samples
that Windows groups into a single message.

### Notes, caution, from DimFann
* I'm not committed to actively maintaining this fork.
* I'm not committed to ensuring that every commit I push maintains compatibility with files created either by source repo Milton, or previous commits of this fork.
* The changes made are just what work for how I draw. If you have suggestions DM me on twitter, but the goal of this fork will mostly be to minimize friction with how I want to draw.
* The vast majority of changes I'll be making and pushing will be implemented by Copilot (AI). At this point I don't think there's a clean path for a merge back into Milton. While that repo seems inactive, I wouldn't want to burden anyone with a huge pile of vibe code if they're by any chance still watching pull requests.
* I take no credit for the hard work done by Sergio and all other original contributors, or by Copilot for that matter. I'm just someone who draws and really, really, really, misses Mischief.

![LatestUi](https://i.imgur.com/iwFWa1o.png)

**Preserving the readme below for posterity.**

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

