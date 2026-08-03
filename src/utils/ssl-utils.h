#pragma once

#include <string>

void init_openssl();
std::string hmacSha256(const std::string &key, const std::string &data, bool isHexKey = false);
std::string sha256(const std::string &data);
std::string getCurrentTimestamp();
std::string getCurrentDate();
std::string PEMrootCerts();
std::string PEMrootCertsPath();
