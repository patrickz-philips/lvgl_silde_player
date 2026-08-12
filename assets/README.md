# Slide Assets

The slide player does not compile image assets into firmware. At runtime it
loads `/sdcard/1.png` through `/sdcard/32.png` with `esp_lv_decoder`.

`convert_image.py` is an optional development utility for converting arbitrary
PNG files to LVGL 9 ARGB8888 C arrays. Generated C files are not included by the
current slide-player CMake component and are not required for normal operation.

Run the utility from a directory containing PNG files:

```bash
python -m pip install pillow
python path/to/convert_image.py
```

Pass one or more PNG paths to convert only those files. The output is written
next to each input with a `.c` extension.

