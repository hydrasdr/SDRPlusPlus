/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "display.h"
#include <cmath>
#include <algorithm>
#include <cstdio>

#define C_LIGHT       299792458.0   /* speed of light (m/s) */
#define LOG10_FLOOR   1e-30f        /* floor for log10 to avoid log(0) */
#define PLOT_MARGIN_L 55.0f         /* left margin for Y-axis labels */
#define PLOT_MARGIN_T 16.0f         /* top margin for title */
#define PLOT_MARGIN_B 16.0f         /* bottom margin for X-axis labels */
#define LABEL_PAD     20.0f         /* padding between X-axis labels */
#define MIN_LABEL_H   40.0f         /* minimum pixels between Y labels */
#define SEP_HIT_H     6.0f          /* separator drag hit zone height */
#define SEP_MIN_H     30.0f         /* minimum sub-plot height */
#define MIN_ZOOM_BINS 10            /* minimum bins visible when zoomed */
#define ZOOM_FACTOR   0.8           /* scroll zoom ratio per step */

/* Convert linear power to dBFS */
static inline float toDb(float linear) {
	if (linear < LOG10_FLOOR) linear = LOG10_FLOOR;
	return 10.0f * log10f(linear);
}

const DisplayEngine::SpectralLine DisplayEngine::spectralLines[] = {
	{ "HI 21cm",      1420405751.768 },
	{ "OH 1612",      1612231000.0 },
	{ "OH 1665",      1665402000.0 },
	{ "OH 1667",      1667359000.0 },
	{ "OH 1720",      1720530000.0 },
	{ "H2O 22GHz",    22235080000.0 },
	{ "CH3OH 6.7GHz", 6668519000.0 },
};
const int DisplayEngine::numSpectralLines =
	sizeof(DisplayEngine::spectralLines) /
	sizeof(DisplayEngine::spectralLines[0]);

DisplayEngine::DisplayEngine() {}

DisplayEngine::~DisplayEngine() {}

void DisplayEngine::drawOverlay(ImGui::WaterFall::FFTRedrawArgs args,
				const float *spectrum, int width,
				double centerFreq, double bandwidth,
				float fftMin, float fftMax)
{
	if (!overlayEnabled || !spectrum || width <= 0)
		return;

	double freqStart = centerFreq - bandwidth / 2.0;
	double freqStep = bandwidth / (double)width;
	float dbRange = fftMax - fftMin;
	if (dbRange < 1.0f)
		dbRange = 1.0f;

	drawTrace(args.window->DrawList, spectrum, width,
		  freqStart, freqStep,
		  args.lowFreq, args.highFreq,
		  fftMin, dbRange,
		  args.min, args.max,
		  overlayColor, false, overlayThickness);

	/* Draw RFI flags as red translucent rectangles */
	ImDrawList *dl = args.window->DrawList;
	if (rfiFlags && (int)rfiFlags->size() == width) {
		int flagStart = -1;
		for (int i = 0; i <= width; i++) {
			bool flagged = (i < width) && (*rfiFlags)[i];
			if (flagged && flagStart < 0) {
				flagStart = i;
			} else if (!flagged && flagStart >= 0) {
				double f1 = freqStart + flagStart * freqStep;
				double f2 = freqStart + i * freqStep;
				float x1 = args.min.x +
					    (float)((f1 - args.lowFreq) *
						    args.freqToPixelRatio);
				float x2 = args.min.x +
					    (float)((f2 - args.lowFreq) *
						    args.freqToPixelRatio);
				x1 = std::clamp(x1, args.min.x, args.max.x);
				x2 = std::clamp(x2, args.min.x, args.max.x);
				if (x2 > x1) {
					dl->AddRectFilled(
						ImVec2(x1, args.min.y),
						ImVec2(x2, args.max.y),
						IM_COL32(255, 0, 0, 40));
				}
				flagStart = -1;
			}
		}
	}
}

