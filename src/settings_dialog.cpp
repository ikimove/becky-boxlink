#include "settings_dialog.h"
#include "config.h"
#include "resource.h"
#include "util.h"
#include <shellapi.h>

namespace {

const wchar_t kTitle[] = L"Becky! BoxLink";

// コンボボックスの並び順と ini の値の対応
const wchar_t* const kInsertModes[] = {L"paste", L"bottom", L"clipboard"};
const wchar_t* const kInsertLabels[] = {
	L"カーソル位置に貼り付け",
	L"本文の末尾に追加",
	L"クリップボードにコピーのみ",
};

struct SettingsState {
	std::wstring iniPath;
	Config cfg;
	bool hasSecret = false;    // 既にシークレットが設定されているか（値は画面に出さない）
	bool clearRecent = false;  // 「履歴を消去」が押された（OK で反映）
};

std::wstring GetText(HWND hDlg, int id)
{
	HWND h = ::GetDlgItem(hDlg, id);
	int len = ::GetWindowTextLengthW(h);
	std::wstring s((size_t)len + 1, L'\0');
	::GetWindowTextW(h, &s[0], len + 1);
	s.resize((size_t)len);
	return s;
}

std::wstring Trim(const std::wstring& s)
{
	const wchar_t* kSpaces = L" \t\r\n";
	size_t b = s.find_first_not_of(kSpaces);
	if (b == std::wstring::npos) return std::wstring();
	size_t e = s.find_last_not_of(kSpaces);
	return s.substr(b, e - b + 1);
}

bool IsChecked(HWND hDlg, int id)
{
	return ::IsDlgButtonChecked(hDlg, id) == BST_CHECKED;
}

void UpdateRedirectLabel(HWND hDlg)
{
	std::wstring port = GetText(hDlg, IDC_S_PORT);
	std::wstring text = L"→ http://localhost:" + (port.empty() ? std::wstring(L"?") : port) + L"/callback";
	::SetDlgItemTextW(hDlg, IDC_S_REDIRECT, text.c_str());
}

void UpdateRootNameLabel(HWND hDlg, const SettingsState& st)
{
	std::wstring id = GetText(hDlg, IDC_S_ROOTID);
	std::wstring text;
	if (id.empty() || id == L"0") {
		text = L"（すべてのファイル）";
	} else if (WideToUtf8(id) == st.cfg.cachedRootId && !st.cfg.cachedRootName.empty()) {
		text = L"（" + st.cfg.cachedRootName + L"）";
	} else {
		text = L"（名前は次回の表示時に取得）";
	}
	::SetDlgItemTextW(hDlg, IDC_S_ROOTNAME, text.c_str());
}

void UpdateRecentLabel(HWND hDlg, const SettingsState& st)
{
	size_t n = st.clearRecent ? 0 : st.cfg.recent.size();
	std::wstring text = L"最近使ったフォルダ: " + std::to_wstring(n) + L" 件";
	if (st.clearRecent) text += L"（OK で消去）";
	::SetDlgItemTextW(hDlg, IDC_S_RECENT, text.c_str());
	::EnableWindow(::GetDlgItem(hDlg, IDC_S_CLEARRECENT), n > 0);
}

void UpdateExpireUi(HWND hDlg)
{
	bool on = IsChecked(hDlg, IDC_S_EXPIRE);
	::EnableWindow(::GetDlgItem(hDlg, IDC_S_DAYS), on);
	::EnableWindow(::GetDlgItem(hDlg, IDC_S_DAYS_LABEL), on);
}

void OnInitDialog(HWND hDlg, SettingsState& st)
{
	const Config& cfg = st.cfg;
	st.hasSecret = !cfg.clientSecret.empty();

	::SendDlgItemMessageW(hDlg, IDC_S_CLIENTID, EM_LIMITTEXT, 256, 0);
	::SendDlgItemMessageW(hDlg, IDC_S_SECRET, EM_LIMITTEXT, 256, 0);
	::SendDlgItemMessageW(hDlg, IDC_S_PORT, EM_LIMITTEXT, 5, 0);
	::SendDlgItemMessageW(hDlg, IDC_S_DAYS, EM_LIMITTEXT, 4, 0);
	::SendDlgItemMessageW(hDlg, IDC_S_ROOTID, EM_LIMITTEXT, 20, 0);

	::SetDlgItemTextW(hDlg, IDC_S_CLIENTID, cfg.clientId.c_str());
	// シークレットは画面に表示しない。空欄のままなら現在の値を維持する
	::SetDlgItemTextW(hDlg, IDC_S_SECRET_NOTE,
		st.hasSecret ? L"設定済み（変更する場合のみ入力）" : L"未設定");
	::SetDlgItemTextW(hDlg, IDC_S_PORT, std::to_wstring(cfg.redirectPort).c_str());

	::CheckDlgButton(hDlg, IDC_S_EXPIRE, cfg.expireEnabled ? BST_CHECKED : BST_UNCHECKED);
	::SetDlgItemTextW(hDlg, IDC_S_DAYS, std::to_wstring(cfg.expireDays).c_str());
	::CheckDlgButton(hDlg, IDC_S_OTP, cfg.otpEnabled ? BST_CHECKED : BST_UNCHECKED);
	::CheckDlgButton(hDlg, IDC_S_DOWNLOAD, cfg.canDownload ? BST_CHECKED : BST_UNCHECKED);

	::SetDlgItemTextW(hDlg, IDC_S_ROOTID, Utf8ToWide(cfg.rootFolderId).c_str());

	HWND hCombo = ::GetDlgItem(hDlg, IDC_S_INSERT);
	int sel = 0;
	for (int i = 0; i < (int)(sizeof(kInsertModes) / sizeof(kInsertModes[0])); ++i) {
		::SendMessageW(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kInsertLabels[i]));
		if (cfg.insertMode == kInsertModes[i]) sel = i;
	}
	::SendMessageW(hCombo, CB_SETCURSEL, sel, 0);

