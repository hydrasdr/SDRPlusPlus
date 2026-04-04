/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <vector>
#include <cstdint>
#include <mutex>

#define ALLAN_MAX_SAMPLES 2000000

struct AllanPoint {
	double tau;
	double adev;
};

class AllanVarianceAnalyzer {
public:
	AllanVarianceAnalyzer();
	~AllanVarianceAnalyzer();

	void setMaxTau(double maxTau);
	void setSampleInterval(double interval);

	void addSample(double power);
	void reset();
	void compute();

	/* Results only written/read on GUI thread */
	const std::vector<AllanPoint> &getResults() const { return results; }
	bool hasResults() const { return !results.empty(); }

private:
	double maxTau = 1000.0;
	int numPoints = 50;
	double sampleInterval = 1.0;

	std::vector<double> timeSeries;
	std::vector<AllanPoint> results;
	std::mutex mtx;
};