void DisplayEngine::drawStandaloneWindow(const float *spectrum,
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
					 bool integrating)
{
	if (!standaloneWindow) {
		standaloneWindowActive = false;
		return;
	}

	bool open = true;
	ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("IF Average - Spectrum", &open)) {
		/* Window is collapsed -- still block waterfall input
		 * if the title bar is hovered or focused, to prevent
		 * clicks from passing through to the waterfall. */
		standaloneWindowActive =
			ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
			ImGui::IsWindowFocused();
		ImGui::End();
		if (!open)
			standaloneWindow = false;
		return;
	}

	if (!open)
		standaloneWindow = false;

	standaloneWindowActive =
		ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
				       ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
		ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

	/*
	 * Save mouse wheel value for zoom, then consume it to prevent
	 * the waterfall from scrolling when this window is active.
	 */
	float savedMouseWheel = 0.0f;
	if (standaloneWindowActive) {
		savedMouseWheel = ImGui::GetIO().MouseWheel;
		ImGui::GetIO().MouseWheel = 0.0f;
		ImGui::GetIO().MouseWheelH = 0.0f;
	}

	ImVec2 contentSize = ImGui::GetContentRegionAvail();
	float cursorX = ImGui::GetCursorScreenPos().x;
	float cursorY = ImGui::GetCursorScreenPos().y;

	/* Cursor readout (updated by zoom handler when mouse is over plot) */
	if (spectrum && width > 0 && cursorFreq > 0.0) {
		double dataStart = centerFreq - bandwidth / 2.0;
		double dataStep = bandwidth / (double)width;
		int bin = std::clamp(
			(int)((cursorFreq - dataStart) / dataStep),
			0, width - 1);
		float power = spectrum[bin];
		float powerDb = toDb(power);

		char buf[256];
		int pos = snprintf(buf, sizeof(buf),
				   "Freq: %.6f MHz", cursorFreq / 1e6);
		if (showVelocityAxis) {
			double velocity = C_LIGHT *
					  (restFrequency - cursorFreq) /
					  restFrequency / 1000.0;
			pos += snprintf(buf + pos, sizeof(buf) - pos,
					" | v: %.1f km/s", velocity);
		}
		pos += snprintf(buf + pos, sizeof(buf) - pos,
				" | %.1f dBFS", (double)powerDb);
		if (kelvin)
			snprintf(buf + pos, sizeof(buf) - pos,
				 " | %.1f K", (double)kelvin[bin]);
		ImGui::Text("%s", buf);
	} else {
		ImGui::Text(" ");
	}
	cursorFreq = 0.0;

	/* Integration stats */
	{
		int h = (int)elapsedSec / 3600;
		int m = ((int)elapsedSec % 3600) / 60;
		int s = (int)elapsedSec % 60;
		if (integrating)
			ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f),
					   "Integrating %02d:%02d:%02d | Spectra: %llu",
					   h, m, s, (unsigned long long)spectraCount);
		else
			ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
					   "Stopped %02d:%02d:%02d | Spectra: %llu",
					   h, m, s, (unsigned long long)spectraCount);
	}

	/* FFT info line */
	if (width > 0 && bandwidth > 0.0) {
		/* Actual resolution is limited by display bin count,
		 * not the SDR++ FFT size setting */
		double actualRes = bandwidth / (double)width;
		char infoBuf[256];
		int pos = snprintf(infoBuf, sizeof(infoBuf),
				   "Center: %.6f MHz | BW: ",
				   centerFreq / 1e6);
		if (bandwidth >= 1e6)
			pos += snprintf(infoBuf + pos, sizeof(infoBuf) - pos,
					"%.3f MHz", bandwidth / 1e6);
		else
			pos += snprintf(infoBuf + pos, sizeof(infoBuf) - pos,
					"%.3f kHz", bandwidth / 1e3);
		pos += snprintf(infoBuf + pos, sizeof(infoBuf) - pos,
				" | %d bins", width);
		if (actualRes >= 1000.0)
			pos += snprintf(infoBuf + pos, sizeof(infoBuf) - pos,
					" @ %.1f kHz", actualRes / 1e3);
		else
			pos += snprintf(infoBuf + pos, sizeof(infoBuf) - pos,
					" @ %.1f Hz", actualRes);
		if (fftSize != width)
			snprintf(infoBuf + pos, sizeof(infoBuf) - pos,
				 " (FFT %d)", fftSize);
		ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
				   "%s", infoBuf);
	}

	/* Update positions after text */
	cursorY = ImGui::GetCursorScreenPos().y;
	contentSize = ImGui::GetContentRegionAvail();

	/* Determine which sub-plots are active */
	bool hasResidual = showResidual;
	bool hasTP = showTotalPowerStrip;
	bool hasAllan = (allan && allan->hasResults());
	/* Normalize ratios to active plots */
	int plotIdx[4];
	int slotCount = 0;
	plotIdx[slotCount++] = 0; /* main spectrum always slot 0 */
	if (hasResidual)  plotIdx[slotCount++] = 1;
	if (hasTP)        plotIdx[slotCount++] = 2;
	if (hasAllan)     plotIdx[slotCount++] = 3;

	/* Ensure ratios sum to 1.0 */
	float ratioSum = 0.0f;
	for (int i = 0; i < slotCount; i++)
		ratioSum += plotRatios[plotIdx[i]];
	if (ratioSum < 0.01f) ratioSum = 1.0f;

	float totalH = contentSize.y;

	/* Initialize or update view range */
	double fullFreqMin = centerFreq - bandwidth / 2.0;
	double fullFreqMax = centerFreq + bandwidth / 2.0;
	if (!viewInitialized || viewFreqMin >= viewFreqMax) {
		viewFreqMin = fullFreqMin;
		viewFreqMax = fullFreqMax;
		viewInitialized = true;
	}

	/* Compute heights for each active sub-plot */
	float slotH[4];
	for (int i = 0; i < slotCount; i++)
		slotH[i] = totalH * plotRatios[plotIdx[i]] / ratioSum;

	/* Main spectrum plot */
	float mainH = slotH[0];
	if (spectrum && width > 0) {
		ImVec2 plotMin = ImVec2(cursorX, cursorY);
		ImVec2 plotMax = ImVec2(cursorX + contentSize.x,
					cursorY + mainH);

		/* Zoom/pan interaction on the main plot area */
		float leftMargin = PLOT_MARGIN_L;
		float topMargin = PLOT_MARGIN_T;
		float bottomMargin = PLOT_MARGIN_B;
		ImVec2 areaMin = ImVec2(plotMin.x + leftMargin,
					plotMin.y + topMargin);
		ImVec2 areaMax = ImVec2(plotMax.x,
					plotMax.y - bottomMargin);

		/*
		 * Zone-based zoom:
		 * - Wheel on Y-axis margin (left): zoom Y only
		 * - Wheel on X-axis margin (bottom): zoom X only
		 * - Wheel on plot area: zoom both X and Y
		 * - Middle-drag anywhere: pan X
		 * - Double-click: reset all zoom
		 */
		ImVec2 mpos = ImGui::GetMousePos();
		bool inPlot = ImGui::IsMouseHoveringRect(areaMin, areaMax);
		bool inYAxis = (mpos.x >= plotMin.x && mpos.x < areaMin.x &&
				mpos.y >= areaMin.y && mpos.y <= areaMax.y);
		bool inXAxis = (mpos.x >= areaMin.x && mpos.x <= areaMax.x &&
				mpos.y > areaMax.y && mpos.y <= plotMax.y);

		/* Update cursor frequency for readout */
		if (inPlot) {
			double t = (double)(mpos.x - areaMin.x) /
				   (double)(areaMax.x - areaMin.x);
			t = std::clamp(t, 0.0, 1.0);
			cursorFreq = viewFreqMin +
				     t * (viewFreqMax - viewFreqMin);
		}

		if (savedMouseWheel != 0.0f && (inPlot || inYAxis || inXAxis)) {
			double zf = (savedMouseWheel > 0.0f) ? ZOOM_FACTOR : (1.0 / ZOOM_FACTOR);

			/* X zoom (plot area or X-axis margin, not Y margin) */
			if (inPlot || inXAxis) {
				double tx = (double)(mpos.x - areaMin.x) /
					    (double)(areaMax.x - areaMin.x);
				tx = std::clamp(tx, 0.0, 1.0);
				double fc = viewFreqMin +
					    tx * (viewFreqMax - viewFreqMin);
				double newSpan = (viewFreqMax - viewFreqMin) * zf;
				if (newSpan > bandwidth) newSpan = bandwidth;
				/* Min zoom: 10 bins worth of resolution */
				double minSpan = bandwidth / (double)width * MIN_ZOOM_BINS;
				if (minSpan < 1.0) minSpan = 1.0;
				if (newSpan < minSpan) newSpan = minSpan;

				viewFreqMin = fc - tx * newSpan;
				viewFreqMax = fc + (1.0 - tx) * newSpan;

				if (viewFreqMin < fullFreqMin) {
					viewFreqMin = fullFreqMin;
					viewFreqMax = viewFreqMin + newSpan;
				}
				if (viewFreqMax > fullFreqMax) {
					viewFreqMax = fullFreqMax;
					viewFreqMin = viewFreqMax - newSpan;
				}
			}

			/* Y zoom (Y-axis margin only) */
			if (inYAxis && spectrum) {
				if (!yZoomActive) {
					/*
					 * First Y zoom: compute auto-range
					 * from visible data as the starting
					 * point for zoom.
					 */
					double dfs = centerFreq - bandwidth / 2.0;
					double dfst = bandwidth / (double)width;
					int b0 = std::clamp(
						(int)((viewFreqMin - dfs) / dfst),
						0, width - 1);
					int b1 = std::clamp(
						(int)((viewFreqMax - dfs) / dfst),
						0, width - 1);
					float lo = 1e30f, hi = -1e30f;
					const float *src = kelvin ? kelvin : spectrum;
					bool lin = (kelvin != nullptr);
					for (int i = b0; i <= b1; i++) {
						float v;
						if (lin) {
							v = src[i];
						} else {
							v = toDb(src[i]);
						}
						if (v < lo) lo = v;
						if (v > hi) hi = v;
					}
					float r = hi - lo;
					if (r < 1e-6f) r = 1.0f;
					viewDbMin = lo - r * 0.05f;
					viewDbMax = hi + r * 0.05f;
					yZoomActive = true;
				}

				float ty = (areaMax.y - mpos.y) /
					   (areaMax.y - areaMin.y);
				ty = std::clamp(ty, 0.0f, 1.0f);
				float vc = viewDbMin +
					   ty * (viewDbMax - viewDbMin);
				float newRange = (viewDbMax - viewDbMin) *
						 (float)zf;
				if (newRange < 0.1f) newRange = 0.1f;

				viewDbMin = vc - ty * newRange;
				viewDbMax = vc + (1.0f - ty) * newRange;
			}
		}

		/* Pan with middle mouse button drag */
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) &&
		    (inPlot || inYAxis || inXAxis)) {
			ImVec2 delta = ImGui::GetIO().MouseDelta;
			double pxPerHz = (double)(areaMax.x - areaMin.x) /
					 (viewFreqMax - viewFreqMin);
			double freqShift = -(double)delta.x / pxPerHz;

			viewFreqMin += freqShift;
			viewFreqMax += freqShift;

			if (viewFreqMin < fullFreqMin) {
				double span = viewFreqMax - viewFreqMin;
				viewFreqMin = fullFreqMin;
				viewFreqMax = fullFreqMin + span;
			}
			if (viewFreqMax > fullFreqMax) {
				double span = viewFreqMax - viewFreqMin;
				viewFreqMax = fullFreqMax;
				viewFreqMin = fullFreqMax - span;
			}

			/* Y pan */
			if (yZoomActive) {
				float pxPerDb = (areaMax.y - areaMin.y) /
						(viewDbMax - viewDbMin);
				float dbShift = (float)delta.y / pxPerDb;
				viewDbMin += dbShift;
				viewDbMax += dbShift;
			}
		}

		/* Double-click to reset all zoom */
		if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
		    (inPlot || inYAxis || inXAxis)) {
			viewFreqMin = fullFreqMin;
			viewFreqMax = fullFreqMax;
			yZoomActive = false;
		}

		if (kelvin) {
			drawSpectrumPlot(kelvin, width, centerFreq,
					 bandwidth, plotMin, plotMax,
					 "Antenna Temperature (K)",
					 overlayColor, true);
		} else {
			drawSpectrumPlot(spectrum, width, centerFreq,
					 bandwidth, plotMin, plotMax,
					 "Averaged Spectrum (dBFS)",
					 overlayColor);
		}
		cursorY += mainH;
	}

	/* Helper: draw drag separator between sub-plots.
	 * Uses InvisibleButton to capture mouse and prevent
	 * ImGui from interpreting the drag as a window move. */
	auto drawSeparator = [&](int sepIdx) {
		float sepH = SEP_HIT_H;
		ImGui::SetCursorScreenPos(ImVec2(cursorX, cursorY - sepH / 2));

		char sepId[32];
		snprintf(sepId, sizeof(sepId), "##ifavg_sep%d", sepIdx);
		ImGui::InvisibleButton(sepId,
				       ImVec2(contentSize.x, sepH));

		if (ImGui::IsItemHovered())
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

		if (ImGui::IsItemActive() &&
		    ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
			float delta = ImGui::GetIO().MouseDelta.y;
			float minH = SEP_MIN_H;
			float r0 = slotH[sepIdx] + delta;
			float r1 = slotH[sepIdx + 1] - delta;
			if (r0 >= minH && r1 >= minH) {
				plotRatios[plotIdx[sepIdx]] +=
					delta / totalH * ratioSum;
				plotRatios[plotIdx[sepIdx + 1]] -=
					delta / totalH * ratioSum;
			}
		}
	};

	int curSlot = 1;

	/* Residual plot */
	if (hasResidual) {
		if (slotCount > curSlot)
			drawSeparator(curSlot - 1);
		float h = slotH[curSlot];
		ImVec2 plotMin = ImVec2(cursorX, cursorY);
		ImVec2 plotMax = ImVec2(cursorX + contentSize.x,
					cursorY + h);
		if (residual && width > 0) {
			drawSpectrumPlot(residual, width, centerFreq,
					 bandwidth, plotMin, plotMax,
					 "Residual (enable Baseline)",
					 IM_COL32(255, 200, 0, 200), true);
		} else {
			ImDrawList *dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled(plotMin, plotMax,
					  IM_COL32(20, 20, 30, 255));
			dl->AddRect(plotMin, plotMax,
				    IM_COL32(80, 80, 80, 255));
			dl->AddText(ImVec2(plotMin.x + 5, plotMin.y + 2),
				    IM_COL32(200, 200, 200, 255),
				    "Residual -- no data");
		}
		cursorY += h;
		curSlot++;
	}

	/* Total power strip chart */
	if (hasTP) {
		if (slotCount > curSlot)
			drawSeparator(curSlot - 1);
		float h = slotH[curSlot];
		ImVec2 plotMin = ImVec2(cursorX, cursorY);
		ImVec2 plotMax = ImVec2(cursorX + contentSize.x,
					cursorY + h);
		if (tpSamples) {
			drawTotalPowerStrip(*tpSamples, plotMin, plotMax);
		} else {
			ImDrawList *dl = ImGui::GetWindowDrawList();
			dl->AddRectFilled(plotMin, plotMax,
					  IM_COL32(20, 20, 30, 255));
			dl->AddRect(plotMin, plotMax,
				    IM_COL32(80, 80, 80, 255));
			dl->AddText(ImVec2(plotMin.x + 5, plotMin.y + 2),
				    IM_COL32(200, 200, 200, 255),
				    "Total Power -- enable Total Power");
		}
		cursorY += h;
		curSlot++;
	}

	/* Allan variance plot */
	if (hasAllan) {
		if (slotCount > curSlot)
			drawSeparator(curSlot - 1);
		float h = slotH[curSlot];
		ImVec2 plotMin = ImVec2(cursorX, cursorY);
		ImVec2 plotMax = ImVec2(cursorX + contentSize.x,
					cursorY + h);
		drawAllanPlot(allan, plotMin, plotMax);
	}

	ImGui::End();
}

