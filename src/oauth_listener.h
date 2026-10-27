#pragma once
#include "util.h"
#include <atomic>
#include <string>
#include <vector>

struct OAuthResult {
	bool ok = false;
	std::string code;
	std::wstring error;
};

// OAuth 2.0 のリダイレクトを localhost で1回だけ受け取る待ち受け。
// Start() は UI スレッドで、Wait() はワーカースレッドで呼ぶ。
// Cancel() はどのスレッドからでも呼べる。破棄は Wait() を抜けた（スレッドを join した）後に行うこと。
class OAuthListener {
public:
	OAuthListener() = default;
	~OAuthListener();
	OAuthListener(const OAuthListener&) = delete;
	OAuthListener& operator=(const OAuthListener&) = delete;

	bool Start(int port, std::wstring& err);
	OAuthResult Wait(const std::string& expectedState, int timeoutSec);
	void Cancel() { cancel_ = true; }

private:
	bool wsaStarted_ = false;
	std::vector<UniqueSocket> sockets_;
	std::atomic<bool> cancel_{false};
};
