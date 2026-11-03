# 待办

## 百度网盘：原片直链还没拿到

点百度行现在下的是**服务端转码档**（1.27 GB / 962 段 / 1920×804 @约 821 kbps），不是原片（那条 24.4 GB 的 4K HDR10 DTS）。

卡在哪：

- 分享侧 `POST /api/sharedownload?type=dlink` 对图片回明文 dlink，对**视频回 4140 字符 base64 → 3104 字节高熵密文**，密钥不在分享页 HTML、也不在页面 bundle 里（`download-all.js` / `common-chunks` 全无 AES/atob/decrypt）——那是给桌面客户端的。这条已证。
- 转存进他自己的盘之后，`api/list` 的 56 条里 **0 条带 dlink**，`/api/download` 和 `download2` 都回 **errno=2**。但那一发是**缺 sign 的失败**：`/api/gettemplatevariable` 只回 `timestamp` + `username`，`sign=MISSING`。所以"原片拿不到"**不能算已证否**，得先把 `/api/download` 要的 `sign` 的来路逆出来。

要做的：逆 `sign`（或找到它现在的正确出处），拿到 dlink 后用自家引擎测一次吞吐，再决定"默认下载原片"能不能换掉转码档。换掉时行的名字后缀、`partialPlayable` 的容器判据都要跟着重量（原片是尾索引 mp4，边下边看对它不成立）。

## 迅雷：地址能拿，拉不动 —— 引擎把"连接被拒"当成了"任务失败"

梯子已经落地并在屏上走通（点迅雷行 → 31.8 s 后下载列表出现一行，名字带对的扩展名）。卡的是拉字节这一步：

- 实测同一条签名地址：**1 路 / 4 路 / 8 路全 206；12 路 = 9~10 发 206 + 2~3 发 503**。上限在"这一条地址同时能开几路"，不是档位、不是头（那颗地址裸发 `Range: bytes=0-63` 就回 206 + 正确 `Content-Range`）。
- 引擎现在的行为：`run()` 里任何一个块出错就 `errc <- err` 并让那个 worker 退出，`errc` 容量 1 ⇒ 整件中止。加了 5 次退避重试（`fetchWithRetry`，只重 5xx/429 和传输错，4xx 照旧立即失败）之后，实测仍然死：那 2 路被拒的 worker 一直在抢同一个不存在的第 11 槽，5 次用完就整件判死。引擎自己的读数：`rate` 爬到 **2.9 MB/s、activePeers=10**，然后 `ERROR 块 4 http 503` ×2，进程退出。
- **已改（11-03 02:00 后）**：`run()` 里一个块出错不再整件中止 —— 429/416/5xx 把块**退回队列**并**少开一路**（地板 = min(4, 设的数)），传输错另给 6 次退避。实测你那行 3.25 GiB 的地址：改前 12 路 = **0 字节落盘**；改后 12 路 40 s 落了 **117 MB / 14 块**，路数日志 `并发降到 11…6…5…4` 收到地板；单独 4 路起 45 s **零拒绝、零降路、1.03 MB/s**。
- **已解决（11-03 03:14）**：截断/读断不再是六次就判死。现在只有一件事能立刻杀死任务 —— 一个**非 429/416 的 4xx**（那枚 412+262 B 页就是它的证据）；其余（5xx、429、416、`块 N 只到 x/y 字节`、`块 N 读断`）走同一条路：块退回队列、还有路就赔一路、到地板就 1.5 s 一等，连错 40 次才认输。实测同一条 720P 地址：改前 12 路 = 0 字节后判死；改后 150 s **活着**，落了 **16,484,344 B**，`err=-`。
- **新的根因（11-03 03:12 量到，这条决定迅雷的天花板）**：迅雷给免费 720P 转码的那条地址**不发整片**。同一个 461,561,620 B 的文件，按 64 字节 Range 逐偏移问：`0–23,619,000` 回 206、最后约 21 MB 回 206、**中间全部 416**（0 s、45 s、90 s 三问一样，窗口不会自己长）。不带 Range 的 GET 也只给 `bytes 0-23619000/461561620`。⇒ 那是**转码只按播放进度准备**，不是并发上限；416 从此不赔路、不计入判死，改成 20 s 一问。代价实测：4 路里 3 路在等 park ⇒ `conns=1`、128 KB/s。
- **所以迅雷这条要他定**：原片 3,506,359,332 B 的 mkv 在 `paid` 那两个 media 里（`medias=4 free=2 paid=2`），他的账号不是会员。免费档能拿的只有"已准备的那一段"。三个选项：(a) 继续等服务器准备（行永远不结束）；(b) 拿完已发的段落就停，行上写「只拿到 X%，中间段服务器不发」；(c) 优先点原片那一档，拿不到就在行上写「这条要会员」。
- **这条改动还没回归夸克/百度**：判据 = 1/4/8/16 路的旧读数（1.11/2.78/2.81/2.75 MB/s）不变差，且百度那 962 段的表还能拼完。
- 顺带记着：这一家有些分享给的是**原片**（`medias=1`、无 `resolution_name`、首 32 字节 `1a 45 df a3` = Matroska、大小就是分享里那个 19,252,448,518 B），有些只给 720P/480P 转码 TS（首字节 `47 40 11 10`）。行的扩展名按这个分，别写死。

