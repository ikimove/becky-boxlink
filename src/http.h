#pragma once
#include <windows.h>
#include <string>

struct HttpResponse {
	DWORD error = 0;     // WinHTTP のエラー（通信自体の失敗）。0 なら status が有効
	int status = 0;
	std::string body;    // UTF-8
};

// HTTPS リクエストを同期実行する。headers は "Name: value\r\n" の連結
HttpResponse HttpsRequest(const wchar_t* method, const wchar_t* host, const std::wstring& pathAndQuery,
                          const std::wstring& headers, const std::string& body);

// 使い回している WinHTTP のハンドルを閉じる。BKC_OnExit から呼ぶ（DllMain からは呼ばない）
void HttpShutdown();

std::wstring HttpErrorText(DWORD error);
