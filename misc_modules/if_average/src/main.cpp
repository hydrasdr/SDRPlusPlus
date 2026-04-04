/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 *
 * IF Average -- spectral averaging plugin for SDR++ radio astronomy.
 *
 * Uses its own IQ FFT engine (independent of the waterfall) with
 * configurable FFT size and window function. Accumulates spectra using
 * one of five averaging modes, with baseline subtraction, RFI mitigation,
 * Y-factor calibration, total power integration, Allan variance, and
 * CSV export.
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <core.h>
#include <signal_path/signal_path.h>
#include <config.h>
#include <utils/flog.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <cmath>
#include <ctime>
#include <cstring>
#include <algorithm>
#include <dsp/buffer/buffer.h>

#include "if_average_interface.h"
#include "accumulator.h"
#include "baseline.h"
#include "rfi.h"
#include "calibration.h"
#include "total_power.h"
#include "allan.h"
#include "display.h"
#include "export.h"
#include "iq_fft_engine.h"

SDRPP_MOD_INFO{
	/* Name:            */ "if_average",
	/* Description:     */ "IF spectral averaging for radio astronomy",
	/* Author:          */ "Benjamin Vernoux",
	/* Version:         */ 1, 0, 0,
	/* Max instances    */ 1
};

#define CONCAT(a, b) ((std::string(a) + b).c_str())

ConfigManager config;

static const char *avgModeNames =
	"Linear Mean\0Weighted Mean\0RMS\0Median\0EMA\0";
static const char *baselineModeNames = "Polynomial\0Reference\0";
static const char *rfiModeNames = "Threshold\0Spectral Kurtosis\0";
static const char *rfiBlankingNames =
	"Replace Mean\0Interpolate\0Zero\0Drop Spectrum\0";

/* Extract Nth null-separated string from ImGui combo list */
static const char *nthComboString(const char *combo, int n)
{
	const char *p = combo;
	for (int i = 0; i < n && *p; i++)
		p += strlen(p) + 1;
	return p;
}

/* Platform-safe UTC time conversion */
static struct tm safegmtime(const time_t *t)
{
	struct tm result;
	memset(&result, 0, sizeof(result));
#ifdef _WIN32
	gmtime_s(&result, t);
#else
	gmtime_r(t, &result);
#endif
	return result;
}

/* Format elapsed seconds as HH:MM:SS */
static void formatHMS(double secs, char *buf, int bufSize)
{
	int h = (int)secs / 3600;
	int m = ((int)secs % 3600) / 60;
	int s = (int)secs % 60;
	snprintf(buf, bufSize, "%02d:%02d:%02d", h, m, s);
}

/* Read an int from core config, with default */
static int readCoreConfigInt(const char *key, int def)
{
	int val = def;
	core::configManager.acquire();
	if (core::configManager.conf.contains(key))
		val = core::configManager.conf[key];
	core::configManager.release();
	return val;
}

/* Read a string from core config, with default */
static std::string readCoreConfigStr(const char *key, const char *def)
{
	std::string val = def;
	core::configManager.acquire();
	if (core::configManager.conf.contains(key))
		val = core::configManager.conf[key];
	core::configManager.release();
	return val;
}

class IFAverageModule : public ModuleManager::Instance {
public:
	IFAverageModule(std::string n) {
		this->name = n;
		loadConfig();

		/* Initialize sub-components with saved config */
		accumulator.init(0, (AveragingMode)avgMode, emaAlpha,
				 medianDepth);
		baseline.setMode((BaselineMode)baselineModeIdx);
		baseline.setPolyOrder(polyOrder);
		rfiMitigator.setMode((RFIMode)rfiModeIdx);
		rfiMitigator.setThresholdSigma(rfiThresholdSigma);
		rfiMitigator.setBlanking((RFIBlanking)rfiBlankingIdx);
		calEngine.setTHot(calTHot);
		calEngine.setTCold(calTCold);
		totalPower.setBandLimits(totalPowerBandStart,
					totalPowerBandStop);
		totalPower.setLogInterval(totalPowerLogInterval);
		allanAnalyzer.setMaxTau(allanMaxTau);

		/* Set IQ FFT callback */
		iqEngine.setCallback([this](const float *p, int w) {
			onIQSpectrum(p, w);
		});

		/* Bind event handlers */
		fftRedrawHandler.ctx = this;
		fftRedrawHandler.handler = onFFTRedraw;
		inputHandler.ctx = this;
		inputHandler.handler = onInputProcess;

		gui::menu.registerEntry(name, menuHandler, this, NULL);
		gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);
		gui::waterfall.onInputProcess.bindHandler(&inputHandler);
		core::modComManager.registerInterface("if_average", name,
						      moduleInterfaceHandler,
						      this);
	}

	~IFAverageModule() {
		stop();
		gui::menu.removeEntry(name);
		gui::waterfall.onFFTRedraw.unbindHandler(&fftRedrawHandler);
		gui::waterfall.onInputProcess.unbindHandler(&inputHandler);
		core::modComManager.unregisterInterface(name);
		freeDSPBuffers();
		freeDisplayBuffers();
	}

	void postInit() {
		lastCenterFreq.store(gui::waterfall.getCenterFrequency());
		lastSampleRate.store(sigpath::iqFrontEnd.getSampleRate());
	}

	void enable() {
		flog::info("IF Average: enable() called");
		enabled.store(true);
	}
	void disable() {
		/* Don't stop integration when the panel is collapsed.
		 * The module keeps running in the background. Only
		 * the overlay drawing is skipped (checked via enabled). */
		flog::info("IF Average: disable() called, running={}",
			   running.load());
		enabled.store(false);
	}
	bool isEnabled() { return enabled.load(); }

