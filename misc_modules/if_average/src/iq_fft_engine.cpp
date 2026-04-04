/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "iq_fft_engine.h"
#include <dsp/buffer/buffer.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <utils/flog.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

IQFFTEngine::IQFFTEngine()
{
	/* Init the sink with NULL input -- will be set on start() */
	iqSink.init(NULL, iqHandler, this);
	sinkInitialized = true;
}

IQFFTEngine::~IQFFTEngine()
{
	stop();
	teardown();
}

void IQFFTEngine::setCallback(std::function<void(const float *, int)> cb)
{
	outputCallback = cb;
}

bool IQFFTEngine::setup(int size, float overlap, int windowType)
{
	teardown();

	fftSize = size;
	overlapFrac = overlap;
	winType = windowType;

	flog::info("IQFFTEngine: allocating for size={}", fftSize);

	fftIn = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * fftSize);
	fftOut = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * fftSize);
	if (!fftIn || !fftOut) {
		flog::error("IQFFTEngine: FFTW malloc failed");
		teardown();
		return false;
	}

	plan = fftwf_plan_dft_1d(fftSize, fftIn, fftOut,
				 FFTW_FORWARD, FFTW_ESTIMATE);
	if (!plan) {
		flog::error("IQFFTEngine: FFTW plan failed");
		teardown();
		return false;
	}

	windowBuf = dsp::buffer::alloc<float>(fftSize);
	powerBuf = dsp::buffer::alloc<float>(fftSize);
	ringBuf = dsp::buffer::alloc<dsp::complex_t>(fftSize * 2);

	ringPos = 0;
	samplesNeeded = fftSize;

	generateWindow();
	resourcesAllocated = true;

	flog::info("IQFFTEngine: ready, size={}", fftSize);
	return true;
}

void IQFFTEngine::teardown()
{
	if (plan) { fftwf_destroy_plan(plan); plan = nullptr; }
	if (fftIn) { fftwf_free(fftIn); fftIn = nullptr; }
	if (fftOut) { fftwf_free(fftOut); fftOut = nullptr; }
	if (windowBuf) { dsp::buffer::free(windowBuf); windowBuf = nullptr; }
	if (powerBuf) { dsp::buffer::free(powerBuf); powerBuf = nullptr; }
	if (ringBuf) { dsp::buffer::free(ringBuf); ringBuf = nullptr; }
	fftSize = 0;
	resourcesAllocated = false;
}

void IQFFTEngine::generateWindow()
{
	double N = (double)fftSize;
	for (int i = 0; i < fftSize; i++) {
		double n = (double)i;
		double x = 2.0 * M_PI * n / (N - 1.0);

		switch (winType) {
		case FFT_WIN_BLACKMAN_HARRIS:
			/* 4-term, 92 dB sidelobe rejection */
			windowBuf[i] = (float)(0.35875
					       - 0.48829 * cos(x)
					       + 0.14128 * cos(2.0 * x)
					       - 0.01168 * cos(3.0 * x));
			break;

		case FFT_WIN_HANN:
			/* General purpose, -31 dB sidelobes */
			windowBuf[i] = (float)(0.5 * (1.0 - cos(x)));
			break;

		case FFT_WIN_NUTTALL:
			/* -93 dB sidelobes */
			windowBuf[i] = (float)(0.3635819
					       - 0.4891775 * cos(x)
					       + 0.1365995 * cos(2.0 * x)
					       - 0.0106411 * cos(3.0 * x));
			break;

		case FFT_WIN_FLAT_TOP: {
			/* Minimal scalloping (0.01 dB), best for
			 * amplitude accuracy in spectral line work */
			windowBuf[i] = (float)(0.21557895
					       - 0.41663158 * cos(x)
					       + 0.277263158 * cos(2.0 * x)
					       - 0.083578947 * cos(3.0 * x)
					       + 0.006947368 * cos(4.0 * x));
			break;
		}

		case FFT_WIN_KAISER_B8: {
			/* Kaiser beta=8, -69 dB sidelobes */
			double beta = 8.0;
			double alpha = (N - 1.0) / 2.0;
			double r = (n - alpha) / alpha;
			double arg = beta * sqrt(1.0 - r * r);

			/* I0(arg) via series expansion (20 terms) */
			double i0arg = 1.0, term = 1.0;
			for (int k = 1; k <= 20; k++) {
				term *= (arg / (2.0 * k)) *
					(arg / (2.0 * k));
				i0arg += term;
			}
			double i0beta = 1.0;
			term = 1.0;
			for (int k = 1; k <= 20; k++) {
				term *= (beta / (2.0 * k)) *
					(beta / (2.0 * k));
				i0beta += term;
			}
			windowBuf[i] = (float)(i0arg / i0beta);
			break;
		}

		default:
			windowBuf[i] = 1.0f;
			break;
		}
	}
}

