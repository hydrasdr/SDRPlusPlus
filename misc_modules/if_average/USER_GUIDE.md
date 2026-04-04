# IF Average -- User Guide

Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>


## Introduction

IF Average is a radio astronomy plugin for SDR++. It accumulates
FFT spectra over time, averaging out random noise to reveal weak
signals that would otherwise be invisible on the waterfall display.

This is the same technique used by professional radio observatories.
The longer you integrate, the more sensitive the measurement becomes.
After a few minutes of integration, signals 20-30 dB below the noise
floor become clearly visible.

The plugin works with any SDR source supported by SDR++, including
HydraSDR RFOne, RTL-SDR, HackRF, BladeRF, and others.


## Getting started

### First observation

1. Start SDR++ and select your SDR source.
2. Tune to a frequency of interest. For a first test, try an FM
   broadcast band (88-108 MHz) where strong signals are easy to see.
3. Start the SDR source (click Play).
4. In the left sidebar, find and expand the "IF Average" panel.
5. Leave all settings at their defaults.
6. Click **Start**.

You will see a green trace appear on the waterfall spectrum display.
This is the averaged spectrum. Watch it over 10-20 seconds: the trace
becomes progressively smoother as noise averages out, while persistent
signals (FM stations) remain sharp and clear.

The status line at the bottom of the panel shows:
- Elapsed integration time (HH:MM:SS)
- Number of accumulated spectra

Click **Stop** when you are done. The averaged trace stays on the
display with the final integration time and spectra count shown in
grey.

Click **Reset** to clear everything and return to the Idle state.

### Observing the hydrogen line

The most common target for amateur radio astronomy is the hydrogen
line at 1420.405 MHz. Neutral hydrogen in our galaxy emits at this
frequency, and with a suitable antenna and low-noise amplifier (LNA),
the signal can be detected with consumer SDR hardware.

1. Tune to 1420.405 MHz with at least 2 MHz bandwidth.
2. Point your antenna at the Milky Way (galactic plane).
3. Click Start in the IF Average panel.
4. Wait 30-60 seconds.

A bump should appear at 1420.405 MHz, rising above the noise floor.
The bump is typically 1-2 MHz wide due to Doppler broadening from
galactic rotation.

If you do not see a bump:
- Check your antenna and LNA connections.
- Make sure the LNA is powered (bias tee).
- Try integrating longer (2-5 minutes).
- Enable baseline subtraction to remove the receiver passband shape.


## Panel controls

The IF Average panel in the sidebar contains all controls. They are
organized from top to bottom: averaging settings, feature toggles,
display settings, export settings, and status/buttons.

### Averaging mode

Controls how spectra are combined. Can only be changed while stopped.

**Linear Mean** (default) -- each spectrum has equal weight. This is
the standard choice for most observations. The noise floor drops as
the square root of the number of accumulated spectra: 100 spectra
gives 10x (20 dB) improvement, 10000 spectra gives 100x (40 dB).

**Weighted Mean** -- automatically gives less weight to spectra that
have high variance (likely contaminated by RFI). Useful if you
observe in an environment with intermittent interference and want to
keep integrating without stopping to deal with each RFI event.

**RMS** -- root-mean-square averaging. Instead of the mean power, it
computes the RMS power. Used for specific power measurement tasks.

**Median** -- instead of averaging, takes the per-bin median over a
sliding window of recent spectra. The most robust method against
impulsive RFI because outliers are rejected automatically. The
window depth is configurable from 8 to 256 spectra. Uses more
memory than other modes.

**EMA** (exponential moving average) -- gives more weight to recent
spectra and less to older ones. The alpha parameter controls the
balance: small alpha (0.001) means very slow, deep averaging;
large alpha (0.5) means fast tracking of changes. Good for
monitoring a varying source in real time.

### Integration time

Sets the target integration time in seconds (0.1 to 86400, which
is 24 hours). This only matters when Continuous mode is unchecked:
integration stops automatically when this time elapses.

When Continuous is checked, this value is ignored and integration
runs until you click Stop.

### Continuous mode

When checked: integration runs indefinitely until you click Stop.
The spectra counter shows "/ inf".

When unchecked: integration runs for the configured time and then
stops automatically. The status changes to "Stopped" and shows the
final elapsed time and spectra count. You can then export the result
or start a new observation.

### Reset on frequency change

When checked: if you retune the SDR (change the center frequency)
while integrating, the observation automatically restarts from zero.
This prevents mixing data from different frequencies in the same
average.

