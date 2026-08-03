#include "curl-helper.h"
#include <mutex>
#include <memory>

bool CurlHelper::is_initialized_ = false;
std::mutex CurlHelper::curl_mutex_;

CurlHelper::CurlHelper()
{
	std::lock_guard<std::mutex> lock(curl_mutex_);
	if (!is_initialized_) {
		if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
			throw std::runtime_error("Failed to initialize CURL");
		}
		is_initialized_ = true;
	}
}

CurlHelper::~CurlHelper()
{
	// Don't call curl_global_cleanup() in destructor
	// Let it clean up when the program exits
}

size_t CurlHelper::WriteCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
	if (!userp) {
		return 0;
	}

	size_t realsize = size * nmemb;
	auto *str = static_cast<std::string *>(userp);
	try {
		str->append(static_cast<char *>(contents), realsize);
		return realsize;
	} catch (const std::exception &) {
		return 0; // Return 0 to indicate error to libcurl
	}
}

std::string CurlHelper::urlEncode(const std::string &value)
{
	std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);

	std::unique_ptr<char, decltype(&curl_free)> escaped(
		curl_easy_escape(curl.get(), value.c_str(), (int)value.length()), curl_free);

	if (!escaped) {
		throw std::runtime_error("Failed to URL encode string");
	}

	return std::string(escaped.get());
}

struct curl_slist *CurlHelper::createBasicHeaders(const std::string &content_type)
{
	struct curl_slist *headers = nullptr;

	try {
		headers = curl_slist_append(headers, ("Content-Type: " + content_type).c_str());

		if (!headers) {
			throw std::runtime_error("Failed to create HTTP headers");
		}

		return headers;
	} catch (...) {
		if (headers) {
			curl_slist_free_all(headers);
		}
		throw;
	}
}

void CurlHelper::setSSLVerification(CURL *curl, bool verify)
{
	if (!curl) {
		throw std::runtime_error("Invalid CURL handle for SSL configuration");
	}

	long verify_peer = verify ? 1L : 0L;
	long verify_host = verify ? 2L : 0L;

	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verify_peer);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verify_host);
}
