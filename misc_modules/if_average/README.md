# IF Average -- SDR++ Radio Astronomy Plugin

Spectral averaging for radio astronomy with SDR++.
Designed for use with HydraSDR RFOne and compatible with any SDR
source supported by SDR++.

Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
License: MIT


## What it does

This plugin accumulates FFT spectra over time, averaging out random
noise to reveal weak signals buried below the noise floor. This is
how radio astronomers detect the hydrogen line at 1420 MHz, measure
galactic rotation, and observe other faint spectral emissions.

The longer you integrate, the deeper you see. The noise floor drops
proportional to the square root of the number of accumulated spectra.
A 10-minute integration gives roughly 10x better sensitivity than a
single FFT frame.

The plugin runs in real time inside SDR++. No external software or
post-processing is needed. Just tune to the frequency of interest,
click Start, and watch the averaged spectrum build up on the
waterfall display.


## Features at a glance

**Spectral averaging** -- accumulate spectra using one of five
algorithms (linear mean, weighted mean, RMS, median, or exponential
moving average). The averaged result is displayed as a colored trace
overlaid on the waterfall.

**Baseline removal** -- subtract the receiver passband shape to
flatten the spectrum and reveal features. Two methods: polynomial
fit (automatic, configurable order 0-15) or reference subtraction
(capture a blank-sky reference and divide it out).

**RFI mitigation** -- detect and blank radio frequency interference
before it corrupts the average. Two detection methods: simple
sigma threshold or spectral kurtosis (a statistical test that
distinguishes Gaussian noise from man-made signals). Flagged bins
are replaced by interpolation, neighbor mean, zeroed, or the entire
corrupted spectrum is dropped.

**Y-factor calibration** -- measure your system's noise temperature
by comparing a hot load (room temperature absorber) against cold
sky. The plugin computes Tsys, Y-factor, and per-channel gain. Once
calibrated, the spectrum can be displayed in Kelvin (antenna
temperature) instead of dBFS.

**Total power radiometry** -- integrate band power over time and
display it as a strip chart. Useful for drift scans (watching a
radio source cross the antenna beam) and monitoring system
stability. Configurable band limits and logging interval.

**Allan variance** -- measure your receiver's stability over
different integration times. Identifies the optimal integration
time before gain drift dominates. Essential for planning long
observations.

**Standalone spectrum window** -- a dedicated analysis window
showing the averaged spectrum with frequency axis (MHz), grid,
sub-grid, cursor readout (frequency, power, velocity, temperature),
integration stats (elapsed time, spectra count), and FFT parameters
(center freq, bandwidth, FFT size, resolution). Optional sub-plots
for baseline residual, total power strip chart, and Allan variance.

**Spectral line tools** -- built-in rest frequency presets for
common radio astronomy lines (HI 21cm, OH 18cm, H2O 22GHz,
CH3OH 6.7GHz). Radial velocity readout converts frequency offset
to km/s using the radio convention, useful for measuring galactic
rotation curves.

**CSV export** -- save the averaged spectrum to a CSV file with
full metadata header (SDR source name and firmware version, center
frequency, sample rate, decimation, FFT size, FFT rate, integration
time, spectra count, averaging mode, calibration data). Automatic
export at configurable intervals for unattended observations.


## Quick start

1. Start your SDR source in SDR++ so the waterfall is running.
2. Open the IF Average panel in the left sidebar.
3. Click **Start**.

A green trace appears on the waterfall showing the averaged
spectrum. Watch it get smoother over time as noise averages out.
The status line shows elapsed time and the number of accumulated
spectra.

To observe the hydrogen line: tune to 1420.405 MHz with at least
2 MHz bandwidth. With a suitable antenna and LNA, a bump should
become visible at 1420.405 MHz after 30-60 seconds of integration.


## Build

### Visual Studio

```
cd build_vs2019_x64
cmake ../ -G "Visual Studio 16 2019" -A x64
cmake --build . --config Release --target if_average
```

### MinGW64

```
export PATH="/d/msys64/mingw64/bin:/d/msys64/usr/bin:$PATH"
mkdir build && cd build
cmake ../ -G "Ninja"
ninja if_average
```

### Linux / macOS

```
mkdir build && cd build
cmake ../
make if_average
```


## Installation

1. Copy `if_average.dll` (or `.so` / `.dylib`) into the SDR++
   `modules/` directory.

2. Add to `config.json` under `moduleInstances`:

```json
"IF Average": {
    "enabled": true,
    "module": "if_average"
}
```

3. Add to `config.json` under `menuElements`:

```json
{
    "name": "IF Average",
    "open": true
}
```

4. Restart SDR++.


## Averaging modes

**Linear Mean** -- the standard choice. Each spectrum has equal
weight. Sensitivity improves as 1/sqrt(N). Use this for most
observations.

**Weighted Mean** -- automatically downweights spectra with high
variance (likely RFI-contaminated). Each spectrum is weighted by
1/variance, estimated from the mean absolute deviation. Useful
in noisy RF environments.

