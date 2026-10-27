#pragma once
#include <windows.h>
#include <winsock2.h>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// ---- RAII ラッパー（手動の解放処理を書かないための型） ----

struct HandleCloser {
	void operator()(HANDLE h) const { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct LocalFreer {
	void operator()(void* p) const { if (p) ::LocalFree(p); }
};

class UniqueSocket {
public:
	UniqueSocket() = default;
	explicit UniqueSocket(SOCKET s) : s_(s) {}
	~UniqueSocket() { reset(); }
	UniqueSocket(UniqueSocket&& o) noexcept : s_(o.release()) {}
	UniqueSocket& operator=(UniqueSocket&& o) noexcept { if (this != &o) reset(o.release()); return *this; }
	UniqueSocket(const UniqueSocket&) = delete;
	UniqueSocket& operator=(const UniqueSocket&) = delete;

	SOCKET get() const { return s_; }
	explicit operator bool() const { return s_ != INVALID_SOCKET; }
	SOCKET release() { SOCKET s = s_; s_ = INVALID_SOCKET; return s; }
	void reset(SOCKET s = INVALID_SOCKET) { if (s_ != INVALID_SOCKET) ::closesocket(s_); s_ = s; }
private:
	SOCKET s_ = INVALID_SOCKET;
};

// ---- 文字コード変換 ----
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);
std::wstring AnsiToWide(const std::string& s);   // Becky! API (CP_ACP) との受け渡し用
std::string WideToAnsi(const std::wstring& s);

// ---- 文字列ユーティリティ ----
std::string UrlEncode(const std::string& utf8);
std::string UrlDecode(const std::string& s);
std::vector<std::wstring> ExtractEmails(const std::wstring& text);  // 小文字化・重複除去済み
std::wstring ToLower(std::wstring s);
std::string RandomHex(size_t bytes);
void SecureClear(std::string& s);
void SecureClear(std::wstring& s);

// JSON から型を確認しつつ値を取り出す（型違い・欠落でも例外を出さない）
std::string JStr(const nlohmann::json& j, const char* key);
long long JInt(const nlohmann::json& j, const char* key, long long def = 0);

// ---- ファイル ----
std::wstring JoinPath(const std::wstring& dir, const std::wstring& name);
bool ReadFileBytes(const std::wstring& path, std::string& out);
bool WriteFileAtomic(const std::wstring& path, const std::string& data);  // 一時ファイル経由で置き換え
