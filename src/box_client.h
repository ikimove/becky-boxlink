#pragma once
#include "config.h"
#include "token_store.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

struct BoxFolder {
	std::string id;
	std::wstring name;
	nlohmann::json sharedLink;  // 共有リンクが無ければ null
};

// Box API クライアント。UI スレッドからのみ使う（スレッドセーフではない）。
// 呼び出し回数を抑えるため、トークンは期限の直前まで使い回し、一覧は必要な分だけ取得する。
class BoxClient {
public:
	BoxClient(const Config& cfg, std::wstring tokenPath);

	bool HasCredentials() const { return !cfg_.clientId.empty() && !cfg_.clientSecret.empty(); }
	bool IsLoggedIn() const { return tokens_.HasRefreshToken(); }
	const Tokens& GetTokens() const { return tokens_; }
	int ApiCallCount() const { return apiCalls_; }

	std::wstring RedirectUri() const;
	std::wstring BuildAuthorizeUrl(const std::string& state) const;
	bool CompleteLogin(const std::string& code, std::wstring& err);
	void Logout();

	// 直下のフォルダだけを返す（ファイルは除外）
	bool ListSubfolders(const std::string& folderId, std::vector<BoxFolder>& out, std::wstring& err);
	bool GetFolder(const std::string& folderId, BoxFolder& out, std::wstring& err);
	// 共有リンクを作成・更新し、リンク URL を返す
	bool UpdateSharedLink(const std::string& folderId, const nlohmann::json& sharedLink,
	                      std::string& url, std::wstring& err);

private:
	bool EnsureAccessToken(bool forceRefresh, std::wstring& err);
	bool RequestToken(const std::string& form, std::wstring& err);
	bool Api(const wchar_t* method, const std::wstring& pathAndQuery, const std::string& body,
	         nlohmann::json& out, std::wstring& err);
	bool FetchCurrentUser(std::wstring& err);
	std::string ClientParams() const;

	const Config& cfg_;
	std::wstring tokenPath_;
	Tokens tokens_;
	int apiCalls_ = 0;
};

// Box のエラーレスポンスを利用者向けの文言にする
std::wstring BoxErrorText(int status, const std::string& body);
