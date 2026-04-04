/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 */
#include "accumulator.h"
#include <volk/volk.h>

SpectrumAccumulator::SpectrumAccumulator() {}

SpectrumAccumulator::~SpectrumAccumulator()
{
	freeBuffers();
}

void SpectrumAccumulator::init(int width, AveragingMode m, float alpha,
			       int mdepth)
{
	std::lock_guard<std::mutex> lck(mtx);
	this->mode = m;
	this->emaAlpha = alpha;
	this->medianDepth = mdepth;
	this->currentWidth = width;
	allocBuffers();
	resetLocked();
}

void SpectrumAccumulator::setMode(AveragingMode newMode)
{
	std::lock_guard<std::mutex> lck(mtx);
	if (newMode == mode)
		return;
	mode = newMode;
	freeBuffers();
	allocBuffers();
	resetLocked();
}

void SpectrumAccumulator::setEmaAlpha(float alpha)
{
	std::lock_guard<std::mutex> lck(mtx);
	emaAlpha = alpha;
}

void SpectrumAccumulator::setMedianDepth(int depth)
{
	std::lock_guard<std::mutex> lck(mtx);
	if (depth == medianDepth)
		return;
	medianDepth = depth;
	freeBuffers();
	allocBuffers();
	resetLocked();
}

void SpectrumAccumulator::resize(int newWidth)
{
	std::lock_guard<std::mutex> lck(mtx);
	if (newWidth == currentWidth)
		return;
	currentWidth = newWidth;
	freeBuffers();
	allocBuffers();
	resetLocked();
}

void SpectrumAccumulator::allocBuffers()
{
	if (currentWidth <= 0)
		return;

	/* Allocate only what the current mode needs */
	switch (mode) {
	case AVG_LINEAR_MEAN:
		sumBuf = dsp::buffer::alloc<float>(currentWidth);
		break;
	case AVG_WEIGHTED_MEAN:
		sumBuf = dsp::buffer::alloc<float>(currentWidth);
		tempBuf = dsp::buffer::alloc<float>(currentWidth);
		break;
	case AVG_RMS:
		sumSqBuf = dsp::buffer::alloc<float>(currentWidth);
		break;
	case AVG_MEDIAN:
		medianRing.resize(medianDepth);
		for (int i = 0; i < medianDepth; i++)
			medianRing[i] = dsp::buffer::alloc<float>(currentWidth);
		break;
	case AVG_EMA:
		emaBuf = dsp::buffer::alloc<float>(currentWidth);
		break;
	}
}

void SpectrumAccumulator::freeBuffers()
{
	if (sumBuf) { dsp::buffer::free(sumBuf); sumBuf = nullptr; }
	if (sumSqBuf) { dsp::buffer::free(sumSqBuf); sumSqBuf = nullptr; }
	if (tempBuf) { dsp::buffer::free(tempBuf); tempBuf = nullptr; }
	if (emaBuf) { dsp::buffer::free(emaBuf); emaBuf = nullptr; }

	for (auto &buf : medianRing) {
		if (buf)
			dsp::buffer::free(buf);
	}
	medianRing.clear();
}

void SpectrumAccumulator::resetLocked()
{
	count.store(0);
	totalWeight = 0.0;
	emaInitialized = false;
	medianPos = 0;
	medianFilled = 0;

	if (currentWidth <= 0)
		return;

	if (sumBuf) dsp::buffer::clear(sumBuf, currentWidth);
	if (sumSqBuf) dsp::buffer::clear(sumSqBuf, currentWidth);
	if (tempBuf) dsp::buffer::clear(tempBuf, currentWidth);
	if (emaBuf) dsp::buffer::clear(emaBuf, currentWidth);
}

void SpectrumAccumulator::reset()
{
	std::lock_guard<std::mutex> lck(mtx);
	resetLocked();
}

