#include "share_dialog.h"
#include "oauth_listener.h"
#include "resource.h"
#include "util.h"
#include <commctrl.h>
#include <shellapi.h>
#include <cstdio>
#include <memory>
#include <set>
#include <thread>

using nlohmann::json;

namespace {

const UINT WM_APP_LOGIN_DONE = WM_APP + 1;
const wchar_t kTitle[] = L"Becky! BoxLink";
const int kLoginTimeoutSec = 300;
const size_t kMaxAllowlist = 1000;

struct Node {
	std::string id;
	std::wstring name;
	std::wstring path;          // 例: "/営業/見積書"
	bool loaded = false;        // 子フォルダを取得済みか
	bool recent = false;        // 「最近使ったフォルダ」の項目
	bool root = false;          // 一覧の起点（共有対象にはしない）
	bool file = false;          // ファイルの項目（表示のみ。共有対象にはしない）
	bool filesShown = false;    // 「ファイル表示」で子にファイルを追加済みか
	long long size = 0;         // ファイルのサイズ（バイト）
	bool sharedKnown = false;   // sharedLink が取得済みか
	json sharedLink;            // 共有リンクが無ければ null
};

struct DialogState {
	explicit DialogState(ShareRequest& r) : req(r) {}
	~DialogState() { StopLogin(); }
	DialogState(const DialogState&) = delete;
	DialogState& operator=(const DialogState&) = delete;

	// ログイン待ちのスレッドを止めて合流する。listener はスレッド終了後に破棄する
	void StopLogin()
	{
		if (listener) listener->Cancel();
		if (loginThread.joinable()) loginThread.join();
		listener.reset();
		loggingIn = false;
	}

	ShareRequest& req;
	HWND hDlg = nullptr;
	HWND hTree = nullptr;
	HWND hList = nullptr;
	HTREEITEM hAllRoot = nullptr;
	// TreeView の lParam が指す実体。ツリー項目より長く生きるよう、ダイアログ終了まで保持する
	std::vector<std::unique_ptr<Node>> nodes;

	std::unique_ptr<OAuthListener> listener;
	std::thread loginThread;
	OAuthResult loginResult;  // ワーカーが書き込み、join 後に UI スレッドが読む
	bool loggingIn = false;
};

// ---------------------------------------------------------------------------
// 小さなヘルパー

bool JBool(const json& j, const char* key, bool def = false)
{
	if (!j.is_object()) return def;
	auto it = j.find(key);
	return (it != j.end() && it->is_boolean()) ? it->get<bool>() : def;
}

std::string OtpMode(const json& sharedLink)
{
	if (!sharedLink.is_object()) return "none";
	auto it = sharedLink.find("one_time_passcode");
	if (it == sharedLink.end() || !it->is_object()) return "none";
	std::string mode = JStr(*it, "mode");
	return mode.empty() ? "none" : mode;
}

std::wstring AccessLabel(const std::string& access)
{
	if (access == "open") return L"リンクを知っている全員";
	if (access == "company") return L"社内のユーザーのみ";
	if (access == "collaborators") return L"招待されたユーザーのみ";
	return Utf8ToWide(access);
}

std::wstring GetText(HWND hDlg, int id)
{
	HWND h = ::GetDlgItem(hDlg, id);
	int len = ::GetWindowTextLengthW(h);
	std::wstring s((size_t)len + 1, L'\0');
	::GetWindowTextW(h, &s[0], len + 1);
	s.resize((size_t)len);
	return s;
}

bool IsChecked(HWND hDlg, int id)
{
	return ::IsDlgButtonChecked(hDlg, id) == BST_CHECKED;
}

void SetStatus(DialogState& st, const std::wstring& text)
{
	::SetDlgItemTextW(st.hDlg, IDC_STATUS, text.c_str());
}

void ShowError(DialogState& st, const std::wstring& text)
{
	SetStatus(st, L"エラーが発生しました");
	::MessageBoxW(st.hDlg, text.c_str(), kTitle, MB_OK | MB_ICONWARNING);
}

// 期限日（ローカル時刻の 23:59）を Box に渡す UTC の ISO 8601 文字列にする
std::string ExpireTimestamp(int days, std::wstring& display)
{
	SYSTEMTIME now{};
	::GetLocalTime(&now);
	FILETIME ft{};
	::SystemTimeToFileTime(&now, &ft);
	ULARGE_INTEGER u{};
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	u.QuadPart += (ULONGLONG)days * 24ULL * 60 * 60 * 10000000ULL;
	ft.dwLowDateTime = u.LowPart;
	ft.dwHighDateTime = u.HighPart;

	SYSTEMTIME local{};
	::FileTimeToSystemTime(&ft, &local);
	local.wHour = 23;
	local.wMinute = 59;
	local.wSecond = 0;
	local.wMilliseconds = 0;

	SYSTEMTIME utc{};
	if (!::TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) utc = local;

	wchar_t disp[40];
	swprintf_s(disp, L"%04u/%02u/%02u 23:59", (unsigned)local.wYear, (unsigned)local.wMonth, (unsigned)local.wDay);
	display = disp;

	char iso[40];
	sprintf_s(iso, "%04u-%02u-%02uT%02u:%02u:%02u+00:00", (unsigned)utc.wYear, (unsigned)utc.wMonth,
	          (unsigned)utc.wDay, (unsigned)utc.wHour, (unsigned)utc.wMinute, (unsigned)utc.wSecond);
	return iso;
}

std::wstring ComposeInsertText(const std::string& url, const std::wstring& expireDisplay, bool otp)
{
	std::wstring text = Utf8ToWide(url) + L"\r\n";
	if (!expireDisplay.empty()) text += L"有効期限: " + expireDisplay + L" まで\r\n";
	if (otp) text += L"※ 開く際にメールアドレスの入力が必要です。入力したアドレスに届く確認コードを入力してください。\r\n";
	return text;
}

// ---------------------------------------------------------------------------
// ツリー

Node* NewNode(DialogState& st)
{
	st.nodes.push_back(std::make_unique<Node>());
	return st.nodes.back().get();
}

HTREEITEM InsertTreeItem(HWND hTree, HTREEITEM parent, const std::wstring& text, Node* node, bool hasChildren)
{
	TVINSERTSTRUCTW tvi{};
	tvi.hParent = parent;
	tvi.hInsertAfter = TVI_LAST;
	tvi.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
	tvi.item.pszText = const_cast<wchar_t*>(text.c_str());
	tvi.item.lParam = reinterpret_cast<LPARAM>(node);
	tvi.item.cChildren = hasChildren ? 1 : 0;
	return reinterpret_cast<HTREEITEM>(::SendMessageW(hTree, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&tvi)));
}

