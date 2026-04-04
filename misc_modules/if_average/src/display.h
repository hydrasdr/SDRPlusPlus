/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 *
 * Display engine for IF Average plugin.
 * Handles waterfall overlay drawing and the standalone spectrum window.
 */
#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <gui/widgets/waterfall.h>
#include <deque>
#include <vector>
#include "total_power.h"
#include "allan.h"

class DisplayEngine {
public:
	DisplayEngine();
	~DisplayEngine();

	/* Draw averaged spectrum overlay on the waterfall FFT area */
	void drawOverlay(ImGui::WaterFall::FFTRedrawArgs args,
			 const float *spectrum, int width,
			 double centerFreq, double bandwidth,
			 float fftMin, float fftMax);

	/* Draw the standalone spectrum analysis window */
	void drawStandaloneWindow(const float *spectrum,
				  const float *residual,
				  const float *kelvin,
				  int width,
				  double centerFreq,
				  double bandwidth,
				  int fftSize,
				  const std::deque<TotalPowerSample> *tpSamples,
				  const AllanVarianceAnalyzer *allan,
				  double elapsedSec,
				  uint64_t spectraCount,
				  bool integrating);

	/* Display settings (persisted in config) */
	bool overlayEnabled = true;
	ImU32 overlayColor = IM_COL32(0, 255, 128, 200);
	float overlayThickness = 2.0f;
	bool standaloneWindow = false;
	bool showResidual = false;
	bool showTotalPowerStrip = false;

	/* RFI flag overlay (pointer valid only during drawOverlay) */
	const std::vector<bool> *rfiFlags = nullptr;

	/* Spectral line presets for velocity readout */
	struct SpectralLine {
		const char *name;
		double freqHz;
	};
	static const SpectralLine spectralLines[];
	static const int numSpectralLines;

	double restFrequency = 1420405751.768; /* HI 21cm (Hz) */
	bool showVelocityAxis = false;

	/* Tracks window hover state for input blocking */
	bool standaloneWindowActive = false;

	/* Cursor frequency (set by zoom handler, read by readout) */
	double cursorFreq = 0.0;

	/* Sub-plot height ratios (sum to 1.0, adjusted by drag) */
	float plotRatios[4] = { 0.5f, 0.2f, 0.15f, 0.15f };
	int dragSeparator = -1;

	/* Zoom/pan state for spectrum plot (reset on new observation) */
	double viewFreqMin = 0.0;	/* Hz, left edge of view */
	double viewFreqMax = 0.0;	/* Hz, right edge of view */
	float viewDbMin = 0.0f;		/* dB or linear, bottom of view */
	float viewDbMax = 0.0f;		/* dB or linear, top of view */
	bool viewInitialized = false;
	bool yZoomActive = false;	/* true when user has zoomed Y */

	void resetView() {
		viewInitialized = false;
		yZoomActive = false;
		viewFreqMin = 0.0;
		viewFreqMax = 0.0;
		viewDbMin = 0.0f;
		viewDbMax = 0.0f;
	}

private:
	void drawSpectrumPlot(const float *data, int width,
			      double centerFreq, double bandwidth,
			      ImVec2 plotMin, ImVec2 plotMax,
			      const char *label, ImU32 color,
			      bool linearScale = false);

	/* Shared trace rendering: downsample + AddLine + shadow */
	void drawTrace(ImDrawList *dl,
		       const float *data, int width,
		       double dataFreqMin, double dataFreqStep,
		       double viewFreqLo, double viewFreqHi,
		       float yMin, float yRange,
		       ImVec2 areaMin, ImVec2 areaMax,
		       ImU32 color, bool linearScale,
		       float lineThickness = 1.0f);

	void drawTotalPowerStrip(const std::deque<TotalPowerSample> &samples,
				 ImVec2 plotMin, ImVec2 plotMax);

	void drawAllanPlot(const AllanVarianceAnalyzer *allan,
			   ImVec2 plotMin, ImVec2 plotMax);
};
