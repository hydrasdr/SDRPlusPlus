/*
 * Copyright (C) 2026 Benjamin Vernoux <bvernoux@hydrasdr.com>
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <vector>
#include <mutex>

enum BaselineMode {
	BL_POLYNOMIAL = 0,
	BL_REFERENCE
};

class BaselineManager {
public:
	BaselineManager();
	~BaselineManager();

	void reset();
	void setMode(BaselineMode mode);
	void setPolyOrder(int order);

	void captureReference(const float *spectrum, int width);

	/* Subtract baseline from spectrum in-place */
	void subtract(float *spectrum, int width);

	bool hasReference() const { return !refSpectrum.empty(); }

private:
	void polyFit(const float *data, int width, float *baseline);

	BaselineMode mode = BL_POLYNOMIAL;
	int polyOrder = 3;

	std::vector<float> refSpectrum;
	std::vector<float> polyBaseline;

	std::mutex mtx;
};