/*
 * Compute a "nice" grid step for axis labels.
 * Returns a round number (1, 2, 5 sequence) that produces
 * approximately targetDivisions divisions across the range.
 */
static double niceGridStep(double range, int targetDivisions)
{
	double raw = range / (double)targetDivisions;
	double mag = pow(10.0, floor(log10(fabs(raw))));
	double norm = raw / mag;

	double nice;
	if (norm <= 1.5)      nice = 1.0;
	else if (norm <= 3.5) nice = 2.0;
	else if (norm <= 7.5) nice = 5.0;
	else                  nice = 10.0;

	return nice * mag;
}

/*
 * Format frequency label with appropriate precision.
 * Shows MHz with enough decimals to distinguish adjacent labels.
 */
static void formatFreqLabel(char *buf, int bufSize, double freqHz,
			    double stepHz)
{
	double freqMHz = freqHz / 1e6;
	if (stepHz >= 1e6)
		snprintf(buf, bufSize, "%.0f", freqMHz);
	else if (stepHz >= 1e5)
		snprintf(buf, bufSize, "%.1f", freqMHz);
	else if (stepHz >= 1e4)
		snprintf(buf, bufSize, "%.2f", freqMHz);
	else if (stepHz >= 1e3)
		snprintf(buf, bufSize, "%.3f", freqMHz);
	else if (stepHz >= 100.0)
		snprintf(buf, bufSize, "%.4f", freqMHz);
	else if (stepHz >= 10.0)
		snprintf(buf, bufSize, "%.5f", freqMHz);
	else
		snprintf(buf, bufSize, "%.6f", freqMHz);
}

