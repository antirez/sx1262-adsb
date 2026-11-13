# Where the T-Echo loses packets

The 1090 MHz recordings contain a packet that the production firmware heard clearly and failed to decode. A change to the software fit recovers all 112 bits from that recording. A separate change to the order of the existing decoder passes reduces the work required to decode packets the T-Echo already recognizes. The measurements below are from saved waveforms and counters. The outdoor receiver was disconnected during this analysis.

## One missed packet we can reproduce

In the quiet coax run that scored 4/16, the RTL-SDR received the first test transmission. The T-Echo saved its waveform as block 871. Four later decoded transmissions put this block in the first 650 ms slot, with about 1 ms of timing uncertainty. The middle of the burst was about 18 dB above the local floor. Its preamble score was 6944, well above the decoder threshold of 3500.

As a cross-check, I compared the varying portion of the waveform against all 16 transmitted words, excluding their shared header. Normalized correlation was 0.96 for the expected word and at most 0.56 for any of the other 15. This comparison used the transmitted words to identify the recording. The decoding experiment below uses only the T-Echo's 256 RSSI samples.

The production decoder chooses sampling alignments by how well they match the preamble and five known bits at the start of a DF17 message. On this recording it found the preamble near sample 62. I tried all 15 distinct timing grids within four samples of that point, using the existing fast 1090 MHz response model. Each candidate gets a first-pass bit decode. The best four fits to the entire packet get the existing ranked search and 24-bit CRC check. I also allowed a 10% increase in the estimated modulation amplitude used by the model. This changes how the software interprets the recording.

| Timing candidates | Original amplitude | Amplitude ×1.10 |
| --- | --- | --- |
| Keep the original preamble shortlist, rank by whole-packet fit | Miss | Miss |
| Evaluate all nearby timings, rank by whole-packet fit | Miss | Exact 112 bits |

A successful alignment began at sample 60, just 1.5 µs from the original preamble estimate. The correct word was `8d000000c8540afe9ffe693c94ea`, identical to the RTL-SDR bytes. The parameters were chosen while studying this miss. One recovered word demonstrates a mechanism, but does not establish how often it will help outdoors.

The production host replay prints nothing for this capture. To run the separate experiment from the repository root on macOS or Linux, build the host tools and feed it the exact same samples. The five-column fixture has no expected word.

```sh
cmake -S host -B host/build -DCMAKE_BUILD_TYPE=Release
cmake --build host/build
host/build/replay < docs/1090-block-871.tsv
cc -std=c11 -O2 -Imain host/experiments/weak_reply.c host/build/libreceiver_core.a -lm -o /tmp/sx-weak-reply
cut -f5 docs/1090-block-871.tsv | xxd -r -p | /tmp/sx-weak-reply
```

## How much decoding work can we save?

The T-Echo currently tries two general response models before its two 1090 MHz models. A failed model can search many possible bit sequences before returning. I tried the 1090 MHz models first at the strongest preamble, kept the old models as fallbacks, and counted complete 112-bit trellis evaluations. The input and the CRC checks stayed the same. The table counts work for every recording, including failures and deferred decoding.

| Replay inputs | Recordings | Current order | 1090 MHz models first |
| --- | ---: | ---: | ---: |
| T-Echo drive journal | 29 | 1,276 | 439 |
| T-Echo 1090 MHz coax journals | 54 | 4,820 | 3,043 |
| T3-S3 drive boot | 176 | 15,435 | 15,495 |
| Physical replays of aircraft signals at 880 MHz | 65 | 15,262 | 14,872 |
| Modeled RSSI derived from recorded aircraft I/Q | 325 | 113,368 | 114,748 |

The two orders produced identical decoded words on these 649 inputs and the reordered build passed the repository's replay fixtures. The biggest saving was on the T-Echo drive with 66% fewer trellis evaluations. On the T3-S3 drive, most saved traces failed to decode, so they still exhausted the available models. These counts measure algorithm work. The next measurement is queue evictions and packets decoded per transmitted packet on the nRF52840.

I also ran the narrower timing-and-amplitude experiment above on all 649 inputs. Block 871 was the only additional exact decode. On 4,000 controls made by scrambling body samples while preserving the preamble, it returned no packets. A broader search across four models, several amplitudes and nearby noise-floor estimates produced a CRC-valid *false* packet on a scrambled recording. Those controls reuse recordings and cannot establish a field false-positive rate. This is a reason to keep the extra decoding bounded. The 880 MHz and modeled data are separate checks. For the two legacy data sets with 248 readings, replay appended a copy of the final eight readings to use the production 256-reading interface. The test corpus includes repeat transmissions and captures already selected for saving.

A separate trial tested the callsign question. Of nine time-matched physical callsign captures in the 880 MHz archive, the current decoder recovered three. Constraining the payload characters to the callsign alphabet added none. Allowing up to three changes among the 20 least certain bits recovered a fourth, TVF43RN, matching the independently recorded source word. Only one response model supported that correction. No additional 1090 MHz capture decoded by this method, so I would test it separately before making it a source of flight names.

## What can be attributed on the bench and outdoors?

- Across the three busy coax runs the improved T-Echo decoded 15/48. Its input queue never filled, but its deferred queue dropped 34 recordings. A record can enter the decoder and still lose its chance at an expensive second pass. The counters do not identify which of these were real transmissions.
- With the decoder idle before each of two quiet runs, the same 16 words produced 10 and 11 exact decodes. Seven appeared in both runs, 14 appeared in at least one, and neither queue reported a drop. In the first run the first and last transmitted words correspond to capture numbers 2423 and 2438, exactly 15 capture numbers apart for 15 transmission intervals. The receiver triggered about once per transmitted packet across that sequence. Capture numbers alone cannot identify the missing words because an unrelated trigger could take the place of a missed transmission.
- The earlier outdoor comparison of flight AEZ157Q showed a different failure. The RTL-SDR decoded 131 transmissions. Even with a generous clock allowance at most two could coincide with a LilyGO recording. The devices had separate antennas and that run used the older 1089.4 MHz dial and receive settings. It demonstrates that loss before software decoding is possible. It does not measure the present T-Echo's outdoor sensitivity.
- On the T-Echo drive, all 22 saved aircraft words passed the current queue filter as long replies and replayed correctly. The seven other saved waveforms yielded no new packet. The firmware saves a failed trace at most once every 30 seconds. Those seven are a small sample of what the receiver heard.

For the next coax comparison I would capture one run's worth of timing and decisions in RAM. Give each trigger its capture number, capture-time interval, queue decision, detected preamble and final decode status. Keep the raw 256-byte waveform for the 16 quiet slots, including failures, and download it after the run so the serial output does not change the timing. Match against the RTL-SDR clock using the known transmission cadence. Then compare the current firmware, the model reordering, and the bounded retry when the primary queue is empty. Use fresh, previously unseen words, including callsigns, positions and velocities, plus silent slots. Score exact words per transmission and unexpected decodes separately. This distinguishes a missing trigger from a capture thrown away under load or a waveform that needs a better decoder.

[1090 MHz run totals](techo-1090-coax.csv) · [Block 871](1090-block-871.tsv) · [Standalone recovery experiment](../host/experiments/weak_reply.c) · [Production decoder](../main/decoder.c) · [T-Echo queues and journal policy](../techo/app.c)