void SetHasChildren(HWND hTree, HTREEITEM h, bool hasChildren)
{
	TVITEMW it{};
	it.mask = TVIF_CHILDREN;
	it.hItem = h;
	it.cChildren = hasChildren ? 1 : 0;
	::SendMessageW(hTree, TVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&it));
}

Node* GetNode(HWND hTree, HTREEITEM h)
{
	if (!h) return nullptr;
	TVITEMW it{};
	it.mask = TVIF_PARAM;
	it.hItem = h;
	if (!::SendMessageW(hTree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&it))) return nullptr;
	return reinterpret_cast<Node*>(it.lParam);
}

Node* SelectedNode(DialogState& st)
{
	return GetNode(st.hTree, TreeView_GetSelection(st.hTree));
}

std::wstring TreeLabel(const Node& n)
{
	return n.sharedLink.is_object() ? n.name + L"  [共有中]" : n.name;
}

std::wstring FormatSize(long long bytes)
{
	const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
	double v = (double)bytes;
	int u = 0;
	while (v >= 1024.0 && u < 4) {
		v /= 1024.0;
		++u;
	}
	wchar_t buf[32];
	swprintf_s(buf, u == 0 ? L"%.0f %s" : L"%.1f %s", v, units[u]);
	return buf;
}

std::wstring FileLabel(const Node& n)
{
	return n.name + L"  (" + FormatSize(n.size) + L")";
}

void UpdateApiCountStatus(DialogState& st, const std::wstring& prefix)
{
	SetStatus(st, prefix + L"（API 呼び出し " + std::to_wstring(st.req.client->ApiCallCount()) + L" 回）");
}

void UpdateUserUi(DialogState& st);

void InsertFolders(DialogState& st, HTREEITEM hItem, Node* node, std::vector<BoxFolder>& folders);

bool LoadChildren(DialogState& st, HTREEITEM hItem, Node* node)
{
	std::vector<BoxFolder> folders;
	std::wstring err;
	HCURSOR old = ::SetCursor(::LoadCursor(nullptr, IDC_WAIT));
	bool ok = st.req.client->ListSubfolders(node->id, folders, err);
	::SetCursor(old);
	if (!ok) {
		ShowError(st, err);
		UpdateUserUi(st);  // ログイン切れの可能性
		return false;
	}
	InsertFolders(st, hItem, node, folders);
	UpdateApiCountStatus(st, folders.empty() ? L"サブフォルダはありません" : L"フォルダを読み込みました");
	return true;
}

