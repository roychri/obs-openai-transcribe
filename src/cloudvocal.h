#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MT_ obs_module_text

void cloudvocal_activate(void *data);
void *cloudvocal_create(obs_data_t *settings, obs_source_t *filter);
void cloudvocal_update(void *data, obs_data_t *s);
void cloudvocal_destroy(void *data);
const char *cloudvocal_name(void *unused);
struct obs_audio_data *cloudvocal_filter_audio(void *data, struct obs_audio_data *audio);
void cloudvocal_deactivate(void *data);
void cloudvocal_defaults(obs_data_t *s);
obs_properties_t *cloudvocal_properties(void *data);
void cloudvocal_remove(void *data, obs_source_t *source);
void cloudvocal_show(void *data);
void cloudvocal_hide(void *data);

const char *const PLUGIN_INFO_TEMPLATE =
	"<a href=\"https://github.com/roychri/obs-openai-transcribe/\">OpenAI Live Transcription</a> "
	"({{plugin_version}}) - a fork of "
	"<a href=\"https://github.com/locaal-ai/cloudvocal\">CloudVocal</a> by "
	"<a href=\"https://github.com/locaal-ai\">Locaal AI</a>";

#ifdef __cplusplus
}
#endif
