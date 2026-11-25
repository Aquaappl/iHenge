# CLAUDE.md — Henge に vphone（仮想 iPhone）を載せるプロジェクト

このファイルは、Henge のフォークで作業する Claude Code 向けの設計書と作業指示です。
最初に全部読んでから、「マイルストーン」の一番上の未完了のものから進めてください。

## ゴール

Henge（iPhone 上で macOS を動かす JIT エミュレーター）に、vphone-cli が作る仮想 iPhone
（`vresearch101` プラットフォーム、`vphone600ap` の iOS 26/27）を起動するモードを足す。
最終的には iPhone 上の Henge アプリで iOS を起動する。
開発と検証は、手元の M1 Mac mini（8 GB / 256 GB）で `hengevm` を動かして行う。
**最終的に動かす端末は iPhone 13 mini**（A15、RAM 4 GB）。Henge の要件（A14 以降）は満たすが、
RAM が少ないのが一番の制約（下の「iPhone 13 mini の制約」）。

## 決まっている方針（理由つき）

- **Henge をベースにする**。Henge は arm64 を arm64 に JIT し、ハイパーバイザーを使わないので
  iOS アプリの中で動く。準仮想化 GPU のコマンドを同じプロセス内で Metal に再生できる
  （`devices/pvg_lite.c`, `devices/pvg_metal.m`）。
- **移植元は qemu-vphone（vphone-linux の QEMU フォーク）**。`vresearch101` マシン、GXF/SPRR、
  SEP コプロセッサ、メールボックス、SEP ストレージ、タッチスクリーン、vsock がすでに動いている。
- **QEMU を丸ごと iOS に持っていく案は採らない**。vphone-linux の GPU（reims-vgpu）は
  GPU タスクを別プロセスで動かす前提で、`process-isolation` を切ると動かない。iOS アプリは
  子プロセスを作れない。
- **メモリマップはほぼ共通**。vresearch101 のデバイスツリーは、QEMU の `vmapple` にあるデバイスを
  すべて同じアドレスに置いている。Henge は `vmapple` のメモリマップで作られている
  （`devices/vmapple.h`）。差分は追加デバイスだけ（下の表）。
- **EL2/EL3 は不要**。qemu-vphone も AP コア、SEP コアともに `has_el2=false`, `has_el3=false`。
  Henge の EL0/EL1 モデルのまま、GXF のガード実行レベル（GL1）を足せばよい。

## 参照するリポジトリ

| 名前 | URL | 使い方 |
|---|---|---|
| Henge | https://github.com/Leviidev/Henge | 本体（フォークして作業する） |
| qemu-vphone | https://github.com/YahiaAngelo/qemu-vphone （branch `vphone`） | 移植元。`hw/vmapple/`, `target/arm/`, `hw/arm/apple-silicon/a13_gxf.c` |
| vphone-linux | https://github.com/YahiaAngelo/vphone-linux | `docs/ARCHITECTURE.md` が最重要資料。起動ログの比較にも使う |
| vphone-cli | https://github.com/Lakr233/vphone-cli | VM の作成。`Research/` に起動フローとパッチの資料 |
| OpenAVB | https://github.com/owngoal-dev/OpenAVB | AP のブート ROM。`docs/external-contract.md` にメモリマップ |
| OpenSEP | https://github.com/owngoal-dev/OpenSEP | SEP のブート ROM。`docs/platforms.md` の `vresearch1` 行 |

`../refs/` に読み取り専用で clone しておくこと（`git clone --depth 1`、qemu-vphone は
`--filter=blob:none --sparse` で `hw/vmapple hw/arm/apple-silicon target/arm include/hw/vmapple`）。

## 守ること

- **Apple のファームウェアをリポジトリにも CI にも入れない**。IPSW、`Disk.img`, `nvram.bin`,
  `SEPStorage`, AVPBooter などは `~/vphone-vms/` のようなリポジトリ外に置き、`.gitignore` する。
  GitHub Actions の成果物やログにも出さない。
- **ホストのセキュリティ設定は変えない**。SIP、boot-args、`csrutil` などが必要になったら、
  手順を書いてユーザーに頼む（vphone-cli のホスト設定は、ユーザーがリカバリーモードで行う）。
