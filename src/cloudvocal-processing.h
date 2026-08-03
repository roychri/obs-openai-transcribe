#pragma once

#include "cloudvocal-data.h"

/**
 * @brief Extracts audio data from the buffer, resamples it, and updates timestamp offsets.
 *
 * This function extracts audio data from the input buffer, resamples it to the rate the
 * active provider requires, and updates gf->resampled_buffer with the resampled data.
 *
 * @param gf Pointer to the transcription filter data structure.
 * @param start_timestamp_offset_ns Reference to the start timestamp offset in nanoseconds.
 * @param end_timestamp_offset_ns Reference to the end timestamp offset in nanoseconds.
 * @return Returns 0 on success, 1 if the input buffer is empty.
 */
int get_data_from_buf_and_resample(cloudvocal_data *gf, uint64_t &start_timestamp_offset_ns,
				   uint64_t &end_timestamp_offset_ns);

/**
 * @brief Ensures the resampler targets @p target_sample_rate, rebuilding it if needed.
 *
 * Providers differ in the rate they accept, so the resampler cannot be built once at
 * filter creation. Safe to call repeatedly; a no-op when the rate already matches.
 *
 * @return true if a usable resampler is in place.
 */
bool ensure_resampler(cloudvocal_data *gf, int target_sample_rate);
