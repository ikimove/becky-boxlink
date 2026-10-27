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
	long long modifiedAt = 0;   // 更新日時（UNIX 秒）。一覧の並べ替えに使う
};

struct BoxFile {
	std::string id;
	std::wstring name;
	long long size = 0;
	long long modifiedAt = 0;   // 更新日時（UNIX 秒）
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

	// 一覧はプロセス内に kListCacheSec 秒キャッシュし、ダイアログを開き直しても API を呼ばない。
	// forceReload（「最新表示」）のときだけキャッシュを使わずに取得する

	// 直下のフォルダだけを返す（ファイルは除外）
	bool ListSubfolders(const std::string& folderId, std::vector<BoxFolder>& out, std::wstring& err,
	                    bool forceReload = false);
	// 直下のフォルダとファイルを返す。ファイルの分だけページ取得が増えるので、利用者が求めた時だけ呼ぶ
	bool ListItems(const std::string& folderId, std::vector<BoxFolder>& folders,
	               std::vector<BoxFile>& files, std::wstring& err, bool forceReload = false);
	// キャッシュした一覧にそのフォルダがあれば API を呼ばずに返す
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
	bool ListFolderItems(const std::string& folderId, std::vector<BoxFolder>& folders,
	                     std::vector<BoxFile>* files, std::wstring& err, bool forceReload);
	std::string ClientParams() const;

	const Config& cfg_;
	std::wstring tokenPath_;
	Tokens tokens_;
	int apiCalls_ = 0;
};

// Box のエラーレスポンスを利用者向けの文言にする
std::wstring BoxErrorText(int status, const std::string& body);