void DisplayEngine::drawSpectrumPlot(const float *data, int width,
				     double centerFreq, double bandwidth,
				     ImVec2 plotMin, ImVec2 plotMax,
				     const char *label, ImU32 color,
				     bool linearScale)
{
	ImDrawList *dl = ImGui::GetWindowDrawList();

	float leftMargin = 55.0f;
	float topMargin = 16.0f;
	float bottomMargin = 16.0f;

	ImVec2 areaMin = ImVec2(plotMin.x + leftMargin,
				plotMin.y + topMargin);
	ImVec2 areaMax = ImVec2(plotMax.x, plotMax.y - bottomMargin);
	float areaW = areaMax.x - areaMin.x;
	float areaH = areaMax.y - areaMin.y;

	/* Background and border */
	dl->AddRectFilled(plotMin, plotMax, IM_COL32(20, 20, 30, 255));
	dl->AddRect(areaMin, areaMax, IM_COL32(80, 80, 80, 255));

	/* Title above grid */
	double viewSpanHz = viewFreqMax - viewFreqMin;
	double fullSpanHz = bandwidth;
	char titleBuf[128];
	if (viewSpanHz < fullSpanHz * 0.99) {
		/* Show zoom level when zoomed in */
		snprintf(titleBuf, sizeof(titleBuf), "%s [zoom: ", label);
		int pos = (int)strlen(titleBuf);
		if (viewSpanHz >= 1e6)
			snprintf(titleBuf + pos, sizeof(titleBuf) - pos,
				 "%.3f MHz]", viewSpanHz / 1e6);
		else if (viewSpanHz >= 1e3)
			snprintf(titleBuf + pos, sizeof(titleBuf) - pos,
				 "%.1f kHz]", viewSpanHz / 1e3);
		else
			snprintf(titleBuf + pos, sizeof(titleBuf) - pos,
				 "%.0f Hz]", viewSpanHz);
	} else {
		snprintf(titleBuf, sizeof(titleBuf), "%s", label);
	}
	dl->AddText(ImVec2(areaMin.x + 5, plotMin.y + 1),
		    IM_COL32(200, 200, 200, 255), titleBuf);

	/* Map data range to view range */
	double dataFreqMin = centerFreq - bandwidth / 2.0;
	double dataFreqStep = bandwidth / (double)width;

	/* Compute visible bin range (extend by 1 bin on each side
	 * so the polyline reaches the plot edges) */
	int viewBinStart = std::clamp(
		(int)((viewFreqMin - dataFreqMin) / dataFreqStep) - 1,
		0, width - 1);
	int viewBinEnd = std::clamp(
		(int)((viewFreqMax - dataFreqMin) / dataFreqStep) + 1,
		0, width - 1);

	/* Y-axis range: use user zoom if active, else auto-range */
	float minVal, maxVal;
	if (yZoomActive) {
		minVal = viewDbMin;
		maxVal = viewDbMax;
	} else {
		minVal = 1e30f;
		maxVal = -1e30f;
		for (int i = viewBinStart; i <= viewBinEnd; i++) {
			float val;
			if (linearScale) {
				val = data[i];
			} else {
				val = toDb(data[i]);
			}
			if (val < minVal) minVal = val;
			if (val > maxVal) maxVal = val;
		}
		float yRange = maxVal - minVal;
		if (yRange < 1e-6f)
			yRange = 1.0f;
		minVal -= yRange * 0.05f;
		maxVal += yRange * 0.05f;

		/* Store auto-range as base for Y zoom */
		viewDbMin = minVal;
		viewDbMax = maxVal;
	}
	float yRange = maxVal - minVal;
	if (yRange < 1e-6f)
		yRange = 1.0f;

	/*
	 * Y-axis grid: adapt number of divisions to plot height.
	 * Aim for ~40 pixels between labels minimum.
	 */
	int yDivTarget = std::clamp((int)(areaH / MIN_LABEL_H), 2, 20);
	double yStep = niceGridStep((double)yRange, yDivTarget);
	double yGridStart = ceil((double)minVal / yStep) * yStep;

	for (double gv = yGridStart; gv < (double)maxVal; gv += yStep) {
		float y = areaMax.y - areaH *
			  (float)((gv - (double)minVal) / (double)yRange);
		if (y <= areaMin.y + 8 || y >= areaMax.y - 2)
			continue;

		dl->AddLine(ImVec2(areaMin.x, y), ImVec2(areaMax.x, y),
			    IM_COL32(70, 70, 70, 255));

		char buf[32];
		if (linearScale)
			snprintf(buf, sizeof(buf), "%.1e", gv);
		else if (yStep >= 1.0)
			snprintf(buf, sizeof(buf), "%.0f", gv);
		else
			snprintf(buf, sizeof(buf), "%.1f", gv);
		dl->AddText(ImVec2(plotMin.x + 2, y - 7),
			    IM_COL32(150, 150, 150, 255), buf);
	}

	/* Y-axis sub-grid */
	double ySubStep = yStep / 2.0;
	for (double gv = yGridStart - yStep; gv < (double)maxVal + yStep;
	     gv += ySubStep) {
		float y = areaMax.y - areaH *
			  (float)((gv - (double)minVal) / (double)yRange);
		if (y <= areaMin.y || y >= areaMax.y)
			continue;
		double rem = fmod(fabs(gv - yGridStart), yStep);
		if (rem < ySubStep * 0.1 || rem > yStep - ySubStep * 0.1)
			continue;
		dl->AddLine(ImVec2(areaMin.x, y), ImVec2(areaMax.x, y),
			    IM_COL32(40, 40, 40, 255));
	}

	/*
	 * X-axis grid: adapt divisions to plot width.
	 * Measure label width to avoid overlap.
	 */
	char sampleLabel[32];
	formatFreqLabel(sampleLabel, sizeof(sampleLabel),
			viewFreqMin + viewSpanHz / 2.0, viewSpanHz / 5.0);
	float labelWidth = ImGui::CalcTextSize(sampleLabel).x + LABEL_PAD;
	int xDivTarget = std::clamp((int)(areaW / labelWidth), 2, 20);
	double xStep = niceGridStep(viewSpanHz, xDivTarget);
	double xGridStart = ceil(viewFreqMin / xStep) * xStep;

	float lastLabelRight = -1e6f;
	for (double gf = xGridStart; gf < viewFreqMax; gf += xStep) {
		float x = areaMin.x + areaW *
			  (float)((gf - viewFreqMin) / viewSpanHz);
		if (x <= areaMin.x + 2 || x >= areaMax.x - 2)
			continue;

		dl->AddLine(ImVec2(x, areaMin.y), ImVec2(x, areaMax.y),
			    IM_COL32(70, 70, 70, 255));

		char buf[32];
		formatFreqLabel(buf, sizeof(buf), gf, xStep);
		ImVec2 textSize = ImGui::CalcTextSize(buf);
		float tx = x - textSize.x / 2.0f;

		/* Skip label if it would overlap the previous one */
		if (tx > lastLabelRight + 4.0f &&
		    tx >= plotMin.x + 2.0f &&
		    tx + textSize.x <= plotMax.x - 2.0f) {
			dl->AddText(ImVec2(tx, areaMax.y + 1),
				    IM_COL32(150, 150, 150, 255), buf);
			lastLabelRight = tx + textSize.x;
		}
	}

	/* X-axis sub-grid */
	double xSubStep = xStep / 2.0;
	for (double gf = xGridStart - xStep; gf < viewFreqMax + xStep;
	     gf += xSubStep) {
		float x = areaMin.x + areaW *
			  (float)((gf - viewFreqMin) / viewSpanHz);
		if (x <= areaMin.x || x >= areaMax.x)
			continue;
		double rem = fmod(fabs(gf - xGridStart), xStep);
		if (rem < xSubStep * 0.1 || rem > xStep - xSubStep * 0.1)
			continue;
		dl->AddLine(ImVec2(x, areaMin.y), ImVec2(x, areaMax.y),
			    IM_COL32(40, 40, 40, 255));
	}

	/* Zero line for residual plots */
	if (linearScale && minVal < 0.0f && maxVal > 0.0f) {
		float zeroY = areaMax.y - areaH * (0.0f - minVal) / yRange;
		dl->AddLine(ImVec2(areaMin.x, zeroY),
			    ImVec2(areaMax.x, zeroY),
			    IM_COL32(100, 100, 100, 200));
	}

	drawTrace(dl, data, width, dataFreqMin, dataFreqStep,
		  viewFreqMin, viewFreqMax, minVal, yRange,
		  areaMin, areaMax, color, linearScale);
}

