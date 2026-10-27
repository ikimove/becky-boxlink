#include "oauth_listener.h"
#include <ws2tcpip.h>
#include <map>

namespace {

std::map<std::string, std::string> ParseQuery(const std::string& query)
{
	std::map<std::string, std::string> params;
	size_t pos = 0;
	while (pos <= query.size()) {
		size_t amp = query.find('&', pos);
		if (amp == std::string::npos) amp = query.size();
		std::string pair = query.substr(pos, amp - pos);
		size_t eq = pair.find('=');
		if (!pair.empty()) {
			if (eq == std::string::npos) params[UrlDecode(pair)] = "";
			else params[UrlDecode(pair.substr(0, eq))] = UrlDecode(pair.substr(eq + 1));
		}
		pos = amp + 1;
	}
	return params;
}

void SendResponse(SOCKET s, int status, const char* reason, const std::string& html)
{
	std::string res = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
		"Content-Type: text/html; charset=utf-8\r\n"
		"Content-Length: " + std::to_string(html.size()) + "\r\n"
		"Cache-Control: no-store\r\n"
		"Connection: close\r\n\r\n" + html;
	size_t sent = 0;
	while (sent < res.size()) {
		int n = ::send(s, res.data() + sent, (int)(res.size() - sent), 0);
		if (n <= 0) break;
		sent += (size_t)n;
	}
}

std::string Page(const char* message)
{
	return std::string(
		"<!doctype html><html lang=\"ja\"><head><meta charset=\"utf-8\"><title>Becky! BoxLink</title>"
		"<style>body{font-family:sans-serif;margin:3em;color:#222}</style></head><body><p>")
		+ message + "</p></body></html>";
}

bool Listen(int family, int port, UniqueSocket& out)
{
	UniqueSocket s(::socket(family, SOCK_STREAM, IPPROTO_TCP));
	if (!s) return false;
	BOOL exclusive = TRUE;  // 他プロセスに同じポートを横取りされないようにする
	::setsockopt(s.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

	int rc;
	if (family == AF_INET) {
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons((u_short)port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		rc = ::bind(s.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
	} else {
		sockaddr_in6 addr{};
		addr.sin6_family = AF_INET6;
		addr.sin6_port = htons((u_short)port);
		addr.sin6_addr = in6addr_loopback;
		rc = ::bind(s.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
	}
	if (rc == SOCKET_ERROR || ::listen(s.get(), 4) == SOCKET_ERROR) return false;
	out = std::move(s);
	return true;
}

} // namespace

OAuthListener::~OAuthListener()
{
	sockets_.clear();  // WSACleanup より先にソケットを閉じる
	if (wsaStarted_) ::WSACleanup();
}

bool OAuthListener::Start(int port, std::wstring& err)
{
	if (!wsaStarted_) {
		WSADATA wsa{};
		if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			err = L"ネットワーク機能を初期化できませんでした。";
			return false;
		}
		wsaStarted_ = true;
	}
	UniqueSocket v4, v6;
	if (!Listen(AF_INET, port, v4)) {
		err = L"ポート " + std::to_wstring(port) + L" で待ち受けできません。他のアプリが使用していないか確認してください。";
		return false;
	}
	sockets_.push_back(std::move(v4));
	// ブラウザが localhost を ::1 で解決する場合に備える（失敗しても IPv4 だけで続行）
	if (Listen(AF_INET6, port, v6)) sockets_.push_back(std::move(v6));
	return true;
}

OAuthResult OAuthListener::Wait(const std::string& expectedState, int timeoutSec)
{
	OAuthResult result;
	const ULONGLONG deadline = ::GetTickCount64() + (ULONGLONG)timeoutSec * 1000;

	while (!cancel_) {
		if (::GetTickCount64() > deadline) {
			result.error = L"ログインがタイムアウトしました。もう一度お試しください。";
			return result;
		}
		fd_set readable;
		FD_ZERO(&readable);
		for (auto& s : sockets_) FD_SET(s.get(), &readable);
		timeval tv{0, 200 * 1000};
		int n = ::select(0, &readable, nullptr, nullptr, &tv);
		if (n == SOCKET_ERROR) {
			result.error = L"ログインの待ち受け中にエラーが発生しました。";
			return result;
		}
		if (n == 0) continue;

		for (auto& s : sockets_) {
			if (!FD_ISSET(s.get(), &readable)) continue;
			UniqueSocket client(::accept(s.get(), nullptr, nullptr));
			if (!client) continue;
			DWORD timeoutMs = 3000;
			::setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

			std::string req;
			char buf[2048];
			while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
				int r = ::recv(client.get(), buf, sizeof(buf), 0);
				if (r <= 0) break;
				req.append(buf, (size_t)r);
			}

			// 例: "GET /callback?code=xxx&state=yyy HTTP/1.1"
			size_t sp1 = req.find(' ');
			size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : req.find(' ', sp1 + 1);
			if (sp2 == std::string::npos) continue;
			std::string target = req.substr(sp1 + 1, sp2 - sp1 - 1);
			size_t q = target.find('?');
			std::string path = target.substr(0, q);
			if (path != "/callback") {
				SendResponse(client.get(), 404, "Not Found", Page("Not Found"));
				continue;  // favicon など。待ち受けを続ける
			}
			auto params = ParseQuery(q == std::string::npos ? std::string() : target.substr(q + 1));

			if (params["state"] != expectedState) {
				// 古いログイン画面からの戻りなど。今回の要求ではないので待ち受けを続ける
				SendResponse(client.get(), 400, "Bad Request",
					Page("このログイン要求は無効です。Becky! の画面からもう一度ログインしてください。"));
				continue;
			}
			if (params.count("error")) {
				SendResponse(client.get(), 200, "OK",
					Page("Box へのアクセスが許可されませんでした。このタブを閉じてください。"));
				std::string desc = params.count("error_description") ? params["error_description"] : params["error"];
				result.error = L"Box へのアクセスが許可されませんでした（" + Utf8ToWide(desc) + L"）。";
				return result;
			}
			if (params["code"].empty()) {
				SendResponse(client.get(), 400, "Bad Request", Page("認可コードがありません。"));
				continue;
			}
			SendResponse(client.get(), 200, "OK",
				Page("ログインが完了しました。このタブを閉じて Becky! に戻ってください。"));
			result.ok = true;
			result.code = params["code"];
			return result;
		}
	}
	result.error = L"ログインを中止しました。";
	return result;
}
