
#include "cloudvocal-processing.h"
#include "cloudvocal-data.h"
// convert_speaker_layout(), used by ensure_resampler()
#include "cloudvocal-utils.h"

#include <obs-module.h>
#include <obs.h>

#include <vector>
#include "plugin-support.h"

bool ensure_resampler(cloudvocal_data *gf, int target_sample_rate)
{
	if (gf->resampler != nullptr && gf->transcription_sample_rate == target_sample_rate) {
		return true;
	}

	if (gf->resampler != nullptr) {
		obs_log(gf->log_level, "rebuilding resampler for %d Hz (was %d Hz)", target_sample_rate,
			gf->transcription_sample_rate);
		audio_resampler_destroy(gf->resampler);
		gf->resampler = nullptr;
	}

	struct resample_info src, dst;
	src.samples_per_sec = gf->sample_rate;
	src.format = AUDIO_FORMAT_FLOAT_PLANAR;
	src.speakers = convert_speaker_layout((uint8_t)gf->channels);

	dst.samples_per_sec = target_sample_rate;
	dst.format = AUDIO_FORMAT_FLOAT_PLANAR;
	dst.speakers = convert_speaker_layout((uint8_t)1);

	gf->resampler = audio_resampler_create(&dst, &src);
	if (gf->resampler == nullptr) {
		obs_log(LOG_ERROR, "Failed to create resampler for %d Hz", target_sample_rate);
		return false;
	}

	gf->transcription_sample_rate = target_sample_rate;
	return true;
}

int get_data_from_buf_and_resample(cloudvocal_data *gf, uint64_t &start_timestamp_offset_ns,
				   uint64_t &end_timestamp_offset_ns)
{
	uint32_t num_frames_from_infos = 0;

	// copy buffers
	std::vector<float> copy_buffers[8];

	{
		// scoped lock the buffer mutex
		std::lock_guard<std::mutex> lock(gf->input_buffers_mutex);

		if (gf->input_buffers[0].empty()) {
			return 1;
		}

#ifdef CLOUDVOCAL_EXTRA_VERBOSE
		obs_log(gf->log_level, "segmentation: currently %lu bytes in the audio input buffer",
			gf->input_buffers[0].size);
#endif

		// max number of frames is 10 seconds worth of audio
		const size_t max_num_frames = gf->sample_rate * 10;

		// pop all infos from the info buffer and mark the beginning timestamp from the first
		// info as the beginning timestamp of the segment
		struct cloudvocal_audio_info info_from_buf = {0};
		while (gf->info_buffer.size() > 0) {
			info_from_buf = gf->info_buffer.front();
			num_frames_from_infos += info_from_buf.frames;
			if (start_timestamp_offset_ns == 0) {
				start_timestamp_offset_ns = info_from_buf.timestamp_offset_ns;
			}
			// Check if we're within the needed segment length
			if (num_frames_from_infos > max_num_frames) {
				// too big, break out of the loop
				num_frames_from_infos -= info_from_buf.frames;
				break;
			} else {
				// pop the info from the info buffer
				gf->info_buffer.pop_front();
			}
		}
		// calculate the end timestamp from the last info plus the number of frames in the packet
		end_timestamp_offset_ns =
			info_from_buf.timestamp_offset_ns + info_from_buf.frames * 1000000000 / gf->sample_rate;

		if (start_timestamp_offset_ns > end_timestamp_offset_ns) {
			// this may happen when the incoming media has a timestamp reset
			// in this case, we should figure out the start timestamp from the end timestamp
			// and the number of frames
			start_timestamp_offset_ns =
				end_timestamp_offset_ns - num_frames_from_infos * 1000000000 / gf->sample_rate;
		}

		/* Pop from input circlebuf */
		for (size_t c = 0; c < gf->channels; c++) {
			// Push the new data to copy_buffers[c]
			copy_buffers[c].resize(num_frames_from_infos);
			std::copy(gf->input_buffers[c].begin(), gf->input_buffers[c].begin() + num_frames_from_infos,
				  copy_buffers[c].begin());
			// Pop the data from the input buffer
			gf->input_buffers[c].erase(gf->input_buffers[c].begin(),
						   gf->input_buffers[c].begin() + num_frames_from_infos);
		}
	}

#ifdef CLOUDVOCAL_EXTRA_VERBOSE
	obs_log(gf->log_level, "found %d frames from info buffer.", num_frames_from_infos);
#endif
	gf->last_num_frames = num_frames_from_infos;

	if (num_frames_from_infos <= 0 || copy_buffers[0].empty()) {
		obs_log(LOG_ERROR, "No audio data found in the input buffer");
		return 1;
	}

	if (gf->resampler == nullptr) {
		obs_log(LOG_ERROR, "Resampler is not initialized");
		return 1;
	}

	{
		// resample to the provider's rate
		float *resampled_16khz[8];
		uint32_t resampled_16khz_frames;
		uint64_t ts_offset;
		uint8_t *copy_buffers_8[8];
		for (size_t c = 0; c < gf->channels; c++) {
			copy_buffers_8[c] = (uint8_t *)copy_buffers[c].data();
		}
		bool success = audio_resampler_resample(gf->resampler, (uint8_t **)resampled_16khz,
							&resampled_16khz_frames, &ts_offset,
							(const uint8_t **)copy_buffers_8,
							(uint32_t)num_frames_from_infos);

		if (!success) {
			obs_log(LOG_ERROR, "Failed to resample audio data");
			return 1;
		}

		// push back resampled data to resampled buffer
		gf->resampled_buffer.insert(gf->resampled_buffer.end(), resampled_16khz[0],
					    resampled_16khz[0] + resampled_16khz_frames);
#ifdef CLOUDVOCAL_EXTRA_VERBOSE
		obs_log(gf->log_level, "resampled: %d channels, %d frames, %f ms, current size: %lu bytes",
			(int)gf->channels, (int)resampled_16khz_frames,
			(float)resampled_16khz_frames / gf->transcription_sample_rate * 1000.0f,
			gf->resampled_buffer.size);
#endif
	}

	return 0;
}
