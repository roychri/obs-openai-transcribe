#include "openai-provider.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <vector>

#include "cloudvocal-utils.h"
#include "language-codes/language-codes.h"

using json = nlohmann::json;
namespace http = beast::http;

static const char *kHost = "api.openai.com";
static const char *kPort = "443";
static const char *kTarget = "/v1/realtime?intent=transcription";
static const char *kModel = "gpt-live-transcribe";

namespace {

const char kB64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const uint8_t *data, size_t len)
{
	std::string out;
	out.reserve(((len + 2) / 3) * 4);

	size_t i = 0;
	for (; i + 2 < len; i += 3) {
		const uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) |
				   uint32_t(data[i + 2]);
		out.push_back(kB64Alphabet[(n >> 18) & 0x3F]);
		out.push_back(kB64Alphabet[(n >> 12) & 0x3F]);
		out.push_back(kB64Alphabet[(n >> 6) & 0x3F]);
		out.push_back(kB64Alphabet[n & 0x3F]);
	}

	if (i < len) {
		uint32_t n = uint32_t(data[i]) << 16;
		const bool haveTwo = (i + 1 < len);
		if (haveTwo) {
			n |= uint32_t(data[i + 1]) << 8;
		}
		out.push_back(kB64Alphabet[(n >> 18) & 0x3F]);
		out.push_back(kB64Alphabet[(n >> 12) & 0x3F]);
		out.push_back(haveTwo ? kB64Alphabet[(n >> 6) & 0x3F] : '=');
		out.push_back('=');
	}

	return out;
}

// Split a free-text keywords box (one per line) into a JSON array.
json keywordsToJson(const std::string &raw)
{
	json arr = json::array();
	std::istringstream stream(raw);
	std::string line;
	while (std::getline(stream, line)) {
		// strip trailing CR from Windows-entered text and surrounding spaces
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
			line.pop_back();
		}
		size_t start = line.find_first_not_of(' ');
		if (start == std::string::npos) {
			continue;
		}
		arr.push_back(line.substr(start));
	}
	return arr;
}

} // namespace

OpenAIProvider::OpenAIProvider(TranscriptionCallback callback, cloudvocal_data *gf_)
	: CloudProvider(callback, gf_),
	  ioc(),
	  ssl_ctx(ssl::context::tlsv12_client),
	  resolver(ioc),
	  ws(nullptr),
	  connected(false),
	  last_audio_sent(std::chrono::steady_clock::now())
{
	needs_results_thread = true;
}

OpenAIProvider::~OpenAIProvider()
{
	stop();
}

bool OpenAIProvider::init()
{
	if (gf->cloud_provider_api_key.empty()) {
		obs_log(LOG_ERROR, "OpenAI API key is empty - set it in the filter settings");
		return false;
	}

	ssl_ctx.set_verify_mode(ssl::verify_peer);
	ssl_ctx.set_default_verify_paths();

	// Deliberately do not connect here. The socket is opened on first audio so an
	// enabled-but-silent filter does not bill.
	obs_log(LOG_INFO, "OpenAI provider ready (model %s, %d Hz), connecting on first audio",
		kModel, kSampleRate);
	return true;
}

bool OpenAIProvider::connect()
{
	try {
		auto const results = resolver.resolve(kHost, kPort);

		// Build and hand-shake on a local stream, and only publish it to `ws` once
		// it is usable - the results thread must never observe a half-open stream.
		auto stream = std::make_shared<WsStream>(ioc, ssl_ctx);

		net::connect(beast::get_lowest_layer(*stream), results);

		if (!SSL_set_tlsext_host_name(stream->next_layer().native_handle(), kHost)) {
			throw beast::system_error(
				beast::error_code(static_cast<int>(::ERR_get_error()),
						  net::error::get_ssl_category()),
				"Failed to set SNI hostname");
		}

		stream->next_layer().handshake(ssl::stream_base::client);

		const std::string api_key = gf->cloud_provider_api_key;
		stream->set_option(websocket::stream_base::decorator(
			[api_key](websocket::request_type &req) {
				// Authorization only. Sending the old `OpenAI-Beta: realtime=v1`
				// header gets the connection closed with 4000
				// invalid_request_error.beta_api_shape_disabled (verified
				// 2026-08-03).
				req.set(http::field::authorization, "Bearer " + api_key);
			}));

		stream->handshake(kHost, kTarget);
		// All frames we send are JSON text, not binary audio.
		stream->text(true);

		{
			std::lock_guard<std::mutex> lock(ws_mutex);
			ws = stream;
		}
		connected = true;
		current_item_id.clear();
		{
			std::lock_guard<std::mutex> lock(pending_mutex);
			pending_text.clear();
			last_delta = std::chrono::steady_clock::now();
		}

		if (!sendSessionUpdate()) {
			disconnect("session.update failed");
			return false;
		}

		obs_log(LOG_INFO, "Connected to OpenAI realtime transcription");
		return true;
	} catch (std::exception const &e) {
		obs_log(LOG_ERROR, "Error connecting to OpenAI: %s", e.what());
		connected = false;
		std::lock_guard<std::mutex> lock(ws_mutex);
		ws.reset();
		return false;
	}
}

