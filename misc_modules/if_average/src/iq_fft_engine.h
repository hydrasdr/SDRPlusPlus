/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 *
 * IQ FFT engine for IF Average plugin.
 * Binds to the SDR++ IQ frontend, runs its own FFTW3 engine with
 * configurable FFT size and overlap. Delivers linear power spectra
 * to a callback.
 */
#pragma once
#include <dsp/types.h>
#include <dsp/stream.h>
#include <dsp/sink/handler_sink.h>
#include <signal_path/signal_path.h>
#include <fftw3.h>
#include <functional>
#include <atomic>

enum FFTWindowType {
	FFT_WIN_BLACKMAN_HARRIS = 0,
	FFT_WIN_HANN,
	FFT_WIN_NUTTALL,
	FFT_WIN_FLAT_TOP,
	FFT_WIN_KAISER_B8
};

class IQFFTEngine {
public:
	IQFFTEngine();
	~IQFFTEngine();

	void setCallback(std::function<void(const float *, int)> cb);

	bool setup(int fftSize, float overlap, int windowType);

	/* Free FFT resources. */
	void teardown();

	/* Start/stop IQ stream processing. */
	void start();
	void stop();

	bool isRunning() const { return running; }
	int getFFTSize() const { return fftSize; }

private:
	static void iqHandler(dsp::complex_t *data, int count, void *ctx);
	void processIQ(dsp::complex_t *data, int count);
	void processFFTFrame();
	void generateWindow();

	int fftSize = 0;
	float overlapFrac = 0.5f;
	int winType = FFT_WIN_BLACKMAN_HARRIS;

	fftwf_complex *fftIn = nullptr;
	fftwf_complex *fftOut = nullptr;
	fftwf_plan plan = nullptr;
	float *windowBuf = nullptr;
	float *powerBuf = nullptr;

	dsp::complex_t *ringBuf = nullptr;
	int ringPos = 0;
	int samplesNeeded = 0;

	/* IQ stream -- created/destroyed on start/stop */
	dsp::stream<dsp::complex_t> *iqStream = nullptr;
	dsp::sink::Handler<dsp::complex_t> iqSink;
	bool sinkInitialized = false;

	std::function<void(const float *, int)> outputCallback;
	std::atomic<bool> running{false};
	bool resourcesAllocated = false;
};
