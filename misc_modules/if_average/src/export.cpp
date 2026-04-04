/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 */
#include "export.h"
#include <cmath>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <utils/flog.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define MKDIR(path) mkdir(path, 0755)
#endif

static struct tm safegmtime(const time_t *t)
{
	struct tm result;
	memset(&result, 0, sizeof(result));
#ifdef _WIN32
	gmtime_s(&result, t);
#else
	gmtime_r(t, &result);
#endif
	return result;
}

ExportEngine::ExportEngine() {}

ExportEngine::~ExportEngine() {}

void ExportEngine::setPath(const std::string &path)
{
	std::lock_guard<std::mutex> lck(mtx);
	exportPath = path;
}

std::string ExportEngine::generateFilename(const ExportMetadata &meta)
{
	time_t now = time(nullptr);
	struct tm utc = safegmtime(&now);

	char buf[256];
	snprintf(buf, sizeof(buf),
		 "%s/ifavg_%.0f_%04d%02d%02d_%02d%02d%02d.csv",
		 exportPath.c_str(), meta.centerFreqHz,
		 utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
		 utc.tm_hour, utc.tm_min, utc.tm_sec);
	return std::string(buf);
}

std::string ExportEngine::exportSpectrum(const float *spectrum,
					 const float *residual,
					 const float *kelvin,
					 const std::vector<bool> *rfiFlags,
					 int width,
					 const ExportMetadata &meta)
{
	std::lock_guard<std::mutex> lck(mtx);

	MKDIR(exportPath.c_str());

	std::string filename = generateFilename(meta);
	std::ofstream file(filename);

	if (!file.is_open()) {
		flog::error("ExportEngine: failed to open {}", filename);
		return "";
	}

	/* Header */
	file << "# IF Average Export\n";
	file << "# Date Start (UTC): " << meta.dateObs << "\n";
	file << "# Date End (UTC): " << meta.dateEnd << "\n";
	file << "# Source: " << meta.sourceName << "\n";
	file << "# Center Frequency (Hz): "
	     << std::fixed << std::setprecision(3)
	     << meta.centerFreqHz << "\n";
	file << "# Effective Sample Rate (Hz): "
	     << std::fixed << std::setprecision(0)
	     << meta.sampleRateHz << "\n";
	file << "# Hardware Sample Rate (Hz): "
	     << std::fixed << std::setprecision(0)
	     << meta.hardwareSampleRateHz << "\n";
	file << "# Decimation: " << meta.decimation << "\n";
	file << "# FFT Size: " << meta.fftSize << "\n";
	file << "# FFT Rate (Hz): "
	     << std::fixed << std::setprecision(1)
	     << meta.fftRate << "\n";
	file << "# Integration Time (s): "
	     << std::fixed << std::setprecision(3)
	     << meta.integrationTimeSec << "\n";
	file << "# Spectra Accumulated: " << meta.spectraAccumulated << "\n";
	file << "# Averaging Mode: " << meta.averagingMode << "\n";
	if (!meta.baselineInfo.empty())
		file << "# Baseline: " << meta.baselineInfo << "\n";
	if (!meta.calibrationInfo.empty())
		file << "# Calibration: " << meta.calibrationInfo << "\n";
	if (meta.tsysKelvin > 0.0)
		file << "# T_sys (K): "
		     << std::fixed << std::setprecision(1)
		     << meta.tsysKelvin << "\n";

	/* Column header */
	file << "# Columns: frequency_hz,power_dbfs,power_linear";
	if (kelvin)
		file << ",power_kelvin";
	if (residual)
		file << ",residual";
	if (rfiFlags)
		file << ",rfi_flag";
	file << "\n";

	/* Use effective sample rate for frequency axis (post-decimation) */
	double freqStart = meta.centerFreqHz - meta.sampleRateHz / 2.0;
	double freqStep = meta.sampleRateHz / (double)width;

	for (int i = 0; i < width; i++) {
		double freq = freqStart + i * freqStep;
		float powerLinear = spectrum[i];
		float powerDb = 10.0f * log10f(powerLinear + 1e-30f);

		file << std::fixed << std::setprecision(3) << freq << ","
		     << std::setprecision(3) << powerDb << ","
		     << std::scientific << std::setprecision(3)
		     << powerLinear;

		if (kelvin)
			file << "," << std::fixed << std::setprecision(1)
			     << kelvin[i];
		if (residual)
			file << "," << std::scientific << std::setprecision(3)
			     << residual[i];
		if (rfiFlags)
			file << "," << ((*rfiFlags)[i] ? 1 : 0);
		file << "\n";
	}

	file.close();
	flog::info("ExportEngine: wrote {}", filename);
	return filename;
}