std::shared_ptr<OpenAIProvider::WsStream> OpenAIProvider::currentStream()
{
	std::lock_guard<std::mutex> lock(ws_mutex);
	return ws;
}

bool OpenAIProvider::sendSessionUpdate()
{
	json transcription = {{"model", kModel}};

	if (!gf->openai_delay.empty()) {
		transcription["delay"] = gf->openai_delay;
	}
	if (!gf->openai_prompt.empty()) {
		transcription["prompt"] = gf->openai_prompt;
	}
	if (!gf->openai_keywords.empty()) {
		json kw = keywordsToJson(gf->openai_keywords);
		if (!kw.empty()) {
			transcription["keywords"] = kw;
		}
	}
	// The UI stores languages in OBS's "__en__" form; OpenAI wants a bare ISO 639-1
	// code. Empty / "auto" means let the model detect it - omit the hint entirely.
	if (!gf->language.empty() && gf->language != "auto") {
		auto it = language_codes_from_underscore.find(gf->language);
		const std::string iso = (it != language_codes_from_underscore.end()) ? it->second
										    : gf->language;
		if (!iso.empty()) {
			transcription["languages"] = json::array({iso});
		}
	}

	json session = {{"type", "session.update"},
			{"session",
			 {{"type", "transcription"},
			  {"audio",
			   {{"input",
			     {{"format", {{"type", "audio/pcm"}, {"rate", kSampleRate}}},
			      {"transcription", transcription},
			      // Verified 2026-08-03: any turn_detection object is rejected
			      // with "Turn detection is not supported for this transcription
			      // model" and the whole session.update fails, leaving the
			      // session unconfigured and silent. It must be null.
			      {"turn_detection", nullptr}}}}}}}};

	return writeFrame(session.dump());
}

bool OpenAIProvider::writeFrame(const std::string &payload)
{
	try {
		std::lock_guard<std::mutex> lock(ws_mutex);
		if (!ws || !connected) {
			return false;
		}
		ws->write(net::buffer(payload));
		return true;
	} catch (std::exception const &e) {
		obs_log(LOG_ERROR, "Error writing to OpenAI: %s", e.what());
		connected = false;
		return false;
	}
}

void OpenAIProvider::sendAudioBufferToTranscription(const std::deque<float> &audio_buffer)
{
	if (audio_buffer.empty()) {
		return;
	}

	if (!connected && !connect()) {
		// Back off a little so a persistent failure (bad key, no network) does not
		// spin the audio thread against the API.
		std::this_thread::sleep_for(std::chrono::seconds(2));
		return;
	}

	std::vector<int16_t> pcm;
	pcm.reserve(audio_buffer.size());
	for (float sample : audio_buffer) {
		const float clamped = std::max(-1.0f, std::min(1.0f, sample));
		pcm.push_back(static_cast<int16_t>(clamped * 32767.0f));
	}

	const std::string encoded =
		base64Encode(reinterpret_cast<const uint8_t *>(pcm.data()),
			     pcm.size() * sizeof(int16_t));

	json frame = {{"type", "input_audio_buffer.append"}, {"audio", encoded}};

	if (writeFrame(frame.dump())) {
		last_audio_sent = std::chrono::steady_clock::now();
	}
}