## 夸克：整条链搬进我们自己的浏览器（客户端不再是前提）

他一句「没有客户端 走网页版啊」把这条线换了地基：登录、Cookie、转存、取地址全走我们为了
迅雷/百度已经开起来的那扇浏览器（9455，profile `panlogin`）。`netdisk.cpp` 里
`clientExecutable / findPort / ensurePage / pageOnPort / launchClient / clientVersion`
六颗函数已删，`online.cpp` 里"夸克分享行唤起客户端"那一跳也删了（现在和普通网盘行一样开默认
浏览器）。夸克的登录判据不再是 Cookie 名字（`_UP_*` 匿名态也在），改成它自己的回答：
`GET drive.quark.cn/1/clouddrive/member?pr=ucpro&fr=pc` —— 没登录回
`401 {"code":31001,"message":"require login [guest]"}`。

- **已量到（11-03 02:47，屏上）**：设置卡「网盘登录」三行同形状 —— 夸克「未登录」+「去登录」，
  迅雷「已登录 · 记录于 11-02 23:18 · 74 条」+「退出登录」，百度「…03:34 · 98 条」+「退出登录」。
  点夸克那颗：`pl launch provider=quark exe=chrome.exe` → `pl list n=6` → `pl probe provider=quark
  code=31001 signed=0`（每 2 s 一发），按钮当场从「去登录」变「等待登录中」、状态行「正在等你在
  浏览器里登录…」，而迅雷那行不动。
- **顺手治了一条真故障**：`PanLogin::setBusy` 原来是 `if (busy() == on) return;` —— 而 `m_waiting`
  在 `startPoll` 里已经先置好了，所以这个判断永远相等、`busyChanged` 永远不发；结果状态行会跟着
  `stateChanged` 改字，按钮那行绑定却不重算，等待十分钟里它一直写着「去登录」。改成让 `setBusy`
  自己持有那个标志。按钮的「等待登录中」同时改成只看自己那一行的状态句（`busy` 是三行共用的一颗
  标志，否则等迅雷时夸克也说自己"等待登录中"）。
- **还没量**：`save / task / file/download` 这三步要他扫完码才有会话。已确认的是前两步在网页 tab
  里就答：`--netdisk-probe` 走 `pan.quark.cn/s/<12 位分享号>` 打出 `nd token code=0`、
  `nd detail n=4 pick=5618601231 bytes`，然后停在 `夸克还没登录：设置-网盘登录里点「去登录」`。
  `file/download` 的 `sys=win32&ve=<客户端版本>` 两个参数跟着客户端一起删掉了 —— 它在网页 tab
  上到底要不要，**没量过不许说不要**，等他登录后按同一把尺子复量。
- **重复点「去登录」会多开一扇标签**：实测攒到 5 扇 `pan.quark.cn`（每次点击都 `chrome.exe <url>`，
  已起来的实例只新开一扇并把窗口带到前面）。我把多余的 4 扇按 `/json/close` 收了，留了记录器
  正附着的那一扇。要做的修法：`login()` 先读 `/json/list`，域名已有页面就**不带 url** 启动（只
  唤起窗口），别再造一行。这条对三家都成立，改完要连迅雷/百度一起复量。

## UC 网盘：整条链匿名走得通，不登录、不转存、不欠副本

他一句「uc 已经在谷歌浏览器登录，不要再让我操作」，量出来的答案是：**根本不用他的登录**。

- **接口是夸克同族**：`pc-api.uc.cn/1/clouddrive/...?pr=UCBrowser&fr=pc`。分享列表不是夸克那对
  token+detail，而是**一发** `POST share/sharepage/v2/detail`，body 键
  `pwd_id,passcode,force,page,size,fetch_banner,fetch_share,fetch_total,sort,pdir_fid,pdir_fid_token,stoken,web_platform,banner_platform`；
  回答 `data.token_info.stoken`(44 位) + `data.detail_info.list[]`（`dir` 项带 `share_fid_token`、
  `include_items`、`more_than_one_layer`）。下钻把 `pdir_fid`+`pdir_fid_token`+`stoken` 一起带上。
- **地址**：`POST file/download` body `{fids:[fid], fid_token_list:[share_fid_token], pwd_id, stoken}`
  → `code=0` + `download_url`（Aliyun OSS 签名，`dl-uf-zb.pds.uc.cn`，query 键
  `Expires,OSSAccessKeyId,Signature,x-oss-traffic-limit,response-content-disposition,callback-var,dfi,callback,ud`）。
  少了 share 三件套 → `31001 require login [share missing]`；`sharepage/download` 不存在（404）。