When unchecked: frequency changes are ignored and integration
continues. This is useful when an external process (such as a
frequency scanning script) controls the tuning and you want to
accumulate regardless.

### Reset on sample rate change

Same as above, but triggers when the sample rate changes (for
example, when you change the SDR bandwidth or decimation setting).


## Baseline subtraction

### What it does

The raw averaged spectrum shows the receiver passband shape: a broad
curve imposed by the analog filters, amplifiers, and ADC frequency
response. This shape has nothing to do with the actual radio signal.
Baseline subtraction removes it, leaving a flat spectrum where real
spectral features (like the hydrogen line) stand out clearly.

### How to use it

1. Check the **Baseline** checkbox. Additional controls appear.
2. Choose a mode: Polynomial or Reference.

**Polynomial mode** (default):
- The plugin fits a mathematical curve to the spectrum and subtracts
  it. The curve order is configurable from 0 to 15.
- Order 0: subtracts a constant offset (DC removal).
- Order 1: removes a linear tilt.
- Order 3 (default): handles the typical U-shaped or S-shaped
  passband of most SDR receivers.
- Higher orders can fit more complex shapes but risk fitting real
  signals (overfitting).
- No user action required beyond setting the order. The fit is
  recomputed automatically on every frame.

**Reference mode**:
- You capture a reference spectrum (on blank sky or a known flat
  source) and the plugin divides all subsequent spectra by it.
- This removes the passband shape exactly.
- Steps:
  1. Point the antenna at blank sky (no sources in the beam).
  2. Let the average stabilize (at least 30 seconds).
  3. Click "Capture Reference". The status shows "Reference captured".
  4. Now point at your target. The passband is removed.
- The reference is preserved if you toggle baseline off and back on.

### Viewing the residual

Check "Show residual" in the Display section, then open the
standalone window via "Separate window". A second plot appears below
the main spectrum showing the baseline-subtracted result. This is
the spectrum with the passband removed.


## RFI mitigation

### What it does

Radio frequency interference (RFI) from terrestrial transmitters,
electronics, WiFi routers, and other sources can corrupt the averaged
spectrum. The RFI mitigator detects interference on each incoming
spectrum and blanks the affected frequency bins before they are added
to the average.

### How to use it

1. Check the **RFI Mitigation** checkbox.
2. Choose a detection mode: Threshold or Spectral Kurtosis.

**Threshold mode** (default):
- For each frequency bin, the plugin maintains a running mean and
  standard deviation.
- If the current value exceeds the mean by more than N sigma
  (configurable, default 5.0), that bin is flagged as RFI.
- The first 4 spectra are used to establish baseline statistics;
  no flagging occurs during this period.
- Lower sigma values are more aggressive (flag more bins). Higher
  values are more conservative (only flag strong RFI).

**Spectral Kurtosis mode**:
- Uses a statistical test based on the fourth moment of the signal.
- Pure Gaussian noise has a spectral kurtosis (SK) of exactly 1.0.
- RFI causes SK to deviate from 1.0.
- More robust than simple thresholding because it can detect
  RFI regardless of amplitude, including weak continuous-wave
  interference.
- Needs at least 8 spectra to produce reliable SK estimates.

### Blanking options

When bins are flagged as RFI, the plugin replaces them before
accumulation. Four strategies are available:

- **Replace Mean**: substitute with the average of the nearest
  unflagged neighbors. Best general-purpose choice.
- **Interpolate**: linear interpolation between the nearest clean
  bins on each side. Produces a smooth fill.
- **Zero**: set flagged bins to zero. Simple but introduces
  artifacts in the average.
- **Drop Spectrum**: discard the entire spectrum if more than 25%
  of bins are flagged. Use when RFI is broadband and pervasive.

Flagged bins are shown as red translucent rectangles on the
waterfall overlay.

### Tips

- Start with Threshold mode and sigma 5.0. Lower sigma if you see
  RFI leaking through; raise it if too many clean bins are flagged.
- Spectral Kurtosis is better for weak continuous RFI that does
  not spike above the noise significantly.
- RFI detection only affects new spectra. Spectra already
  accumulated before enabling RFI are not retroactively cleaned.
  For best results, enable RFI before clicking Start.


## Y-factor calibration

### What it does

Calibration converts raw power readings (arbitrary dBFS units) into
antenna temperature in Kelvin. This lets you measure the actual
brightness of radio sources in physical units, compare measurements
across sessions, and compute system performance parameters.

