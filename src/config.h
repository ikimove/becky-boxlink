#pragma once
#include <string>
#include <vector>

struct RecentFolder {
	std::string id;
	std::wstring path;
};

struct Config {
	// 環境変数 BOXLINK_CLIENT_ID / BOXLINK_CLIENT_SECRET（設定ファイルには保存しない）
	std::wstring clientId;
	std::wstring clientSecret;
	// [box]
	int redirectPort = 53682;
	// [link]
	bool expireEnabled = true;
	int expireDays = 14;
	bool otpEnabled = true;
	bool canDownload = true;
	// [browse]  フォルダ一覧の起点（"0" = すべてのファイル）。名前は表示用で、空なら ID を表示する
	std::string rootFolderId = "0";
	std::wstring rootFolderName;
	// [insert]  paste: カーソル位置に貼り付け / bottom: 本文末尾に追加 / clipboard: コピーのみ
	std::wstring insertMode = L"paste";
	// [recent]
	std::vector<RecentFolder> recent;
};

constexpr size_t kMaxRecentFolders = 10;
constexpr wchar_t kEnvClientId[] = L"BOXLINK_CLIENT_ID";
constexpr wchar_t kEnvClientSecret[] = L"BOXLINK_CLIENT_SECRET";

// 設定ファイルが無ければひな形を UTF-16LE で作成する（日本語のフォルダ名を保存できるように）
void EnsureConfigFile(const std::wstring& iniPath);
void LoadConfig(const std::wstring& iniPath, Config& cfg);
// ダイアログで変更される項目（[link] と [recent]）だけを書き戻す
void SaveUserSettings(const std::wstring& iniPath, const Config& cfg);
void AddRecentFolder(Config& cfg, const std::string& id, const std::wstring& path);
