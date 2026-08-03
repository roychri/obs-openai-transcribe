#ifndef TIMED_METADATA_UTILS_H
#define TIMED_METADATA_UTILS_H

#include <string>
#include <vector>

#include "cloudvocal-data.h"

enum Translation_Mode { ONLY_TARGET, SOURCE_AND_TARGET, ONLY_SOURCE };

void send_timed_metadata_to_server(struct cloudvocal_data *gf, Translation_Mode mode, const std::string &source_text,
				   const std::string &source_lang, const std::string &target_text,
				   const std::string &target_lang);

#endif // TIMED_METADATA_UTILS_H
