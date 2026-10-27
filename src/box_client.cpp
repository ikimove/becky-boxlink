#include "box_client.h"
#include "http.h"
#include "util.h"
#include <ctime>
#include <map>

namespace {
const wchar_t kApiHost[] = L"api.box.com";
const wchar_t kAuthorizeBase[] = L"https://account.box.com/api/oauth2/authorize";
// アクセストークンの期限がこの秒数以内なら先に更新する（401 を受けてからの再試行を避ける）
const long long kRefreshMarginSec = 120;
// フォルダ一覧のキャッシュ有効期間。他の人が Box 上で変更した内容は、この時間か「最新表示」で反映される
const long long kListCacheSec = 600;

// フォルダ一覧のキャッシュ（UI スレッドからのみ使う）。BoxClient はダイアログごとに作り直すので、
// ダイアログを開き直しても再取得しないようプロセス内に持つ。中身は標準コンテナだけなので静的破棄で問題ない
struct ListCacheEntry {
	long long fetchedAt = 0;
	bool withFiles = false;
	std::vector<BoxFolder> folders;
	std::vector<BoxFile> files;
};
std::wstring g_cacheOwner;  // キャッシュを取得したユーザー。別ユーザーでログインしたら捨てる
std::map<std::string, ListCacheEntry> g_listCache;

void ClearListCache()
{
	g_listCache.clear();
	g_cacheOwner.clear();
}

const ListCacheEntry* FindFreshEntry(const std::string& folderId, bool needFiles)
{
	auto it = g_listCache.find(folderId);
	if (it == g_listCache.end()) return nullptr;
	if ((long long)time(nullptr) - it->second.fetchedAt > kListCacheSec) return nullptr;
	if (needFiles && !it->second.withFiles) return nullptr;
	return &it->second;
}
}

BoxClient::BoxClient(const Config& cfg, std::wstring tokenPath)
	: cfg_(cfg), tokenPath_(std::move(tokenPath))
{
	LoadTokens(tokenPath_, tokens_);
}

std::wstring BoxClient::RedirectUri() const
{
	return L"http://localhost:" + std::to_wstring(cfg_.redirectPort) + L"/callback";
}

std::wstring BoxClient::BuildAuthorizeUrl(const std::string& state) const
{
	return std::wstring(kAuthorizeBase)
		+ L"?response_type=code"
		+ L"&client_id=" + Utf8ToWide(UrlEncode(WideToUtf8(cfg_.clientId)))
		+ L"&redirect_uri=" + Utf8ToWide(UrlEncode(WideToUtf8(RedirectUri())))
		+ L"&state=" + Utf8ToWide(state);
}

std::string BoxClient::ClientParams() const
{
	return "&client_id=" + UrlEncode(WideToUtf8(cfg_.clientId))
		+ "&client_secret=" + UrlEncode(WideToUtf8(cfg_.clientSecret));
}

bool BoxClient::CompleteLogin(const std::string& code, std::wstring& err)
{
	std::string form = "grant_type=authorization_code&code=" + UrlEncode(code)
		+ "&redirect_uri=" + UrlEncode(WideToUtf8(RedirectUri())) + ClientParams();
	if (!RequestToken(form, err)) return false;
	// 表示用のユーザー名はログイン時に1回だけ取得して保存する
	std::wstring userErr;
	FetchCurrentUser(userErr);
	SaveTokens(tokenPath_, tokens_);
	return true;
}

void BoxClient::Logout()
{
	if (tokens_.HasRefreshToken() && HasCredentials()) {
		// リフレッシュトークンを失効させる（関連するアクセストークンも無効になる）。失敗しても続行
		std::string form = "token=" + UrlEncode(tokens_.refreshToken) + ClientParams();
		HttpsRequest(L"POST", kApiHost, L"/oauth2/revoke",
		             L"Content-Type: application/x-www-form-urlencoded\r\n", form);
		SecureClear(form);
	}
	tokens_.Clear();
	DeleteTokens(tokenPath_);
	ClearListCache();
}

bool BoxClient::RequestToken(const std::string& form, std::wstring& err)
{
	if (!HasCredentials()) {
		err = L"環境変数 BOXLINK_CLIENT_ID と BOXLINK_CLIENT_SECRET を設定してください。";
		return false;
	}
	HttpResponse r = HttpsRequest(L"POST", kApiHost, L"/oauth2/token",
	                              L"Content-Type: application/x-www-form-urlencoded\r\n", form);
	if (r.error) { err = HttpErrorText(r.error); return false; }

	nlohmann::json j = nlohmann::json::parse(r.body, nullptr, false);
	if (r.status != 200) {
		std::string code = JStr(j, "error");
		if (r.status == 400 && (code == "invalid_grant" || code == "invalid_request")) {
			// リフレッシュトークンの期限切れ・失効。再ログインが必要
			tokens_.Clear();
			DeleteTokens(tokenPath_);
			err = L"Box のログイン有効期限が切れました。もう一度ログインしてください。";
		} else if (code == "invalid_client") {
			err = L"クライアントID またはクライアントシークレットが正しくありません。環境変数 BOXLINK_CLIENT_ID / BOXLINK_CLIENT_SECRET を確認してください。";
		} else {
			err = BoxErrorText(r.status, r.body);
		}
		return false;
	}

	std::string access = JStr(j, "access_token");
	std::string refresh = JStr(j, "refresh_token");
	if (access.empty() || refresh.empty()) {
		err = L"Box からトークンを取得できませんでした。";
		return false;
	}
	tokens_.accessToken = access;
	tokens_.refreshToken = refresh;  // Box のリフレッシュトークンは使い捨てなので必ず保存し直す
	tokens_.expiresAt = (long long)time(nullptr) + JInt(j, "expires_in", 3600);
	SecureClear(access);
	SecureClear(refresh);

	if (!SaveTokens(tokenPath_, tokens_)) {
		err = L"ログイン情報を保存できませんでした。次回は再ログインが必要になります。";
		// トークン自体は取得できているので処理は続行する
	}
	return true;
}