### What you need

1. A hot load at a known temperature. Room temperature (290 K) is
   standard. A piece of microwave absorber foam placed over the
   feed horn, or a 50-ohm terminator on the antenna port, works.

2. A cold reference at a known temperature. Cold sky (pointed away
   from the Sun, ground, and bright sources) is approximately
   3-10 K at GHz frequencies.

### Procedure

1. Check the **Calibration** checkbox.
2. Set T_hot to your hot load temperature (default 290 K).
3. Set T_cold to your cold sky temperature (default 10 K).
4. Connect or point at the hot load.
5. Click Start and let the average stabilize (30+ seconds).
6. Click **Capture HOT**. The status shows "HOT captured".
7. Point at cold sky. Click Reset to clear the hot-load average,
   then click Start and let it stabilize again.
8. Click **Capture COLD**. The status shows "COLD captured".
9. Click **Compute Cal**.

The display shows:
- Y-factor in dB (the hot/cold power ratio)
- System temperature (Tsys) in Kelvin

Typical values for amateur systems: Y = 3-15 dB, Tsys = 50-500 K.

### After calibration

- Open the standalone window ("Separate window"). The main spectrum
  plot switches from "Averaged Spectrum (dBFS)" to "Antenna
  Temperature (K)".
- The cursor readout shows temperature in Kelvin when you hover.
- CSV exports include a `power_kelvin` column.

### Notes

- The calibration is preserved if you toggle the checkbox off and
  back on. You do not need to redo the capture procedure.
- Changing T_hot or T_cold invalidates the calibration. You must
  click "Compute Cal" again.
- Recalibrate whenever you change the antenna, LNA, or cabling.
- If Tsys comes out negative, the captures may be swapped (hot and
  cold reversed) or T_cold is set too high.


## Total power radiometry

### What it does

Integrates the average power across a configurable frequency band
and records it over time. The result is a time series of band power
that can be displayed as a strip chart.

### Typical uses

**Drift scans**: park the antenna at a fixed position and let a
radio source drift through the beam due to Earth's rotation. The
total power curve shows a peak when the source crosses the beam
center. By recording the peak time and knowing the antenna pointing,
you can map radio sources across the sky.

**System monitoring**: watch for gain changes, temperature drift,
or intermittent RFI over long observations.

### How to use it

1. Check the **Total Power** checkbox.
2. Set the band limits as frequency offsets from center:
   - Start offset (Hz): lower edge relative to center frequency.
   - Stop offset (Hz): upper edge relative to center frequency.
   - Leave both at 0 to use the full bandwidth.
3. Set the log interval (how often a sample is recorded, default
   1 second).
4. Check "Total power strip" in the Display section to see the
   strip chart in the standalone window.

The current power reading is shown in dBFS below the controls.

Changing band limits or log interval clears the history and
restarts collection with the new settings.

The history is capped at 86,400 samples (24 hours at 1 sample
per second). Older samples are discarded automatically.


## Allan variance

### What it does

Measures your receiver's stability as a function of integration
time. This tells you how long you can usefully integrate before
gain drift starts degrading your measurement.

### How to read the plot

The Allan variance plot shows Allan deviation (ADEV) vs integration
time (tau) on a log-log scale.

- At short tau: noise dominates. ADEV decreases with increasing tau.
  This is the region where averaging helps.
- At the minimum: optimal integration time. This is the sweet spot
  for your hardware.
- At long tau: gain drift dominates. ADEV increases. Longer
  integration makes things worse.

For example, if the minimum is at tau = 60 seconds, integrating
beyond 60 seconds without recalibration does not improve sensitivity.

### How to use it

1. Check the **Allan Variance** checkbox.
2. Start integration and let it run for at least several minutes
   (the longer, the better the long-tau estimates).
3. Click **Compute Allan**.
4. Open the standalone window to see the plot.

You can click Compute Allan multiple times as more data accumulates.
Each click recomputes from the full time series.


## FFT engine

The plugin runs its own FFT engine on the raw IQ stream from the
SDR. This is independent of the SDR++ waterfall FFT, giving you
full spectral resolution in the standalone window regardless of
the waterfall display width.

Two controls in the sidebar panel (can only be changed while
stopped):

**FFT Size** controls the frequency resolution -- the ability to
distinguish two signals that are close together in frequency.
The resolution equals the sample rate divided by the FFT size.
Larger FFT = finer resolution, but more CPU and slower FFT rate.

Examples at 2.5 MS/s sample rate:

