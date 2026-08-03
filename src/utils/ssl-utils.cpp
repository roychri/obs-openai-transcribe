#include "ssl-utils.h"

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/err.h>

#include <vector>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <fstream>

#include <util/bmem.h>

#include "plugin-support.h"
#include <obs-module.h>

void init_openssl()
{
	OpenSSL_add_all_algorithms();
	ERR_load_crypto_strings();
}

// HMAC SHA-256 function
std::string hmacSha256(const std::string &key, const std::string &data, bool isHexKey)
{
	unsigned char *digest = (unsigned char *)bzalloc(EVP_MAX_MD_SIZE);
	size_t len = EVP_MAX_MD_SIZE;

	// Prepare the key
	std::vector<unsigned char> keyBytes;
	if (isHexKey) {
		for (size_t i = 0; i < key.length(); i += 2) {
			std::string byteString = key.substr(i, 2);
			unsigned char byte = (unsigned char)strtol(byteString.c_str(), NULL, 16);
			keyBytes.push_back(byte);
		}
	} else {
		keyBytes.assign(key.begin(), key.end());
	}

	// HMAC takes the key length as int; keyBytes is small (a hex or raw API key).
	if (!HMAC(EVP_sha256(), keyBytes.data(), (int)keyBytes.size(), (unsigned char *)data.c_str(), data.length(),
		  digest, (unsigned int *)&len)) {
		obs_log(LOG_ERROR, "hmacSha256 failed during HMAC operation");
		return {};
	}

	std::stringstream ss;
	for (size_t i = 0; i < len; ++i) {
		ss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
	}

	bfree(digest);
	return ss.str();
}

std::string sha256(const std::string &data)
{
	unsigned char hash[EVP_MAX_MD_SIZE];
	unsigned int lengthOfHash = 0;

	EVP_MD_CTX *context = EVP_MD_CTX_new();

	if (context != nullptr) {
		if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr)) {
			if (EVP_DigestUpdate(context, data.c_str(), data.length())) {
				if (EVP_DigestFinal_ex(context, hash, &lengthOfHash)) {
					EVP_MD_CTX_free(context);

					std::stringstream ss;
					for (unsigned int i = 0; i < lengthOfHash; ++i) {
						ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
					}
					return ss.str();
				}
			}
		}
		EVP_MD_CTX_free(context);
	}

	return "";
}

std::string getCurrentTimestamp()
{
	auto now = std::chrono::system_clock::now();
	auto in_time_t = std::chrono::system_clock::to_time_t(now);
	std::stringstream ss;
	ss << std::put_time(std::gmtime(&in_time_t), "%Y%m%dT%H%M%SZ");
	return ss.str();
}

std::string getCurrentDate()
{
	auto now = std::chrono::system_clock::now();
	auto in_time_t = std::chrono::system_clock::to_time_t(now);
	std::stringstream ss;
	ss << std::put_time(std::gmtime(&in_time_t), "%Y%m%d");
	return ss.str();
}

std::string PEMrootCerts()
{
	// Read the contents of the root CA file bundled with this plugin module
	char *root_cert_file_path = obs_module_file("roots.pem");
	if (root_cert_file_path == nullptr) {
		obs_log(LOG_ERROR, "Failed to get root certificate file path");
		return "";
	}
	std::ifstream root_cert_file(root_cert_file_path);
	if (!root_cert_file.is_open()) {
		obs_log(LOG_ERROR, "Failed to open certificate file");
		return "";
	}

	std::stringstream cert_stream;
	cert_stream << root_cert_file.rdbuf();
	root_cert_file.close();
	return cert_stream.str();
}

std::string PEMrootCertsPath()
{
	char *root_cert_file_path = obs_module_file("roots.pem");
	if (root_cert_file_path == nullptr) {
		obs_log(LOG_ERROR, "Failed to get root certificate file path");
		return "";
	}

	return std::string(root_cert_file_path);
}
