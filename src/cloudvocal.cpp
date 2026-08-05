#include <obs-module.h>
#include <obs-frontend-api.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <bitset>
#include <regex>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

#include "plugin-support.h"
#include "cloudvocal.h"
#include "cloudvocal-data.h"
#include "cloudvocal-callbacks.h"
#include "cloudvocal-utils.h"
#include "cloudvocal-processing.h"
#include "cloud-providers/cloud-provider.h"

void set_source_signals(cloudvocal_data *gf, obs_source_t *parent_source)
{
	signal_handler_t *sh = obs_source_get_signal_handler(parent_source);
	signal_handler_connect(sh, "media_play", media_play_callback, gf);
	signal_handler_connect(sh, "media_started", media_started_callback, gf);
	signal_handler_connect(sh, "media_pause", media_pause_callback, gf);
	signal_handler_connect(sh, "media_restart", media_restart_callback, gf);
	signal_handler_connect(sh, "media_stopped", media_stopped_callback, gf);
	gf->source_signals_set = true;
}

void disconnect_source_signals(cloudvocal_data *gf, obs_source_t *parent_source)
{
	signal_handler_t *sh = obs_source_get_signal_handler(parent_source);
	signal_handler_disconnect(sh, "media_play", media_play_callback, gf);
	signal_handler_disconnect(sh, "media_started", media_started_callback, gf);
	signal_handler_disconnect(sh, "media_pause", media_pause_callback, gf);
	signal_handler_disconnect(sh, "media_restart", media_restart_callback, gf);
	signal_handler_disconnect(sh, "media_stopped", media_stopped_callback, gf);
	gf->source_signals_set = false;
}

struct obs_audio_data *cloudvocal_filter_audio(void *data, struct obs_audio_data *audio)
{
	if (!audio) {
		return nullptr;
	}

	if (data == nullptr) {
		return audio;
	}

	struct cloudvocal_data *gf = static_cast<struct cloudvocal_data *>(data);

	// Lazy initialization of source signals
	if (!gf->source_signals_set) {
		// obs_filter_get_parent only works in the filter function
		obs_source_t *parent_source = obs_filter_get_parent(gf->context);
		if (parent_source != nullptr) {
			set_source_signals(gf, parent_source);
		}
	}

	if (!gf->active) {
		return audio;
	}

	// Check if process while muted is not enabled (e.g. the user wants to avoid processing audio
	// when the source is muted)
	if (!gf->process_while_muted) {
		// Check if the parent source is muted
		obs_source_t *parent_source = obs_filter_get_parent(gf->context);
		if (parent_source != nullptr && obs_source_muted(parent_source)) {
			// Source is muted, do not process audio
			return audio;
		}
	}

	if (gf->cloud_provider != nullptr && gf->cloud_provider->isRunning()) {
		std::lock_guard<std::mutex> lock(gf->input_buffers_mutex); // scoped lock
		// audio->data[c] holds uint8_t data but it's actually float data
		// so we need to convert it to a float data pointer
		float *audio_data_f32[8];
		for (size_t c = 0; c < gf->channels; c++) {
			audio_data_f32[c] = (float *)audio->data[c];
		}
		// push back current audio data to input circlebuf
		for (size_t c = 0; c < gf->channels; c++) {
			gf->input_buffers[c].insert(gf->input_buffers[c].end(), audio_data_f32[c],
						    audio_data_f32[c] + audio->frames);
		}
		// push audio packet info (timestamp/frame count) to info circlebuf
		struct cloudvocal_audio_info info = {0};
		info.frames = audio->frames; // number of frames in this packet
		// calculate timestamp offset from the start of the stream
		info.timestamp_offset_ns = now_ns() - gf->start_timestamp_ms * 1000000;
		gf->info_buffer.push_back(info);
		gf->input_buffers_cv.notify_one();
	}

	return audio;
}

const char *cloudvocal_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return MT_("cloudvocalAudioFilter");
}

void cloudvocal_remove(void *data, obs_source_t *source)
{
	struct cloudvocal_data *gf = static_cast<struct cloudvocal_data *>(data);

	obs_log(gf->log_level, "filter remove");

	disconnect_source_signals(gf, source);
}

