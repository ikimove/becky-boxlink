#include "token_store.h"
#include "util.h"
#include <wincrypt.h>

namespace {
const wchar_t kEntropyDesc[] = L"BeckyBoxLink";
}

void Tokens::Clear()
{
	SecureClear(accessToken);
	SecureClear(refreshToken);
	expiresAt = 0;
	userName.clear();
	userLogin.clear();
}

bool LoadTokens(const std::wstring& path, Tokens& out)
{
	out.Clear();
	std::string enc;
	if (!ReadFileBytes(path, enc) || enc.empty()) return false;

	DATA_BLOB in{(DWORD)enc.size(), reinterpret_cast<BYTE*>(&enc[0])};
	DATA_BLOB plain{};
	if (!::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
		return false;
	}
	// plain.pbData は LocalFree で解放する必要がある
	std::unique_ptr<BYTE, LocalFreer> plainOwner(plain.pbData);
	std::string text(reinterpret_cast<const char*>(plain.pbData), plain.cbData);
	::SecureZeroMemory(plain.pbData, plain.cbData);

	nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
	SecureClear(text);
	if (!j.is_object()) return false;

	out.accessToken = JStr(j, "access_token");
	out.refreshToken = JStr(j, "refresh_token");
	out.expiresAt = JInt(j, "expires_at");
	out.userName = Utf8ToWide(JStr(j, "user_name"));
	out.userLogin = Utf8ToWide(JStr(j, "user_login"));
	return out.HasRefreshToken();
}

bool SaveTokens(const std::wstring& path, const Tokens& tokens)
{
	nlohmann::json j = {
		{"access_token", tokens.accessToken},
		{"refresh_token", tokens.refreshToken},
		{"expires_at", tokens.expiresAt},
		{"user_name", WideToUtf8(tokens.userName)},
		{"user_login", WideToUtf8(tokens.userLogin)},
	};
	std::string text = j.dump();

	DATA_BLOB in{(DWORD)text.size(), reinterpret_cast<BYTE*>(&text[0])};
	DATA_BLOB enc{};
	BOOL ok = ::CryptProtectData(&in, kEntropyDesc, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &enc);
	SecureClear(text);
	if (!ok) return false;
	std::unique_ptr<BYTE, LocalFreer> encOwner(enc.pbData);

	return WriteFileAtomic(path, std::string(reinterpret_cast<const char*>(enc.pbData), enc.cbData));
}

void DeleteTokens(const std::wstring& path)
{
	::DeleteFileW(path.c_str());
}