void DisplayEngine::drawTrace(ImDrawList *dl,
			      const float *data, int width,
			      double dataFreqMin, double dataFreqStep,
			      double viewFreqLo, double viewFreqHi,
			      float yMin, float yRange,
			      ImVec2 areaMin, ImVec2 areaMax,
			      ImU32 color, bool linearScale,
			      float lineThickness)
{
	float areaW = areaMax.x - areaMin.x;
	float areaH = areaMax.y - areaMin.y;
	double viewSpan = viewFreqHi - viewFreqLo;
	if (viewSpan <= 0.0 || areaW < 2.0f || areaH < 2.0f)
		return;

	int pixelWidth = (int)areaW;
	if (pixelWidth < 2) pixelWidth = 2;

	/* Visible bin range (extend by 1 on each side) */
	int binStart = std::clamp(
		(int)((viewFreqLo - dataFreqMin) / dataFreqStep) - 1,
		0, width - 1);
	int binEnd = std::clamp(
		(int)((viewFreqHi - dataFreqMin) / dataFreqStep) + 1,
		0, width - 1);
	int numBins = binEnd - binStart + 1;
	if (numBins < 1) return;

	/* Pre-convert to display values */
	std::vector<float> vals(numBins);
	for (int i = 0; i < numBins; i++) {
		if (linearScale) {
			vals[i] = data[binStart + i];
		} else {
			vals[i] = toDb(data[binStart + i]);
		}
	}

	double binsPerPixel = (double)numBins / (double)pixelWidth;

	/* Compute one Y value per pixel */
	std::vector<float> pixelY(pixelWidth);
	for (int px = 0; px < pixelWidth; px++) {
		float val;
		if (binsPerPixel <= 2.0) {
			/* Interpolate */
			double freq = viewFreqLo + viewSpan *
				      ((double)px + 0.5) / (double)pixelWidth;
			double bp = (freq - dataFreqMin) / dataFreqStep
				    - (double)binStart;
			int b0 = std::clamp((int)floor(bp), 0, numBins - 1);
			int b1 = std::clamp(b0 + 1, 0, numBins - 1);
			float frac = (float)(bp - floor(bp));
			val = vals[b0] * (1.0f - frac) + vals[b1] * frac;
		} else {
			/* Peak per pixel */
			int b0 = (int)((double)px * numBins / pixelWidth);
			int b1 = (int)((double)(px + 1) * numBins / pixelWidth);
			b0 = std::clamp(b0, 0, numBins - 1);
			b1 = std::clamp(b1, 0, numBins - 1);
			val = vals[b0];
			for (int i = b0 + 1; i <= b1; i++)
				if (vals[i] > val) val = vals[i];
		}
		float y = areaMax.y - areaH * (val - yMin) / yRange;
		pixelY[px] = std::clamp(y, areaMin.y, areaMax.y);
	}

	/* Draw trace + shadow fill */
	ImU32 shadow = (color & 0x00FFFFFF) | 0x20000000;
	dl->PushClipRect(areaMin, areaMax, true);
	for (int i = 1; i < pixelWidth; i++) {
		float x0 = areaMin.x + (float)(i - 1);
		float x1 = areaMin.x + (float)i;
		float y0 = roundf(pixelY[i - 1]);
		float y1 = roundf(pixelY[i]);
		dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), color,
			    lineThickness);
		dl->AddLine(ImVec2(x1, y1), ImVec2(x1, areaMax.y),
			    shadow, 1.0f);
	}
	dl->PopClipRect();
}

