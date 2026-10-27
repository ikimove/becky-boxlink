#include "util.h"
#include <bcrypt.h>
#include <algorithm>
#include <cwctype>
#include <regex>
#include <set>

namespace {

std::wstring ToWide(UINT cp, const std::string& s)
{
	if (s.empty()) return {};
	int n = ::MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
	if (n <= 0) return {};
	std::wstring w((size_t)n, L'\0');
	::MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
	return w;
}

std::string FromWide(UINT cp, const std::wstring& w)
{
	if (w.empty()) return {};
	int n = ::WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	if (n <= 0) return {};
	std::string s((size_t)n, '\0');
	::WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

int HexValue(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

} // namespace

std::wstring Utf8ToWide(const std::string& s) { return ToWide(CP_UTF8, s); }
std::string WideToUtf8(const std::wstring& s) { return FromWide(CP_UTF8, s); }
std::wstring AnsiToWide(const std::string& s) { return ToWide(CP_ACP, s); }
std::string WideToAnsi(const std::wstring& s) { return FromWide(CP_ACP, s); }

std::string UrlEncode(const std::string& utf8)
{
	static const char hex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(utf8.size() * 3);
	for (unsigned char c : utf8) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			c == '-' || c == '_' || c == '.' || c == '~') {
			out += (char)c;
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 0x0F];
		}
	}
	return out;
}

std::string UrlDecode(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '+') {
			out += ' ';
		} else if (s[i] == '%' && i + 2 < s.size() && HexValue(s[i + 1]) >= 0 && HexValue(s[i + 2]) >= 0) {
			out += (char)(HexValue(s[i + 1]) * 16 + HexValue(s[i + 2]));
			i += 2;
		} else {
			out += s[i];
		}
	}
	return out;
}

std::wstring ToLower(std::wstring s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
	return s;
}

std::vector<std::wstring> ExtractEmails(const std::wstring& text)
{
	// CP932 の2バイト目に '@' が現れることがあるため、必ずワイド文字に変換してから照合する
	static const std::wregex re(LR"([A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+)");
	std::vector<std::wstring> result;
	std::set<std::wstring> seen;
	for (std::wsregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
		std::wstring addr = ToLower(it->str());
		if (seen.insert(addr).second) result.push_back(addr);
	}
	return result;
}

std::string RandomHex(size_t bytes)
{
	std::vector<unsigned char> buf(bytes);
	if (!BCRYPT_SUCCESS(::BCryptGenRandom(nullptr, buf.data(), (ULONG)buf.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
		return {};
	}
	static const char hex[] = "0123456789abcdef";
	std::string out;
	for (unsigned char c : buf) {
		out += hex[c >> 4];
		out += hex[c & 0x0F];
	}
	return out;
}

void SecureClear(std::string& s)
{
	if (!s.empty()) ::SecureZeroMemory(&s[0], s.size());
	s.clear();
}

void SecureClear(std::wstring& s)
{
	if (!s.empty()) ::SecureZeroMemory(&s[0], s.size() * sizeof(wchar_t));
	s.clear();
}

std::string JStr(const nlohmann::json& j, const char* key)
{
	if (!j.is_object()) return {};
	auto it = j.find(key);
	if (it == j.end() || !it->is_string()) return {};
	return it->get<std::string>();
}

long long JInt(const nlohmann::json& j, const char* key, long long def)
{
	if (!j.is_object()) return def;
	auto it = j.find(key);
	if (it == j.end() || !it->is_number()) return def;
	return it->get<long long>();
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name)
{
	if (dir.empty()) return name;
	wchar_t last = dir.back();
	if (last == L'\\' || last == L'/') return dir + name;
	return dir + L"\\" + name;
}

bool ReadFileBytes(const std::wstring& path, std::string& out)
{
	out.clear();
	HANDLE raw = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (raw == INVALID_HANDLE_VALUE) return false;
	UniqueHandle h(raw);
	LARGE_INTEGER size{};
	if (!::GetFileSizeEx(h.get(), &size) || size.QuadPart > 16 * 1024 * 1024) return false;
	out.resize((size_t)size.QuadPart);
	DWORD read = 0;
	if (!out.empty() && (!::ReadFile(h.get(), &out[0], (DWORD)out.size(), &read, nullptr) || read != out.size())) {
		out.clear();
		return false;
	}
	return true;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data)
{
	std::wstring tmp = path + L".tmp";
	{
		HANDLE raw = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (raw == INVALID_HANDLE_VALUE) return false;
		UniqueHandle h(raw);
		DWORD written = 0;
		if (!::WriteFile(h.get(), data.data(), (DWORD)data.size(), &written, nullptr) || written != data.size()) {
			h.reset();
			::DeleteFileW(tmp.c_str());
			return false;
		}
		::FlushFileBuffers(h.get());
	}
	if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		::DeleteFileW(tmp.c_str());
		return false;
	}
	return true;
}
