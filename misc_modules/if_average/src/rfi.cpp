/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 */
#include "rfi.h"
#include <cmath>
#include <cstring>
#include <algorithm>

RFIMitigator::RFIMitigator() {}

RFIMitigator::~RFIMitigator() {}

void RFIMitigator::resize(int newWidth)
{
	std::lock_guard<std::mutex> lck(mtx);
	currentWidth = newWidth;
	flagBuf.assign(newWidth, false);
	runningSum.assign(newWidth, 0.0);
	runningSumSq.assign(newWidth, 0.0);
	skSum.assign(newWidth, 0.0);
	skSumSq.assign(newWidth, 0.0);
	runningCount = 0;
	skCount = 0;
}

void RFIMitigator::clearStats()
{
	std::fill(flagBuf.begin(), flagBuf.end(), false);
	std::fill(runningSum.begin(), runningSum.end(), 0.0);
	std::fill(runningSumSq.begin(), runningSumSq.end(), 0.0);
	runningCount = 0;
	std::fill(skSum.begin(), skSum.end(), 0.0);
	std::fill(skSumSq.begin(), skSumSq.end(), 0.0);
	skCount = 0;
}

void RFIMitigator::reset()
{
	std::lock_guard<std::mutex> lck(mtx);
	clearStats();
}

void RFIMitigator::setMode(RFIMode m)
{
	std::lock_guard<std::mutex> lck(mtx);
	mode = m;
	clearStats();
}

void RFIMitigator::setThresholdSigma(float sigma)
{
	thresholdSigma.store(sigma, std::memory_order_relaxed);
}

void RFIMitigator::setBlanking(RFIBlanking b)
{
	blankingMode.store((int)b, std::memory_order_relaxed);
}

void RFIMitigator::copyFlags(std::vector<bool> &dst)
{
	std::lock_guard<std::mutex> lck(mtx);
	dst = flagBuf;
}

void RFIMitigator::feedKurtosisLocked(const float *power, int width)
{
	for (int i = 0; i < width; i++) {
		double p = (double)power[i];
		skSum[i] += p;
		skSumSq[i] += p * p;
	}
	skCount++;
}

bool RFIMitigator::process(float *power, int width)
{
	std::lock_guard<std::mutex> lck(mtx);
	if (width != currentWidth)
		return false;

	float sigma = thresholdSigma.load(std::memory_order_relaxed);
	int blanking = blankingMode.load(std::memory_order_relaxed);

	std::fill(flagBuf.begin(), flagBuf.end(), false);

	switch (mode) {
	case RFI_THRESHOLD: {
		for (int i = 0; i < width; i++) {
			runningSum[i] += (double)power[i];
			runningSumSq[i] += (double)power[i] * (double)power[i];
		}
		runningCount++;

		if (runningCount < 4)
			break;

		double invN = 1.0 / (double)runningCount;
		for (int i = 0; i < width; i++) {
			double mean = runningSum[i] * invN;
			double var = runningSumSq[i] * invN - mean * mean;
			if (var < 0.0)
				var = 0.0;
			double stddev = sqrt(var);
			double thresh = mean + (double)sigma * stddev;
			if ((double)power[i] > thresh)
				flagBuf[i] = true;
		}
		break;
	}

	case RFI_SPECTRAL_KURTOSIS: {
		/*
		 * SK = (M+1)/(M-1) * (M * S2 / S1^2 - 1)
		 * SK = 1.0 for Gaussian noise; deviates for RFI.
		 */
		feedKurtosisLocked(power, width);

		if (skCount < 8)
			break;

		double M = (double)skCount;
		double factor = (M + 1.0) / (M - 1.0);

		for (int i = 0; i < width; i++) {
			double s1 = skSum[i];
			double s2 = skSumSq[i];
			if (s1 * s1 < 1e-30)
				continue;
			double sk = factor * (M * s2 / (s1 * s1) - 1.0);
			if (fabs(sk - 1.0) > (double)sigma * 0.5)
				flagBuf[i] = true;
		}
		break;
	}

	default:
		break;
	}

	int flagCount = 0;
	for (int i = 0; i < width; i++) {
		if (flagBuf[i])
			flagCount++;
	}

	if (blanking == RFI_DROP_SPECTRUM && flagCount > width / 4)
		return true;

	if (flagCount > 0)
		applyBlanking(power, width);

	return false;
}

void RFIMitigator::applyBlanking(float *power, int width)
{
	int blanking = blankingMode.load(std::memory_order_relaxed);

	switch (blanking) {
	case RFI_REPLACE_MEAN: {
		for (int i = 0; i < width; i++) {
			if (!flagBuf[i])
				continue;
			float sum = 0.0f;
			int cnt = 0;
			for (int j = i - 1; j >= 0 && j >= i - 8; j--) {
				if (!flagBuf[j]) {
					sum += power[j];
					cnt++;
					break;
				}
			}
			for (int j = i + 1; j < width && j <= i + 8; j++) {
				if (!flagBuf[j]) {
					sum += power[j];
					cnt++;
					break;
				}
			}
			power[i] = (cnt > 0) ? sum / (float)cnt : 0.0f;
		}
		break;
	}

	case RFI_REPLACE_INTERPOLATE: {
		for (int i = 0; i < width; i++) {
			if (!flagBuf[i])
				continue;
			int left = -1, right = -1;
			for (int j = i - 1; j >= 0; j--) {
				if (!flagBuf[j]) { left = j; break; }
			}
			for (int j = i + 1; j < width; j++) {
				if (!flagBuf[j]) { right = j; break; }
			}
			if (left >= 0 && right >= 0) {
				float t = (float)(i - left) /
					  (float)(right - left);
				power[i] = power[left] * (1.0f - t) +
					   power[right] * t;
			} else if (left >= 0) {
				power[i] = power[left];
			} else if (right >= 0) {
				power[i] = power[right];
			} else {
				power[i] = 0.0f;
			}
		}
		break;
	}

	case RFI_ZERO:
		for (int i = 0; i < width; i++) {
			if (flagBuf[i])
				power[i] = 0.0f;
		}
		break;

	case RFI_DROP_SPECTRUM:
		break;

	default:
		break;
	}
}