| FFT Size | Resolution | FFT rate (50% overlap) |
|----------|-----------|------------------------|
| 65536 | 38.1 Hz | 76 FFT/s |
| 131072 | 19.1 Hz | 38 FFT/s |
| 262144 | 9.5 Hz | 19 FFT/s |
| 524288 | 4.8 Hz | 9.5 FFT/s |
| 1048576 | 2.4 Hz | 4.8 FFT/s |

For hydrogen line work, 65536 (38 Hz resolution) is sufficient.
For narrow maser lines or precise velocity measurements, try
131072 or 262144.

**Overlap %** controls how fast spectra accumulate, not the
frequency resolution. With 0% overlap, each FFT uses a completely
new set of samples. With 50% overlap, each FFT reuses half the
samples from the previous one, doubling the FFT rate. With 75%,
the rate quadruples.

Higher overlap means more spectra per second, so the noise floor
drops faster during integration. The spectral resolution (ability
to resolve close frequencies) is unchanged -- that depends only
on the FFT size.

Default 50% is a good balance between speed and CPU usage.

The computed FFT rate and resolution are shown below the controls
in the sidebar.

**Window** selects the FFT window function. Each window trades off
frequency resolution (main lobe width) against spectral leakage
(sidelobe level):

- **Blackman-Harris** (default): 92 dB sidelobe rejection. Best
  for detecting weak spectral lines near strong signals or RFI.
  Slightly wider main lobe than Hann.
- **Hann**: 31 dB sidelobes, narrower main lobe. Use when frequency
  resolution matters more than dynamic range, in clean RF
  environments.
- **Nuttall**: 93 dB sidelobes, very similar to Blackman-Harris.
  Marginally better leakage rejection.
- **Flat-Top**: only 44 dB sidelobes, but near-zero scalloping
  loss (0.01 dB). Use when you need precise amplitude measurements
  of spectral lines -- the measured peak height is accurate to
  within 0.01 dB regardless of where the line falls relative to
  FFT bin centers.
- **Kaiser B=8**: 69 dB sidelobes with a good compromise between
  main lobe width and leakage. Intermediate choice.

For most radio astronomy work, Blackman-Harris is the right choice.
Switch to Flat-Top only when measuring line flux accurately.


## Standalone spectrum window

Open by checking "Separate window" in the Display section.

### Main spectrum plot

Shows the averaged spectrum at full FFT resolution. With a
65536-point FFT, you have 65536 data points to explore by
zooming in. The plot includes:
- Frequency axis in MHz along the bottom
- Power axis (dBFS or Kelvin) along the left
- Grid and sub-grid lines at round values (1-2-5 sequence)
- Shadow fill under the trace (same style as SDR++ spectrum)

### Zoom and pan

- **Mouse wheel on plot area**: zoom the frequency axis in/out
  centered on the cursor. Each step changes the span by 20%.
  Minimum span depends on FFT resolution (10 bins).
- **Mouse wheel on Y-axis** (left margin): zoom the power axis
  only.
- **Middle mouse drag**: pan on both axes.
- **Double-click**: reset all zoom to full range.

When zoomed in, the title shows the visible span (e.g.
"Averaged Spectrum (dBFS) [zoom: 95.4 Hz]"). The Y-axis
auto-rescales to the visible data when not manually zoomed.

Grid labels adapt to window size and zoom level. Labels never
overlap -- the number of labels adjusts to the available width.
Frequency precision increases automatically as you zoom deeper.

The zoom resets on frequency or sample rate change. Start and
Reset preserve the current zoom.

### Cursor readout

Hover the mouse over the plot area to see:
- Frequency in MHz
- Power in dBFS
- Radial velocity in km/s (when Velocity axis is enabled)
- Antenna temperature in K (when calibrated)

### Information lines

Above the plot:
1. Cursor readout (when hovering) or blank (when not)
2. Integration status: elapsed time and spectra count
3. FFT info: center frequency, bandwidth, bin count, resolution

### Sub-plots and resizing

Below the main spectrum, optional sub-plots appear when enabled:
- **Show residual**: baseline-subtracted spectrum
- **Total power strip**: band power vs time
- **Allan variance**: ADEV vs tau (after Compute Allan)

When multiple sub-plots are visible, you can **drag the border
between them** vertically to resize their heights. The cursor
changes to a resize arrow when hovering over the border.

### Input isolation

The window blocks all waterfall mouse input (clicks, drags,
scroll wheel) when active. You can zoom, pan, and interact
freely without affecting the SDR++ tuning.