	UpdateRedirectLabel(hDlg);
	UpdateRootNameLabel(hDlg, st);
	UpdateRecentLabel(hDlg, st);
	UpdateExpireUi(hDlg);
}

bool Fail(HWND hDlg, int focusId, const wchar_t* message)
{
	::MessageBoxW(hDlg, message, kTitle, MB_OK | MB_ICONWARNING);
	::SetFocus(::GetDlgItem(hDlg, focusId));
	::SendDlgItemMessageW(hDlg, focusId, EM_SETSEL, 0, -1);
	return false;
}

bool OnOk(HWND hDlg, SettingsState& st)
{
	// 入力チェック（すべて通ってから保存する）
	std::wstring clientId = Trim(GetText(hDlg, IDC_S_CLIENTID));
	std::wstring secret = Trim(GetText(hDlg, IDC_S_SECRET));
	BOOL valid = FALSE;
	UINT port = ::GetDlgItemInt(hDlg, IDC_S_PORT, &valid, FALSE);
	if (!valid || port < 1024 || port > 65535) {
		return Fail(hDlg, IDC_S_PORT, L"リダイレクトポートは 1024～65535 の数字で入力してください。");
	}
	const bool expire = IsChecked(hDlg, IDC_S_EXPIRE);
	UINT days = ::GetDlgItemInt(hDlg, IDC_S_DAYS, &valid, FALSE);
	if (expire && (!valid || days < 1 || days > 3650)) {
		return Fail(hDlg, IDC_S_DAYS, L"有効期限の日数は 1～3650 で入力してください。");
	}
	std::wstring rootId = Trim(GetText(hDlg, IDC_S_ROOTID));
	if (rootId.empty()) rootId = L"0";
	if (rootId.find_first_not_of(L"0123456789") != std::wstring::npos) {
		return Fail(hDlg, IDC_S_ROOTID, L"起点フォルダ ID は数字で入力してください（0 = すべてのファイル）。");
	}
	if (rootId.size() > 1 && rootId.find_first_not_of(L'0') == std::wstring::npos) rootId = L"0";

	// 環境変数（変更があったものだけ書き込む）
	bool envChanged = false;
	if (clientId != st.cfg.clientId) {
		if (!SaveUserEnvironment(kEnvClientId, clientId)) {
			return Fail(hDlg, IDC_S_CLIENTID, L"環境変数 BOXLINK_CLIENT_ID を保存できませんでした。");
		}
		envChanged = true;
	}
	if (!secret.empty()) {
		bool ok = SaveUserEnvironment(kEnvClientSecret, secret);
		SecureClear(secret);
		::SetDlgItemTextW(hDlg, IDC_S_SECRET, L"");
		if (!ok) return Fail(hDlg, IDC_S_SECRET, L"環境変数 BOXLINK_CLIENT_SECRET を保存できませんでした。");
		envChanged = true;
	}
	if (envChanged) NotifyEnvironmentChanged();

	// 設定ファイル
	Config& cfg = st.cfg;
	cfg.redirectPort = (int)port;
	cfg.expireEnabled = expire;
	if (expire) cfg.expireDays = (int)days;
	cfg.otpEnabled = IsChecked(hDlg, IDC_S_OTP);
	cfg.canDownload = IsChecked(hDlg, IDC_S_DOWNLOAD);
	cfg.rootFolderId = WideToUtf8(rootId);
	int sel = (int)::SendDlgItemMessageW(hDlg, IDC_S_INSERT, CB_GETCURSEL, 0, 0);
	if (sel >= 0 && sel < (int)(sizeof(kInsertModes) / sizeof(kInsertModes[0]))) cfg.insertMode = kInsertModes[sel];
	if (st.clearRecent) cfg.recent.clear();
	SaveAllSettings(st.iniPath, cfg);
	return true;
}

INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	auto* st = reinterpret_cast<SettingsState*>(::GetWindowLongPtrW(hDlg, DWLP_USER));
	try {
		switch (msg) {
		case WM_INITDIALOG:
			st = reinterpret_cast<SettingsState*>(lParam);
			::SetWindowLongPtrW(hDlg, DWLP_USER, lParam);
			OnInitDialog(hDlg, *st);
			return TRUE;

		case WM_COMMAND:
			if (!st) break;
			switch (LOWORD(wParam)) {
			case IDOK:
				if (OnOk(hDlg, *st)) ::EndDialog(hDlg, IDOK);
				return TRUE;
			case IDCANCEL:
				::EndDialog(hDlg, IDCANCEL);
				return TRUE;
			case IDC_S_PORT:
				if (HIWORD(wParam) == EN_CHANGE) UpdateRedirectLabel(hDlg);
				return TRUE;
			case IDC_S_ROOTID:
				if (HIWORD(wParam) == EN_CHANGE) UpdateRootNameLabel(hDlg, *st);
				return TRUE;
			case IDC_S_EXPIRE:
				UpdateExpireUi(hDlg);
				return TRUE;
			case IDC_S_CLEARRECENT:
				st->clearRecent = true;
				UpdateRecentLabel(hDlg, *st);
				return TRUE;
			case IDC_S_OPENINI:
				::ShellExecuteW(hDlg, L"open", L"notepad.exe", (L"\"" + st->iniPath + L"\"").c_str(),
				                nullptr, SW_SHOWNORMAL);
				return TRUE;
			}
			break;
		}
	} catch (...) {
		// 例外を Becky! 本体へ伝播させない
		::MessageBoxW(hDlg, L"設定画面で予期しないエラーが発生しました。", kTitle, MB_OK | MB_ICONERROR);
	}
	return FALSE;
}

} // namespace

INT_PTR ShowSettingsDialog(HINSTANCE hInst, HWND hParent, const std::wstring& iniPath)
{
	SettingsState st;
	st.iniPath = iniPath;
	EnsureConfigFile(iniPath);
	LoadConfig(iniPath, st.cfg);
	INT_PTR r = ::DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_SETTINGS), hParent, SettingsDlgProc,
	                              reinterpret_cast<LPARAM>(&st));
	SecureClear(st.cfg.clientSecret);
	return r;
}