void SpectrumAccumulator::addSpectrum(const float *power, int width)
{
	std::lock_guard<std::mutex> lck(mtx);
	if (width != currentWidth)
		return;

	switch (mode) {
	case AVG_LINEAR_MEAN:
		if (!sumBuf) break;
		volk_32f_x2_add_32f(sumBuf, sumBuf, power, width);
		count.fetch_add(1);
		break;

	case AVG_WEIGHTED_MEAN: {
		if (!sumBuf || !tempBuf) break;
		std::vector<float> sorted(power, power + width);
		std::nth_element(sorted.begin(),
				 sorted.begin() + width / 2,
				 sorted.end());
		float median = sorted[width / 2];

		float mad = 0.0f;
		for (int i = 0; i < width; i++)
			mad += fabsf(power[i] - median);
		mad /= (float)width;
		if (mad < 1e-20f)
			mad = 1e-20f;

		float weight = 1.0f / (mad * mad);
		totalWeight += (double)weight;

		volk_32f_s32f_multiply_32f(tempBuf, power, weight, width);
		volk_32f_x2_add_32f(sumBuf, sumBuf, tempBuf, width);
		count.fetch_add(1);
		break;
	}

	case AVG_RMS:
		if (!sumSqBuf) break;
		for (int i = 0; i < width; i++)
			sumSqBuf[i] += power[i] * power[i];
		count.fetch_add(1);
		break;

	case AVG_MEDIAN:
		if (medianRing.empty()) break;
		memcpy(medianRing[medianPos], power, width * sizeof(float));
		medianPos = (medianPos + 1) % medianDepth;
		if (medianFilled < medianDepth)
			medianFilled++;
		count.fetch_add(1);
		break;

	case AVG_EMA:
		if (!emaBuf) break;
		if (!emaInitialized) {
			memcpy(emaBuf, power, width * sizeof(float));
			emaInitialized = true;
		} else {
			float oneMinusAlpha = 1.0f - emaAlpha;
			for (int i = 0; i < width; i++)
				emaBuf[i] = emaAlpha * power[i] +
					    oneMinusAlpha * emaBuf[i];
		}
		count.fetch_add(1);
		break;
	}
}

void SpectrumAccumulator::getResult(float *out, int width)
{
	std::lock_guard<std::mutex> lck(mtx);
	if (width != currentWidth || count.load() == 0) {
		memset(out, 0, width * sizeof(float));
		return;
	}

	switch (mode) {
	case AVG_LINEAR_MEAN:
		if (!sumBuf) { memset(out, 0, width * sizeof(float)); break; }
		volk_32f_s32f_multiply_32f(out, sumBuf,
					   1.0f / (float)count.load(),
					   width);
		break;

	case AVG_WEIGHTED_MEAN:
		if (!sumBuf || totalWeight < 1e-20) {
			memset(out, 0, width * sizeof(float));
			break;
		}
		volk_32f_s32f_multiply_32f(out, sumBuf,
					   1.0f / (float)totalWeight,
					   width);
		break;

	case AVG_RMS:
		if (!sumSqBuf) { memset(out, 0, width * sizeof(float)); break; }
		{
			float inv = 1.0f / (float)count.load();
			for (int i = 0; i < width; i++)
				out[i] = sqrtf(sumSqBuf[i] * inv);
		}
		break;

	case AVG_MEDIAN:
		if (medianFilled == 0) {
			memset(out, 0, width * sizeof(float));
			break;
		}
		{
			std::vector<float> col(medianFilled);
			for (int i = 0; i < width; i++) {
				for (int j = 0; j < medianFilled; j++)
					col[j] = medianRing[j][i];
				std::nth_element(col.begin(),
						 col.begin() + medianFilled / 2,
						 col.end());
				out[i] = col[medianFilled / 2];
			}
		}
		break;

	case AVG_EMA:
		if (!emaBuf || !emaInitialized) {
			memset(out, 0, width * sizeof(float));
			break;
		}
		memcpy(out, emaBuf, width * sizeof(float));
		break;

	default:
		memset(out, 0, width * sizeof(float));
		break;
	}
}
