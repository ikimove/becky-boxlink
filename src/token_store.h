#pragma once
#include <string>

struct Tokens {
	std::string accessToken;
	std::string refreshToken;
	long long expiresAt = 0;     // アクセストークンの有効期限（UNIX 時刻）
	std::wstring userName;       // 表示用（ログイン時に1回だけ取得）
	std::wstring userLogin;

	bool HasRefreshToken() const { return !refreshToken.empty(); }
	void Clear();
};

// DPAPI（現在の Windows ユーザーでのみ復号可能）で暗号化して保存する
bool LoadTokens(const std::wstring& path, Tokens& out);
bool SaveTokens(const std::wstring& path, const Tokens& tokens);
void DeleteTokens(const std::wstring& path);