void InsertFolders(DialogState& st, HTREEITEM hItem, Node* node, std::vector<BoxFolder>& folders)
{
	node->loaded = true;
	for (auto& f : folders) {
		Node* child = NewNode(st);
		child->id = f.id;
		child->name = f.name;
		child->path = node->path + L"/" + f.name;
		child->sharedKnown = true;
		child->sharedLink = std::move(f.sharedLink);
		InsertTreeItem(st.hTree, hItem, TreeLabel(*child), child, true);
	}
	if (folders.empty()) SetHasChildren(st.hTree, hItem, false);
}

// 選択中のフォルダ直下のファイルをツリーに追加する。
// 未展開のフォルダなら、同じ呼び出しでサブフォルダも取得して展開済みにする（API を二重に呼ばない）
void ShowFiles(DialogState& st)
{
	HTREEITEM h = TreeView_GetSelection(st.hTree);
	Node* node = GetNode(st.hTree, h);
	if (!node) {
		SetStatus(st, L"ファイルを表示するフォルダを選択してください");
		return;
	}
	if (node->file) {
		h = TreeView_GetParent(st.hTree, h);
		node = GetNode(st.hTree, h);
		if (!node) return;
	}
	if (node->filesShown) {
		TreeView_Expand(st.hTree, h, TVE_EXPAND);
		SetStatus(st, L"ファイルは表示済みです（取得し直すには「最新表示」）");
		return;
	}

	std::vector<BoxFolder> folders;
	std::vector<BoxFile> files;
	std::wstring err;
	HCURSOR old = ::SetCursor(::LoadCursor(nullptr, IDC_WAIT));
	bool ok = st.req.client->ListItems(node->id, folders, files, err);
	::SetCursor(old);
	if (!ok) {
		ShowError(st, err);
		UpdateUserUi(st);
		return;
	}

	if (!node->loaded) InsertFolders(st, h, node, folders);
	node->filesShown = true;
	for (const auto& f : files) {
		Node* child = NewNode(st);
		child->id = f.id;
		child->name = f.name;
		child->path = node->path + L"/" + f.name;
		child->file = true;
		child->size = f.size;
		InsertTreeItem(st.hTree, h, FileLabel(*child), child, false);
	}
	if (!files.empty()) SetHasChildren(st.hTree, h, true);
	TreeView_Expand(st.hTree, h, TVE_EXPAND);
	UpdateApiCountStatus(st, files.empty() ? L"ファイルはありません"
		: L"ファイル " + std::to_wstring(files.size()) + L" 件を表示しました");
}

void BuildTree(DialogState& st)
{
	TreeView_DeleteAllItems(st.hTree);
	st.nodes.clear();
	st.hAllRoot = nullptr;

	const Config& cfg = *st.req.cfg;
	const bool allFiles = cfg.rootFolderId == "0";
	std::wstring rootName = allFiles ? std::wstring(L"すべてのファイル")
		: !cfg.rootFolderName.empty() ? cfg.rootFolderName
		: L"フォルダ " + Utf8ToWide(cfg.rootFolderId);
	// 起点が「すべてのファイル」なら子のパスは "/A"、特定フォルダなら "/起点名/A"
	std::wstring rootPath = allFiles ? std::wstring() : L"/" + rootName;

	// 最近使ったフォルダは設定ファイルから表示するだけなので API を呼ばない。
	// 起点を絞っているときは、起点の下で作成したもの（パスが起点名で始まるもの）だけを表示する
	std::vector<const RecentFolder*> recents;
	for (const auto& r : cfg.recent) {
		if (allFiles || r.path.compare(0, rootPath.size() + 1, rootPath + L"/") == 0) recents.push_back(&r);
	}
	if (!recents.empty()) {
		HTREEITEM hRecent = InsertTreeItem(st.hTree, TVI_ROOT, L"最近使ったフォルダ", nullptr, true);
		for (const RecentFolder* rp : recents) {
			const RecentFolder& r = *rp;
			Node* n = NewNode(st);
			n->id = r.id;
			n->path = r.path;
			size_t slash = r.path.find_last_of(L'/');
			n->name = (slash == std::wstring::npos) ? r.path : r.path.substr(slash + 1);
			n->recent = true;
			n->loaded = true;
			InsertTreeItem(st.hTree, hRecent, r.path, n, false);
		}
		TreeView_Expand(st.hTree, hRecent, TVE_EXPAND);
	}

	Node* root = NewNode(st);
	root->id = cfg.rootFolderId;
	root->name = rootName;
	root->path = rootPath;
	root->root = true;
	st.hAllRoot = InsertTreeItem(st.hTree, TVI_ROOT, root->name, root, true);

	// 最近使ったフォルダがあれば、起点は展開されるまで読み込まない
	if (st.req.client->IsLoggedIn() && recents.empty()) {
		TreeView_Expand(st.hTree, st.hAllRoot, TVE_EXPAND);
	}
}

