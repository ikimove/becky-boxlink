# Becky! BoxLink（新版）設計メモ

## 目的

Becky! のメール作成画面から Box にログインし、フォルダを選んで共有リンクを作成する。
共有リンクには **有効期限** と **ワンタイムパスコード（メール認証）** を任意で設定できる。
ワンタイムパスコードで許可するメールアドレスは、作成中メールの宛先（To/Cc）から取得する。

## 方針

- **すべて C++ の 1 つの DLL**（Becky! プラグイン）で完結させる。Python / .NET の実行環境は不要。
- Becky! 2.8.3 は 32bit 版のため、**x86 (Win32) でビルド**する。
- ビルドは **GitHub Actions**（windows-latest + MSVC）。成果物の `BoxLink.dll` を Becky! の `PlugIns` に配置する。
- 旧 Python 版（`BoxLink/`）は参考のみ。リポジトリには含めない。

## 構成

```
Becky! (B2.exe, 32bit)
 └ PlugIns\BoxLink.dll
     ├ メニュー登録   … 作成画面の右クリックメニュー（BKC_MENU_COMPEDIT）
     ├ 宛先取得       … To / Cc ヘッダを Becky! API で取得しアドレスを抽出
     ├ ダイアログ      … Win32 ダイアログ（フォルダツリー + 共有設定）
     ├ Box API        … WinHTTP で REST 呼び出し、JSON 解析
     ├ OAuth 2.0      … ブラウザでログイン → localhost の一時待ち受けでコード受信
     └ トークン保存    … DPAPI（CryptProtectData）で暗号化してデータフォルダに保存
```

## 画面の流れ

1. 作成画面の右クリック →「Box 共有リンクを作成...」
2. 未ログインならブラウザで Box ログイン（初回のみ。以後はトークンを再利用）
3. ダイアログにフォルダツリーを表示（ルートのみ取得し、展開したときだけ子を取得）
4. フォルダを選択し、以下を設定
   - 有効期限：なし / N 日後（既定値は設定ファイル）
   - ワンタイムパスコード：オン / オフ。オンなら宛先から取得したアドレスを一覧表示し、チェックで選択（手入力の追加も可）
5. 「作成して挿入」→ 共有リンクを作成し、本文のカーソル位置にリンク（と有効期限）を挿入。クリップボードにもコピー
   - Becky! API の `CompSetText` は先頭/末尾にしか挿入できないため、クリップボード経由で本文エディタに `WM_PASTE` を送る
   - 貼り付けがうまくいかない環境向けに `[insert] mode=bottom`（末尾に追加）/ `clipboard`（コピーのみ）を用意

## Box API 呼び出しの節約

| 操作 | 呼び出し | 備考 |
|---|---|---|
| フォルダ展開 | `GET /folders/{id}/items?fields=type,name,modified_at,shared_link&sort=date&direction=DESC&limit=1000` | 展開した時だけ。結果はダイアログを閉じるまで保持。通常フォルダは「フォルダ→ファイル」順が保証されるため、ファイルが現れたら次ページを取得しない |
| ファイル表示 | `GET /folders/{id}/items?fields=type,name,size,modified_at,shared_link&sort=date&direction=DESC&limit=1000` | 「ファイル表示」ボタンを押した時だけ。未展開のフォルダなら同じ呼び出しでサブフォルダも登録する（展開時に再取得しない）。表示済みなら呼ばない |
| 最近使ったフォルダの共有状態 | `GET /folders/{id}?fields=name,shared_link` | 最近使ったフォルダから選んで作成するときだけ（ツリーから選んだ場合は一覧で判明済み） |
| 共有リンク作成 | `PUT /folders/{id}?fields=shared_link` | 1 回で設定と URL 取得を兼ねる |
| トークン更新 | `POST /oauth2/token` | 有効期限（約60分）の2分前を過ぎていたときのみ。想定外の 401 でも1回だけ更新して再試行 |
| ユーザー名取得 | `GET /users/me?fields=name,login` | ログイン時に1回だけ（保存して再利用） |

