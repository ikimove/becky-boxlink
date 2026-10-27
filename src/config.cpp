#include "config.h"
#include "util.h"
#include <algorithm>

namespace {

const wchar_t kTemplate[] =
	L"; Becky! BoxLink 設定ファイル\r\n"
	L";\r\n"
	L"; Box アプリのクライアントID とクライアントシークレットは、この設定ファイルではなく\r\n"
	L"; ユーザー環境変数 BOXLINK_CLIENT_ID / BOXLINK_CLIENT_SECRET に設定してください。\r\n"
	L"; リダイレクトURI には http://localhost:<redirect_port>/callback を登録します。\r\n"
	L"\r\n"
	L"[box]\r\n"
	L"redirect_port=53682\r\n"
	L"\r\n"
	L"[link]\r\n"
	L"; 有効期限の既定値（1=設定する / 0=しない）と日数\r\n"
	L"expire_enabled=1\r\n"
	L"expire_days=14\r\n"
	L"; ワンタイムパスコード（メール認証）の既定値\r\n"
	L"otp_enabled=1\r\n"
	L"; ダウンロードを許可するか\r\n"
	L"can_download=1\r\n"
	L"\r\n"
	L"[browse]\r\n"
	L"; フォルダ一覧の起点にする Box フォルダの ID（0=すべてのファイル）\r\n"
	L"; ID は Box の Web 画面でフォルダを開いたときの URL https://app.box.com/folder/<ID> の数字\r\n"
	L"root_folder_id=0\r\n"
	L"; 起点フォルダの表示名（API 呼び出しを省くため設定ファイルで指定。空なら ID を表示）\r\n"
	L"root_folder_name=\r\n"
	L"\r\n"
	L"[insert]\r\n"
	L"; paste=カーソル位置に貼り付け / bottom=本文の末尾に追加 / clipboard=コピーのみ\r\n"
	L"mode=paste\r\n";

std::wstring ReadStr(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, const wchar_t* def)
{
	std::vector<wchar_t> buf(2048);
	::GetPrivateProfileStringW(sec, key, def, buf.data(), (DWORD)buf.size(), ini.c_str());
	return buf.data();
}

int ReadInt(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, int def)
{
	return (int)::GetPrivateProfileIntW(sec, key, def, ini.c_str());
}

void WriteStr(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, const std::wstring& value)
{
	::WritePrivateProfileStringW(sec, key, value.c_str(), ini.c_str());
}

void WriteInt(const std::wstring& ini, const wchar_t* sec, const wchar_t* key, int value)
{
	WriteStr(ini, sec, key, std::to_wstring(value));
}

// プロセスの環境変数を優先し、無ければレジストリのユーザー環境変数を読む。
// 環境変数を登録した後に Becky! を起動し直さなくても（起動元が古い環境を渡していても）読めるようにするため
std::wstring ReadEnv(const wchar_t* name)
{
	std::vector<wchar_t> buf(1024);
	DWORD n = ::GetEnvironmentVariableW(name, buf.data(), (DWORD)buf.size());
	if (n >= buf.size()) {
		buf.resize(n);
		n = ::GetEnvironmentVariableW(name, buf.data(), (DWORD)buf.size());
	}
	if (n > 0 && n < buf.size()) return std::wstring(buf.data(), n);

	DWORD bytes = (DWORD)(buf.size() * sizeof(wchar_t));
	LSTATUS rc = ::RegGetValueW(HKEY_CURRENT_USER, L"Environment", name, RRF_RT_REG_SZ, nullptr, buf.data(), &bytes);
	if (rc == ERROR_MORE_DATA) {
		buf.resize(bytes / sizeof(wchar_t) + 1);
		bytes = (DWORD)(buf.size() * sizeof(wchar_t));
		rc = ::RegGetValueW(HKEY_CURRENT_USER, L"Environment", name, RRF_RT_REG_SZ, nullptr, buf.data(), &bytes);
	}
	return rc == ERROR_SUCCESS ? std::wstring(buf.data()) : std::wstring();
}

std::wstring Trim(const std::wstring& s)
{
	const wchar_t* kSpaces = L" \t\r\n";
	size_t b = s.find_first_not_of(kSpaces);
	if (b == std::wstring::npos) return std::wstring();
	size_t e = s.find_last_not_of(kSpaces);
	return s.substr(b, e - b + 1);
}

} // namespace

