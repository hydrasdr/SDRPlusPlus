/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <vector>
#include <cstdint>
#include <mutex>
#include <atomic>

enum RFIMode {
	RFI_THRESHOLD = 0,
	RFI_SPECTRAL_KURTOSIS
};

enum RFIBlanking {
	RFI_REPLACE_MEAN = 0,
	RFI_REPLACE_INTERPOLATE,
	RFI_ZERO,
	RFI_DROP_SPECTRUM
};

class RFIMitigator {
public:
	RFIMitigator();
	~RFIMitigator();

	void resize(int newWidth);
	void reset();

	void setMode(RFIMode mode);
	void setThresholdSigma(float sigma);
	void setBlanking(RFIBlanking blanking);

	bool process(float *power, int width);
	void copyFlags(std::vector<bool> &dst);

private:
	void clearStats();
	void feedKurtosisLocked(const float *power, int width);
	void applyBlanking(float *power, int width);

	RFIMode mode = RFI_THRESHOLD;
	std::atomic<float> thresholdSigma{5.0f};
	std::atomic<int> blankingMode{RFI_REPLACE_MEAN};

	std::vector<bool> flagBuf;

	std::vector<double> runningSum;
	std::vector<double> runningSumSq;
	uint64_t runningCount = 0;

	std::vector<double> skSum;
	std::vector<double> skSumSq;
	uint64_t skCount = 0;

	int currentWidth = 0;
	std::mutex mtx;
};
