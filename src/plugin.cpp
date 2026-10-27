// Becky! BoxLink プラグイン本体
//
// メモリ管理の約束:
//  - Becky! API が返す LPSTR（SerializeRcpts 等）は BeckyPtr で受け、bka.Free() で解放する。free/delete は使わない
//  - GetDataFolder 等が返すポインタは「次の API 呼び出しまで有効」なだけで所有権は無い。すぐ std::wstring にコピーする
//  - クリップボードに渡した HGLOBAL は SetClipboardData が成功したらシステムの所有になるので解放しない

#include <windows.h>
#include <shellapi.h>
#include "BeckyAPI.h"
#include "box_client.h"
#include "config.h"
#include "share_dialog.h"
#include "util.h"
#include "version.h"
#include <cstring>
#include <memory>
#include <set>

CBeckyAPI bka;  // SDK の約束で 1 プロジェクトに 1 インスタンス

namespace {

HINSTANCE g_hInstance = nullptr;
const wchar_t kTitle[] = L"Becky! BoxLink";

struct BeckyFreer {
	void operator()(char* p) const { if (p) bka.Free(p); }
};
using BeckyPtr = std::unique_ptr<char, BeckyFreer>;

struct Paths {
	std::wstring ini;
	std::wstring tokens;
};

// 設定は <データフォルダ>\PlugIns\BoxLink.ini、トークンは <データフォルダ>\PlugIns\BoxLink\token.dat
Paths GetPaths()
{
	const char* data = bka.GetDataFolder();
	std::wstring dataDir = AnsiToWide(data ? std::string(data) : std::string());  // 直ちにコピー
	std::wstring plugDir = JoinPath(dataDir, L"PlugIns");
	std::wstring stateDir = JoinPath(plugDir, L"BoxLink");
	::CreateDirectoryW(plugDir.c_str(), nullptr);
	::CreateDirectoryW(stateDir.c_str(), nullptr);
	return {JoinPath(plugDir, L"BoxLink.ini"), JoinPath(stateDir, L"token.dat")};
}

std::vector<std::wstring> GetRecipients(HWND hCompose)
{
	std::vector<std::wstring> result;
	std::set<std::wstring> seen;
	std::vector<char> buf(64 * 1024);
	for (const char* header : {"To", "Cc", "Bcc"}) {
		buf[0] = '\0';
		bka.CompGetSpecifiedHeader(hCompose, header, buf.data(), (int)buf.size());
		buf.back() = '\0';
		if (buf[0] == '\0') continue;
		// アドレス帳のグループを展開する。戻り値は Becky! が確保したメモリなので bka.Free で解放
		BeckyPtr expanded(bka.SerializeRcpts(buf.data()));
		std::string text = expanded ? std::string(expanded.get()) : std::string(buf.data());
		for (auto& addr : ExtractEmails(AnsiToWide(text))) {
			if (seen.insert(addr).second) result.push_back(addr);
		}
	}
	return result;
}

bool SetClipboardText(HWND hOwner, const std::wstring& text)
{
	if (!::OpenClipboard(hOwner)) return false;
	bool ok = false;
	::EmptyClipboard();
	size_t bytes = (text.size() + 1) * sizeof(wchar_t);
	HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (hMem) {
		void* p = ::GlobalLock(hMem);
		if (p) {
			memcpy(p, text.c_str(), bytes);
			::GlobalUnlock(hMem);
			ok = ::SetClipboardData(CF_UNICODETEXT, hMem) != nullptr;
		}
		if (!ok) ::GlobalFree(hMem);  // 成功時はシステムの所有になるので解放しない
	}
	::CloseClipboard();
	return ok;
}

void InsertIntoCompose(HWND hCompose, HWND hEditor, const std::wstring& text, const std::wstring& mode)
{
	if (mode == L"bottom") {
		std::string ansi = WideToAnsi(L"\r\n" + text);
		bka.CompSetText(hCompose, 1, ansi.c_str());
		SetClipboardText(hCompose, text);
		return;
	}
	bool copied = SetClipboardText(hCompose, text);
	if (mode == L"clipboard" || !copied) {
		::MessageBoxW(hCompose, copied ? L"共有リンクをクリップボードにコピーしました。本文に貼り付けてください。"
		                               : L"クリップボードへのコピーに失敗しました。", kTitle,
		              MB_OK | (copied ? MB_ICONINFORMATION : MB_ICONWARNING));
		return;
	}
	// paste: メニューを開く前にフォーカスがあった本文エディタへ貼り付ける
	if (hEditor && ::IsWindow(hEditor)) {
		::SetFocus(hEditor);
		::SendMessageW(hEditor, WM_PASTE, 0, 0);
	}
}

void CALLBACK CmdCreateShareLink(HWND hCmdWnd, LPARAM /*lParam*/)
{
	// 本文エディタ（子ウィンドウ）が渡された場合でも、作成画面のフレームに対して API を呼ぶ
	HWND hWnd = ::GetAncestor(hCmdWnd, GA_ROOT);
	if (!hWnd) hWnd = hCmdWnd;
	try {
		HWND hEditor = ::GetFocus();
		Paths paths = GetPaths();
		EnsureConfigFile(paths.ini);
		Config cfg;
		LoadConfig(paths.ini, cfg);

		BoxClient client(cfg, paths.tokens);
		ShareRequest req;
		req.cfg = &cfg;
		req.iniPath = paths.ini;
		req.client = &client;
		req.recipients = GetRecipients(hWnd);

		if (ShowShareDialog(g_hInstance, hWnd, req) != IDOK || req.resultText.empty()) return;
		InsertIntoCompose(hWnd, hEditor, req.resultText, cfg.insertMode);
	} catch (const std::exception&) {
		// 例外を Becky! 本体へ伝播させない
		::MessageBoxW(hWnd, L"予期しないエラーが発生しました。", kTitle, MB_OK | MB_ICONERROR);
	}
}

void CopyAnsi(char* dst, size_t size, const std::wstring& src)
{
	std::string s = WideToAnsi(src);
	strncpy_s(dst, size, s.c_str(), _TRUNCATE);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/)
{
	if (reason == DLL_PROCESS_ATTACH) {
		g_hInstance = hModule;
		::DisableThreadLibraryCalls(hModule);
		if (!bka.InitAPI()) return FALSE;
	}
	return TRUE;
}

extern "C" {

int WINAPI BKC_OnStart()
{
	return 0;
}

int WINAPI BKC_OnExit()
{
	return 0;
}

int WINAPI BKC_OnMenuInit(HWND /*hWnd*/, HMENU hMenu, int nType)
{
	if (nType == BKC_MENU_COMPEDIT) {
		// メール作成画面の本文エディタの右クリックメニュー
		UINT nID = bka.RegisterCommand("Create Box shared link", nType, CmdCreateShareLink);
		::AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
		::AppendMenuW(hMenu, MF_STRING, nID, L"Box 共有リンクを作成(&X)...");
	}
	return 0;
}

int WINAPI BKC_OnPlugInSetup(HWND hWnd)
{
	Paths paths = GetPaths();
	EnsureConfigFile(paths.ini);
	::ShellExecuteW(hWnd, L"open", L"notepad.exe", (L"\"" + paths.ini + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
	return 1;
}

typedef struct tagBKPLUGININFO {
	char szPlugInName[80];
	char szVendor[80];
	char szVersion[80];
	char szDescription[256];
} BKPLUGININFO, *LPBKPLUGININFO;

int WINAPI BKC_OnPlugInInfo(LPBKPLUGININFO info)
{
	// このコールバックでは Becky! API を呼んではいけない
	CopyAnsi(info->szPlugInName, sizeof(info->szPlugInName), L"Becky! BoxLink");
	CopyAnsi(info->szVendor, sizeof(info->szVendor), L"ikimove");
	CopyAnsi(info->szVersion, sizeof(info->szVersion), BOXLINK_VERSION_W);
	CopyAnsi(info->szDescription, sizeof(info->szDescription),
	         L"メール作成画面から Box の共有リンク（有効期限・ワンタイムパスコード付き）を作成します。");
	return 0;
}

} // extern "C"