void DisplayEngine::drawTotalPowerStrip(const std::deque<TotalPowerSample> &samples,
					ImVec2 plotMin, ImVec2 plotMax)
{
	ImDrawList *dl = ImGui::GetWindowDrawList();

	dl->AddRectFilled(plotMin, plotMax, IM_COL32(20, 20, 30, 255));
	dl->AddRect(plotMin, plotMax, IM_COL32(80, 80, 80, 255));
	dl->AddText(ImVec2(plotMin.x + 5, plotMin.y + 2),
		    IM_COL32(200, 200, 200, 255), "Total Power vs Time");

	if (samples.size() < 2) {
		dl->AddText(ImVec2(plotMin.x + 5, plotMin.y + 16),
			    IM_COL32(150, 150, 150, 255),
			    "Waiting for data...");
		return;
	}

	float plotW = plotMax.x - plotMin.x;
	float plotH = plotMax.y - plotMin.y;

	double minDb = 1e30, maxDb = -1e30;
	for (auto &s : samples) {
		if (s.powerDb < minDb) minDb = s.powerDb;
		if (s.powerDb > maxDb) maxDb = s.powerDb;
	}
	double range = maxDb - minDb;
	if (range < 1.0) range = 1.0;
	minDb -= range * 0.05;
	maxDb += range * 0.05;
	range = maxDb - minDb;

	double tStart = samples.front().timestamp;
	double tEnd = samples.back().timestamp;
	double tRange = tEnd - tStart;
	if (tRange < 0.001)
		tRange = 1.0;

	/* Y-axis grid (dBFS) */
	for (int i = 1; i < 4; i++) {
		float y = plotMin.y + plotH * (float)i / 4.0f;
		dl->AddLine(ImVec2(plotMin.x, y), ImVec2(plotMax.x, y),
			    IM_COL32(60, 60, 60, 255));
		char buf[32];
		snprintf(buf, sizeof(buf), "%.1f dB",
			 maxDb - range * (double)i / 4.0);
		dl->AddText(ImVec2(plotMin.x + 2, y - 12),
			    IM_COL32(150, 150, 150, 255), buf);
	}

	/* X-axis time labels */
	for (int i = 0; i <= 4; i++) {
		float x = plotMin.x + plotW * (float)i / 4.0f;
		if (i > 0 && i < 4)
			dl->AddLine(ImVec2(x, plotMin.y),
				    ImVec2(x, plotMax.y),
				    IM_COL32(60, 60, 60, 255));
		double t = tRange * (double)i / 4.0;
		char buf[32];
		if (tRange < 120.0)
			snprintf(buf, sizeof(buf), "%.0fs", t);
		else if (tRange < 7200.0)
			snprintf(buf, sizeof(buf), "%.1fm", t / 60.0);
		else
			snprintf(buf, sizeof(buf), "%.1fh", t / 3600.0);
		dl->AddText(ImVec2(x + 2, plotMax.y - 14),
			    IM_COL32(150, 150, 150, 255), buf);
	}

	std::vector<ImVec2> points;
	points.reserve(samples.size());

	for (auto &s : samples) {
		float x = plotMin.x + plotW *
			  (float)((s.timestamp - tStart) / tRange);
		float y = plotMax.y - plotH *
			  (float)((s.powerDb - minDb) / range);
		y = std::clamp(y, plotMin.y, plotMax.y);
		points.push_back(ImVec2(x, y));
	}

	if (points.size() >= 2) {
		dl->PushClipRect(plotMin, plotMax, true);
		dl->AddPolyline(points.data(), (int)points.size(),
				IM_COL32(255, 200, 50, 255),
				ImDrawFlags_None, 1.5f);
		dl->PopClipRect();
	}
}

