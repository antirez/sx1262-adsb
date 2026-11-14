# The T-Echo receiver dial at 1090 MHz

On 14 November 2026 we measured one T-Echo with firmware `cdd3045`. A coax splitter fed its antenna connector and an RTL-SDR from the same HackRF One. We changed the T-Echo dial over USB and read the 256 RSSI samples saved by each hardware trigger. Both the dial and transmit level varied in shuffled order within each series.

## Weak signals

Each transmission in the first test contained three short carrier pulses. Each run sent 12 transmissions with an irregular but repeatable spacing. The stronger level used twice the digital amplitude of the weaker one. Only the receiver dial changed between rows.

| T-Echo dial (MHz) | Weak bursts captured / 48 | Stronger bursts captured / 48 | Stronger burst above floor |
| ---: | ---: | ---: | ---: |
| 1089.700 | 47 | 48 | 11.2 dB |
| 1089.800 | 48 | 48 | 11.9 dB |
| 1089.900 | 48 | 48 | 11.7 dB |
| 1090.000 | 15 confirmed, at most 17 | 48 | 8.5 dB |
| 1090.100 | 48 | 48 | 12.0 dB |

The last column uses the strongest 12 µs average in each capture relative to the board's quiet-level reading, then takes the median across captures. We matched trigger times to transmit slots within 2.1 ms and required at least four matches to establish the timing of a run. One weak run at 1090.000 had just two triggers. Counting both would bring the result to 17/48. The four silent control runs produced three triggers in total and no matching sequence.

## Recovering the bits

A second test used 0.5 µs pulse-position modulation. We transmitted 12 different 80-bit calibration words, each bracketed by known timing bits, five times per setting. The T-Echo captured all 60 transmissions at each of the settings below. We decoded the saved samples on the Mac with the same pre-existing response model at all three frequencies. The decoder had the timing bits. We used the transmitted payload only to score its answer.

| T-Echo dial (MHz) | Correct payload bits at lower power | Correct payload bits at higher power | Exact words at higher power |
| ---: | ---: | ---: | ---: |
| 1089.800 | 90.6% | 97.5% | 20/60 |
| 1090.000 | 62.9% | 73.6% | 0/60 |
| 1090.100 | 86.3% | 96.4% | 11/60 |

These are bench calibration words. They measure how much modulation survived in the RSSI capture. An aircraft packet also has to pass the firmware's preamble search and CRC check.

## Checking the HackRF

With the HackRF set to produce 1090.000 MHz, the RTL-SDR measured the carrier 18.8 and 19.3 kHz low. We took those readings at two RTL tuning frequencies, placing the wanted carrier on opposite sides of the HackRF oscillator, and identified it by a deliberate 80 kHz step. The two measured steps were 80.001 and 80.007 kHz.

We independently counted USB samples against the Mac's monotonic clock. The RTL-SDR ran 5.94 ppm fast over 125 seconds. Applying that correction gives a HackRF carrier about 12–13 kHz below nominal. The HackRF's own sample clock ran 12.26 ppm slow over 105 seconds, which independently predicts about 13 kHz at 1090 MHz. The Mac was the time reference for these estimates.

We then **raised the HackRF setting by 13 kHz** and repeated the comparison. At 1089.800 the T-Echo caught at least 24/48 weak markers. At 1090.000 it recorded zero triggers during all 48. At higher power both settings captured all 48 calibration words. The same host decoder recovered 9/48 exactly at 1089.800 and 0/48 at 1090.000.

We also moved the source down 200 kHz, then up 200 kHz from nominal. In both cases the weak response followed the carrier. The T-Echo captured at least 30/36 markers with its dial 200 kHz to either side of the source, while the matching dial produced only five or six raw triggers per 36 transmissions.

This T-Echo receives better with the signal away from the center of its selected receive channel. Which part of the radio creates the dip is still open. The measurements support 1089.800 MHz as a starting point on the two boards tested. They also show why several nearby settings can work. [Measurements for every run](techo-tuning.csv)
