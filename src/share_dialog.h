#pragma once
#include "box_client.h"
#include "config.h"
#include <windows.h>
#include <string>
#include <vector>

struct ShareRequest {
	Config* cfg = nullptr;
	std::wstring iniPath;
	BoxClient* client = nullptr;
	std::vector<std::wstring> recipients;  // 作成中メールの To/Cc/Bcc
	std::wstring resultText;               // [out] IDOK のとき、本文に挿入する文面
};

INT_PTR ShowShareDialog(HINSTANCE hInst, HWND hParent, ShareRequest& req);