bool BoxClient::EnsureAccessToken(bool forceRefresh, std::wstring& err)
{
	if (!tokens_.HasRefreshToken()) {
		err = L"Box にログインしていません。";
		return false;
	}
	if (!forceRefresh && !tokens_.accessToken.empty() &&
	    tokens_.expiresAt - kRefreshMarginSec > (long long)time(nullptr)) {
		return true;
	}
	std::string form = "grant_type=refresh_token&refresh_token=" + UrlEncode(tokens_.refreshToken) + ClientParams();
	std::wstring saveWarning;
	bool ok = RequestToken(form, saveWarning);
	SecureClear(form);
	if (!ok) err = saveWarning;
	return ok;
}

bool BoxClient::Api(const wchar_t* method, const std::wstring& pathAndQuery, const std::string& body,
                    nlohmann::json& out, std::wstring& err)
{
	if (!EnsureAccessToken(false, err)) return false;

	for (int attempt = 0; attempt < 2; ++attempt) {
		std::wstring headers = L"Authorization: Bearer " + Utf8ToWide(tokens_.accessToken) + L"\r\n";
		if (!body.empty()) headers += L"Content-Type: application/json\r\n";

		HttpResponse r = HttpsRequest(method, kApiHost, pathAndQuery, headers, body);
		++apiCalls_;
		if (r.error) { err = HttpErrorText(r.error); return false; }

		if (r.status == 401 && attempt == 0) {
			// 期限前に失効していた場合のみ、1回だけ更新して再試行する
			if (!EnsureAccessToken(true, err)) return false;
			continue;
		}
		if (r.status < 200 || r.status >= 300) {
			err = BoxErrorText(r.status, r.body);
			return false;
		}
		out = nlohmann::json::parse(r.body, nullptr, false);
		if (out.is_discarded()) {
			err = L"Box の応答を解析できませんでした。";
			return false;
		}
		return true;
	}
	err = L"Box の認証に失敗しました。もう一度ログインしてください。";
	return false;
}

bool BoxClient::FetchCurrentUser(std::wstring& err)
{
	nlohmann::json j;
	if (!Api(L"GET", L"/2.0/users/me?fields=name,login", "", j, err)) return false;
	tokens_.userName = Utf8ToWide(JStr(j, "name"));
	tokens_.userLogin = Utf8ToWide(JStr(j, "login"));
	return true;
}

bool BoxClient::ListSubfolders(const std::string& folderId, std::vector<BoxFolder>& out, std::wstring& err,
                               bool forceReload)
{
	return ListFolderItems(folderId, out, nullptr, err, forceReload);
}

bool BoxClient::ListItems(const std::string& folderId, std::vector<BoxFolder>& folders,
                          std::vector<BoxFile>& files, std::wstring& err, bool forceReload)
{
	return ListFolderItems(folderId, folders, &files, err, forceReload);
}