private:
	/* ================================================================
	 * Configuration persistence
	 * ================================================================ */

	void loadConfig() {
		config.acquire();
		auto &c = config.conf;
		if (c.contains("avgMode"))          avgMode = c["avgMode"];
		if (c.contains("integrationTime"))  integrationTime = c["integrationTime"];
		if (c.contains("emaAlpha"))         emaAlpha = c["emaAlpha"];
		if (c.contains("medianDepth"))      medianDepth = c["medianDepth"];
		if (c.contains("iqFFTSize"))        iqFFTSize = c["iqFFTSize"];
		if (c.contains("iqFFTOverlap"))     iqFFTOverlap = c["iqFFTOverlap"];
		if (c.contains("iqFFTWindow"))      iqFFTWindow = c["iqFFTWindow"];
		if (c.contains("continuousMode"))   continuousMode = c["continuousMode"];
		if (c.contains("resetOnFreqChange")) resetOnFreqChange = c["resetOnFreqChange"];
		if (c.contains("resetOnSrChange"))  resetOnSrChange = c["resetOnSrChange"];
		if (c.contains("baselineEnabled"))  baselineEnabled = c["baselineEnabled"];
		if (c.contains("baselineMode"))     baselineModeIdx = c["baselineMode"];
		if (c.contains("polyOrder"))        polyOrder = c["polyOrder"];
		if (c.contains("rfiEnabled"))       rfiEnabled = c["rfiEnabled"];
		if (c.contains("rfiMode"))          rfiModeIdx = c["rfiMode"];
		if (c.contains("rfiThresholdSigma")) rfiThresholdSigma = c["rfiThresholdSigma"];
		if (c.contains("rfiBlanking"))      rfiBlankingIdx = c["rfiBlanking"];
		if (c.contains("calEnabled"))       calEnabled = c["calEnabled"];
		if (c.contains("calTHot"))          calTHot = c["calTHot"];
		if (c.contains("calTCold"))         calTCold = c["calTCold"];
		if (c.contains("totalPowerEnabled")) totalPowerEnabled = c["totalPowerEnabled"];
		if (c.contains("totalPowerBandStart")) totalPowerBandStart = c["totalPowerBandStart"];
		if (c.contains("totalPowerBandStop")) totalPowerBandStop = c["totalPowerBandStop"];
		if (c.contains("totalPowerLogInterval")) totalPowerLogInterval = c["totalPowerLogInterval"];
		if (c.contains("allanEnabled"))     allanEnabled = c["allanEnabled"];
		if (c.contains("allanMaxTau"))      allanMaxTau = c["allanMaxTau"];
		if (c.contains("overlayEnabled"))   display.overlayEnabled = c["overlayEnabled"];
		if (c.contains("overlayColor"))     display.overlayColor = c["overlayColor"];
		if (c.contains("overlayThickness")) display.overlayThickness = c["overlayThickness"];
		if (c.contains("standaloneWindow")) display.standaloneWindow = c["standaloneWindow"];
		if (c.contains("showResidual"))     display.showResidual = c["showResidual"];
		if (c.contains("showTotalPowerStrip")) display.showTotalPowerStrip = c["showTotalPowerStrip"];
		if (c.contains("exportPath"))       exportEngine.setPath(c["exportPath"]);
		if (c.contains("autoExportEnabled")) autoExportEnabled = c["autoExportEnabled"];
		if (c.contains("autoExportInterval")) autoExportInterval = c["autoExportInterval"];
		if (c.contains("restFrequency"))    display.restFrequency = c["restFrequency"];
		if (c.contains("showVelocityAxis")) display.showVelocityAxis = c["showVelocityAxis"];
		config.release();
	}

	void saveConfig() {
		config.acquire();
		config.conf["avgMode"] = avgMode;
		config.conf["integrationTime"] = integrationTime;
		config.conf["emaAlpha"] = emaAlpha;
		config.conf["medianDepth"] = medianDepth;
		config.conf["iqFFTSize"] = iqFFTSize;
		config.conf["iqFFTOverlap"] = iqFFTOverlap;
		config.conf["iqFFTWindow"] = iqFFTWindow;
		config.conf["continuousMode"] = continuousMode;
		config.conf["resetOnFreqChange"] = resetOnFreqChange;
		config.conf["resetOnSrChange"] = resetOnSrChange;
		config.conf["baselineEnabled"] = baselineEnabled;
		config.conf["baselineMode"] = baselineModeIdx;
		config.conf["polyOrder"] = polyOrder;
		config.conf["rfiEnabled"] = rfiEnabled;
		config.conf["rfiMode"] = rfiModeIdx;
		config.conf["rfiThresholdSigma"] = rfiThresholdSigma;
		config.conf["rfiBlanking"] = rfiBlankingIdx;
		config.conf["calEnabled"] = calEnabled;
		config.conf["calTHot"] = calTHot;
		config.conf["calTCold"] = calTCold;
		config.conf["totalPowerEnabled"] = totalPowerEnabled;
		config.conf["totalPowerBandStart"] = totalPowerBandStart;
		config.conf["totalPowerBandStop"] = totalPowerBandStop;
		config.conf["totalPowerLogInterval"] = totalPowerLogInterval;
		config.conf["allanEnabled"] = allanEnabled;
		config.conf["allanMaxTau"] = allanMaxTau;
		config.conf["overlayEnabled"] = display.overlayEnabled;
		config.conf["overlayColor"] = (uint32_t)display.overlayColor;
		config.conf["overlayThickness"] = display.overlayThickness;
		config.conf["standaloneWindow"] = display.standaloneWindow;
		config.conf["showResidual"] = display.showResidual;
		config.conf["showTotalPowerStrip"] = display.showTotalPowerStrip;
		config.conf["exportPath"] = exportEngine.getExportPath();
		config.conf["autoExportEnabled"] = autoExportEnabled;
		config.conf["autoExportInterval"] = autoExportInterval;
		config.conf["restFrequency"] = display.restFrequency;
		config.conf["showVelocityAxis"] = display.showVelocityAxis;
		config.release(true);
	}

	/* ================================================================
	 * Buffer management (bufMtx protects display buffers + currentWidth)
	 * ================================================================ */

	/* Allocate DSP-thread-only work buffers (no lock needed) */
	void resizeDSPBuffers(int newWidth) {
		if (dspWorkBuf) dsp::buffer::free(dspWorkBuf);
		if (dspResultBuf) dsp::buffer::free(dspResultBuf);
		dspWorkBuf = dsp::buffer::alloc<float>(newWidth);
		dspResultBuf = dsp::buffer::alloc<float>(newWidth);
		dspWidth = newWidth;
	}

	void freeDSPBuffers() {
		if (dspWorkBuf) { dsp::buffer::free(dspWorkBuf); dspWorkBuf = nullptr; }
		if (dspResultBuf) { dsp::buffer::free(dspResultBuf); dspResultBuf = nullptr; }
		dspWidth = 0;
	}

	/* Allocate shared display buffers (protected by bufMtx) */
	void resizeBuffers(int newWidth) {
		std::lock_guard<std::mutex> lck(bufMtx);
		if (newWidth == currentWidth && displayBuf)
			return;
		freeDisplayBuffersLocked();
		displayBuf = dsp::buffer::alloc<float>(newWidth);
		residualBuf = dsp::buffer::alloc<float>(newWidth);
		kelvinBuf = dsp::buffer::alloc<float>(newWidth);
		dsp::buffer::clear(displayBuf, newWidth);
		dsp::buffer::clear(residualBuf, newWidth);
		dsp::buffer::clear(kelvinBuf, newWidth);
		currentWidth = newWidth;
		accumulator.resize(newWidth);
		rfiMitigator.resize(newWidth);
	}

	void freeDisplayBuffers() {
		std::lock_guard<std::mutex> lck(bufMtx);
		freeDisplayBuffersLocked();
	}

	void freeDisplayBuffersLocked() {
		if (displayBuf) { dsp::buffer::free(displayBuf); displayBuf = nullptr; }
		if (residualBuf) { dsp::buffer::free(residualBuf); residualBuf = nullptr; }
		if (kelvinBuf) { dsp::buffer::free(kelvinBuf); kelvinBuf = nullptr; }
		currentWidth = 0;
	}

	void clearDisplayBuffers() {
		std::lock_guard<std::mutex> lck(bufMtx);
		if (displayBuf && currentWidth > 0) {
			dsp::buffer::clear(displayBuf, currentWidth);
			dsp::buffer::clear(residualBuf, currentWidth);
			dsp::buffer::clear(kelvinBuf, currentWidth);
		}
		rfiDisplayFlags.clear();
	}

	/* ================================================================
	 * Lifecycle: start / stop / reset
	 *
	 * lifeCycleMtx serializes start/stop/reset transitions.
	 * The worker thread is always joined before a new one is created.
	 * ================================================================ */

	void start() {
		std::lock_guard<std::mutex> lck(lifeCycleMtx);
		if (running)
			return;

		doReset();
		resizeDSPBuffers(iqFFTSize);
		resizeBuffers(iqFFTSize);

		/* Setup FFT engine (allocates FFTW resources) */
		if (!iqEngine.setup(iqFFTSize, iqFFTOverlap, iqFFTWindow)) {
			flog::error("IF Average: FFT setup failed");
			return;
		}

		running = true;

		auto now = std::chrono::system_clock::now();
		startTimeUTC = std::chrono::system_clock::to_time_t(now);
		startTimePoint = std::chrono::steady_clock::now();

		/* Set Allan sample interval from the computed FFT rate */
		double sr = sigpath::iqFrontEnd.getSampleRate();
		double advance = (double)iqFFTSize * (1.0 - (double)iqFFTOverlap);
		if (advance > 0.0 && sr > 0.0)
			allanAnalyzer.setSampleInterval(advance / sr);

		guiLastFreq = gui::waterfall.getCenterFrequency();
		guiLastSR = sr;
		lastCenterFreq.store(guiLastFreq);
		lastSampleRate.store(guiLastSR);
		needsReset.store(false);

		iqEngine.start();
	}

	void stop() {
		std::lock_guard<std::mutex> lck(lifeCycleMtx);
		stopLocked();
	}

	void stopLocked() {
		if (running) {
			lastElapsedSec.store(currentElapsedSec.load());
			lastSpectraCount.store(accumulator.getCount());
		}
		running = false;
		iqEngine.stop();
		iqEngine.teardown();
		freeDSPBuffers();
	}

	void resetAccumulator() {
		std::lock_guard<std::mutex> lck(lifeCycleMtx);
		doReset();
	}

	void resetCore() {
		accumulator.reset();
		rfiMitigator.reset();
		totalPower.reset();
		droppedFrames.store(0);
		lastAutoExportTime.store(0.0);
		currentElapsedSec.store(0.0);
		clearDisplayBuffers();
	}

	/* Full reset (Start/Reset buttons). Preserves zoom. */
	void doReset() {
		resetCore();
		lastElapsedSec.store(0.0);
		lastSpectraCount.store(0);
	}

	/* Partial reset (freq/SR change while running). Resets zoom. */
	void resetIntegration() {
		resetCore();
		display.resetView();
		startTimePoint = std::chrono::steady_clock::now();
	}

	/* Called from worker when non-continuous timer expires */
	void workerTimerExpired() {
		lastElapsedSec.store(currentElapsedSec.load());
		lastSpectraCount.store(accumulator.getCount());
		running = false;
	}

	/* ================================================================
	 * IQ FFT Callback (called from DSP thread)
	 *
	 * The IQ engine delivers linear power spectra at a rate
	 * determined by: sampleRate / (fftSize * (1 - overlap)).
	 * For example, 2.5 MS/s with 65536-point FFT and 50% overlap
	 * gives about 76 FFTs/sec.
	 * ================================================================ */

	void onIQSpectrum(const float *power, int width) {
		if (!running || !power || width <= 0)
			return;

		/* Freq/SR change detection is done in the GUI thread
		 * (updateTunerState) to avoid false triggers from
		 * floating-point drift during window resize. */

		if (needsReset.load()) {
			needsReset.store(false);
			resetIntegration();
		}

		/* Resize if width doesn't match */
		if (width != dspWidth) {
			resizeDSPBuffers(width);
			resizeBuffers(width);
		}

		processSpectrum(power, width);

		/* Update elapsed time and check timer */
		auto elapsed = std::chrono::steady_clock::now() -
			       startTimePoint;
		double secs = std::chrono::duration<double>(elapsed)
				      .count();
		currentElapsedSec.store(secs);
		if (!continuousMode && secs >= integrationTime)
			workerTimerExpired();
	}

	/* ================================================================
	 * Processing Pipeline
	 *
	 * Called from the worker thread for each FFT frame.
	 * Uses pre-allocated dspWorkBuf/dspResultBuf to avoid per-frame alloc.
	 * ================================================================ */

	/*
	 * Called from the DSP thread for each FFT frame.
	 *
	 * dspWorkBuf and dspResultBuf are DSP-thread-only buffers,
	 * not shared with the GUI. Only displayBuf/residualBuf/kelvinBuf
	 * are shared (protected by bufMtx).
	 */
	void processSpectrum(const float *power, int width) {
		if (width != dspWidth || !dspWorkBuf)
			return;

		/* RFI: copy to work buffer for in-place blanking */
		const float *input = power;
		if (rfiEnabled) {
			memcpy(dspWorkBuf, power, width * sizeof(float));
			bool drop = rfiMitigator.process(dspWorkBuf, width);
			if (drop)
				return;
			input = dspWorkBuf;
		}

		/* Accumulate */
		accumulator.addSpectrum(input, width);

		/* Get averaged result into DSP-local buffer */
		accumulator.getResult(dspResultBuf, width);

		/* Total power and Allan (DSP-thread only, no lock) */
		double timestamp =
			(double)std::chrono::duration_cast<
				std::chrono::microseconds>(
				std::chrono::system_clock::now()
					.time_since_epoch())
				.count() / 1e6;

		double freq = lastCenterFreq.load();
		double sr = lastSampleRate.load();

		if (totalPowerEnabled)
			totalPower.addSample(dspResultBuf, width, freq,
					     sr, timestamp);

		if (allanEnabled) {
			double tp = 0.0;
			for (int i = 0; i < width; i++)
				tp += (double)dspResultBuf[i];
			tp /= (double)width;
			allanAnalyzer.addSample(tp);
		}

		/* Copy result to display buffers (single lock, minimal) */
		{
			std::lock_guard<std::mutex> lck(bufMtx);
			if (currentWidth != width || !displayBuf)
				return;

			memcpy(displayBuf, dspResultBuf,
			       width * sizeof(float));

			/* Residual: copy then subtract baseline in-place */
			memcpy(residualBuf, dspResultBuf,
			       width * sizeof(float));
			if (baselineEnabled)
				baseline.subtract(residualBuf, width);

			/* Kelvin: only copy+apply if calibrated */
			if (calEnabled && calEngine.isCalibrated()) {
				memcpy(kelvinBuf, dspResultBuf,
				       width * sizeof(float));
				calEngine.apply(kelvinBuf, width);
			}

			if (rfiEnabled)
				rfiMitigator.copyFlags(rfiDisplayFlags);
			else
				rfiDisplayFlags.clear();
		}

		/* Auto-export */
		double lastExport = lastAutoExportTime.load();
		if (autoExportEnabled && lastExport > 0.0 &&
		    timestamp - lastExport >= autoExportInterval) {
			doExport();
			lastAutoExportTime.store(timestamp);
		}
		if (lastExport == 0.0)
			lastAutoExportTime.store(timestamp);
	}

	/* ================================================================
	 * CSV Export
	 * ================================================================ */

	void doExport() {
		std::lock_guard<std::mutex> lck(bufMtx);
		if (currentWidth <= 0 || !displayBuf ||
		    accumulator.getCount() == 0)
			return;

		ExportMetadata meta;
		meta.centerFreqHz = lastCenterFreq.load();
		meta.sampleRateHz = lastSampleRate.load();
		meta.fftSize = iqFFTSize;
		/* Use the plugin's own IQ FFT rate, not the waterfall rate */
		double sr_exp = meta.sampleRateHz;
		double adv = (double)iqFFTSize * (1.0 - (double)iqFFTOverlap);
		meta.fftRate = (adv > 0.0 && sr_exp > 0.0) ?
			       sr_exp / adv : 0.0;
		meta.spectraAccumulated = accumulator.getCount();
		meta.averagingMode = nthComboString(avgModeNames, avgMode);
		meta.tsysKelvin = calEngine.getTsys();
		meta.integrationTimeSec = currentElapsedSec.load();

		/* Source name with firmware version */
		meta.sourceName = readCoreConfigStr("source", "Unknown");
		char fwBuf[128] = "";
		if (core::modComManager.callInterface("HydraSDR Source",
						      0, nullptr, fwBuf)) {
			if (fwBuf[0])
				meta.sourceName += " fw:" +
						   std::string(fwBuf);
		}

		/* Decimation and hardware sample rate */
		int decimation = readCoreConfigInt("decimation", 1);
		meta.decimation = decimation;
		meta.hardwareSampleRateHz = meta.sampleRateHz *
					   (double)decimation;

		/* Timestamps */
		char buf[64];
		struct tm t;
		t = safegmtime(&startTimeUTC);
		strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
		meta.dateObs = buf;

		time_t now = time(nullptr);
		t = safegmtime(&now);
		strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
		meta.dateEnd = buf;

		if (baselineEnabled) {
			if (baselineModeIdx == BL_POLYNOMIAL)
				meta.baselineInfo = "POLYNOMIAL order " +
						    std::to_string(polyOrder);
			else
				meta.baselineInfo = "REFERENCE";
		}
		if (calEnabled && calEngine.isCalibrated())
			meta.calibrationInfo = "Y_FACTOR T_sys=" +
				std::to_string((int)calEngine.getTsys()) + "K";

		bool hasCal = calEnabled && calEngine.isCalibrated();
		exportEngine.exportSpectrum(
			displayBuf,
			baselineEnabled ? residualBuf : nullptr,
			hasCal ? kelvinBuf : nullptr,
			rfiEnabled ? &rfiDisplayFlags : nullptr,
			currentWidth, meta);
	}

	/* ================================================================
	 * Waterfall Event Handlers (GUI thread)
	 * ================================================================ */

	/* Draw averaged spectrum overlay on waterfall */
	static void onFFTRedraw(ImGui::WaterFall::FFTRedrawArgs args,
				void *ctx) {
		IFAverageModule *_this = (IFAverageModule *)ctx;
		if (!_this->enabled.load() ||
		    _this->accumulator.getCount() == 0)
			return;

		std::lock_guard<std::mutex> lck(_this->bufMtx);
		if (!_this->displayBuf || _this->currentWidth <= 0)
			return;

		if (!_this->rfiDisplayFlags.empty())
			_this->display.rfiFlags = &_this->rfiDisplayFlags;
		else
			_this->display.rfiFlags = nullptr;

		_this->display.drawOverlay(
			args, _this->displayBuf, _this->currentWidth,
			_this->lastCenterFreq.load(),
			_this->lastSampleRate.load(),
			gui::waterfall.getFFTMin(),
			gui::waterfall.getFFTMax());
	}

	/* Block waterfall input when standalone window is active */
	static void onInputProcess(ImGui::WaterFall::InputHandlerArgs,
				   void *ctx) {
		IFAverageModule *_this = (IFAverageModule *)ctx;
		if (_this->display.standaloneWindow &&
		    _this->display.standaloneWindowActive)
			gui::waterfall.inputHandled = true;
	}

	/* ================================================================
	 * Module Communication Interface
	 * ================================================================ */

	static void moduleInterfaceHandler(int code, void *in, void *out,
					   void *ctx) {
		IFAverageModule *_this = (IFAverageModule *)ctx;
		switch (code) {
		case IF_AVG_CMD_START:  _this->start(); break;
		case IF_AVG_CMD_STOP:   _this->stop(); break;
		case IF_AVG_CMD_RESET:  _this->resetAccumulator(); break;
		case IF_AVG_CMD_GET_STATUS:
			if (out) *(int *)out = _this->running ? 1 : 0;
			break;
		case IF_AVG_CMD_GET_INT_TIME:
			if (out) *(double *)out = _this->currentElapsedSec.load();
			break;
		case IF_AVG_CMD_GET_INT_COUNT:
			if (out) *(uint64_t *)out = _this->accumulator.getCount();
			break;
		case IF_AVG_CMD_EXPORT: _this->doExport(); break;
		case IF_AVG_CMD_CAPTURE_REF: {
			std::lock_guard<std::mutex> lck(_this->bufMtx);
			if (_this->displayBuf && _this->currentWidth > 0)
				_this->baseline.captureReference(
					_this->displayBuf, _this->currentWidth);
			break;
		}
		case IF_AVG_CMD_CAPTURE_HOT: {
			std::lock_guard<std::mutex> lck(_this->bufMtx);
			if (_this->displayBuf && _this->currentWidth > 0)
				_this->calEngine.captureHot(
					_this->displayBuf, _this->currentWidth);
			break;
		}
		case IF_AVG_CMD_CAPTURE_COLD: {
			std::lock_guard<std::mutex> lck(_this->bufMtx);
			if (_this->displayBuf && _this->currentWidth > 0)
				_this->calEngine.captureCold(
					_this->displayBuf, _this->currentWidth);
			break;
		}
		case IF_AVG_CMD_SET_MODE:
			if (in) {
				int mode = *(int *)in;
				_this->avgMode = mode;
				_this->accumulator.setMode((AveragingMode)mode);
			}
			break;
		default: break;
		}
	}

	/* ================================================================
	 * GUI -- menu section drawing helpers
	 * ================================================================ */

	void drawAveragingSection() {
		if (running) { style::beginDisabled(); }
		ImGui::LeftLabel("Avg. mode");
		ImGui::FillWidth();
		if (ImGui::Combo(CONCAT("##ifavg_mode_", name), &avgMode, avgModeNames)) {
			accumulator.setMode((AveragingMode)avgMode);
			saveConfig();
		}
		if (running) { style::endDisabled(); }

		ImGui::LeftLabel("Time (s)");
		ImGui::FillWidth();
		if (ImGui::InputDouble(CONCAT("##ifavg_time_", name), &integrationTime, 1.0, 10.0, "%.1f")) {
			integrationTime = std::clamp(integrationTime, 0.1, 86400.0);
			saveConfig();
		}
		if (ImGui::Checkbox(CONCAT("Continuous##ifavg_", name), &continuousMode))
			saveConfig();
		if (ImGui::Checkbox(CONCAT("Reset on freq change##ifavg_", name), &resetOnFreqChange))
			saveConfig();
		if (ImGui::Checkbox(CONCAT("Reset on SR change##ifavg_", name), &resetOnSrChange))
			saveConfig();

		if (avgMode == AVG_EMA) {
			ImGui::LeftLabel("EMA Alpha");
			ImGui::FillWidth();
			if (ImGui::SliderFloat(CONCAT("##ifavg_ema_", name), &emaAlpha, 0.001f, 1.0f, "%.3f", ImGuiSliderFlags_Logarithmic)) {
				accumulator.setEmaAlpha(emaAlpha);
				saveConfig();
			}
		}
		if (avgMode == AVG_MEDIAN) {
			ImGui::LeftLabel("Median Depth");
			ImGui::FillWidth();
			if (ImGui::InputInt(CONCAT("##ifavg_mdep_", name), &medianDepth, 8, 32)) {
				medianDepth = std::clamp(medianDepth, 8, 256);
				accumulator.setMedianDepth(medianDepth);
				saveConfig();
			}
		}
	}

	void drawFFTSection() {
		ImGui::Separator();
		if (running) { style::beginDisabled(); }

		ImGui::LeftLabel("FFT Size");
		ImGui::FillWidth();
		const char *fftSizes =
			"1024\0002048\0004096\0008192\000"
			"16384\00032768\00065536\000"
			"131072\000262144\000524288\000"
			"1048576\000";
		int sizeIdx = 0;
		int sz = iqFFTSize;
		while (sz > 1024 && sizeIdx < 10) { sz >>= 1; sizeIdx++; }
		if (ImGui::Combo(CONCAT("##ifavg_fftsz_", name),
				 &sizeIdx, fftSizes)) {
			iqFFTSize = 1024 << sizeIdx;
			saveConfig();
		}

		ImGui::LeftLabel("Overlap %");
		ImGui::FillWidth();
		float pct = iqFFTOverlap * 100.0f;
		if (ImGui::SliderFloat(CONCAT("##ifavg_fftov_", name),
				       &pct, 0.0f, 75.0f, "%.0f")) {
			iqFFTOverlap = pct / 100.0f;
			saveConfig();
		}

		ImGui::LeftLabel("Window");
		ImGui::FillWidth();
		static const char *winNames =
			"Blackman-Harris\0Hann\0Nuttall\0"
			"Flat-Top\0Kaiser B8\0";
		if (ImGui::Combo(CONCAT("##ifavg_fftwin_", name),
				 &iqFFTWindow, winNames))
			saveConfig();

		if (running) { style::endDisabled(); }

		/* Show computed FFT rate */
		double sr = lastSampleRate.load();
		if (sr > 0.0 && iqFFTSize > 0) {
			double advance = (double)iqFFTSize *
					 (1.0 - (double)iqFFTOverlap);
			double rate = sr / advance;
			double resolution = sr / (double)iqFFTSize;
			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
					   "Rate: %.1f FFT/s | Res: %.1f Hz",
					   rate, resolution);
		}
	}

	void drawBaselineSection(float menuWidth) {
		ImGui::Separator();
		if (ImGui::Checkbox(CONCAT("Baseline##ifavg_", name), &baselineEnabled))
			saveConfig();
		if (!baselineEnabled) return;

		ImGui::LeftLabel("BL Mode");
		ImGui::FillWidth();
		if (ImGui::Combo(CONCAT("##ifavg_blmode_", name), &baselineModeIdx, baselineModeNames)) {
			baseline.setMode((BaselineMode)baselineModeIdx);
			saveConfig();
		}
		if (baselineModeIdx == BL_POLYNOMIAL) {
			ImGui::LeftLabel("Poly Order");
			ImGui::FillWidth();
			if (ImGui::InputInt(CONCAT("##ifavg_blord_", name), &polyOrder, 1, 3)) {
				polyOrder = std::clamp(polyOrder, 0, 15);
				baseline.setPolyOrder(polyOrder);
				saveConfig();
			}
		}
		if (baselineModeIdx == BL_REFERENCE) {
			if (ImGui::Button(CONCAT("Capture Reference##ifavg_", name), ImVec2(menuWidth, 0))) {
				std::lock_guard<std::mutex> lck(bufMtx);
				if (displayBuf && currentWidth > 0)
					baseline.captureReference(displayBuf, currentWidth);
			}
			if (baseline.hasReference())
				ImGui::TextColored(ImVec4(0, 1, 0, 1), "Reference captured");
		}
	}

	void drawRFISection() {
		ImGui::Separator();
		if (ImGui::Checkbox(CONCAT("RFI Mitigation##ifavg_", name), &rfiEnabled)) {
			if (!rfiEnabled) rfiMitigator.reset();
			saveConfig();
		}
		if (!rfiEnabled) return;

		ImGui::LeftLabel("RFI Mode");
		ImGui::FillWidth();
		if (ImGui::Combo(CONCAT("##ifavg_rfimode_", name), &rfiModeIdx, rfiModeNames)) {
			rfiMitigator.setMode((RFIMode)rfiModeIdx);
			saveConfig();
		}
		ImGui::LeftLabel("Threshold (sigma)");
		ImGui::FillWidth();
		if (ImGui::SliderFloat(CONCAT("##ifavg_rfisig_", name), &rfiThresholdSigma, 1.0f, 20.0f, "%.1f")) {
			rfiMitigator.setThresholdSigma(rfiThresholdSigma);
			saveConfig();
		}
		ImGui::LeftLabel("Blanking");
		ImGui::FillWidth();
		if (ImGui::Combo(CONCAT("##ifavg_rfibl_", name), &rfiBlankingIdx, rfiBlankingNames)) {
			rfiMitigator.setBlanking((RFIBlanking)rfiBlankingIdx);
			saveConfig();
		}
	}

	void drawCalibrationSection(float menuWidth) {
		ImGui::Separator();
		if (ImGui::Checkbox(CONCAT("Calibration##ifavg_", name), &calEnabled))
			saveConfig();
		if (!calEnabled) return;

		ImGui::LeftLabel("T_hot (K)");
		ImGui::FillWidth();
		if (ImGui::InputDouble(CONCAT("##ifavg_thot_", name), &calTHot, 1.0, 10.0, "%.1f")) {
			calTHot = std::clamp(calTHot, 1.0, 100000.0);
			calEngine.setTHot(calTHot);
			saveConfig();
		}
		ImGui::LeftLabel("T_cold (K)");
		ImGui::FillWidth();
		if (ImGui::InputDouble(CONCAT("##ifavg_tcold_", name), &calTCold, 0.5, 5.0, "%.1f")) {
			calTCold = std::clamp(calTCold, 1.0, 1000.0);
			calEngine.setTCold(calTCold);
			saveConfig();
		}
		ImGui::Text("Step 1: Point at hot load");
		if (ImGui::Button(CONCAT("Capture HOT##ifavg_", name), ImVec2(menuWidth, 0))) {
			std::lock_guard<std::mutex> lck(bufMtx);
			if (displayBuf && currentWidth > 0)
				calEngine.captureHot(displayBuf, currentWidth);
		}
		if (calEngine.hasHot())
			ImGui::TextColored(ImVec4(0, 1, 0, 1), "HOT captured");
		ImGui::Text("Step 2: Point at cold sky");
		if (ImGui::Button(CONCAT("Capture COLD##ifavg_", name), ImVec2(menuWidth, 0))) {
			std::lock_guard<std::mutex> lck(bufMtx);
			if (displayBuf && currentWidth > 0)
				calEngine.captureCold(displayBuf, currentWidth);
		}
		if (calEngine.hasCold())
			ImGui::TextColored(ImVec4(0, 1, 0, 1), "COLD captured");
		ImGui::Text("Step 3:");
		if (ImGui::Button(CONCAT("Compute Cal##ifavg_", name), ImVec2(menuWidth, 0)))
			calEngine.computeCalibration();
		if (calEngine.isCalibrated())
			ImGui::TextColored(ImVec4(0, 1, 0.5f, 1),
					   "Y=%.1f dB | T_sys=%.0f K",
					   10.0 * log10(calEngine.getYFactor()),
					   calEngine.getTsys());
	}

	void drawTotalPowerSection() {
		ImGui::Separator();
		if (ImGui::Checkbox(CONCAT("Total Power##ifavg_", name), &totalPowerEnabled)) {
			if (!totalPowerEnabled) totalPower.reset();
			saveConfig();
		}
		if (totalPowerEnabled) {
			ImGui::LeftLabel("Start offset (Hz)");
			ImGui::FillWidth();
			if (ImGui::InputDouble(CONCAT("##ifavg_tpbs_", name), &totalPowerBandStart)) {
				totalPower.reset();
				totalPower.setBandLimits(totalPowerBandStart, totalPowerBandStop);
				saveConfig();
			}
			ImGui::LeftLabel("Stop offset (Hz)");
			ImGui::FillWidth();
			if (ImGui::InputDouble(CONCAT("##ifavg_tpbe_", name), &totalPowerBandStop)) {
				totalPower.reset();
				totalPower.setBandLimits(totalPowerBandStart, totalPowerBandStop);
				saveConfig();
			}
			ImGui::LeftLabel("Log Interval (s)");
			ImGui::FillWidth();
			if (ImGui::InputDouble(CONCAT("##ifavg_tpli_", name), &totalPowerLogInterval, 0.1, 1.0, "%.2f")) {
				totalPowerLogInterval = std::clamp(totalPowerLogInterval, 0.01, 3600.0);
				totalPower.reset();
				totalPower.setLogInterval(totalPowerLogInterval);
				saveConfig();
			}
			ImGui::Text("Power: %.1f dBFS", totalPower.getLatestPowerDb());
		}
		if (ImGui::Checkbox(CONCAT("Allan Variance##ifavg_", name), &allanEnabled)) {
			if (!allanEnabled) allanAnalyzer.reset();
			saveConfig();
		}
		if (allanEnabled) {
			ImGui::LeftLabel("Max Tau (s)");
			ImGui::FillWidth();
			if (ImGui::InputDouble(CONCAT("##ifavg_atau_", name), &allanMaxTau, 10.0, 100.0)) {
				allanMaxTau = std::clamp(allanMaxTau, 10.0, 100000.0);
				allanAnalyzer.setMaxTau(allanMaxTau);
				saveConfig();
			}
			float mw = ImGui::GetContentRegionAvail().x;
			if (ImGui::Button(CONCAT("Compute Allan##ifavg_", name), ImVec2(mw, 0)))
				allanAnalyzer.compute();
		}
	}

	void drawDisplaySection() {
		ImGui::Separator();
		if (ImGui::Checkbox(CONCAT("Overlay on spectrum##ifavg_", name), &display.overlayEnabled))
			saveConfig();
		ImVec4 col = ImGui::ColorConvertU32ToFloat4(display.overlayColor);
		if (ImGui::ColorEdit4(CONCAT("Color##ifavg_", name), (float *)&col, ImGuiColorEditFlags_NoInputs)) {
			display.overlayColor = ImGui::ColorConvertFloat4ToU32(col);
			saveConfig();
		}
		ImGui::LeftLabel("Thickness");
		ImGui::FillWidth();
		if (ImGui::SliderFloat(CONCAT("##ifavg_thick_", name), &display.overlayThickness, 1.0f, 5.0f, "%.1f"))
			saveConfig();
		if (ImGui::Checkbox(CONCAT("Separate window##ifavg_", name), &display.standaloneWindow))
			saveConfig();
		if (ImGui::Checkbox(CONCAT("Show residual##ifavg_", name), &display.showResidual))
			saveConfig();
		if (ImGui::Checkbox(CONCAT("Total power strip##ifavg_", name), &display.showTotalPowerStrip))
			saveConfig();
		if (ImGui::Checkbox(CONCAT("Velocity axis##ifavg_", name), &display.showVelocityAxis))
			saveConfig();
		ImGui::LeftLabel("Rest Freq (MHz)");
		ImGui::FillWidth();
		double restMHz = display.restFrequency / 1e6;
		if (ImGui::InputDouble(CONCAT("##ifavg_rest_", name), &restMHz, 0.1, 1.0, "%.6f")) {
			display.restFrequency = restMHz * 1e6;
			saveConfig();
		}
		/* Line preset selector */
		const char *presetLabel = "Custom";
		for (int i = 0; i < DisplayEngine::numSpectralLines; i++) {
			if (display.restFrequency == DisplayEngine::spectralLines[i].freqHz) {
				presetLabel = DisplayEngine::spectralLines[i].name;
				break;
			}
		}
		ImGui::LeftLabel("Line");
		ImGui::FillWidth();
		if (ImGui::BeginCombo(CONCAT("##ifavg_preset_", name), presetLabel)) {
			for (int i = 0; i < DisplayEngine::numSpectralLines; i++) {
				bool selected = (display.restFrequency == DisplayEngine::spectralLines[i].freqHz);
				if (ImGui::Selectable(DisplayEngine::spectralLines[i].name, selected)) {
					display.restFrequency = DisplayEngine::spectralLines[i].freqHz;
					saveConfig();
				}
			}
			ImGui::EndCombo();
		}
	}

	void drawExportSection() {
		ImGui::Separator();
		ImGui::LeftLabel("Export path");
		ImGui::FillWidth();
		char pathBuf[512];
		snprintf(pathBuf, sizeof(pathBuf), "%s", exportEngine.getExportPath().c_str());
		if (ImGui::InputText(CONCAT("##ifavg_path_", name), pathBuf, sizeof(pathBuf))) {
			exportEngine.setPath(pathBuf);
			saveConfig();
		}
		if (ImGui::Checkbox(CONCAT("Auto-export##ifavg_", name), &autoExportEnabled))
			saveConfig();
		if (autoExportEnabled) {
			ImGui::LeftLabel("Interval (s)");
			ImGui::FillWidth();
			if (ImGui::InputDouble(CONCAT("##ifavg_aint_", name), &autoExportInterval, 1.0, 10.0, "%.0f")) {
				autoExportInterval = std::clamp(autoExportInterval, 1.0, 86400.0);
				saveConfig();
			}
		}
	}

	void drawStatusSection(float menuWidth) {
		ImGui::Separator();
		char hms[16];
		if (running) {
			formatHMS(currentElapsedSec.load(), hms, sizeof(hms));
			ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f),
					   "Integrating  %s", hms);
			ImGui::Text("Spectra: %llu%s",
				    (unsigned long long)accumulator.getCount(),
				    continuousMode ? " / inf" : "");
		} else if (lastSpectraCount.load() > 0) {
			formatHMS(lastElapsedSec.load(), hms, sizeof(hms));
			ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
					   "Stopped  %s", hms);
			ImGui::Text("Spectra: %llu",
				    (unsigned long long)lastSpectraCount.load());
		} else {
			ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_Text), "Idle");
		}

		uint64_t dropped = droppedFrames.load();
		if (dropped > 0)
			ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
					   "Dropped: %llu",
					   (unsigned long long)dropped);

		if (!running) {
			if (ImGui::Button(CONCAT("Start##ifavg_", name), ImVec2(menuWidth, 0)))
				start();
		} else {
			if (ImGui::Button(CONCAT("Stop##ifavg_", name), ImVec2(menuWidth, 0)))
				stop();
		}
		ImGui::BeginTable(CONCAT("ifavg_btn2_", name), 2);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		if (ImGui::Button(CONCAT("Reset##ifavg_", name), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
			resetAccumulator();
		ImGui::TableSetColumnIndex(1);
		if (ImGui::Button(CONCAT("Export Now##ifavg_", name), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
			doExport();
		ImGui::EndTable();
	}

	void drawStandaloneSection() {
		if (!display.standaloneWindow || accumulator.getCount() == 0)
			return;

		std::deque<TotalPowerSample> tpCopy;
		totalPower.copySamples(tpCopy);

		int fftSizeCfg = iqFFTSize;

		std::lock_guard<std::mutex> lck(bufMtx);
		if (!displayBuf || currentWidth <= 0)
			return;

		double elapsed = running
			? currentElapsedSec.load()
			: lastElapsedSec.load();
		bool hasCal = calEnabled && calEngine.isCalibrated();

		display.drawStandaloneWindow(
			displayBuf, residualBuf,
			hasCal ? kelvinBuf : nullptr,
			currentWidth,
			lastCenterFreq.load(),
			lastSampleRate.load(),
			fftSizeCfg,
			tpCopy.empty() ? nullptr : &tpCopy,
			&allanAnalyzer,
			elapsed,
			accumulator.getCount(),
			running.load());
	}

	/* ================================================================
	 * GUI -- main menu handler
	 * ================================================================ */

	/* Check for frequency/sample rate changes from the GUI thread.
	 * Safer than checking in the DSP callback because
	 * getCenterFrequency() is updated during GUI draw. */
	/* Detect actual retune/SR changes using waterfall event flags
	 * rather than floating-point frequency comparison. The
	 * centerFreqMoved flag is only set on real tuning events,
	 * not on window resize rounding. */
	void updateTunerState() {
		if (!running)
			return;

		double curFreq = gui::waterfall.getCenterFrequency();
		double curSR = sigpath::iqFrontEnd.getSampleRate();

		if (resetOnFreqChange && guiLastFreq > 0.0 &&
		    fabs(curFreq - guiLastFreq) > 1.0) {
			flog::info("IF Average: freq changed, resetting");
			needsReset.store(true);
		}

		if (resetOnSrChange && guiLastSR > 0.0 &&
		    fabs(curSR - guiLastSR) > 1.0) {
			flog::info("IF Average: SR changed, resetting");
			needsReset.store(true);
		}

		guiLastFreq = curFreq;
		guiLastSR = curSR;
		lastCenterFreq.store(curFreq);
		lastSampleRate.store(curSR);
	}

	static void menuHandler(void *ctx) {
		IFAverageModule *_this = (IFAverageModule *)ctx;
		float menuWidth = ImGui::GetContentRegionAvail().x;

		_this->updateTunerState();
		_this->drawAveragingSection();
		_this->drawFFTSection();
		_this->drawBaselineSection(menuWidth);
		_this->drawRFISection();
		_this->drawCalibrationSection(menuWidth);
		_this->drawTotalPowerSection();
		_this->drawDisplaySection();
		_this->drawExportSection();
		_this->drawStatusSection(menuWidth);
		_this->drawStandaloneSection();
	}

	/* ================================================================
	 * Member variables
	 *
	 * Thread access:
	 *   GUI:    menuHandler, onFFTRedraw, onInputProcess, start/stop
	 *   Worker: fftWorker -> processSpectrum
	 *
	 * Synchronization:
	 *   bufMtx       - display/residual/kelvin bufs,
	 *                  currentWidth, rfiDisplayFlags
	 *   lifeCycleMtx - start/stop/reset serialization
	 *   atomic       - running, enabled, lastCenterFreq, lastSampleRate,
	 *                  droppedFrames, currentElapsedSec, lastElapsedSec,
	 *                  lastSpectraCount, lastAutoExportTime
	 *   GUI-only     - all config params written only from GUI thread
	 * ================================================================ */

	std::string name;
	std::atomic<bool> enabled{true};
	std::atomic<bool> running{false};

	/* DSP-thread-only buffers (no lock needed, only DSP thread access) */
	int dspWidth = 0;
	float *dspWorkBuf = nullptr;	/* RFI processing scratch */
	float *dspResultBuf = nullptr;	/* accumulator output */

	/* Display buffers (shared with GUI, protected by bufMtx) */
	int currentWidth = 0;
	float *displayBuf = nullptr;
	float *residualBuf = nullptr;
	float *kelvinBuf = nullptr;
	std::vector<bool> rfiDisplayFlags;
	std::mutex bufMtx;

	/* Lifecycle */
	std::mutex lifeCycleMtx;
	time_t startTimeUTC = 0;
	std::chrono::steady_clock::time_point startTimePoint;

	/* Shared between DSP and GUI threads */
	std::atomic<uint64_t> droppedFrames{0};
	std::atomic<double> lastCenterFreq{0.0};
	std::atomic<double> lastSampleRate{0.0};
	std::atomic<bool> needsReset{false};
	std::atomic<double> currentElapsedSec{0.0};
	std::atomic<double> lastElapsedSec{0.0};
	std::atomic<uint64_t> lastSpectraCount{0};
	std::atomic<double> lastAutoExportTime{0.0};

	/* GUI-thread-only freq/SR tracking */
	double guiLastFreq = 0.0;
	double guiLastSR = 0.0;

	/* Config params (GUI-thread writes only) */
	int avgMode = AVG_LINEAR_MEAN;
	double integrationTime = 10.0;
	float emaAlpha = 0.01f;
	int medianDepth = 64;
	bool continuousMode = true;
	bool resetOnFreqChange = true;
	bool resetOnSrChange = true;

	bool baselineEnabled = false;
	int baselineModeIdx = BL_POLYNOMIAL;
	int polyOrder = 3;

	bool rfiEnabled = false;
	int rfiModeIdx = RFI_THRESHOLD;
	float rfiThresholdSigma = 5.0f;
	int rfiBlankingIdx = RFI_REPLACE_MEAN;

	bool calEnabled = false;
	double calTHot = 290.0;
	double calTCold = 10.0;

	bool totalPowerEnabled = false;
	double totalPowerBandStart = 0.0;
	double totalPowerBandStop = 0.0;
	double totalPowerLogInterval = 1.0;

	bool allanEnabled = false;
	double allanMaxTau = 1000.0;

	bool autoExportEnabled = false;
	double autoExportInterval = 60.0;

	/* IQ FFT engine config (change requires stop/start) */
	int iqFFTSize = 65536;
	float iqFFTOverlap = 0.5f;
	int iqFFTWindow = FFT_WIN_BLACKMAN_HARRIS;

	/* Sub-components */
	SpectrumAccumulator accumulator;
	BaselineManager baseline;
	RFIMitigator rfiMitigator;
	CalibrationEngine calEngine;
	TotalPowerIntegrator totalPower;
	AllanVarianceAnalyzer allanAnalyzer;
	DisplayEngine display;
	ExportEngine exportEngine;
	IQFFTEngine iqEngine;

	/* Event handlers */
	EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler;
	EventHandler<ImGui::WaterFall::InputHandlerArgs> inputHandler;
};

#undef CONCAT

/* ================================================================
 * Module entry points
 * ================================================================ */

MOD_EXPORT void _INIT_() {
	json def = json({});
	config.setPath(core::args["root"].s() + "/if_average_config.json");
	config.load(def);
	config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance *_CREATE_INSTANCE_(std::string name) {
	return new IFAverageModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void *instance) {
	delete (IFAverageModule *)instance;
}

MOD_EXPORT void _END_() {
	config.disableAutoSave();
	config.save();
}