void RefreshSelected(DialogState& st)
{
	HTREEITEM h = TreeView_GetSelection(st.hTree);
	Node* node = GetNode(st.hTree, h);
	if (node && node->file) {
		h = TreeView_GetParent(st.hTree, h);
		node = GetNode(st.hTree, h);
	}
	if (!node || node->recent) {
		h = st.hAllRoot;
		node = GetNode(st.hTree, h);
	}
	if (!h || !node) return;
	// 子項目を破棄して未取得の状態に戻し、展開し直す（ここで1回だけ API を呼ぶ）
	TreeView_Expand(st.hTree, h, TVE_COLLAPSE | TVE_COLLAPSERESET);
	node->loaded = false;
	node->filesShown = false;
	SetHasChildren(st.hTree, h, true);
	TreeView_Expand(st.hTree, h, TVE_EXPAND);
}

// ---------------------------------------------------------------------------
// 画面の状態更新

void UpdateOkButton(DialogState& st)
{
	Node* node = SelectedNode(st);
	bool enable = st.req.client->IsLoggedIn() && !st.loggingIn && node && !node->root && !node->file;
	::EnableWindow(::GetDlgItem(st.hDlg, IDOK), enable);
}

void UpdateUserUi(DialogState& st)
{
	const Tokens& t = st.req.client->GetTokens();
	bool loggedIn = st.req.client->IsLoggedIn();
	std::wstring user = L"(未ログイン)";
	if (loggedIn) {
		user = t.userName.empty() ? L"(ログイン済み)" : t.userName;
		if (!t.userLogin.empty()) user += L"  <" + t.userLogin + L">";
	}
	::SetDlgItemTextW(st.hDlg, IDC_USER, user.c_str());
	::SetDlgItemTextW(st.hDlg, IDC_LOGIN, st.loggingIn ? L"ログイン中止" : (loggedIn ? L"ログアウト" : L"ログイン"));
	::EnableWindow(st.hTree, loggedIn && !st.loggingIn);
	::EnableWindow(::GetDlgItem(st.hDlg, IDC_REFRESH), loggedIn && !st.loggingIn);
	::EnableWindow(::GetDlgItem(st.hDlg, IDC_SHOWFILES), loggedIn && !st.loggingIn);
	UpdateOkButton(st);
}

void UpdateExpireUi(DialogState& st)
{
	bool on = IsChecked(st.hDlg, IDC_EXPIRE_CHK);
	::EnableWindow(::GetDlgItem(st.hDlg, IDC_EXPIRE_DAYS), on);
	::EnableWindow(::GetDlgItem(st.hDlg, IDC_EXPIRE_SPIN), on);
	::EnableWindow(::GetDlgItem(st.hDlg, IDC_EXPIRE_LABEL), on);

	std::wstring label;
	BOOL valid = FALSE;
	int days = (int)::GetDlgItemInt(st.hDlg, IDC_EXPIRE_DAYS, &valid, FALSE);
	if (on && valid && days >= 1 && days <= 3650) {
		std::wstring display;
		ExpireTimestamp(days, display);
		label = L"（" + display + L" まで）";
	}
	::SetDlgItemTextW(st.hDlg, IDC_EXPIRE_DATE, label.c_str());
}

void UpdateOtpUi(DialogState& st)
{
	bool on = IsChecked(st.hDlg, IDC_OTP_CHK);
	::EnableWindow(st.hList, on);
	::EnableWindow(::GetDlgItem(st.hDlg, IDC_ADDR_NEW), on);
	::EnableWindow(::GetDlgItem(st.hDlg, IDC_ADDR_ADD), on);
}

// ---------------------------------------------------------------------------
// 宛先リスト

std::wstring ListItemText(HWND hList, int index)
{
	wchar_t buf[512] = {};
	LVITEMW it{};
	it.iSubItem = 0;
	it.pszText = buf;
	it.cchTextMax = (int)(sizeof(buf) / sizeof(buf[0]));
	::SendMessageW(hList, LVM_GETITEMTEXTW, (WPARAM)index, reinterpret_cast<LPARAM>(&it));
	return buf;
}