void EnsureConfigFile(const std::wstring& iniPath)
{
	if (::GetFileAttributesW(iniPath.c_str()) != INVALID_FILE_ATTRIBUTES) return;
	std::string data("\xFF\xFE", 2);  // UTF-16LE BOM
	data.append(reinterpret_cast<const char*>(kTemplate), (sizeof(kTemplate) - sizeof(wchar_t)));
	WriteFileAtomic(iniPath, data);
}

void LoadConfig(const std::wstring& iniPath, Config& cfg)
{
	cfg.clientId = Trim(ReadEnv(kEnvClientId));
	cfg.clientSecret = Trim(ReadEnv(kEnvClientSecret));
	cfg.redirectPort = ReadInt(iniPath, L"box", L"redirect_port", 53682);
	if (cfg.redirectPort <= 0 || cfg.redirectPort > 65535) cfg.redirectPort = 53682;

	cfg.expireEnabled = ReadInt(iniPath, L"link", L"expire_enabled", 1) != 0;
	cfg.expireDays = std::clamp(ReadInt(iniPath, L"link", L"expire_days", 14), 1, 3650);
	cfg.otpEnabled = ReadInt(iniPath, L"link", L"otp_enabled", 1) != 0;
	cfg.canDownload = ReadInt(iniPath, L"link", L"can_download", 1) != 0;

	std::wstring rootId = ReadStr(iniPath, L"browse", L"root_folder_id", L"0");
	if (rootId.empty() || rootId.find_first_not_of(L"0123456789") != std::wstring::npos) rootId = L"0";
	cfg.rootFolderId = WideToUtf8(rootId);
	cfg.rootFolderName = ReadStr(iniPath, L"browse", L"root_folder_name", L"");

	cfg.insertMode = ReadStr(iniPath, L"insert", L"mode", L"paste");

	cfg.recent.clear();
	for (size_t i = 0; i < kMaxRecentFolders; ++i) {
		std::wstring n = std::to_wstring(i);
		std::wstring id = ReadStr(iniPath, L"recent", (L"id" + n).c_str(), L"");
		std::wstring path = ReadStr(iniPath, L"recent", (L"path" + n).c_str(), L"");
		if (id.empty()) continue;
		cfg.recent.push_back({WideToUtf8(id), path});
	}
}

void SaveUserSettings(const std::wstring& iniPath, const Config& cfg)
{
	WriteInt(iniPath, L"link", L"expire_enabled", cfg.expireEnabled ? 1 : 0);
	WriteInt(iniPath, L"link", L"expire_days", cfg.expireDays);
	WriteInt(iniPath, L"link", L"otp_enabled", cfg.otpEnabled ? 1 : 0);
	WriteInt(iniPath, L"link", L"can_download", cfg.canDownload ? 1 : 0);

	// セクションごと書き直す（件数が減った場合に古いキーを残さない）
	::WritePrivateProfileStringW(L"recent", nullptr, nullptr, iniPath.c_str());
	for (size_t i = 0; i < cfg.recent.size() && i < kMaxRecentFolders; ++i) {
		std::wstring n = std::to_wstring(i);
		WriteStr(iniPath, L"recent", (L"id" + n).c_str(), Utf8ToWide(cfg.recent[i].id));
		WriteStr(iniPath, L"recent", (L"path" + n).c_str(), cfg.recent[i].path);
	}
}

void AddRecentFolder(Config& cfg, const std::string& id, const std::wstring& path)
{
	cfg.recent.erase(std::remove_if(cfg.recent.begin(), cfg.recent.end(),
		[&](const RecentFolder& r) { return r.id == id; }), cfg.recent.end());
	cfg.recent.insert(cfg.recent.begin(), RecentFolder{id, path});
	if (cfg.recent.size() > kMaxRecentFolders) cfg.recent.resize(kMaxRecentFolders);
}