void cloudvocal_destroy(void *data)
{
	struct cloudvocal_data *gf = static_cast<struct cloudvocal_data *>(data);
	gf->active = false;

	signal_handler_t *sh_filter = obs_source_get_signal_handler(gf->context);
	signal_handler_disconnect(sh_filter, "enable", enable_callback, gf);

	obs_log(gf->log_level, "filter destroy");

	if (gf->cloud_provider != nullptr) {
		gf->cloud_provider->stop();
		gf->cloud_provider = nullptr;
	}

	if (gf->resampler) {
		audio_resampler_destroy(gf->resampler);
	}

	{
		std::lock_guard<std::mutex> lockbuf(gf->input_buffers_mutex);
		for (size_t i = 0; i < gf->channels; i++) {
			gf->input_buffers[i].clear();
		}
	}
	gf->info_buffer.clear();
	gf->resampled_buffer.clear();
	gf->context = nullptr;

	bfree(gf);
}

void cloudvocal_update(void *data, obs_data_t *s)
{
	struct cloudvocal_data *gf = static_cast<struct cloudvocal_data *>(data);
	obs_log(gf->log_level, "cloudvocal filter update");

	gf->log_level = (int)obs_data_get_int(s, "log_level");
	gf->log_words = obs_data_get_bool(s, "log_words");
	gf->caption_to_stream = obs_data_get_bool(s, "caption_to_stream");
	gf->save_to_file = obs_data_get_bool(s, "file_output_enable");
	gf->save_srt = obs_data_get_bool(s, "subtitle_save_srt");
	gf->truncate_output_file = obs_data_get_bool(s, "truncate_output_file");
	gf->save_only_while_recording = obs_data_get_bool(s, "only_while_recording");
	gf->rename_file_to_match_recording = obs_data_get_bool(s, "rename_file_to_match_recording");
	// Get the current timestamp using the system clock
	gf->start_timestamp_ms = now_ms();
	gf->sentence_number = 1;
	gf->process_while_muted = obs_data_get_bool(s, "process_while_muted");
	gf->min_sub_duration = (int)obs_data_get_int(s, "min_sub_duration");
	gf->max_sub_duration = (int)obs_data_get_int(s, "max_sub_duration");
	gf->last_sub_render_time = now_ms();
	const char *filter_words_replace = obs_data_get_string(s, "filter_words_replace");
	if (filter_words_replace != nullptr && strlen(filter_words_replace) > 0) {
		obs_log(gf->log_level, "filter_words_replace: %s", filter_words_replace);
		// deserialize the filter words replace
		// gf->filter_words_replace = deserialize_filter_words_replace(filter_words_replace);
	} else {
		// clear the filter words replace
		gf->filter_words_replace.clear();
	}

	if (gf->save_to_file) {
		gf->output_file_path = "";
		// set the output file path
		const char *output_file_path = obs_data_get_string(s, "subtitle_output_filename");
		if (output_file_path != nullptr && strlen(output_file_path) > 0) {
			gf->output_file_path = output_file_path;
		} else {
			obs_log(gf->log_level, "output file path is empty, but selected to save");
		}
	}

	gf->translate = obs_data_get_bool(s, "translate_cloud");
	gf->translate_cloud_config.provider = obs_data_get_string(s, "translate_cloud_provider");
	gf->target_lang = obs_data_get_string(s, "translate_cloud_target_language");
	gf->translation_output = obs_data_get_string(s, "translate_cloud_output");
	gf->translate_only_full_sentences = obs_data_get_bool(s, "translate_cloud_only_full_sentences");
	gf->translate_cloud_config.access_key = obs_data_get_string(s, "translate_cloud_api_key");
	gf->translate_cloud_config.secret_key = obs_data_get_string(s, "translate_cloud_secret_key");
	gf->translate_cloud_config.free = obs_data_get_bool(s, "translate_cloud_deepl_free");
	gf->translate_cloud_config.region = obs_data_get_string(s, "translate_cloud_region");
	gf->translate_cloud_config.endpoint = obs_data_get_string(s, "translate_cloud_endpoint");
	gf->translate_cloud_config.body = obs_data_get_string(s, "translate_cloud_body");
	gf->translate_cloud_config.response_json_path = obs_data_get_string(s, "translate_cloud_response_json_path");

	obs_log(gf->log_level, "update text source");
	// update the text source
	const char *new_text_source_name = obs_data_get_string(s, "subtitle_sources");

	if (new_text_source_name == nullptr || strcmp(new_text_source_name, "none") == 0 ||
	    strcmp(new_text_source_name, "(null)") == 0 || strlen(new_text_source_name) == 0) {
		// new selected text source is not valid, release the old one
		gf->text_source_name.clear();
	} else {
		gf->text_source_name = new_text_source_name;
	}

	const char *transcription_language_select = obs_data_get_string(s, "transcription_language_select");
	std::string new_language = transcription_language_select;
	std::string new_cloud_provider_api_key = obs_data_get_string(s, "transcription_cloud_provider_api_key");
	std::string new_cloud_provider_selection = obs_data_get_string(s, "transcription_cloud_provider");

	// OpenAI context options. Pushed to the live session rather than reconnecting.
	std::string new_openai_delay = obs_data_get_string(s, "openai_delay");
	std::string new_openai_prompt = obs_data_get_string(s, "openai_prompt");
	std::string new_openai_keywords = obs_data_get_string(s, "openai_keywords");

	// Idle timeout is read live - it does not need a reconnect.
	gf->openai_idle_timeout_sec = (int)obs_data_get_int(s, "openai_idle_timeout");

	// OBS calls update() on every keystroke. Restarting the provider here joins two
	// threads and tears down a socket, which froze the UI one letter at a time while
	// typing into Context. These options go to the open session instead.
	const bool session_opts_changed = gf->openai_delay != new_openai_delay ||
					  gf->openai_prompt != new_openai_prompt ||
					  gf->openai_keywords != new_openai_keywords;
	gf->openai_delay = new_openai_delay;
	gf->openai_prompt = new_openai_prompt;
	gf->openai_keywords = new_openai_keywords;

	// A different provider, language or credential cannot be applied to an open
	// socket, so those still reconnect.
	const bool reconnect_needed = gf->cloud_provider_selection != new_cloud_provider_selection ||
				      gf->language != new_language ||
				      gf->cloud_provider_api_key != new_cloud_provider_api_key;

	if (reconnect_needed) {
		obs_log(gf->log_level, "provider, language or key changed - restarting");
		gf->cloud_provider_selection = new_cloud_provider_selection;
		gf->language = new_language;
		gf->cloud_provider_api_key = new_cloud_provider_api_key;

		restart_cloud_provider(gf);
	} else if (session_opts_changed && gf->cloud_provider != nullptr) {
		gf->cloud_provider->onConfigChanged();
	}

	// Update timed metadata options
	gf->send_timed_metadata = obs_data_get_bool(s, "timed_metadata_group");
	gf->timed_metadata_config.aws_access_key = obs_data_get_string(s, "timed_metadata_aws_access_key");
	gf->timed_metadata_config.aws_secret_key = obs_data_get_string(s, "timed_metadata_aws_secret_key");
	gf->timed_metadata_config.ivs_channel_arn = obs_data_get_string(s, "timed_metadata_channel_arn");
	gf->timed_metadata_config.aws_region = obs_data_get_string(s, "timed_metadata_aws_region");

	if (gf->context != nullptr && (obs_source_enabled(gf->context) || gf->initial_creation)) {
		if (gf->initial_creation) {
			obs_log(LOG_INFO, "Initial filter creation and source enabled");

			// source was enabled on creation
			gf->active = true;
			gf->initial_creation = false;

			restart_cloud_provider(gf);
		}
	} else {
		obs_log(LOG_INFO, "Filter not enabled.");
	}
}

