#pragma once

#include <string>
#include <map>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <deque>
#include <obs-module.h>
#include <media-io/audio-resampler.h>

#include "cloud-translation/translation-cloud.h"

// Fallback only. The real rate comes from the selected provider (CloudProvider::sampleRate)
// and the resampler is rebuilt whenever that changes.
#define DEFAULT_TRANSCRIPTION_SAMPLE_RATE 24000

// Audio packet info
struct cloudvocal_audio_info {
	uint32_t frames;
	uint64_t timestamp_offset_ns; // offset (since start of processing) timestamp in ns
};

enum DetectionResult {
	DETECTION_RESULT_UNKNOWN = 0,
	DETECTION_RESULT_SPEECH = 1,
	DETECTION_RESULT_SILENCE = 2,
	DETECTION_RESULT_PARTIAL = 3
};

struct DetectionResultWithText {
	uint64_t start_timestamp_ms;
	uint64_t end_timestamp_ms;
	std::string text;
	std::string language;
	enum DetectionResult result;
};

class CloudProvider;

struct TimedMetadataConfig {
	std::string aws_access_key;
	std::string aws_secret_key;
	std::string ivs_channel_arn;
	std::string aws_region;
};

struct cloudvocal_data {
	int log_level;
	bool active;
	bool initial_creation;
	bool source_signals_set;
	obs_source_t *context;
	// Toggles the filter (and therefore the billed connection) without hunting
	// through the Filters dialog of an audio source.
	obs_hotkey_id toggle_hotkey;

	size_t channels;
	int sample_rate;
	std::deque<float> input_buffers[8];
	std::deque<cloudvocal_audio_info> info_buffer;
	std::deque<float> resampled_buffer;
	uint32_t last_num_frames;

	// File output options
	bool save_only_while_recording;
	bool truncate_output_file;
	bool save_srt;
	bool save_to_file;
	std::string output_file_path;
	uint64_t sentence_number;
	uint64_t start_timestamp_ms;
	bool rename_file_to_match_recording;

	// Transcription options
	std::string language;
	std::string text_source_name;
	bool caption_to_stream;
	bool process_while_muted;
	uint64_t last_sub_render_time;
	bool cleared_last_sub;
	std::string last_transcription_sentence;
	// One-shot guard so a missing caption target warns once, not per caption.
	bool warned_missing_text_source;
	// How many lines the caption source shows: the in-progress line plus this many
	// finished ones above it, so the end of a sentence stays readable while the next
	// one is still being spoken.
	int caption_lines;
	std::deque<std::string> caption_history;
	audio_resampler_t *resampler;
	int min_sub_duration;
	int max_sub_duration;
	bool log_words;
	// smart pointer to the cloud provider
	std::shared_ptr<CloudProvider> cloud_provider;
	std::string cloud_provider_selection;
	std::string cloud_provider_api_key;
	// Rate the resampler currently targets; kept in sync with the active provider.
	int transcription_sample_rate;

	// OpenAI gpt-live-transcribe options
	std::string openai_delay;    // minimal | low | medium | high | xhigh
	std::string openai_prompt;   // free-form description of the recording
	std::string openai_keywords; // domain terms, one per line
	int openai_idle_timeout_sec; // drop the billed socket after this much silence

	std::map<std::string, std::string> filter_words_replace;

	// Translation options
	bool translate_only_full_sentences;
	bool translate;
	std::string translation_output;
	std::string target_lang;
	std::string last_text_for_translation;
	std::string last_text_translation;
	CloudTranslatorConfig translate_cloud_config;

	// Timed metadata options
	bool send_timed_metadata;
	TimedMetadataConfig timed_metadata_config;

	std::mutex input_buffers_mutex;
	std::condition_variable input_buffers_cv;
	std::mutex resampled_buffer_mutex;
};
