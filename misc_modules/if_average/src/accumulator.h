/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <vector>
#include <mutex>
#include <atomic>
#include <dsp/buffer/buffer.h>

enum AveragingMode {
	AVG_LINEAR_MEAN = 0,
	AVG_WEIGHTED_MEAN,
	AVG_RMS,
	AVG_MEDIAN,
	AVG_EMA
};

class SpectrumAccumulator {
public:
	SpectrumAccumulator();
	~SpectrumAccumulator();

	void init(int width, AveragingMode mode, float emaAlpha = 0.01f,
		  int medianDepth = 64);
	void setMode(AveragingMode mode);
	void setEmaAlpha(float alpha);
	void setMedianDepth(int depth);
	void resize(int newWidth);
	void reset();
	void addSpectrum(const float *power, int width);
	void getResult(float *out, int width);

	uint64_t getCount() const { return count.load(); }

private:
	void allocBuffers();
	void freeBuffers();
	void resetLocked();

	AveragingMode mode = AVG_LINEAR_MEAN;
	int currentWidth = 0;
	std::atomic<uint64_t> count{0};

	float *sumBuf = nullptr;
	float *sumSqBuf = nullptr;
	float *tempBuf = nullptr;
	double totalWeight = 0.0;

	float *emaBuf = nullptr;
	float emaAlpha = 0.01f;
	bool emaInitialized = false;

	std::vector<float *> medianRing;
	int medianDepth = 64;
	int medianPos = 0;
	int medianFilled = 0;

	std::mutex mtx;
};