- 「最近使った項目」・共有状態の先読み・プレビュー等の裏で走る通信は持たない。
- 「最近使ったフォルダ」（最大10件）を設定ファイルに保存し、ツリーに表示する。ここから選べばルートから辿る通信が不要。最近使ったフォルダがある間は、ルートも展開されるまで読み込まない。
- ダイアログ下部に、そのダイアログで行った API 呼び出し回数を表示する。
- 一覧の再取得は「最新表示」ボタンを押した時だけ。
- フォルダ一覧はプロセス内に 10 分間キャッシュし、ダイアログを開き直しても API を呼ばない（ログアウト・別ユーザーで破棄）。共有リンクを作成したら、キャッシュ中の共有状態も更新する。最近使ったフォルダの共有状態や起点名も、キャッシュにあれば API を呼ばない。
- WinHTTP のセッションと接続はプロセス内で使い回し（Keep-Alive）、TLS ハンドシェイクとプロキシ検出を毎回やり直さない。gzip 圧縮と HTTP/2 を有効にする。ハンドルは `BKC_OnExit` で閉じる（`DllMain` では閉じない）。

## フォルダ一覧の起点

`[browse] root_folder_id` で一覧の起点を指定する（既定 0 = すべてのファイル）。起点の名前は ID が変わったときだけ `GET /folders/{id}?fields=name,shared_link` で1回取得し、`[cache] root_id / root_name` に保存して以後は API を呼ばない。起点ノードは共有対象外。最近使ったフォルダは、パスが `/<起点名>/` で始まるものだけ表示する（API を呼ばずに起点外を除くため）。

## 共有リンクの設定内容

```json
{
  "shared_link": {
    "access": "open",
    "unshared_at": "<有効期限 ISO8601>",
    "permissions": { "can_download": true },
    "one_time_passcode": {
      "mode": "allowlist",
      "allowlist": [ { "type": "email", "value": "<宛先アドレス>" } ]
    }
  }
}
```

- OTP は `access=open` が前提。パスワードとは併用不可。Business プラン以上。
- `allowlist` は指定すると全置換される。リンク再設定時は既存の許可リストを引き継がず、今回チェックしたアドレスだけを送る（確認ダイアログに削除件数を表示）。
- 既に共有リンクがあるフォルダの場合は上書き前に確認する。

## 設定ファイル（`<データフォルダ>\PlugIns\BoxLink.ini`）

SDK の推奨に従い、DLL と同じ名前で PlugIns フォルダに置く。日本語のフォルダ名を保存できるよう UTF-16LE（BOM 付き）で作成する。

```ini
[box]
redirect_port = 53682

[link]
expire_enabled = 1
expire_days = 14
otp_enabled = 1
can_download = 1
```

トークンは `<データフォルダ>\PlugIns\BoxLink\token.dat`（DPAPI 暗号化。同じ Windows ユーザーでのみ復号可能）。Box のリフレッシュトークンは使い捨てなので、更新のたびに一時ファイル経由で置き換えて保存する。

## メモリ管理の約束

- Becky! API が返す `LPSTR`（`SerializeRcpts` など）は `BeckyPtr`（`unique_ptr` + `bka.Free`）で受ける。`free` / `delete` は使わない
- `GetDataFolder` などが返すポインタは「次の API 呼び出しまで有効」なだけなので、解放せずにすぐコピーする
- WinHTTP ハンドル・ソケット・ファイルハンドル・DPAPI の出力バッファ（`LocalFree`）はすべて RAII 型で持つ
- クリップボードに渡した `HGLOBAL` は、`SetClipboardData` が成功したらシステムの所有になるので解放しない（失敗時のみ解放）
- ツリー項目の `lParam` が指す `Node` はダイアログ終了まで `vector<unique_ptr<Node>>` で保持する
- ログイン待ちスレッドは、ダイアログ終了時に必ず停止・`join` してから待ち受けオブジェクトを破棄する
- コールバックやダイアログプロシージャから例外を Becky! 本体へ伝播させない

## 確認事項（実機で検証が必要）

- 右クリックメニューのコールバックに渡る `hWnd` が作成画面のフレームか本文エディタか（`GetAncestor(GA_ROOT)` で両対応にしている）
- 本文エディタが `WM_PASTE` を受け付けるか（HTML メール作成時も含む）
- Box テナントで公開リンク（`access=open`）とワンタイムパスコードが管理者に許可されているか

## 参照

- Becky! Plug-in SDK: https://www.rimarts.jp/downloads/B2/B2PinSDK-26400.zip（`sdk/` に必要分のみ配置）
- Box API: `PUT /folders/{id}#add_shared_link`（`shared_link.one_time_passcode`）、`GET /folders/{id}/items`