void IQFFTEngine::start()
{
	if (running || !resourcesAllocated || !sinkInitialized)
		return;

	/* Create IQ stream and bind (same pattern as recorder module) */
	iqStream = new dsp::stream<dsp::complex_t>();
	iqSink.setInput(iqStream);
	iqSink.start();
	sigpath::iqFrontEnd.bindIQStream(iqStream);
	running = true;

	flog::info("IQFFTEngine: started, size={}", fftSize);
}

void IQFFTEngine::stop()
{
	if (!running)
		return;

	sigpath::iqFrontEnd.unbindIQStream(iqStream);
	iqSink.stop();
	delete iqStream;
	iqStream = nullptr;
	running = false;
	ringPos = 0;
	samplesNeeded = fftSize;

	flog::info("IQFFTEngine: stopped");
}

void IQFFTEngine::iqHandler(dsp::complex_t *data, int count, void *ctx)
{
	IQFFTEngine *_this = (IQFFTEngine *)ctx;
	_this->processIQ(data, count);
}

void IQFFTEngine::processIQ(dsp::complex_t *data, int count)
{
	if (!resourcesAllocated)
		return;

	int pos = 0;
	while (pos < count) {
		int toCopy = std::min(count - pos, samplesNeeded);
		memcpy(&ringBuf[ringPos], &data[pos],
		       toCopy * sizeof(dsp::complex_t));
		ringPos += toCopy;
		pos += toCopy;
		samplesNeeded -= toCopy;

		if (samplesNeeded <= 0) {
			processFFTFrame();

			int advance = (int)((1.0f - overlapFrac) * (float)fftSize);
			if (advance < 1)
				advance = 1;

			int remaining = ringPos - advance;
			if (remaining > 0) {
				memmove(ringBuf, &ringBuf[advance],
					remaining * sizeof(dsp::complex_t));
			}
			ringPos = std::max(0, remaining);
			samplesNeeded = advance;
		}
	}
}

void IQFFTEngine::processFFTFrame()
{
	if (!plan || !fftIn || !fftOut || !powerBuf || !windowBuf) {
		flog::error("IQFFTEngine: null pointer in processFFTFrame");
		return;
	}

	/* Apply window function */
	for (int i = 0; i < fftSize; i++) {
		fftIn[i][0] = ringBuf[i].re * windowBuf[i];
		fftIn[i][1] = ringBuf[i].im * windowBuf[i];
	}

	fftwf_execute(plan);

	/* Power spectrum with FFT-shift (DC to center) */
	float invN2 = 1.0f / ((float)fftSize * (float)fftSize);
	int half = fftSize / 2;

	for (int i = 0; i < fftSize; i++) {
		int j = (i + half) % fftSize;
		float re = fftOut[j][0];
		float im = fftOut[j][1];
		powerBuf[i] = (re * re + im * im) * invN2;
	}

	if (outputCallback)
		outputCallback(powerBuf, fftSize);
}