- **ライセンス**。Henge は GPL-2.0-or-later。qemu-vphone のうち `hw/vmapple/vresearch101.c`,
  `vpiop.c`, `sep-storage.c`, `avp-rtc.c`, `vusb*.c` は GPL-2.0-or-later なので移植してよい
  （ファイル先頭に出典と SPDX を残す）。`hw/arm/apple-silicon/*`（`a13_gxf.c` を含む）と
  ChefKissInc 由来の `target/arm` の変更は **AGPL-3.0**。コピーすると、この部分を含む成果物は
  AGPL-3.0 の条件になる。個人で使う分には問題になりにくいが、配布するなら要注意。
  **既定では AGPL のコードはコピーせず、挙動を理解して Henge の流儀で書き直す**。
  コピーしたほうがいい場面が来たら、作業を止めてユーザーに確認する。
- 変更は小さなコミットに分ける。`make test` を通してからコミットする。
- macOS ゲストの起動を壊さない。新しいモードは `--machine vresearch101` で切り替え、
  既定は今の `vmapple` のまま。

## 足りないもの（Henge と vresearch101 の差分）

### 1. マシン定義（易）

新規 `devices/vresearch101.c`。`devices/vmapple.c` の cfg、bdif、aes、virtio-blk、GIC、
PL011、PCIe、xHCI をそのまま使い、以下を足す（アドレスは qemu-vphone の `memmap[]`）。

| デバイス | アドレス | SPI | 内容 |
|---|---|---|---|
| スクラッチ RAM | `0x00410000`, size `0x2f0000` | – | vmapple にはない。AVPBooter と iBoot が作業領域に使う |
| cfg の `rnd`（`+0x38`） | cfg 内 | – | 読むたびに新しい乱数。定数だと sepOS の TRNG が止まる |
| cfg の board ID | – | – | `0x90` |
| avp-rtc | `0x30240000`, `0x1000` | `0x13` | `hw/vmapple/avp-rtc.c` を移植 |
| SEP メールボックス（vpiop） | `0x30250000`, `0x1000` | `0x14`（AP 側。INTID は +32） | `hw/vmapple/vpiop.c` を移植。AP 側と SEP 側の 2 バンク |
| avp-ctrr | `0x30260000`, `0x10000` | – | 未実装デバイス（読み 0、書き無視、ログだけ） |
| RAM | `0x70000000` から | – | 既定 8 GB。まず 4 GB で試す |

- ブート ROM は **OpenAVB**（VM バンドルの `OpenAVB.bin`）を `0x00100000` に置く。
  Henge の `BootROM.swift` が使う AVPBooter（macOS 14.6.1 の物）は使わない。
- `nvram.bin` は Virtualization.framework の補助ストレージ（先頭 16 KiB がヘッダー）。
  Henge の `hv_vm_create` はすでに `aux_offset` 既定 16384 で開くので、そのまま渡せるはず。
  qemu-vphone は同じファイルを pflash と aux の virtio-blk の両方に見せている。
- タイマーは 24 MHz（Henge は `CNTFRQ 24000000` で一致済み）。
- GIC の MSI フレーム（`0x1fff0000`）の `MSI_TYPER` bit 31 を立てる（AppleVirtualPlatform が見る）。

### 2. GXF と SPRR（難。一番大事）

SPTM と TXM は Apple 独自の「ガード実行レベル」で動く（TXM のイメージ名が
`TrustedExecutionMonitor_Guarded`）。Henge のコアにはこれがまったくない。

**命令**（EL1 でのみ有効。EL0 では未定義命令）

- `GENTER` = `0x00201420`、`GEXIT` = `0x00201400`。
  qemu-vphone の `disas_apple_insn()`（`target/arm/tcg/translate-a64.c`）では、bit 31 が 0、
  bits 28:25 が 0、bits 15:10 が 5、bits 9:5 が 1 なら GENTER、0 なら GEXIT。
- 同じグループに Apple の `WKdmC`/`WKdmD`（opcode 2/3、メモリ圧縮）もある。iOS が使うかは
  未確認。未定義命令の例外で止まったら、まずこれを疑う。
- `core/classify.c` は今これらを `HV_INSN_UNDEFINED` にする。新しい種類 `HV_INSN_APPLE` を足し、
  ヘルパー呼び出しで処理してブロックを抜ける。

**意味**

- `GENTER`：`GXF_ENTER_EL1` の番地へ飛び、ガード状態に入る。シンドロームを `ESR_GL1` に書く。
  戻り先と PSTATE を GL1 側のレジスタ（`ELR_GL1`, `SPSR_GL1`）に保存する。
