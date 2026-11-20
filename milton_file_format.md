Milton File (.MLT) format v1
============================

The major number in Milton's version corresponds to the maximum MLT file format
it can read. For instance, Milton x.N.x can read MLT files vN, vN-1, ..., v1.
At the time of this writing there is only Milton v1.x.x and MLT v1.

The files src/persist.(c|h), functions `milton_save` and `milton_load` serve as
documentation for the file format. If this is not enough, please contact me at
bigmonachus@gmail.com

The color history keeps five swatches in the main file body for older readers.
Newer saves append an optional `HST1` footer (four-byte tag, followed by five
RGBA `float32` swatches) for the additional colors.

Stroke flag bit 5 formerly enabled Preserve opacity. It is now reserved and
ignored: old flagged strokes render as ordinary paint. Bit 6 still denotes blur.
No stroke layout or file-format version change is required.

