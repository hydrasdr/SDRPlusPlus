/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 *
 * Total power integrator for continuum radio astronomy.
 * Integrates band power from averaged spectra over time and
 * maintains a sample history for strip chart display.
 */
#pragma once
#include <deque>
#include <cstdint>
#include <mutex>

/* Cap history to 24 hours at 1 sample/sec */
#define TP_MAX_SAMPLES 86400

struct TotalPowerSample {
	double timestamp;	/* UTC seconds since epoch */
	double power;		/* linear power (mean over band) */
	double powerDb;		/* 10*log10(power) */
};

class TotalPowerIntegrator {
public:
	TotalPowerIntegrator();
	~TotalPowerIntegrator();

	void setBandLimits(double startHz, double stopHz);
	void setLogInterval(double interval);

	/*
	 * Feed one averaged spectrum. The integrator computes band-average
	 * power and logs a sample when logInterval has elapsed.
	 */
	void addSample(const float *spectrum, int width, double centerFreq,
		       double bandwidth, double timestamp);

	void reset();

	/* Thread-safe copy of sample history for display */
	void copySamples(std::deque<TotalPowerSample> &dst);
	double getLatestPowerDb();

private:
	double bandStartHz = 0.0;
	double bandStopHz = 0.0;
	double logInterval = 1.0;

	/* Accumulator between log intervals */
	double accumPower = 0.0;
	int accumCount = 0;
	double lastLogTime = 0.0;

	std::deque<TotalPowerSample> samples;
	std::mutex mtx;
};