- `GEXIT`：`SPSR_GL1` と `ELR_GL1` から戻る。`ASPSR_GL1` の bit 0 が 1 なら GL に戻る
  （qemu-vphone の `HELPER(gexit)`）。
- ガード中の例外は `VBAR_GL1` へ（`GXF_ABORT_EL1` も関係する）。詳細な振り分けは
  qemu-vphone の `target/arm/helper.c` の `EXCP_GENTER` 付近と、ガード中の例外処理を読むこと。

**レジスタ**（`a13_gxf.c` の `apple_a13_gxf_cp_reginfo[]` に名前とエンコーディングの一覧がある）

- `GXF_CONFIG_EL1`, `GXF_STATUS_EL1`, `GXF_ENTER_EL1`, `GXF_ABORT_EL1`
- GL1 バンク：`ASPSR_GL1`, `SP_GL1`, `TPIDR_GL1`, `VBAR_GL1`, `SPSR_GL1`, `ESR_GL1`, `ELR_GL1`, `FAR_GL1`
  （M1 のエンコーディング `S3_6_C15_C10_n` の別名もある：`apple_gxf_define_m1_gl_cpregs`）
- SPRR：`SPRR_CONFIG_EL1`, `SPRR_CONFIG_EL0`, `SPRR_EL0BR0/1_EL1`, `SPRR_EL1BR0/1_EL1`
- MPRR：`MPRR_EL0BR0/1_EL1`, `MPRR_EL1BR0/1_EL1`, `S3_6_C15_C3_5`

**vphone-linux が踏んだ罠**（`docs/ARCHITECTURE.md` の apple-gxf の節）

- **M1 方式にする**。ガード中に別のレジスタに差し替える EL1 レジスタは `VBAR_EL1` だけ。
  A13 方式で `ELR/SPSR/ESR/FAR_EL1` まで GL 版に差し替えたら、SPTM の FIQ からの戻りが
  無限ループした。
- SPRR の権限の番号は、リーフディスクリプターの AP/UXN/PXN から作る
  （`(ap << 2) | (xn << 1) | pxn`）。テーブル階層の XN は実行権限を消すだけ。
- GENTER は `ESR_GL` を書く。GEXIT は `ASPSR` に従う。
- 全コアの `cntfrq` を 24 MHz に（sepOS が確かめる）。

**Henge での実装場所**

- `core/cpu.h`：`HvCpu` の「C からだけ触る」部分（`struct HvMachine *m;` より後）に
  `guarded` フラグと上記のレジスタを足す。翻訳済みコードが固定オフセットで読む前半は触らない。
- `core/machine.h`：`HV_BLOCK_GUARDED` フラグ。ガード中に翻訳したブロックを、通常の EL1 の
  ブロックと混ぜない。
- TLB：`HvCpu.tlb` は `[EL0, EL1, EL1+PAN, unused]` の 4 表。**空いている 4 番目を GL1 用**に使う
  （`hv_tlb_code` と、表を選ぶ箇所すべて）。
- `core/mmu.c` の `hv_mmu_walk()`：今は AP ビットで権限を計算している（`int ap = (desc >> 6) & 3;`
  の後）。SPRR が有効なら、番号を作って `SPRR_EL{0,1}BR*` から EL ごと、ガード中かどうかで
  権限を引く。SPRR/MPRR のレジスタへの書き込みでは TLB を全部捨てる。
- `core/sysreg.c`：`read_reg()`/`write_reg()` に上記のレジスタを足す。
- `core/exception.c` の `hv_take_exception()` と、`core/dispatch.c` にある `hv_eret()` に
  ガード状態を通す。GENTER/GEXIT のヘルパーは `core/exception.c` に置く。
- `core/translate.c`：EL1 の MRS/MSR を直接読み書きする高速パスが、ガード中も正しいか確認する
  （M1 方式なら `VBAR_EL1` 以外は共通なので、ほぼそのままでよいはず）。

### 3. SEP コプロセッサ（中〜難）

SEP は、**専用のアドレス空間と専用の GIC を持つ 2 つ目のコア**。

- SEP 専用の領域：ROM `0x02000000`（size `0x80000`、OpenSEP）、SRAM `0x04000000`（`0x800000`）、
  TZ0 `0x06000000`（`0x2000000`、sepOS の置き場所）、VSTD `0x08000000`（`0x4000`、RAM）、
  SEP ストレージのレジスタ窓 `0x30020000`（`SEPStorage` ファイル。`hw/vmapple/sep-storage.c`）、
  `0x30220000` は SEP からは未実装デバイス（AP からは AES）。
