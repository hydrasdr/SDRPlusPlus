/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 */
#include "baseline.h"
#include <cmath>
#include <cstring>
#include <algorithm>

BaselineManager::BaselineManager() {}

BaselineManager::~BaselineManager() {}

void BaselineManager::reset()
{
	std::lock_guard<std::mutex> lck(mtx);
	refSpectrum.clear();
	polyBaseline.clear();
}

void BaselineManager::setMode(BaselineMode m)
{
	std::lock_guard<std::mutex> lck(mtx);
	mode = m;
}

void BaselineManager::setPolyOrder(int order)
{
	std::lock_guard<std::mutex> lck(mtx);
	polyOrder = std::clamp(order, 0, 15);
}

void BaselineManager::captureReference(const float *spectrum, int width)
{
	std::lock_guard<std::mutex> lck(mtx);
	refSpectrum.assign(spectrum, spectrum + width);
}

/*
 * Polynomial baseline fitting using QR decomposition via
 * Householder reflections on a Vandermonde matrix.
 * X-axis normalized to [-1, 1] for numerical stability.
 */
void BaselineManager::polyFit(const float *data, int width, float *baseline)
{
	int order = polyOrder + 1;
	if (order > width)
		order = width;

	std::vector<double> A(width * order);
	std::vector<double> b(width);

	for (int i = 0; i < width; i++) {
		double x = (width > 1)
			? 2.0 * i / (width - 1) - 1.0
			: 0.0;
		double xp = 1.0;
		for (int j = 0; j < order; j++) {
			A[i * order + j] = xp;
			xp *= x;
		}
		b[i] = (double)data[i];
	}

	std::vector<double> tau(order);
	for (int j = 0; j < order; j++) {
		double norm = 0.0;
		for (int i = j; i < width; i++)
			norm += A[i * order + j] * A[i * order + j];
		norm = sqrt(norm);

		if (norm < 1e-30) {
			tau[j] = 0.0;
			continue;
		}

		double sign = (A[j * order + j] >= 0.0) ? 1.0 : -1.0;
		double u1 = A[j * order + j] + sign * norm;
		tau[j] = sign * u1 / norm;

		if (fabs(u1) < 1e-30)
			u1 = 1e-30;

		for (int i = j + 1; i < width; i++)
			A[i * order + j] /= u1;
		A[j * order + j] = -sign * norm;

		for (int k = j + 1; k < order; k++) {
			double dot = A[j * order + k];
			for (int i = j + 1; i < width; i++)
				dot += A[i * order + j] * A[i * order + k];
			dot *= tau[j];
			A[j * order + k] -= dot;
			for (int i = j + 1; i < width; i++)
				A[i * order + k] -= A[i * order + j] * dot;
		}
		{
			double dot = b[j];
			for (int i = j + 1; i < width; i++)
				dot += A[i * order + j] * b[i];
			dot *= tau[j];
			b[j] -= dot;
			for (int i = j + 1; i < width; i++)
				b[i] -= A[i * order + j] * dot;
		}
	}

	std::vector<double> coeffs(order, 0.0);
	for (int j = order - 1; j >= 0; j--) {
		double sum = b[j];
		for (int k = j + 1; k < order; k++)
			sum -= A[j * order + k] * coeffs[k];
		double diag = A[j * order + j];
		coeffs[j] = (fabs(diag) > 1e-30) ? sum / diag : 0.0;
	}

	for (int i = 0; i < width; i++) {
		double x = (width > 1)
			? 2.0 * i / (width - 1) - 1.0
			: 0.0;
		double val = 0.0;
		double xp = 1.0;
		for (int j = 0; j < order; j++) {
			val += coeffs[j] * xp;
			xp *= x;
		}
		baseline[i] = (float)val;
	}
}

void BaselineManager::subtract(float *spectrum, int width)
{
	std::lock_guard<std::mutex> lck(mtx);

	switch (mode) {
	case BL_POLYNOMIAL: {
		polyBaseline.resize(width);
		polyFit(spectrum, width, polyBaseline.data());
		for (int i = 0; i < width; i++)
			spectrum[i] -= polyBaseline[i];
		break;
	}

	case BL_REFERENCE: {
		if ((int)refSpectrum.size() != width)
			break;
		for (int i = 0; i < width; i++) {
			if (refSpectrum[i] > 1e-30f)
				spectrum[i] /= refSpectrum[i];
		}
		break;
	}

	default:
		break;
	}
}
