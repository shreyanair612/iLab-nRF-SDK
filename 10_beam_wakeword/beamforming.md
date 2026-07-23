# Beamforming approach

## Physics intuition

Sound travels at $c \approx 343 \text{ m/s}$, so a wavefront reaches the two microphones at slightly different times depending on its angle of arrival.
This angle-dependent **Time Difference Of Arrival (TDOA)** is the core cue exploited for spatial selectivity.

## Why beamforming on glasses

A single omnidirectional microphone picks up everything from all directions, which is bad for glasses in noisy environments.
Beamforming gives **spatial selectivity**: it increases sensitivity to one direction and suppresses others.
On the glasses, the beam is steered toward the user’s voice and away from competing sound sources.

## Geometry and what must be exact

On the glasses, the angle of arrival of the user’s voice is approximately fixed by the geometry of the head and microphone placement.
That angle can be estimated from average head size and mic spacing, then hard-coded into a delay-and-sum beamformer.
Because beams are generally wide, this approximation is acceptable.

The quantity that must be measured precisely is **$$d$$**, the physical distance between the left and right microphones, because it directly determines the TDOA that must be compensated.

## Hardware and data format

Each INMP441 outputs **24-bit, two’s-complement PCM audio** over I2S.
Both microphones share WS and SCK, so their streams are synchronized in time.

At each sample index $$n$$:

- $$x_L[n]$$: left microphone signed amplitude sample
- $$x_R[n]$$: right microphone signed amplitude sample

So the data being processed is **time-domain PCM**: a signed amplitude value per sample, over time, for each microphone.

## Algorithmic idea

If a source is roughly equidistant from both microphones, the same waveform reaches them at nearly the same time and shape, so $$x_L[n]$$ and $$x_R[n]$$ look very similar.
If a source is off-axis, one microphone receives it earlier than the other, so the two streams are misaligned in time.

Delay-and-sum beamforming uses this difference:

1. Compensate the expected TDOA for the desired direction by delaying one channel relative to the other.
2. Sum the aligned channels.
3. Correctly aligned signals add constructively; misaligned signals add weakly or partially cancel.

For the simplest center-beam prototype, the desired direction is approximately the midpoint between the microphones, so the compensation is close to zero delay.
In that case, the beamformed signal is:

$$
y[n] = \frac{x_L[n] + x_R[n]}{2}
$$

This amplifies signals that look similar across both microphones and attenuates signals that do not.