## Spectral line tools

### Velocity readout

Check "Velocity axis" in the Display section. When you hover the
mouse over the standalone spectrum plot, the cursor readout adds
radial velocity in km/s.

The velocity is computed using the radio convention:
  v = c * (f_rest - f_obs) / f_rest

At the rest frequency, v = 0 km/s. Frequencies below the rest
frequency correspond to positive velocity (receding), above
corresponds to negative velocity (approaching).

This is essential for galactic hydrogen work: the hydrogen line
from different spiral arms is Doppler-shifted by their rotation
velocity, producing multiple peaks at different velocities.

### Rest frequency presets

The "Line" dropdown provides presets for common radio astronomy
spectral lines:

- HI 21cm: 1420.405752 MHz (neutral hydrogen)
- OH 18cm: 1612.231, 1665.402, 1667.359, 1720.530 MHz (hydroxyl)
- H2O 22 GHz: 22235.080 MHz (water maser)
- CH3OH 6.7 GHz: 6668.519 MHz (methanol maser)

Select a preset to set the rest frequency. The dropdown shows the
currently selected line name, or "Custom" if you entered a manual
frequency.


## Data export

### Manual export

Click **Export Now** to save the current averaged spectrum as a
CSV file. The file is saved to the configured export path (default:
`radio_astronomy/` in the SDR++ directory).

The filename is generated automatically from the center frequency
and current UTC time, for example:
  `ifavg_1420405752_20260318_103000.csv`

### Auto-export

Check "Auto-export" and set the interval in seconds. The plugin
will automatically write a CSV file at each interval. This is
useful for long unattended observations where you want periodic
snapshots of the averaged spectrum.

For example, to save a file every 5 minutes for a 3-hour
observation: set the interval to 300 seconds, start integration,
and leave the system running. You will get 36 files.

### CSV file format

The file starts with a metadata header containing all observation
parameters:

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

Followed by data columns:
- `frequency_hz`: center frequency of each FFT bin
- `power_dbfs`: power in decibels relative to full scale
- `power_linear`: raw linear power
- `power_kelvin`: antenna temperature (only if calibrated)
- `residual`: baseline-subtracted value (only if baseline enabled)
- `rfi_flag`: 0 = clean, 1 = flagged as RFI (only if RFI enabled)


## Tips and best practices

### Getting the best sensitivity

- Use **Linear Mean** mode for most observations.
- Set the FFT size as large as your computer can handle (65536 or
  higher) for better frequency resolution.
- Enable **RFI Mitigation** before starting, especially in urban
  environments.
- Integrate as long as possible, but check your Allan variance
  first to know the useful limit.

### Dealing with the receiver passband

- Enable **Baseline** with Polynomial order 3 as a starting point.
- If the passband shape is complex, try order 5 or higher.
- For the best results, use Reference mode: capture on blank sky,
  then observe your target.
- The residual plot in the standalone window shows how well the
  baseline removal is working.

### Long unattended observations

- Check **Continuous** mode.
- Enable **Auto-export** with an appropriate interval (e.g., 300
  seconds for 5-minute snapshots).
- Enable **Total Power** to get a time series of band power.
- Check **Reset on freq change** to protect against accidental
  retuning.

### Comparing with and without processing

- You can toggle Baseline and Calibration on and off at any time
  without losing your reference capture or calibration data.
- This lets you quickly compare the raw and processed views.
- RFI, Total Power, and Allan Variance clear their internal state
  when disabled, so re-enabling starts fresh.

### Troubleshooting

**No green trace appears after clicking Start:**
- Make sure the SDR source is running (Play button active).
- Check that "Overlay on spectrum" is checked.

**The green trace does not get smoother:**
- The SDR source may have stopped.
- Check for excessive RFI (most of the spectrum is interference).

**Baseline subtraction produces strange results:**
- Try a lower polynomial order.
- In Reference mode, make sure the reference was captured on blank
  sky with no signals present.

**Calibration gives negative Tsys:**
- The HOT and COLD captures may be swapped.
- T_cold may be set too high.

**Total power strip chart shows no data:**
- Check that "Total power strip" is enabled in Display.
- Wait at least 2 seconds for the first data points to appear.

**Integration resets unexpectedly:**
- Check if "Reset on freq change" or "Reset on SR change" is
  enabled and the frequency/sample rate is changing.
- The plugin's FFT size can only be changed while stopped (requires
  a new Start to take effect).