void DisplayEngine::drawAllanPlot(const AllanVarianceAnalyzer *allan,
				  ImVec2 plotMin, ImVec2 plotMax)
{
	ImDrawList *dl = ImGui::GetWindowDrawList();

	dl->AddRectFilled(plotMin, plotMax, IM_COL32(20, 20, 30, 255));
	dl->AddRect(plotMin, plotMax, IM_COL32(80, 80, 80, 255));
	dl->AddText(ImVec2(plotMin.x + 5, plotMin.y + 2),
		    IM_COL32(200, 200, 200, 255),
		    "Allan Deviation vs Tau (log-log)");

	auto &results = allan->getResults();
	if (results.size() < 2)
		return;

	float plotW = plotMax.x - plotMin.x;
	float plotH = plotMax.y - plotMin.y;

	double logTauMin = log10(results.front().tau);
	double logTauMax = log10(results.back().tau);
	double logAdevMin = 1e30, logAdevMax = -1e30;

	for (auto &r : results) {
		if (r.adev <= 0.0)
			continue;
		double la = log10(r.adev);
		if (la < logAdevMin) logAdevMin = la;
		if (la > logAdevMax) logAdevMax = la;
	}

	double tauRange = logTauMax - logTauMin;
	double adevRange = logAdevMax - logAdevMin;
	if (tauRange < 0.1) tauRange = 1.0;
	if (adevRange < 0.1) adevRange = 1.0;
	logAdevMin -= adevRange * 0.05;
	logAdevMax += adevRange * 0.05;
	adevRange = logAdevMax - logAdevMin;

	/* Y-axis grid (ADEV) */
	for (int i = 1; i < 4; i++) {
		float y = plotMin.y + plotH * (float)i / 4.0f;
		dl->AddLine(ImVec2(plotMin.x, y), ImVec2(plotMax.x, y),
			    IM_COL32(60, 60, 60, 255));
		double logVal = logAdevMax - adevRange * (double)i / 4.0;
		char buf[32];
		snprintf(buf, sizeof(buf), "%.1e", pow(10.0, logVal));
		dl->AddText(ImVec2(plotMin.x + 2, y - 12),
			    IM_COL32(150, 150, 150, 255), buf);
	}

	/* X-axis grid (tau) */
	for (int i = 0; i <= 4; i++) {
		float x = plotMin.x + plotW * (float)i / 4.0f;
		if (i > 0 && i < 4)
			dl->AddLine(ImVec2(x, plotMin.y),
				    ImVec2(x, plotMax.y),
				    IM_COL32(60, 60, 60, 255));
		double logVal = logTauMin + tauRange * (double)i / 4.0;
		double tau = pow(10.0, logVal);
		char buf[32];
		if (tau < 1.0)
			snprintf(buf, sizeof(buf), "%.0fms", tau * 1000.0);
		else if (tau < 60.0)
			snprintf(buf, sizeof(buf), "%.1fs", tau);
		else if (tau < 3600.0)
			snprintf(buf, sizeof(buf), "%.0fm", tau / 60.0);
		else
			snprintf(buf, sizeof(buf), "%.1fh", tau / 3600.0);
		dl->AddText(ImVec2(x + 2, plotMax.y - 14),
			    IM_COL32(150, 150, 150, 255), buf);
	}

	std::vector<ImVec2> points;
	for (auto &r : results) {
		if (r.adev <= 0.0)
			continue;
		float x = plotMin.x + plotW *
			  (float)((log10(r.tau) - logTauMin) / tauRange);
		float y = plotMax.y - plotH *
			  (float)((log10(r.adev) - logAdevMin) / adevRange);
		y = std::clamp(y, plotMin.y, plotMax.y);
		points.push_back(ImVec2(x, y));
	}

	if (points.size() >= 2) {
		dl->PushClipRect(plotMin, plotMax, true);
		dl->AddPolyline(points.data(), (int)points.size(),
				IM_COL32(100, 200, 255, 255),
				ImDrawFlags_None, 2.0f);
		dl->PopClipRect();
	}

	for (auto &pt : points)
		dl->AddCircleFilled(pt, 3.0f, IM_COL32(100, 200, 255, 255));
}