- それ以外のシステムのアドレス空間（メールボックス、AP の RAM）は AP と共有。
- SEP の GIC は、AP と同じ物理アドレスにある **別の 1 コア用 GICv3**。SEP コアの MPIDR の
  affinity は 0。メールボックスの SPI は `0x34/0x35`（INTID）。
- SEP コアも `apple-gxf`（GXF/SPRR を使う）。リセット時の PC は `0x02000000`。

**Henge での設計案**

- **SEP 用に 2 つ目の `HvMachine` を作る**。`ram[]` には AP の RAM と同じホストメモリを指す
  エントリ（`hv_machine_add_ram` に同じ `host` ポインタを渡す）と、SEP 専用の領域を入れる。
  `mmio[]` には SEP 専用のデバイス、SEP 用の GIC、AP と共有するデバイス（メールボックス）を入れる。
  JIT 領域、ブロック、TLB はマシンごとに別になる。
- 共有するデバイス（メールボックス）は 2 つのスレッドから呼ばれるので、自前のロックを持たせ、
  `HvMmio.self_locked = 1` にする。
- **最初の作業は監査**：`core/` と `host/vm.c` に、`HvMachine` を 2 つ同時に動かすと壊れる
  グローバル変数がないか（JIT 領域、シグナルハンドラー、`static` なデバイス状態、`ncpus` など）を
  洗い出して、マイルストーンの前にユーザーに報告する。
- 自己書き換えコードの検出は `HvRam` ごと。SEP は自分の TZ0 と ROM からしか実行しないはずなので、
  AP が共有 RAM に書いても SEP 側の翻訳には影響しない想定。外れたら要対応。
- SEP は WFE で待ってポーリングする。WFE で CPU を空回りさせない（Henge の WFE 処理を確認）。

### 4. タッチスクリーン（易〜中）

- Virtualization.framework の USB タッチスクリーン（`05ac:8108`、"Virtual USB Touch Screen"）を
  xHCI に出す。指 4 本、各指に transducer index、In Range、Touch、X、Y。
- iOS は「本体内蔵」のデジタイザーからしかタッチを受け付けない。xHCI のルートポートの
  PORTSC bit 30（Device Removable = 取り外し不可）を立てる。qemu-vphone はハブの下につないでいる。
- ホームは、下端からのスワイプで USB キーボードの Cmd+H を送る（iOS はこのタッチスクリーン
  からホームジェスチャーを始めない）。
- Henge の Magic Trackpad 2（`devices/xhci.c`）の作りを流用する。

### 5. vsock（後回し）

vphoned（vphone-cli のゲスト側デーモン）がポート 1339 で使う。起動そのものには不要。
仮想キューは 256 エントリ、リングはページ境界にそろえる（iOS のドライバーの前提）。

### 6. GPU（リスク）

- Mac の `hengevm` は ParavirtualizedGraphics.framework を使う（`devices/pvg_mac.m`）。
  iPhone では `pvg_lite` + `pvg_metal`。**iOS ゲストで動くかは未確認**。
- vphone-cli の調査では、iOS 27 は画面表示に IOMFB の `_virt_*` という別の経路を使う
  （`Research/0_binary_patch_comparison.md` の SwapEnd の項）。まず iOS 26 で試す。
- vphone-linux では GPU の MMIO は `0x30200000`（SPI `0x11`）と IOSurface マッパー
  `0x30210000`（SPI `0x10`）。Henge の vmapple と同じはず。

## iPhone 13 mini の制約

- **RAM 4 GB**。アプリが使える量はそれより少ない。macOS 用の Henge は 4/6/8 GB をゲストに渡すが、
  13 mini では 4 GB でも無理。増量メモリのエンタイトルメントがないと、Henge はゲストの RAM を
  ファイルに置く方式に切り替わる（`.github/workflows/build-henge.yml` の冒頭コメント）。
  遅くなるが、動く可能性はある。
- そのため、**ゲストの RAM をどこまで減らせるか**を早めに測る。Mac で `hengevm` を 4 GB → 3 GB →
  2 GB と下げて、iOS がどこまで起動するか記録する（M7 の後でよい）。vphone-cli 側も
  `vm config --memory` で同じ値にして比べる。
- 同時に、ゲスト以外のメモリ（JIT 領域、翻訳ブロック、TLB、SEP 用の 2 つ目の `HvMachine`）も
  13 mini 向けに小さくできるか確認する。`HvCpu` は TLB だけで 1 コア 2 MB 以上ある。