void OpenAIProvider::readResultsFromTranscription()
{
	// Hold a reference for the whole read so a concurrent teardown cannot free the
	// stream underneath us.
	auto stream = currentStream();
	if (!connected || !stream) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		return;
	}

	try {
		beast::flat_buffer buffer;
		stream->read(buffer);
		handleEvent(beast::buffers_to_string(buffer.data()));
	} catch (std::exception const &e) {
		if (connected) {
			// A close driven by onIdleTick() lands here too, but with connected
			// already false, so only log genuine failures.
			obs_log(LOG_ERROR, "Error reading from OpenAI: %s", e.what());
			connected = false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

void OpenAIProvider::handleEvent(const std::string &message)
{
	json event;
	try {
		event = json::parse(message);
	} catch (std::exception const &e) {
		obs_log(LOG_ERROR, "Malformed event from OpenAI: %s", e.what());
		return;
	}

	const std::string type = event.value("type", "");

	if (type == "conversation.item.input_audio_transcription.delta") {
		const std::string item_id = event.value("item_id", "");
		if (item_id != current_item_id) {
			// Not observed in practice - the model keeps one item for the whole
			// session - but if it ever does rotate, close out the previous line.
			flushPending();
			current_item_id = item_id;
		}
		appendDelta(event.value("delta", ""));
	} else if (type == "conversation.item.input_audio_transcription.completed") {
		// Not emitted by gpt-live-transcribe as of 2026-08. Handled anyway so the
		// provider does the right thing if that changes, or on another model.
		const std::string text = event.value("transcript", "");
		if (!text.empty()) {
			{
				std::lock_guard<std::mutex> lock(pending_mutex);
				pending_text.clear();
			}
			emit(text, true);
		} else {
			flushPending();
		}
		current_item_id.clear();
	} else if (type == "error") {
		const auto err = event.value("error", json::object());
		obs_log(LOG_ERROR, "OpenAI realtime error: %s (%s)",
			err.value("message", "unknown").c_str(), err.value("code", "").c_str());
	} else if (type == "session.updated") {
		obs_log(gf->log_level, "OpenAI session configured");
	}
}

namespace {

// Index just past the last sentence-ending punctuation in `s`, or npos.
// ASCII plus the CJK full-width stops, which are what the model emits for those
// languages; anything else falls through to the pause/length fallbacks.
// Common abbreviations whose trailing period is not a sentence end. Without this a
// caption splits after "Dr." and flashes a two-word line.
bool endsWithAbbreviation(const std::string &s, size_t period_pos)
{
	static const char *abbrevs[] = {"mr",  "mrs", "ms",  "dr",   "st",  "jr",
					"sr",  "vs",  "etc", "prof", "inc", "ltd",
					"no",  "fig", "approx"};

	size_t start = period_pos;
	while (start > 0 && (isalpha((unsigned char)s[start - 1]) != 0)) {
		start--;
	}
	std::string word = s.substr(start, period_pos - start);
	for (char &c : word) {
		c = (char)tolower((unsigned char)c);
	}

	for (const char *abbrev : abbrevs) {
		if (word == abbrev) {
			return true;
		}
	}
	return false;
}

size_t lastSentenceEnd(const std::string &s)
{
	static const char *ascii_enders = ".!?";
	// Don't cut a caption down to a fragment; below this we wait for more text.
	static const size_t kMinSentenceChars = 12;
	size_t best = std::string::npos;

	for (size_t i = 0; i < s.size(); i++) {
		if (strchr(ascii_enders, s[i]) != nullptr) {
			// Require a following space so decimals ("3.5", "1.2.3") do not
			// split mid-sentence on every digit.
			if (i + 1 < s.size() && s[i + 1] != ' ') {
				continue;
			}
			if (i + 1 < kMinSentenceChars) {
				continue;
			}
			if (s[i] == '.' && endsWithAbbreviation(s, i)) {
				continue;
			}
			best = i + 1;
		}
	}

	// U+3002 IDEOGRAPHIC FULL STOP / U+FF01 / U+FF1F in UTF-8
	static const char *wide_enders[] = {"\xE3\x80\x82", "\xEF\xBC\x81", "\xEF\xBC\x9F"};
	for (const char *ender : wide_enders) {
		size_t pos = s.rfind(ender);
		if (pos != std::string::npos) {
			const size_t end = pos + strlen(ender);
			if (best == std::string::npos || end > best) {
				best = end;
			}
		}
	}

	return best;
}

std::string trimmed(const std::string &s)
{
	const size_t start = s.find_first_not_of(" \t\r\n");
	if (start == std::string::npos) {
		return "";
	}
	const size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(start, end - start + 1);
}

} // namespace

void OpenAIProvider::appendDelta(const std::string &delta)
{
	if (delta.empty()) {
		return;
	}

	std::string final_line, partial_line;
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		pending_text += delta;
		last_delta = std::chrono::steady_clock::now();

		const size_t end = lastSentenceEnd(pending_text);
		if (end != std::string::npos) {
			// Close out every complete sentence, keep the tail as the partial.
			final_line = trimmed(pending_text.substr(0, end));
			pending_text = trimmed(pending_text.substr(end));
			partial_line = pending_text;
		} else if (pending_text.size() >= kMaxPendingChars) {
			// Nothing punctuated it and it is getting long - cut it loose so the
			// caption line cannot grow without bound over a multi-hour stream.
			final_line = trimmed(pending_text);
			pending_text.clear();
		} else {
			partial_line = trimmed(pending_text);
		}
	}

	// Emit outside the lock: the callback runs the whole caption/translation path.
	if (!final_line.empty()) {
		emit(final_line, true);
	}
	if (!partial_line.empty()) {
		emit(partial_line, false);
	}
}

void OpenAIProvider::flushPending()
{
	std::string text;
	{
		std::lock_guard<std::mutex> lock(pending_mutex);
		text = trimmed(pending_text);
		pending_text.clear();
	}
	if (!text.empty()) {
		emit(text, true);
	}
}

void OpenAIProvider::emit(const std::string &text, bool final)
{
	if (text.empty()) {
		return;
	}

	DetectionResultWithText result;
	result.text = text;
	result.language = gf->language;
	result.result = final ? DETECTION_RESULT_SPEECH : DETECTION_RESULT_PARTIAL;
	// gpt-live-transcribe does not return word timestamps, so subtitle timing is
	// filter-side wall-clock. Good enough for live captions; not for authored SRT.
	const uint64_t now = now_ms();
	result.start_timestamp_ms = now > gf->start_timestamp_ms ? now - gf->start_timestamp_ms
								 : 0;
	result.end_timestamp_ms = result.start_timestamp_ms;

	transcription_callback(result);
}

void OpenAIProvider::onIdleTick()
{
	if (!connected) {
		return;
	}

	// A trailing fragment that never got punctuated would otherwise sit as a partial
	// forever - no stream caption, no SRT line. Finalise it once the speaker pauses.
	{
		bool stale = false;
		{
			std::lock_guard<std::mutex> lock(pending_mutex);
			if (!pending_text.empty()) {
				const auto since = std::chrono::duration_cast<
							   std::chrono::milliseconds>(
							   std::chrono::steady_clock::now() -
							   last_delta)
							   .count();
				stale = since >= kFinalizeSilenceMs;
			}
		}
		if (stale) {
			flushPending();
		}
	}

	const int timeout = gf->openai_idle_timeout_sec;
	if (timeout <= 0) {
		return; // user disabled the idle disconnect
	}

	const auto idle = std::chrono::duration_cast<std::chrono::seconds>(
				  std::chrono::steady_clock::now() - last_audio_sent)
				  .count();
	if (idle >= timeout) {
		disconnect("idle timeout");
	}
}

void OpenAIProvider::disconnect(const char *reason)
{
	if (!connected) {
		return;
	}

	obs_log(LOG_INFO, "Closing OpenAI connection (%s)", reason);
	// Don't strand a half-finished caption when the socket drops.
	flushPending();
	// Clear the flag first so the read thread treats the resulting read failure as
	// expected rather than an error.
	connected = false;

	try {
		std::lock_guard<std::mutex> lock(ws_mutex);
		if (ws) {
			beast::error_code ec;
			// Closing the underlying socket is what unblocks the pending read on
			// the results thread; a graceful WS close would deadlock against it.
			beast::get_lowest_layer(*ws).close(ec);
		}
	} catch (std::exception const &e) {
		obs_log(gf->log_level, "Error closing OpenAI connection: %s", e.what());
	}
}

void OpenAIProvider::shutdown()
{
	disconnect("shutdown");
	// The results thread may still hold a reference while unwinding its failed read;
	// the base class joins it before the provider is destroyed.
	std::lock_guard<std::mutex> lock(ws_mutex);
	ws.reset();
}