void *cloudvocal_create(obs_data_t *settings, obs_source_t *filter)
{
	obs_log(LOG_INFO, "cloudvocal filter create");

	void *data = bmalloc(sizeof(struct cloudvocal_data));
	struct cloudvocal_data *gf = new (data) cloudvocal_data();

	// Get the number of channels for the input source
	gf->channels = audio_output_get_channels(obs_get_audio());
	gf->sample_rate = audio_output_get_sample_rate(obs_get_audio());
	gf->min_sub_duration = (int)obs_data_get_int(settings, "min_sub_duration");
	gf->max_sub_duration = (int)obs_data_get_int(settings, "max_sub_duration");
	gf->last_sub_render_time = now_ms();
	gf->log_level = (int)obs_data_get_int(settings, "log_level");
	gf->save_srt = obs_data_get_bool(settings, "subtitle_save_srt");
	gf->truncate_output_file = obs_data_get_bool(settings, "truncate_output_file");
	gf->save_only_while_recording = obs_data_get_bool(settings, "only_while_recording");
	gf->rename_file_to_match_recording = obs_data_get_bool(settings, "rename_file_to_match_recording");
	gf->process_while_muted = obs_data_get_bool(settings, "process_while_muted");
	gf->initial_creation = true;

	for (size_t i = 0; i < gf->channels; i++) {
		gf->input_buffers[i].clear();
	}
	gf->info_buffer.clear();
	gf->resampled_buffer.clear();
	gf->context = filter;

	obs_log(gf->log_level, "channels %d, sample_rate %d", (int)gf->channels, gf->sample_rate);

	obs_log(gf->log_level, "setup audio resampler");
	// Provisional rate only - restart_cloud_provider() rebuilds this to whatever the
	// selected provider actually wants.
	if (!ensure_resampler(gf, DEFAULT_TRANSCRIPTION_SAMPLE_RATE)) {
		gf->active = false;
		return nullptr;
	}

	obs_log(gf->log_level, "clear text source data");
	const char *subtitle_sources = obs_data_get_string(settings, "subtitle_sources");
	if (subtitle_sources == nullptr || strlen(subtitle_sources) == 0 || strcmp(subtitle_sources, "none") == 0 ||
	    strcmp(subtitle_sources, "(null)") == 0) {
		obs_log(gf->log_level, "Create text source");
		create_obs_text_source_if_needed();
		gf->text_source_name = CAPTIONS_TEXT_SOURCE_NAME;
		obs_data_set_string(settings, "subtitle_sources", CAPTIONS_TEXT_SOURCE_NAME);
	} else {
		// set the text source name
		gf->text_source_name = subtitle_sources;
	}
	gf->output_file_path = std::string("");

	signal_handler_t *sh_filter = obs_source_get_signal_handler(gf->context);
	if (sh_filter == nullptr) {
		obs_log(LOG_ERROR, "Failed to get signal handler");
		gf->active = false;
		return nullptr;
	}

	signal_handler_connect(sh_filter, "enable", enable_callback, gf);

	obs_log(gf->log_level, "run update");
	// get the settings updated on the filter data struct
	cloudvocal_update(gf, settings);

	// handle the event OBS_FRONTEND_EVENT_RECORDING_STARTING to reset the srt sentence number
	// to match the subtitles with the recording
	obs_frontend_add_event_callback(recording_state_callback, gf);

	clear_current_caption(gf);

	obs_log(gf->log_level, "filter created.");
	return gf;
}

void cloudvocal_activate(void *data)
{
	struct cloudvocal_data *gf = static_cast<struct cloudvocal_data *>(data);
	obs_log(gf->log_level, "filter activated");
	gf->active = true;
}

void cloudvocal_deactivate(void *data)
{
	struct cloudvocal_data *gf = static_cast<struct cloudvocal_data *>(data);
	obs_log(gf->log_level, "filter deactivated");
	gf->active = false;
}

void cloudvocal_show(void *data)
{
	struct cloudvocal_data *gf = static_cast<struct cloudvocal_data *>(data);
	obs_log(gf->log_level, "filter show");
}

void cloudvocal_hide(void *data)
{
	struct cloudvocal_data *gf = static_cast<struct cloudvocal_data *>(data);
	obs_log(gf->log_level, "filter hide");
}
