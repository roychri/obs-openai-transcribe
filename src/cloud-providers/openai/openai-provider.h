#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include "cloud-providers/cloud-provider.h"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

// Realtime transcription against OpenAI's `gpt-live-transcribe` over WebSocket.
//
// Differs from a typical streaming STT vendor in two ways that matter here:
//  1. Audio is sent as base64 inside a JSON *text* frame, not as a binary frame.
//  2. Billing is by session wall-clock, not by speech, so an idle socket costs
//     real money. The connection is therefore opened lazily on first audio and
//     dropped again after `idle_timeout_sec` of silence.
class OpenAIProvider : public CloudProvider {
public:
	OpenAIProvider(TranscriptionCallback callback, cloudvocal_data *gf_);
	~OpenAIProvider() override;

	bool init() override;

	// gpt-live-transcribe takes 24 kHz PCM.
	static constexpr int kSampleRate = 24000;
	int sampleRate() const override { return kSampleRate; }

protected:
	void sendAudioBufferToTranscription(const std::deque<float> &audio_buffer) override;
	void readResultsFromTranscription() override;
	void onIdleTick() override;
	void shutdown() override;

private:
	bool connect();
	void disconnect(const char *reason);
	bool sendSessionUpdate();
	bool writeFrame(const std::string &payload);
	void handleEvent(const std::string &message);
	void emit(const std::string &text, bool final);

	using WsStream = websocket::stream<beast::ssl_stream<tcp::socket>>;

	// Shared rather than unique so the results thread can hold the stream alive for
	// the duration of a read while the audio thread tears the connection down.
	std::shared_ptr<WsStream> currentStream();

	net::io_context ioc;
	ssl::context ssl_ctx;
	tcp::resolver resolver;
	// Recreated on every connect - a Beast stream cannot be reused after close.
	std::shared_ptr<WsStream> ws;

	std::mutex ws_mutex;
	std::atomic<bool> connected;
	std::chrono::steady_clock::time_point last_audio_sent;

	// Deltas arrive per transcription item; accumulate so partial captions render
	// as a growing line instead of disconnected fragments.
	std::string current_item_id;
	std::string current_item_text;
};