void AddAddress(DialogState& st, const std::wstring& addr, bool checked)
{
	int count = ListView_GetItemCount(st.hList);
	for (int i = 0; i < count; ++i) {
		if (ToLower(ListItemText(st.hList, i)) == ToLower(addr)) {
			ListView_SetCheckState(st.hList, i, TRUE);
			return;
		}
	}
	LVITEMW item{};
	item.mask = LVIF_TEXT;
	item.iItem = count;
	item.pszText = const_cast<wchar_t*>(addr.c_str());
	int index = (int)::SendMessageW(st.hList, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
	if (index >= 0) ListView_SetCheckState(st.hList, index, checked ? TRUE : FALSE);
}

std::vector<std::wstring> CheckedAddresses(DialogState& st)
{
	std::vector<std::wstring> result;
	int count = ListView_GetItemCount(st.hList);
	for (int i = 0; i < count; ++i) {
		if (ListView_GetCheckState(st.hList, i)) result.push_back(ToLower(ListItemText(st.hList, i)));
	}
	return result;
}

void OnAddAddress(DialogState& st)
{
	std::wstring text = GetText(st.hDlg, IDC_ADDR_NEW);
	std::vector<std::wstring> addrs = ExtractEmails(text);
	if (addrs.empty()) {
		::MessageBoxW(st.hDlg, L"メールアドレスの形式が正しくありません。", kTitle, MB_OK | MB_ICONINFORMATION);
		return;
	}
	for (const auto& a : addrs) AddAddress(st, a, true);
	::SetDlgItemTextW(st.hDlg, IDC_ADDR_NEW, L"");
}

// ---------------------------------------------------------------------------
// ログイン

void OnLoginButton(DialogState& st)
{
	BoxClient& client = *st.req.client;

	if (st.loggingIn) {
		if (st.listener) st.listener->Cancel();  // 完了通知は WM_APP_LOGIN_DONE で受ける
		SetStatus(st, L"ログインを中止しています...");
		return;
	}
	if (client.IsLoggedIn()) {
		if (::MessageBoxW(st.hDlg, L"Box からログアウトしますか？", kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
		client.Logout();
		BuildTree(st);
		UpdateUserUi(st);
		SetStatus(st, L"ログアウトしました");
		return;
	}
	if (!client.HasCredentials()) {
		std::wstring msg = std::wstring(L"Box アプリのクライアントID とクライアントシークレットが設定されていません。\r\n\r\n")
			+ L"ユーザー環境変数 " + kEnvClientId + L" と " + kEnvClientSecret + L" に設定してから、\r\n"
			+ L"このダイアログを開き直してください。\r\n\r\n環境変数の設定画面を開きますか？";
		if (::MessageBoxW(st.hDlg, msg.c_str(), kTitle, MB_YESNO | MB_ICONINFORMATION) == IDYES) {
			::ShellExecuteW(st.hDlg, L"open", L"rundll32.exe", L"sysdm.cpl,EditEnvironmentVariables", nullptr, SW_SHOWNORMAL);
		}
		return;
	}

	auto listener = std::make_unique<OAuthListener>();
	std::wstring err;
	if (!listener->Start(st.req.cfg->redirectPort, err)) {
		ShowError(st, err);
		return;
	}
	std::string state = RandomHex(16);
	if (state.empty()) {
		ShowError(st, L"ログイン用の乱数を生成できませんでした。");
		return;
	}
	std::wstring url = client.BuildAuthorizeUrl(state);
	HINSTANCE r = ::ShellExecuteW(st.hDlg, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR>(r) <= 32) {
		ShowError(st, L"ブラウザを起動できませんでした。");
		return;
	}

	st.listener = std::move(listener);
	OAuthListener* l = st.listener.get();
	OAuthResult* out = &st.loginResult;
	HWND hDlg = st.hDlg;
	st.loginThread = std::thread([l, out, hDlg, state]() {
		*out = l->Wait(state, kLoginTimeoutSec);
		::PostMessageW(hDlg, WM_APP_LOGIN_DONE, 0, 0);
	});
	st.loggingIn = true;
	UpdateUserUi(st);
	SetStatus(st, L"ブラウザで Box にログインし、アクセスを許可してください...");
}

void OnLoginDone(DialogState& st)
{
	if (st.loginThread.joinable()) st.loginThread.join();
	st.listener.reset();
	st.loggingIn = false;
	OAuthResult res = std::move(st.loginResult);
	st.loginResult = OAuthResult();

	::SetForegroundWindow(st.hDlg);
	if (!res.ok) {
		SetStatus(st, res.error);
		UpdateUserUi(st);
		return;
	}
	std::wstring err;
	HCURSOR old = ::SetCursor(::LoadCursor(nullptr, IDC_WAIT));
	bool ok = st.req.client->CompleteLogin(res.code, err);
	::SetCursor(old);
	SecureClear(res.code);
	if (!ok) {
		ShowError(st, err);
	} else if (!err.empty()) {
		SetStatus(st, err);  // ログインはできたが保存に失敗した等の警告
	} else {
		SetStatus(st, L"ログインしました");
	}
	UpdateUserUi(st);
	BuildTree(st);
}

// ---------------------------------------------------------------------------
// 共有リンクの作成

void OnCreateLink(DialogState& st)
{
	HWND hDlg = st.hDlg;
	Config& cfg = *st.req.cfg;
	BoxClient& client = *st.req.client;

	Node* node = SelectedNode(st);
	if (!node || node->root || node->file) {
		::MessageBoxW(hDlg, L"共有するフォルダを選択してください。", kTitle, MB_OK | MB_ICONINFORMATION);
		return;
	}

	const bool expire = IsChecked(hDlg, IDC_EXPIRE_CHK);
	BOOL valid = FALSE;
	const int days = (int)::GetDlgItemInt(hDlg, IDC_EXPIRE_DAYS, &valid, FALSE);
	if (expire && (!valid || days < 1 || days > 3650)) {
		::MessageBoxW(hDlg, L"有効期限の日数は 1～3650 の範囲で指定してください。", kTitle, MB_OK | MB_ICONINFORMATION);
		return;
	}
	const bool otp = IsChecked(hDlg, IDC_OTP_CHK);
	const std::vector<std::wstring> emails = CheckedAddresses(st);
	if (otp && emails.empty()) {
		::MessageBoxW(hDlg, L"ワンタイムパスコードで許可するメールアドレスを 1 件以上チェックしてください。", kTitle, MB_OK | MB_ICONINFORMATION);
		return;
	}
	const bool canDownload = IsChecked(hDlg, IDC_DOWNLOAD);

	if (!otp && !expire) {
		if (::MessageBoxW(hDlg, L"ワンタイムパスコードも有効期限も設定されていません。\r\n"
		                        L"リンクを知っている人なら誰でも、期限なくアクセスできます。\r\n\r\nこのまま作成しますか？",
		                  kTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
			return;
		}
	}

	// 既存の共有リンクの状態（ツリーから選んだ場合は一覧取得時に判明しているので API を呼ばない）
	std::wstring err;
	if (!node->sharedKnown) {
		BoxFolder f;
		HCURSOR old = ::SetCursor(::LoadCursor(nullptr, IDC_WAIT));
		bool ok = client.GetFolder(node->id, f, err);
		::SetCursor(old);
		if (!ok) { ShowError(st, err); UpdateUserUi(st); return; }
		node->sharedLink = std::move(f.sharedLink);
		node->sharedKnown = true;
		if (!f.name.empty()) node->name = f.name;
	}
	const json& existing = node->sharedLink;
	const bool hasExisting = existing.is_object();
	const std::string existingOtp = OtpMode(existing);

	// 送信する shared_link を組み立てる
	std::wstring expireDisplay;
	json sl = {
		{"access", "open"},
		{"permissions", {{"can_download", canDownload}}},
	};
	if (expire) {
		sl["unshared_at"] = ExpireTimestamp(days, expireDisplay);
	} else if (hasExisting && existing.contains("unshared_at") && !existing["unshared_at"].is_null()) {
		sl["unshared_at"] = nullptr;  // 既存の期限を解除
	}

	size_t removedCount = 0;
	if (otp) {
		// OTP とパスワードは併用できない
		if (JBool(existing, "is_password_enabled")) sl["password"] = nullptr;

		// 再設定のときは既存の許可リストをすべて消し、今回チェックしたアドレスだけにする
		// （allowlist は指定すると全体が置き換わるので、今回の分だけを送ればよい）
		if (existingOtp == "allowlist") {
			auto it = existing["one_time_passcode"].find("allowlist");
			if (it != existing["one_time_passcode"].end() && it->is_array()) removedCount = it->size();
		}
		json allow = json::array();
		std::set<std::string> seen;
		for (const auto& addr : emails) {
			std::string v = WideToUtf8(addr);
			if (seen.insert("email:" + v).second) allow.push_back(json{{"type", "email"}, {"value", v}});
		}
		if (allow.size() > kMaxAllowlist) {
			::MessageBoxW(hDlg, L"許可リストが Box の上限（1000 件）を超えます。", kTitle, MB_OK | MB_ICONWARNING);
			return;
		}
		sl["one_time_passcode"] = {{"mode", "allowlist"}, {"allowlist", allow}};
	} else if (existingOtp != "none") {
		sl["one_time_passcode"] = {{"mode", "none"}};
	}

	if (hasExisting) {
		std::wstring msg = L"このフォルダには既に共有リンクがあります。\r\n"
		                   L"リンクの URL は変わらず、設定が次のように更新されます。\r\n\r\n";
		msg += L"・アクセス: " + AccessLabel(JStr(existing, "access")) + L" → リンクを知っている全員";
		msg += otp ? L"（メール認証あり）\r\n" : L"\r\n";
		msg += L"・有効期限: " + (expire ? expireDisplay + L" まで" : std::wstring(L"なし")) + L"\r\n";
		if (otp) {
			msg += L"・許可リスト: 既存の " + std::to_wstring(removedCount) + L" 件をすべて削除し、今回の "
				+ std::to_wstring(emails.size()) + L" 件に置き換え\r\n";
		} else if (existingOtp != "none") {
			msg += L"・ワンタイムパスコード: 解除\r\n";
		}
		msg += L"\r\n既にこのリンクを受け取った相手にも影響します。続けますか？";
		if (::MessageBoxW(hDlg, msg.c_str(), kTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
	}

	std::string url;
	HCURSOR old = ::SetCursor(::LoadCursor(nullptr, IDC_WAIT));
	bool ok = client.UpdateSharedLink(node->id, sl, url, err);
	::SetCursor(old);
	if (!ok) {
		ShowError(st, err);
		UpdateUserUi(st);
		return;
	}

	st.req.resultText = ComposeInsertText(url, expire ? expireDisplay : std::wstring(), otp);

	// 次回の既定値と「最近使ったフォルダ」を保存（ファイル書き込みのみ。API は呼ばない）
	cfg.expireEnabled = expire;
	if (expire) cfg.expireDays = days;
	cfg.otpEnabled = otp;
	cfg.canDownload = canDownload;
	AddRecentFolder(cfg, node->id, node->path);
	SaveUserSettings(st.req.iniPath, cfg);

	::EndDialog(hDlg, IDOK);
}

// ---------------------------------------------------------------------------

void OnInitDialog(DialogState& st)
{
	HWND hDlg = st.hDlg;
	const Config& cfg = *st.req.cfg;
	st.hTree = ::GetDlgItem(hDlg, IDC_TREE);
	st.hList = ::GetDlgItem(hDlg, IDC_ADDRS);

	ListView_SetExtendedListViewStyle(st.hList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
	RECT rc{};
	::GetClientRect(st.hList, &rc);
	LVCOLUMNW col{};
	col.mask = LVCF_WIDTH;
	col.cx = rc.right - rc.left - ::GetSystemMetrics(SM_CXVSCROLL);
	::SendMessageW(st.hList, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&col));
	for (const auto& r : st.req.recipients) AddAddress(st, r, true);

	::SendDlgItemMessageW(hDlg, IDC_EXPIRE_SPIN, UDM_SETRANGE32, 1, 3650);
	::SetDlgItemInt(hDlg, IDC_EXPIRE_DAYS, (UINT)cfg.expireDays, FALSE);
	::CheckDlgButton(hDlg, IDC_EXPIRE_CHK, cfg.expireEnabled ? BST_CHECKED : BST_UNCHECKED);
	::CheckDlgButton(hDlg, IDC_OTP_CHK, cfg.otpEnabled ? BST_CHECKED : BST_UNCHECKED);
	::CheckDlgButton(hDlg, IDC_DOWNLOAD, cfg.canDownload ? BST_CHECKED : BST_UNCHECKED);
	UpdateExpireUi(st);
	UpdateOtpUi(st);

	UpdateUserUi(st);
	BuildTree(st);

	if (!st.req.client->HasCredentials()) {
		SetStatus(st, L"環境変数 BOXLINK_CLIENT_ID / BOXLINK_CLIENT_SECRET を設定してください");
	} else if (!st.req.client->IsLoggedIn()) {
		SetStatus(st, L"「ログイン」を押して Box にログインしてください");
	} else if (!st.req.cfg->recent.empty()) {
		SetStatus(st, L"フォルダを選択してください");
	}
}

INT_PTR OnNotify(DialogState& st, NMHDR* hdr)
{
	if (hdr->idFrom != IDC_TREE) return FALSE;
	// 親が Unicode ダイアログなら W 版が届くが、念のため A 版も同じ扱いにする（使うメンバーの配置は同じ）
	if (hdr->code == TVN_ITEMEXPANDINGW || hdr->code == TVN_ITEMEXPANDINGA) {
		auto* nm = reinterpret_cast<NMTREEVIEWW*>(hdr);
		if ((nm->action & TVE_EXPAND) == 0) return FALSE;
		Node* node = reinterpret_cast<Node*>(nm->itemNew.lParam);
		if (!node || node->loaded || node->recent) return FALSE;
		if (!LoadChildren(st, nm->itemNew.hItem, node)) {
			::SetWindowLongPtrW(st.hDlg, DWLP_MSGRESULT, TRUE);  // 展開を取り消す
			return TRUE;
		}
		return FALSE;
	}
	if (hdr->code == TVN_SELCHANGEDW || hdr->code == TVN_SELCHANGEDA) {
		auto* nm = reinterpret_cast<NMTREEVIEWW*>(hdr);
		Node* node = reinterpret_cast<Node*>(nm->itemNew.lParam);
		std::wstring path = (node && !node->root) ? node->path : std::wstring();
		::SetDlgItemTextW(st.hDlg, IDC_SELPATH, path.c_str());
		if (node && node->file) {
			SetStatus(st, L"ファイルは表示のみです。共有リンクはフォルダに作成します");
		} else if (node && node->sharedLink.is_object()) {
			SetStatus(st, L"このフォルダには既に共有リンクがあります");
		}
		UpdateOkButton(st);
	}
	return FALSE;
}

INT_PTR CALLBACK ShareDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	auto* st = reinterpret_cast<DialogState*>(::GetWindowLongPtrW(hDlg, DWLP_USER));
	try {
		switch (msg) {
		case WM_INITDIALOG:
			st = reinterpret_cast<DialogState*>(lParam);
			::SetWindowLongPtrW(hDlg, DWLP_USER, lParam);
			st->hDlg = hDlg;
			OnInitDialog(*st);
			return TRUE;

		case WM_COMMAND:
			if (!st) break;
			switch (LOWORD(wParam)) {
			case IDOK:
				// アドレス入力欄で Enter を押した場合は「追加」として扱う
				if (::GetFocus() == ::GetDlgItem(hDlg, IDC_ADDR_NEW) && !GetText(hDlg, IDC_ADDR_NEW).empty()) {
					OnAddAddress(*st);
				} else {
					OnCreateLink(*st);
				}
				return TRUE;
			case IDCANCEL:
				st->StopLogin();
				::EndDialog(hDlg, IDCANCEL);
				return TRUE;
			case IDC_LOGIN:
				OnLoginButton(*st);
				return TRUE;
			case IDC_REFRESH:
				RefreshSelected(*st);
				return TRUE;
			case IDC_SHOWFILES:
				ShowFiles(*st);
				return TRUE;
			case IDC_ADDR_ADD:
				OnAddAddress(*st);
				return TRUE;
			case IDC_EXPIRE_CHK:
				UpdateExpireUi(*st);
				return TRUE;
			case IDC_EXPIRE_DAYS:
				if (HIWORD(wParam) == EN_CHANGE) UpdateExpireUi(*st);
				return TRUE;
			case IDC_OTP_CHK:
				UpdateOtpUi(*st);
				return TRUE;
			}
			break;

		case WM_NOTIFY:
			if (!st) break;
			return OnNotify(*st, reinterpret_cast<NMHDR*>(lParam));

		case WM_APP_LOGIN_DONE:
			if (st) OnLoginDone(*st);
			return TRUE;

		case WM_DESTROY:
			if (st) st->StopLogin();
			break;
		}
	} catch (const std::exception&) {
		// 例外を Becky! 本体へ伝播させない
		::MessageBoxW(hDlg, L"予期しないエラーが発生しました。", kTitle, MB_OK | MB_ICONERROR);
	}
	return FALSE;
}

} // namespace

INT_PTR ShowShareDialog(HINSTANCE hInst, HWND hParent, ShareRequest& req)
{
	INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS};
	::InitCommonControlsEx(&icc);
	DialogState st(req);
	return ::DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_SHARE), hParent, ShareDlgProc, reinterpret_cast<LPARAM>(&st));
}
