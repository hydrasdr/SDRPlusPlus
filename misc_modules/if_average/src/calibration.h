/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <vector>
#include <mutex>
#include <atomic>

class CalibrationEngine {
public:
	CalibrationEngine();
	~CalibrationEngine();

	void reset();
	void setTHot(double tHot);
	void setTCold(double tCold);

	void captureHot(const float *spectrum, int width);
	void captureCold(const float *spectrum, int width);
	bool computeCalibration();

	void apply(float *spectrum, int width);

	bool isCalibrated() const { return calibrated.load(); }
	double getTsys() const { return tsys; }
	double getYFactor() const { return yFactor; }

	bool hasHot() const { return !hotSpectrum.empty(); }
	bool hasCold() const { return !coldSpectrum.empty(); }

private:
	double tHot = 290.0;
	double tCold = 10.0;

	std::vector<float> hotSpectrum;
	std::vector<float> coldSpectrum;
	std::vector<float> gainPerChannel;

	double tsys = 0.0;
	double yFactor = 0.0;
	std::atomic<bool> calibrated{false};

	std::mutex mtx;
};
