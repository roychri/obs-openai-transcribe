#pragma once

#include <string>

#include <obs-frontend-api.h>

#include "cloudvocal-data.h"

void send_caption_to_source(const std::string &target_source_name, const std::string &str_copy,
			    struct cloudvocal_data *gf);
std::string send_sentence_to_translation(const std::string &sentence, struct cloudvocal_data *gf);

void audio_chunk_callback(struct cloudvocal_data *gf, const float *pcm32f_data, size_t frames, int vad_state,
			  const DetectionResultWithText &result);

void set_text_callback(struct cloudvocal_data *gf, const DetectionResultWithText &resultIn);

void clear_current_caption(cloudvocal_data *gf_);

void recording_state_callback(enum obs_frontend_event event, void *data);

void media_play_callback(void *data_, calldata_t *cd);
void media_started_callback(void *data_, calldata_t *cd);
void media_pause_callback(void *data_, calldata_t *cd);
void media_restart_callback(void *data_, calldata_t *cd);
void media_stopped_callback(void *data_, calldata_t *cd);
void enable_callback(void *data_, calldata_t *cd);
