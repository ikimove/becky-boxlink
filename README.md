# Becky! BoxLink

Becky! Internet Mail のメール作成画面から Box にログインし、フォルダの共有リンクを作成して本文に挿入するプラグインです。

- 共有リンクに **有効期限** と **ワンタイムパスコード（メール認証）** を任意で設定できます
- ワンタイムパスコードで許可するメールアドレスは、作成中メールの宛先（To / Cc / Bcc）から自動で取り込みます
- Box API の呼び出しは必要最小限です（フォルダは展開したときだけ取得、共有リンクは 1 回の呼び出しで作成）

## 動作環境

- Becky! Internet Mail 2.8 以降（32bit 版）
- Windows 10 / 11
- Box の Business プラン以上（ワンタイムパスコードを使う場合）

## インストール

1. [Actions](../../actions) または [Releases](../../releases) から `BoxLink-x86` をダウンロードします
2. `BoxLink.dll` を Becky! の `PlugIns` フォルダ（例: `C:\Becky!\<ユーザー>\PlugIns\`）にコピーし、Becky! を再起動します
3. メール作成画面の本文を右クリック →「Box 共有リンクを作成...」を選びます
4. 初回は `<データフォルダ>\PlugIns\BoxLink.ini` が作られます。下記の Box アプリの情報を設定してください

## Box アプリの準備（管理者・初回のみ）

1. [Box Developer Console](https://app.box.com/developers/console) で「Platform App」→「ユーザー認証 (OAuth 2.0)」のアプリを作成
2. 「構成」タブで次を設定
   - リダイレクトURI: `http://localhost:53682/callback`
   - アプリケーションスコープ: 「Box に格納されているすべてのファイルとフォルダへの読み取り/書き込み」
3. クライアントID・クライアントシークレットを `BoxLink.ini` の `[box]` に設定
4. 社内の Box 管理者による承認が必要な場合は、管理コンソールでアプリを有効化してもらいます

## 使い方

1. 「ログイン」を押すとブラウザが開きます。Box にログインしてアクセスを許可してください（次回以降は不要）
2. 共有するフォルダを選びます（「最近使ったフォルダ」からも選べます）
3. 有効期限・ワンタイムパスコード（許可するアドレス）・ダウンロード可否を設定し、「作成して挿入」を押します
4. 本文のカーソル位置にリンクが挿入されます（クリップボードにもコピーされます）

既に共有リンクがあるフォルダでは URL は変わらず設定だけが更新されます。ワンタイムパスコードの許可リストは既存の分を残したまま宛先を追加します。

## ファイルの場所

| ファイル | 内容 |
|---|---|
| `<データフォルダ>\PlugIns\BoxLink.dll` | プラグイン本体 |
| `<データフォルダ>\PlugIns\BoxLink.ini` | 設定（client_id など）と最近使ったフォルダ |
| `<データフォルダ>\PlugIns\BoxLink\token.dat` | ログイン情報（Windows の DPAPI で暗号化。同じ Windows ユーザーでのみ復号可能） |

## ビルド

GitHub Actions（`.github/workflows/build.yml`）で `main` への push ごとにビルドされます。`v*` タグを push すると Release が作成されます。

ローカルでビルドする場合（Visual Studio 2022 Build Tools と CMake が必要）:

```
cmake -S . -B build -A Win32
cmake --build build --config Release
```

設計の詳細は [docs/DESIGN.md](docs/DESIGN.md) を参照してください。
