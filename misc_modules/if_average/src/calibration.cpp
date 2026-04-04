/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 */
#include "calibration.h"
#include <cmath>

CalibrationEngine::CalibrationEngine() {}

CalibrationEngine::~CalibrationEngine() {}

void CalibrationEngine::reset()
{
	std::lock_guard<std::mutex> lck(mtx);
	hotSpectrum.clear();
	coldSpectrum.clear();
	gainPerChannel.clear();
	tsys = 0.0;
	yFactor = 0.0;
	calibrated = false;
}

void CalibrationEngine::setTHot(double t)
{
	std::lock_guard<std::mutex> lck(mtx);
	tHot = t;
	calibrated = false;
}

void CalibrationEngine::setTCold(double t)
{
	std::lock_guard<std::mutex> lck(mtx);
	tCold = t;
	calibrated = false;
}

void CalibrationEngine::captureHot(const float *spectrum, int width)
{
	std::lock_guard<std::mutex> lck(mtx);
	hotSpectrum.assign(spectrum, spectrum + width);
	calibrated = false;
}

void CalibrationEngine::captureCold(const float *spectrum, int width)
{
	std::lock_guard<std::mutex> lck(mtx);
	coldSpectrum.assign(spectrum, spectrum + width);
	calibrated = false;
}

bool CalibrationEngine::computeCalibration()
{
	std::lock_guard<std::mutex> lck(mtx);

	if (hotSpectrum.empty() || coldSpectrum.empty())
		return false;
	if (hotSpectrum.size() != coldSpectrum.size())
		return false;

	int width = (int)hotSpectrum.size();
	gainPerChannel.resize(width);

	/*
	 * Y-factor calibration:
	 *   Y = P_hot / P_cold
	 *   T_sys = (T_hot - Y * T_cold) / (Y - 1)
	 *   Gain = P_hot / (T_hot + T_sys)
	 */
	double totalHot = 0.0, totalCold = 0.0;
	for (int i = 0; i < width; i++) {
		totalHot += (double)hotSpectrum[i];
		totalCold += (double)coldSpectrum[i];
	}

	if (totalCold < 1e-30)
		return false;

	yFactor = totalHot / totalCold;
	if (yFactor <= 1.0)
		return false;

	tsys = (tHot - yFactor * tCold) / (yFactor - 1.0);

	for (int i = 0; i < width; i++) {
		double pHot = (double)hotSpectrum[i];
		double pCold = (double)coldSpectrum[i];

		if (pCold < 1e-30) {
			gainPerChannel[i] = 0.0f;
			continue;
		}

		double yLocal = pHot / pCold;
		if (yLocal <= 1.0) {
			gainPerChannel[i] = 0.0f;
			continue;
		}

		double tsysLocal = (tHot - yLocal * tCold) / (yLocal - 1.0);
		double gain = pHot / (tHot + tsysLocal);
		gainPerChannel[i] = (float)gain;
	}

	calibrated = true;
	return true;
}

void CalibrationEngine::apply(float *spectrum, int width)
{
	std::lock_guard<std::mutex> lck(mtx);
	if (!calibrated || (int)gainPerChannel.size() != width)
		return;

	/*
	 * T_ant = P / gain - T_sys
	 */
	for (int i = 0; i < width; i++) {
		if (gainPerChannel[i] > 1e-30f)
			spectrum[i] = (float)((double)spectrum[i] /
					      (double)gainPerChannel[i] - tsys);
		else
			spectrum[i] = 0.0f;
	}
}