// files が nullptr ならフォルダだけを集め、ファイルが現れた時点でページ取得を打ち切る
bool BoxClient::ListFolderItems(const std::string& folderId, std::vector<BoxFolder>& folders,
                                std::vector<BoxFile>* files, std::wstring& err, bool forceReload)
{
	folders.clear();
	if (files) files->clear();

	if (g_cacheOwner != tokens_.userLogin) {
		ClearListCache();
		g_cacheOwner = tokens_.userLogin;
	}
	if (!forceReload) {
		if (const ListCacheEntry* hit = FindFreshEntry(folderId, files != nullptr)) {
			folders = hit->folders;
			if (files) *files = hit->files;
			return true;
		}
	}
	// 通常のフォルダでは「フォルダ → ファイル → ウェブリンク」の順に並ぶことが API 仕様で保証されている。
	// そのためファイルが現れた時点で以降のページ取得を打ち切れる。ルート（ID 0）は順序の保証が異なるので最後まで読む
	const bool foldersFirst = folderId != "0";
	const long long limit = 1000;
	long long offset = 0;
	const wchar_t* fields = files ? L"type,name,size,shared_link" : L"type,name,shared_link";

	for (;;) {
		std::wstring path = L"/2.0/folders/" + Utf8ToWide(UrlEncode(folderId))
			+ L"/items?fields=" + fields + L"&sort=name&direction=ASC&limit=" + std::to_wstring(limit)
			+ L"&offset=" + std::to_wstring(offset);
		nlohmann::json j;
		if (!Api(L"GET", path, "", j, err)) return false;

		auto entries = j.find("entries");
		if (entries == j.end() || !entries->is_array()) break;

		bool reachedFiles = false;
		for (const auto& e : *entries) {
			const std::string type = JStr(e, "type");
			if (type == "file") {
				reachedFiles = true;
				if (!files) continue;
				BoxFile f;
				f.id = JStr(e, "id");
				f.name = Utf8ToWide(JStr(e, "name"));
				f.size = JInt(e, "size", 0);
				if (!f.id.empty()) files->push_back(std::move(f));
				continue;
			}
			if (type != "folder") {
				reachedFiles = true;  // ウェブリンク
				continue;
			}
			BoxFolder f;
			f.id = JStr(e, "id");
			f.name = Utf8ToWide(JStr(e, "name"));
			auto sl = e.find("shared_link");
			if (sl != e.end() && sl->is_object()) f.sharedLink = *sl;
			if (!f.id.empty()) folders.push_back(std::move(f));
		}

		long long total = JInt(j, "total_count", 0);
		offset += (long long)entries->size();
		if (entries->empty() || offset >= total) break;
		if (!files && foldersFirst && reachedFiles) break;
	}

	ListCacheEntry& entry = g_listCache[folderId];
	entry.fetchedAt = (long long)time(nullptr);
	entry.withFiles = files != nullptr;
	entry.folders = folders;
	if (files) entry.files = *files; else entry.files.clear();
	return true;
}

bool BoxClient::GetFolder(const std::string& folderId, BoxFolder& out, std::wstring& err)
{
	if (g_cacheOwner == tokens_.userLogin) {
		for (const auto& kv : g_listCache) {
			if ((long long)time(nullptr) - kv.second.fetchedAt > kListCacheSec) continue;
			for (const auto& f : kv.second.folders) {
				if (f.id == folderId) {
					out = f;
					return true;
				}
			}
		}
	}
	nlohmann::json j;
	std::wstring path = L"/2.0/folders/" + Utf8ToWide(UrlEncode(folderId)) + L"?fields=name,shared_link";
	if (!Api(L"GET", path, "", j, err)) return false;
	out.id = JStr(j, "id");
	out.name = Utf8ToWide(JStr(j, "name"));
	auto sl = j.find("shared_link");
	out.sharedLink = (sl != j.end() && sl->is_object()) ? *sl : nlohmann::json();
	return true;
}

bool BoxClient::UpdateSharedLink(const std::string& folderId, const nlohmann::json& sharedLink,
                                 std::string& url, std::wstring& err)
{
	nlohmann::json body = {{"shared_link", sharedLink}};
	nlohmann::json j;
	// fields=shared_link を付けると、作成と URL の取得を1回の呼び出しで済ませられる
	std::wstring path = L"/2.0/folders/" + Utf8ToWide(UrlEncode(folderId)) + L"?fields=shared_link";
	if (!Api(L"PUT", path, body.dump(), j, err)) return false;
	auto sl = j.find("shared_link");
	url = (sl != j.end()) ? JStr(*sl, "url") : std::string();
	if (url.empty()) {
		err = L"共有リンクは更新されましたが、URL を取得できませんでした。";
		return false;
	}
	// キャッシュ中の一覧にも反映し、次に開いたとき「[共有中]」の表示や上書き確認が正しく出るようにする
	for (auto& kv : g_listCache) {
		for (auto& f : kv.second.folders) {
			if (f.id == folderId) f.sharedLink = *sl;
		}
	}
	return true;
}

std::wstring BoxErrorText(int status, const std::string& body)
{
	nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
	std::string code = JStr(j, "code");
	std::wstring message = Utf8ToWide(JStr(j, "message"));

	std::wstring hint;
	switch (status) {
	case 400: hint = L"設定の組み合わせが Box で許可されていません。"
	                 L"（ワンタイムパスコードは Business プラン以上が必要で、管理者が公開リンクを禁止している場合も使えません）"; break;
	case 403: hint = L"この操作を行う権限がありません。"; break;
	case 404: hint = L"フォルダが見つかりません。削除または移動された可能性があります。「最新表示」で一覧を更新してください。"; break;
	case 429: hint = L"Box API の呼び出し回数の上限に達しました。しばらく待ってからお試しください。"; break;
	default:
		if (status >= 500) hint = L"Box 側で一時的なエラーが発生しています。しばらく待ってからお試しください。";
		break;
	}
	std::wstring text = L"Box エラー（HTTP " + std::to_wstring(status);
	if (!code.empty()) text += L" / " + Utf8ToWide(code);
	text += L"）";
	if (!hint.empty()) text += L"\r\n" + hint;
	if (!message.empty()) text += L"\r\n詳細: " + message;
	return text;
}
