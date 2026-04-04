/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 */
#include "total_power.h"
#include <cmath>
#include <algorithm>

TotalPowerIntegrator::TotalPowerIntegrator() {}

TotalPowerIntegrator::~TotalPowerIntegrator() {}

void TotalPowerIntegrator::setBandLimits(double startHz, double stopHz)
{
	std::lock_guard<std::mutex> lck(mtx);
	bandStartHz = startHz;
	bandStopHz = stopHz;
}

void TotalPowerIntegrator::setLogInterval(double interval)
{
	std::lock_guard<std::mutex> lck(mtx);
	logInterval = interval;
}

void TotalPowerIntegrator::reset()
{
	std::lock_guard<std::mutex> lck(mtx);
	samples.clear();
	accumPower = 0.0;
	accumCount = 0;
	lastLogTime = 0.0;
}

void TotalPowerIntegrator::addSample(const float *spectrum, int width,
				     double centerFreq, double bandwidth,
				     double timestamp)
{
	std::lock_guard<std::mutex> lck(mtx);

	double freqStart = centerFreq - bandwidth / 2.0;
	double freqStep = (width > 0) ? bandwidth / (double)width : 1.0;

	/* Determine bin range for band integration */
	int startBin = 0;
	int stopBin = width - 1;

	if (bandStartHz != 0.0 || bandStopHz != 0.0) {
		double absStart = centerFreq + bandStartHz;
		double absStop = centerFreq + bandStopHz;
		startBin = std::clamp((int)((absStart - freqStart) / freqStep),
				      0, width - 1);
		stopBin = std::clamp((int)((absStop - freqStart) / freqStep),
				     0, width - 1);
	}

	/* Mean power across the selected band */
	double power = 0.0;
	for (int i = startBin; i <= stopBin; i++)
		power += (double)spectrum[i];

	int binCount = stopBin - startBin + 1;
	if (binCount > 0)
		power /= (double)binCount;

	accumPower += power;
	accumCount++;

	if (lastLogTime == 0.0)
		lastLogTime = timestamp;

	/* Log a sample when the interval elapses */
	if (timestamp - lastLogTime >= logInterval) {
		TotalPowerSample s;
		s.timestamp = timestamp;
		s.power = accumPower / (double)accumCount;
		s.powerDb = 10.0 * log10(s.power + 1e-30);

		/* O(1) pop from front with deque */
		if ((int)samples.size() >= TP_MAX_SAMPLES)
			samples.pop_front();

		samples.push_back(s);

		accumPower = 0.0;
		accumCount = 0;
		lastLogTime = timestamp;
	}
}

void TotalPowerIntegrator::copySamples(std::deque<TotalPowerSample> &dst)
{
	std::lock_guard<std::mutex> lck(mtx);
	dst = samples;
}

double TotalPowerIntegrator::getLatestPowerDb()
{
	std::lock_guard<std::mutex> lck(mtx);
	if (samples.empty())
		return -200.0;
	return samples.back().powerDb;
}