**RMS** -- root-mean-square averaging. Computes sqrt(mean(x^2)).
Used for power measurements where the mean may not be appropriate.

**Median** -- per-bin median over a sliding window of the last
N spectra (configurable, 8 to 256). The most robust method against
impulsive RFI since outliers are rejected by definition. Uses more
memory than other modes.

**EMA** -- exponential moving average. Recent spectra have more
weight than older ones. The alpha parameter controls the time
constant: smaller alpha gives deeper averaging, larger alpha tracks
changes faster. Good for monitoring varying sources in real time.


## Baseline removal

The receiver's analog chain (filters, amplifiers, ADC) adds a
frequency-dependent gain shape to the spectrum. This passband
shape can mask weak spectral features. Baseline subtraction removes
it.

**Polynomial fit** -- fits a polynomial curve (order 0 to 15) to
the averaged spectrum and subtracts it. Order 0 removes the DC
offset. Order 3 handles the typical U-shaped passband of most SDR
receivers. Higher orders fit more complex shapes but risk absorbing
real signals.

**Reference subtraction** -- capture a reference spectrum on blank
sky (no sources in the beam) or a known flat source, then divide
all subsequent spectra by the reference. This removes the passband
shape exactly. Click "Capture Reference" to store the current
averaged spectrum as the reference.

Toggling baseline off preserves your reference capture. You can
freely compare the view with and without baseline subtraction
without losing your reference.

The residual (spectrum after baseline removal) can be viewed in
the standalone window by enabling "Show residual".


## RFI mitigation

Radio frequency interference from terrestrial transmitters,
electronics, and other sources can corrupt the averaged spectrum.
The RFI mitigator detects and blanks interference on a per-spectrum
basis, before accumulation.

**Threshold detection** -- for each FFT bin, maintains a running
mean and standard deviation. Flags bins where the current value
exceeds the mean by more than N sigma (configurable, default 5.0).
Needs at least 4 spectra to establish baseline statistics.

**Spectral kurtosis** -- a statistical test based on the fourth
moment. Pure Gaussian noise has a spectral kurtosis of 1.0. RFI
causes deviations from 1.0. More robust than simple thresholding
because it detects non-Gaussian signals regardless of their
amplitude. The sigma slider controls sensitivity for both modes.

**Blanking options** -- what to do with flagged bins:
- Replace Mean: substitute with the mean of nearest clean neighbors
- Interpolate: linear interpolation between nearest clean bins
- Zero: set to zero
- Drop Spectrum: discard the entire spectrum if more than 25% of
  bins are flagged

Flagged bins are highlighted as red translucent rectangles on the
waterfall overlay.


## Y-factor calibration

Converts raw power readings to antenna temperature in Kelvin.

1. Set T_hot (default 290 K, room temperature) and T_cold
   (default 10 K, cold sky at GHz frequencies).
2. Point your antenna at the hot load (absorber foam, 50-ohm
   terminator). Let the average stabilize. Click "Capture HOT".
3. Point at cold sky. Let the average stabilize. Click
   "Capture COLD".
4. Click "Compute Cal".

The plugin computes:
- Y-factor = P_hot / P_cold (the power ratio)
- T_sys = (T_hot - Y * T_cold) / (Y - 1) (system temperature)
- Per-channel gain for Kelvin conversion

Once calibrated, the standalone spectrum window switches to
Kelvin display and the cursor readout shows antenna temperature.
The CSV export includes a `power_kelvin` column.

Toggling calibration off preserves your HOT/COLD captures. You
can compare calibrated and uncalibrated views without redoing
the capture procedure.


## Total power radiometry

Integrates the average power across a configurable frequency band
and logs it over time. The result is displayed as a strip chart in
the standalone window.

Use this for:
- **Drift scans**: park the antenna and let a radio source drift
  through the beam. The total power peak shows when the source
  crosses.
- **System monitoring**: watch for gain changes, temperature drift,
  or intermittent RFI over long observations.

Settings:
- Start/Stop offset (Hz from center): defines the integration band.
  0/0 uses the full bandwidth.
- Log interval: how often a sample is recorded (default 1 second).
- History is capped at 86,400 samples (24 hours at 1 sample/sec).

Changing band limits or log interval clears the history and
restarts collection with the new settings.


## Allan variance

Measures your receiver's stability as a function of integration
time (tau). After accumulating data, click "Compute Allan" to
generate a log-log plot of Allan deviation vs tau.

The plot shows:
- At short tau: noise dominates, ADEV drops with increasing tau
  (the averaging is helping).
- At the minimum: optimal integration time for your system.
- At long tau: gain drift dominates, ADEV rises (longer averaging
  makes things worse).

This tells you the maximum useful integration time for your
hardware. Beyond the minimum, you need gain stabilization or
more frequent calibration.


## Standalone spectrum window

A dedicated analysis window opened via the "Separate window"
checkbox. Shows:

