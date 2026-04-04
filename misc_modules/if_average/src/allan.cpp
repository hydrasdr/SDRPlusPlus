/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "allan.h"
#include <cmath>
#include <algorithm>

AllanVarianceAnalyzer::AllanVarianceAnalyzer() {}

AllanVarianceAnalyzer::~AllanVarianceAnalyzer() {}

void AllanVarianceAnalyzer::setMaxTau(double t)
{
	std::lock_guard<std::mutex> lck(mtx);
	maxTau = t;
}

void AllanVarianceAnalyzer::setSampleInterval(double interval)
{
	std::lock_guard<std::mutex> lck(mtx);
	sampleInterval = interval;
}

void AllanVarianceAnalyzer::addSample(double power)
{
	std::lock_guard<std::mutex> lck(mtx);
	if ((int)timeSeries.size() >= ALLAN_MAX_SAMPLES)
		return;
	timeSeries.push_back(power);
}

void AllanVarianceAnalyzer::reset()
{
	std::lock_guard<std::mutex> lck(mtx);
	timeSeries.clear();
	results.clear();
}

void AllanVarianceAnalyzer::compute()
{
	std::lock_guard<std::mutex> lck(mtx);
	results.clear();

	int N = (int)timeSeries.size();
	if (N < 4)
		return;

	double minTau = sampleInterval;
	double logMin = log10(minTau);
	double logMax = log10(std::min(maxTau,
				       (double)(N / 2) * sampleInterval));

	if (logMax <= logMin)
		return;

	double logStep = (logMax - logMin) / (double)(numPoints - 1);

	int prevM = 0;
	for (int p = 0; p < numPoints; p++) {
		double tau = pow(10.0, logMin + p * logStep);
		int m = (int)round(tau / sampleInterval);
		if (m < 1)
			m = 1;
		if (m == prevM)
			continue;
		if (m > N / 2)
			break;
		prevM = m;

		int numBins = N / m;
		if (numBins < 2)
			continue;

		std::vector<double> binAvg(numBins, 0.0);
		for (int b = 0; b < numBins; b++) {
			double sum = 0.0;
			for (int j = 0; j < m; j++)
				sum += timeSeries[b * m + j];
			binAvg[b] = sum / (double)m;
		}

		double avar = 0.0;
		int diffCount = numBins - 1;
		for (int b = 0; b < diffCount; b++) {
			double diff = binAvg[b + 1] - binAvg[b];
			avar += diff * diff;
		}
		avar *= 0.5 / (double)diffCount;

		AllanPoint pt;
		pt.tau = (double)m * sampleInterval;
		pt.adev = sqrt(avar);
		results.push_back(pt);
	}
}
