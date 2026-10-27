#pragma once
#include <string>
#include <vector>

struct RecentFolder {
	std::string id;
	std::wstring path;
};

struct Config {
	// [box]
	std::wstring clientId;
	std::wstring clientSecret;
	int redirectPort = 53682;
	// [link]
	bool expireEnabled = true;
	int expireDays = 14;
	bool otpEnabled = true;
	bool canDownload = true;
	// [insert]  paste: カーソル位置に貼り付け / bottom: 本文末尾に追加 / clipboard: コピーのみ
	std::wstring insertMode = L"paste";
	// [recent]
	std::vector<RecentFolder> recent;
};

constexpr size_t kMaxRecentFolders = 10;

// 設定ファイルが無ければひな形を UTF-16LE で作成する（日本語のフォルダ名を保存できるように）
void EnsureConfigFile(const std::wstring& iniPath);
void LoadConfig(const std::wstring& iniPath, Config& cfg);
// ダイアログで変更される項目（[link] と [recent]）だけを書き戻す
void SaveUserSettings(const std::wstring& iniPath, const Config& cfg);
void AddRecentFolder(Config& cfg, const std::string& id, const std::wstring& path);