- **读**：那条地址裸取回 **403 `RequestDeniedByCallback`**；带上**发起那一发的标签页自己的 Cookie**
  （匿名态的 9 条里只有 2 条命中那个 host，`header_len=401`）就 **206 + `bytes 0-63/1338670508`**。
  ⇒ 和夸克一样，门槛是 Cookie，但 UC 的匿名 Cookie 就够了，不需要谁去扫码。
- **端到端实测（04:0x，`--netdisk-live https://drive.uc.cn/s/<13 位分享号> --dl-dir probe/dl-scratch`）**：
  `nd in uc` → `nd uc walk seen=3 files=2 code=0 size=1338670508 url=1676` →
  `nd cookies host=dl-uf-zb.pds.uc.cn jar=100 sent=2` → `nd uc ready bytes=1338670508 headers=401 name_chars=9`
  → `downloads spawn id=3 kind=http conns=4 name=02 4K.mkv` → 120 s 后盘上
  `parts=160 done=2 prefix=8,366,691`，速率约 **0.5 MB/s（4 路，免费档）**。全程没有转存，所以没有副本要还。
- **还没做/还没量**：① 屏上点通（墙上一行 UC → 详情栏「正在问UC要地址…」→ 下载行）——代码分支按
  `online.cpp:105` 已有的 `kind==="uc"` 接上了，但**没在屏上量过**；② 8/16 路会不会像迅雷那样开始拒；
  ③ 一条分享里多集怎么挑（现在按“最大的 ≤8 GB”，一部剧 14 集时会拿错，需要他定规则：拿最大的一集还是整包）；
  ④ 设置卡「网盘登录」里**故意没有 UC 行**——它不需要会话，加一行就是他不许的那种占位。

## 分享行验活（`da9c7ad` 起；2026-11-03 改成后台静默扫）

- **判据**（三家各自认的行为，不是 http 码）：夸克/UC `token`/`v2/detail` 回
  `code ∈ {41006, 41011, 41012}`（http 404）；迅雷 `share_status=NOT_FOUND`（http 仍是 200）；
  **百度不判**。
- **两条钥匙**：判据落在 `sh:<家>:<分享号>`，墙滤的是 `hop:<行号>`，一次判死同时写两条。
  账本 = `AppData/Roaming/local/Playerui/share-dead.json`，只存键、码和时间。
- **现在怎么跑**（他点开一部就自己跑，没有按钮、没有提示）：只扫**当前页签**，切页签重新起扫；
  一次一条、1.5 s 一拍；站点回 429 就歇 60 s 且**不写状态行**；本轮答过活的行不再问。
  实测（蜜蛛侠 百度(110)，1300x723 @2540,1420）：前 35 s 问了 **23 条全不重复**；切去夸克再切回来
  又问 **26 条**，与那 23 条**交集 0**；整个过程 `hop 429` = 0 次，屏上那条橙字没出现。
- **撤掉的界面**：「验一遍」那颗、状态行的「验完了，收起 N 条失效」「在验这 N 条…」、
  页签上的「已收 N」。页签的数现在就是列表里真有的行数（`shownCount`）。
- **点一行点到死的那一行**：`noteDead()` 现在先发 `dead(url, code)` 再写账本（键已存在也发，
  否则「分享号已知道死、行号还没记」的那一行永远不会消失），界面按 `hopByUrl` 把它收掉。
- **新量到的一条，没敢用**：百度梯子里挂掉的行回 **`errno=140` + `「阿哦，链接出错了」`**（屏上那行
  橙字就是它）。只见过一次，不敢当判据——把它当死会把好行永久藏掉，那是比误报更靠不住的错。
  要也得先多抓几条真死的百度行把 140 和 116 对上号。
- **还欠**：① 没有“反悔”的入口，判死只能删账本文件；② 百度还没有判据；
  ③ 一页 331 条要问到天涯（1.5 s/条 ≈ 8 分钟），要不要只验屏幕里露出来的那几行得他定；
  ④ 验活脚本 `probe/deadcheck.mjs` 没进仓库（probe/ 一向零跟踪）。

## 会话走了自己重记（`22d2f28`）

- 设置 → 网盘登录 第四行「会话到期自动重记」，存 `HKCU\Software\local\Playerui\netdisk\autoRelogin`。
- 只有迅雷有钟：它的 bearer 实测 12.0 h，头到 **11 h** 自己重记一次；其他两家阈值写 0，因为没量过它们的钟。
- 静默跑的两个硬规矩：不抬窗（跳过 `Page.bringToFront`）、等待上限 2 分钟（手动那 10 分钟不动）；
  失败退 1 小时再试，行上写「自动没记上，点一下重记」。
- 屏上量过 ON：头 12.0h → **0.03h**，前台始终是 Player；OFF：头 12.00h → **12.01h**，零声。
- **第一遍量出来的错**：`loginQuiet()` 先置标志再调 `login()`，而 `login()` 是手动那扇门，进门就把标志复位
  —— 那次自动重记实际按手动跑的（10 分钟 + 会抬窗）。改成直接 `startLogin(provider, true)`。
