/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>

struct ExportMetadata {
	double centerFreqHz;
	double sampleRateHz;		/* effective (post-decimation) */
	double hardwareSampleRateHz;	/* raw (pre-decimation) */
	int decimation;
	int fftSize;
	double fftRate;
	double integrationTimeSec;
	uint64_t spectraAccumulated;
	std::string sourceName;
	std::string averagingMode;
	std::string baselineInfo;
	std::string calibrationInfo;
	double tsysKelvin;
	std::string dateObs;
	std::string dateEnd;
};

class ExportEngine {
public:
	ExportEngine();
	~ExportEngine();

	void setPath(const std::string &path);

	std::string exportSpectrum(const float *spectrum,
				   const float *residual,
				   const float *kelvin,
				   const std::vector<bool> *rfiFlags,
				   int width,
				   const ExportMetadata &meta);

	std::string getExportPath() const { return exportPath; }

private:
	std::string generateFilename(const ExportMetadata &meta);

	std::string exportPath = "radio_astronomy";

	std::mutex mtx;
};
