909 cymbal samples: hh.wav (hi-hat, open and closed), ride.wav, crash.wav.

Source: ER-99 by Matthew Cieplak (https://github.com/matthewcieplak/er-99),
shipped unchanged by 9W9 (Charles Vestal), which this firmware's drum engine
is ported from. Licence: GPL-3.0 (ER-99 and 9W9 are GPL-3.0).

tools/x0x/gen_drum_samples.py converts them to C arrays in
build/gen/x0x_drum_samples.h (hh.wav is 24-bit and is rounded to 16 bits), stored
as 6-bit block floating point (16 samples share a shift) so they fit the app's
flash; the 909's own cymbals are 6-bit too.