- **Averaged spectrum plot** with frequency axis (MHz), Y-axis
  labels (dBFS or Kelvin), grid and sub-grid. The spectrum is
  rendered at full FFT resolution -- with a 65536-point FFT you
  get 65536 data points to explore by zooming in.
- **Cursor readout**: hover the mouse over the spectrum to see
  frequency, power, and optionally radial velocity and antenna
  temperature.
- **Integration stats**: elapsed time, spectra count, FFT info
  (center frequency, bandwidth, number of bins, resolution).
- **Residual plot** (optional): baseline-subtracted spectrum.
- **Total power strip chart** (optional): power vs time.
- **Allan variance plot** (optional): ADEV vs tau.

### FFT engine

The plugin runs its own FFT engine on the raw IQ stream,
independent of the SDR++ waterfall. This gives full spectral
resolution in the standalone window regardless of the waterfall
display width.

- **FFT Size** (1024 to 1,048,576): controls frequency
  resolution. Larger = finer detail, more CPU. At 2.5 MS/s:
  65536 gives 38 Hz resolution, 262144 gives 9.5 Hz.
- **Overlap %** (0 to 75%): controls how fast spectra
  accumulate, not the resolution. Higher overlap = more FFTs
  per second = faster noise reduction. Does not change the
  ability to resolve close frequencies.

- **Window** function selector:
  - Blackman-Harris (default): 92 dB sidelobe rejection,
    good all-rounder for spectral line work
  - Hann: narrower main lobe, less sidelobe rejection
  - Nuttall: 93 dB sidelobes, similar to Blackman-Harris
  - Flat-Top: best amplitude accuracy (0.01 dB scalloping),
    use for precise power measurements
  - Kaiser B=8: compromise between resolution and sidelobes

All three can only be changed while stopped. The computed FFT
rate and resolution are shown below the controls.

### Zoom and pan

The main spectrum plot supports interactive zoom and pan:

- **Mouse wheel on plot area**: zoom frequency axis centered
  on the cursor position. Minimum span adapts to the FFT
  resolution (10 bins).
- **Mouse wheel on Y-axis margin** (left of plot): zoom the
  power axis only.
- **Middle mouse button drag**: pan on both axes.
- **Double-click**: reset all zoom to full range.

When zoomed in, the title shows the visible span (e.g.
"Averaged Spectrum (dBFS) [zoom: 250.0 kHz]"). The Y-axis
auto-rescales to the visible data when not manually zoomed.

Grid labels adapt to the window size: the number of labels
adjusts so they never overlap, and frequency precision increases
with zoom level. The 1-2-5 grid step sequence ensures round
values at all scales.

The view resets to full bandwidth on frequency or sample rate
change. Start and Reset preserve the current zoom.

### Resizable sub-plots

When multiple sub-plots are visible (residual, total power,
Allan), you can drag the border between them vertically to
resize their heights. The cursor changes to a resize arrow
when hovering over the border.

### Input isolation

The window blocks all waterfall mouse input (clicks, drags,
scroll wheel) when active, so you can interact with it without
accidentally changing the tuning.


## Data export

Averaged spectra are exported as CSV files with a metadata header
containing all observation parameters:

```
# IF Average Export
# Date Start (UTC): 2026-03-18T10:30:00Z
# Date End (UTC): 2026-03-18T10:35:23Z
# Source: HydraSDR lib v1.1.0 fw:RFOne v2.3.1
# Center Frequency (Hz): 1420405751.768
# Effective Sample Rate (Hz): 2400000
# Hardware Sample Rate (Hz): 2400000
# Decimation: 1
# FFT Size: 65536
# FFT Rate (Hz): 20.0
# Integration Time (s): 323.456
# Spectra Accumulated: 6469
# Averaging Mode: Linear Mean
```

Columns: frequency (Hz), power (dBFS), power (linear), power
(Kelvin if calibrated), residual (if baseline enabled), RFI flag.

**Auto-export** writes a file at a configurable interval (e.g.,
every 60 seconds). Useful for long unattended observations where
you want periodic snapshots.


## Controls summary

| Control | Effect |
|---------|--------|
| Start | Clear all data, begin new observation |
| Stop | Halt integration, keep result on display |
| Reset | Clear all data; if running, continues from zero |
| Export Now | Write CSV file immediately |
| Continuous | Run until Stop (vs auto-stop after Time) |
| Reset on freq/SR change | Auto-restart on retune |
| Baseline on/off | Toggle passband removal (preserves reference) |
| Calibration on/off | Toggle Kelvin display (preserves captures) |
| RFI on/off | Toggle interference detection (disable clears stats) |
| Total Power on/off | Toggle band power logging (disable clears history) |
| Allan on/off | Toggle stability analysis (disable clears series) |

## References

- Radiometer equation: Kraus, "Radio Astronomy", 2nd edition
- Spectral kurtosis: Nita & Gary, 2010, MNRAS 406, L60
- Allan variance: Allan, 1966, Proc. IEEE 54(2), 221-230
- Y-factor calibration: Pozar, "Microwave Engineering", 4th edition