- vCPU は少なめ（2〜4）で試す。

## マイルストーン

各マイルストーンの「完了の条件」を満たしたら、このファイルのチェックを付けてコミットする。

- [ ] **M0 足場**：Mac mini で `make` と `make test` が通る。`hengevm --install` で macOS を入れて
  起動できる。必要な Homebrew パッケージ（`libslirp` など）を `docs/` にメモする。
- [ ] **M1 VM を用意**：ユーザーが vphone-cli（Launchpad）で iOS 26.x の VM を作る。
  M1 で動かなければ、vphone-linux の `docs/CREATE-ON-LINUX.md` の手順を検討する。
  VM のフォルダーの中身（`Disk.img`, `nvram.bin`, `SEPStorage`, `OpenAVB.bin`, `OpenSEP.bin`,
  `config.plist`）と ECID をメモする。
- [ ] **M2 リファレンス**：qemu-vphone を Mac でビルドし（GPU なし、`GFX=0` 相当）、同じ VM を
  起動してシリアルログを `runs/ref/` に保存する。ここまでの起動ログを「正解」として使う。
  ビルドできなければ、ユーザーに伝えて M3 に進む。
- [ ] **M3 骨組み**：`--machine vresearch101` で OpenAVB が UART に `OpenBootROM:` を出す。
- [ ] **M4 iBoot**：LLB と iBoot が動き、iBoot のシリアル出力が出る。
- [ ] **M5 GXF**：SPTM と TXM が起動し、XNU のログが出始める。
  （GXF の単体テストを `tests/` に足す：GENTER → ガード中の MRS/MSR → GEXIT）
- [ ] **M6 SEP**：OpenSEP → sepOS が起動し、AP の `AppleSEPManager` とメールボックスで話せる。
- [ ] **M7 ユーザーランド**：launchd が動き、SpringBoard が表示される（Mac の GPU 経路で）。
- [ ] **M8 タッチ**：タップとスクロールが効く。
- [ ] **M9 iPhone**：IPA に組み込み、iPhone 13 mini で起動する（`pvg_lite` の経路）。
  IPA は GitHub Actions の `Build Henge IPA` で作る（下の「GitHub Actions」）。

## デバッグのやり方

- **リファレンスと並べる**：同じ VM を qemu-vphone で起動したログと、Henge のログを比べて、
  最初にずれた箇所を探す。qemu-vphone は GXF の出入りをログに出す（`gxf: tagged enter`,
  `gxf: exit snapshot`）ので、Henge にも同じ形式のログを `HENGE_GXF_LOG=1` で出せるようにする。
- Henge には `--stats`、直近の例外の出入りの記録（`HvStats.elchg`）、直近のブロック
  （`HvStats.recent`）がある。止まったらまずこれを出す。
- 未知のシステムレジスタは、まず「読むと 0、書いても無視」にしてログを出し、一覧を
  `docs/vresearch101-sysregs.md` に貯める。
- 起動が遅い場所は、qemu-vphone の TCG での時間（vphone-linux の `docs/PERFORMANCE.md`）と比べる。

## GitHub Actions（ビルドはここで行う）

- `.github/workflows/engine.yml`：main への push ごとに、`xcode-27` ランナーで `make` と
  `make test` を回し、`hengevm` を成果物にする（約 2 分）。ハイパーバイザーを使わないので
  ランナーの中で動く。**コミットしたら、このワークフローが通ることを確認する**。
- `.github/workflows/build-henge.yml`：手動実行（Actions タブ → Build Henge IPA → Run workflow）で
  Husk の iOS 依存と IPA を作る（約 6 分）。失敗すると「Show build errors in context」に
  エラーの出どころが出る。
- ファームウェアを使うテストは CI に入れない。

## わかっていないこと（見つけたらここに書き足す）

- M1 Mac mini で vphone-cli が動くか（公式に記載なし）。
- iOS ゲストで `WKdmC/WKdmD` などの Apple 独自命令が使われるか。
- `HvMachine` を 2 つ同時に動かせるか（監査が必要）。
- iOS ゲストが何 GB の RAM まで減らして起動するか（13 mini は RAM 4 GB）。
- 13 mini の iOS のバージョンと、JIT を有効にする手段（StikDebug など）。
- Henge の `pvg_lite` が iOS ゲストの準仮想化 GPU のコマンドをそのまま再生できるか。
