#include "http.h"
#include "version.h"
#include <winhttp.h>
#include <memory>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace {

struct InetCloser {
	void operator()(HINTERNET h) const { if (h) ::WinHttpCloseHandle(h); }
};
using UniqueInet = std::unique_ptr<void, InetCloser>;

const wchar_t kUserAgent[] = L"BeckyBoxLink/" BOXLINK_VERSION_W;  // 隣接する L"" リテラルは連結される

} // namespace

HttpResponse HttpsRequest(const wchar_t* method, const wchar_t* host, const std::wstring& pathAndQuery,
                          const std::wstring& headers, const std::string& body)
{
	HttpResponse res;

	// 解放順は宣言の逆（request → connect → session）
	UniqueInet session(::WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
	                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	if (!session) {
		session.reset(::WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	}
	if (!session) { res.error = ::GetLastError(); return res; }
	::WinHttpSetTimeouts(session.get(), 10000, 10000, 30000, 30000);

	UniqueInet connect(::WinHttpConnect(session.get(), host, INTERNET_DEFAULT_HTTPS_PORT, 0));
	if (!connect) { res.error = ::GetLastError(); return res; }

	UniqueInet request(::WinHttpOpenRequest(connect.get(), method, pathAndQuery.c_str(), nullptr,
	                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
	if (!request) { res.error = ::GetLastError(); return res; }

	BOOL ok = ::WinHttpSendRequest(request.get(),
		headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
		headers.empty() ? 0 : (DWORD)-1L,
		body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
		(DWORD)body.size(), (DWORD)body.size(), 0);
	if (ok) ok = ::WinHttpReceiveResponse(request.get(), nullptr);
	if (!ok) { res.error = ::GetLastError(); return res; }

	DWORD status = 0;
	DWORD len = sizeof(status);
	::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
	                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
	res.status = (int)status;

	for (;;) {
		DWORD avail = 0;
		if (!::WinHttpQueryDataAvailable(request.get(), &avail)) { res.error = ::GetLastError(); return res; }
		if (avail == 0) break;
		size_t old = res.body.size();
		res.body.resize(old + avail);
		DWORD read = 0;
		if (!::WinHttpReadData(request.get(), &res.body[old], avail, &read)) { res.error = ::GetLastError(); return res; }
		res.body.resize(old + read);
		if (read == 0) break;
	}
	return res;
}

std::wstring HttpErrorText(DWORD error)
{
	switch (error) {
	case ERROR_WINHTTP_TIMEOUT:
		return L"Box への接続がタイムアウトしました。";
	case ERROR_WINHTTP_NAME_NOT_RESOLVED:
	case ERROR_WINHTTP_CANNOT_CONNECT:
		return L"Box に接続できません。ネットワーク接続を確認してください。";
	case ERROR_WINHTTP_SECURE_FAILURE:
		return L"Box との安全な接続（TLS）を確立できませんでした。";
	default:
		return L"通信エラーが発生しました（エラーコード " + std::to_wstring(error) + L"）。";
	}
}
