// PreloadViewController.swift — 预加载演示页：列出 3 个 URL，显示各自预加载
// 进度。
//
// 与另外两页同一条纪律：**一行 C 类型都没有**。数据来源是 SYPlayerPreloader，
// 唯一的"轮询"是一个 @MainActor 的 async 循环（预加载没有回调、没有 delegate，
// 统计只能拉），页面离开窗口就取消它，不留后台定时器。
//
// ## 这一页刻意要演示的四件事（都是容易被误解的语义）
//
// 1. **`add` 返回 `true` 不等于"下得下来"。** 它只说"条目已经进表"。返回
//    `false` 只覆盖三种同步查得出的情形（`.file` 源 / 空 `absoluteString` /
//    底层没建起来）；拼错的主机、404、没有后端的 scheme 统统返回 `true`
//    然后**异步**失败，唯一的观测口是 `statistics.failed`。所以第三行默认
//    填一条必然 404 的 URL：让"true 之后失败数 +1"这件事在页面上看得见。
// 2. **`providerMiss` 是"按时间预加载整体上有多少条目退化了"的观测口。**
//    它涨就说明那些条目退化成了按字节暖（默认 1 MiB）。这一页把它放在跟
//    entries 同一行，不藏在角落。
//    ⚠️ **订正**：上一版这里（以及三处库内文档）写的是"唯一
//    可观测信号"，那句话把一个**与缓存冷热有关**的量说成了 URL 的属性。
//    moov 在尾部的容器只退化**一次**：第一次探测放弃时拉下来的 ~2 MiB 前缀
//    留在缓存里，第二次探测从暖缓存起步只需再新下 49,669 B 就**成功**。
//    所以同一个 URL 冷态让它涨、热态不涨。
// 3. **`downloadedBytes` 会低报真实网络用量**，所以界面上标的是"条目源已下"
//    而不是"网络用量"。有**两类**字节确实走了网络、也确实落进了同一份缓存，
//    却不计入这个字段：
//      (a) 按时间预加载时底层先只读 header 探一次容器的那条 provider 探测源
//          （上界 kProbeMaxBytes = 2 MiB **每探一次**，不是每个 URL——失败的
//          探测不被记忆，同一个 URL 会被反复探测，实测连探三次合计
//          2,265,605 B > 2,097,152。本页那份 1,440,157 B 的
//          faststart MP4 实测落盘 524,288 B）；
//      (b) **每一张 HLS 播放列表**——fetch_text 为每张 m3u8 另开一次性源
//          （上界 kMaxPlaylistBytes = 8 MiB/张；本页实测 master 120 B +
//          media 271 B）。这一条是实测量出来的，
//          include/syplayer/syp_preload.h 的注释里原本只写了 (a)。
//    合起来实测一轮：统计报 545,588 B，本次落盘的数据文件 1,070,267 B，
//    低报 **524,679 B = 49.0%**。页面下方把这个差额实时算给你看。
//    ⚠️ 反方向同样要记住：这个字段**不是**缓存目录的占用量——目录里装的是
//    历次会话的字节，见下面 cacheLabel 那段注释。
// 4. **HLS 会把一条 URL 展开成多条条目**（master → media 播放列表 → EXT-X-MAP
//    → 前若干分片），所以 entries 会比填进去的 URL 多。第二行默认是 HLS。
//
// ## 为什么有"合并 / 分行"两种统计口径
//
// `SYPlayerPreloadStatistics` 是**对象级**的，没有"按条目"的口径——公开 API
// 里拿不到"第 2 条 URL 下了多少字节"。"显示各自预加载进度"与"一个
// SYPlayerPreloader 管三行"这两种设计是冲突的，只能二选一，所以这里
// 两种都给，用一个分段控件切：
//   · **合并**（默认）：一个 preloader 管三行。这是**真实业务里
//     唯一正确的用法**——三级优先级的全部意义就是在同一个 preloader 的额度
//     池里分配连接（maxTotalTasks / reservedForPlaying），跨对象不竞争。
//   · **分行**（各自展示进度）：每行一个 preloader，于是每行都有自己的
//     六个数。**这一半只是为了把"每条 URL 各自下了多少"显示出来，不是一种
//     可以抄进真实 App 的摆法**，代价比"优先级不跨行竞争"大得多，页面上
//     也照下面这三条写着：
//       (1) **并发额度悄悄变成 3 倍。** `Preloader::allocate_locked`
//           （src/dl/preloader.cpp）的 `budget = has_playing ? total :
//           total - reserve` 是**逐对象**算的：第 1 行有 .playing 条目 ⇒
//           budget 6，第 2/3 行各 3，聚合上界 **12**，而单个 preloader 是 6。
//       (2) **reservedForPlaying 的让路保证失效。** 按 syp_preload.h:46-53，
//           那 3 个额度是留给"**本对象之外**那条真正在播的流"的。三个互不
//           知情的池子各留各的 3 个，对外净效果是在播的那条流面对的竞争连接
//           从 3 变成 **9**——正好与设计意图相反。
//       (3) 实测：分行口径下聚合「在下载」出现过 **2**，合并口径全程不超过
//           **1**；且 src/dl/preloader.h:63 自己就写着多实例不在支持的拓扑内。
//     所以它能看清"每条 URL 下了多少"，但看不出优先级调度，而且照抄会让
//     预加载去抢自己播放的带宽。
// 四个 preloader 共用同一份 `SYPlayerCacheConfiguration`（同一个目录），
// 所以**缓存仍然是合一的**：任一口径暖过的字节，播放页用同一个地址打开
// 都会命中。
import SYPlayerKit
import UIKit

@MainActor
final class PreloadViewController: UIViewController {
    private enum Mode: Int {
        case merged = 0
        case perRow = 1
    }

    /// 一行 = 一条 URL 的全部 UI 与登记状态。
    ///
    /// `@MainActor` 不是装饰：嵌套类型**不会**继承外层的 actor 隔离，不标的话
    /// 这几个 UIKit 控件的属性初始化就是"在非隔离上下文里调主线程隔离的
    /// init"（编译器实测会为每个控件各报一条 warning）。
    @MainActor
    private final class Row {
        let field = UITextField()
        let priority = UISegmentedControl(items: ["背景", "下一个", "正在播"])
        let status = UILabel()
        let removeButton = UIButton(type: .system)
        /// 最近一次 `add` 成功登记的 URL；`remove` 要用它，没有它就不知道摘谁。
        var registered: URL?
        var note = "未填写"
    }

    private let rows: [Row] = (0..<3).map { _ in Row() }
    /// 「分行」那一格刻意带上"仅演示"：它满足"各自进度"这个展示需求，但把并发
    /// 额度变成 3 倍、让 reservedForPlaying 失效（见文件头「合并 / 分行」那段），
    /// 不是一个可以照抄的摆法。段标题是使用者第一眼看到的东西，代价不能只
    /// 写在注释里。
    private let modeControl = UISegmentedControl(items: ["合并（一个 preloader）",
                                                         "分行（每行一个·仅演示）"])
    /// 进程级下行限速。**用分段控件而非滑块**：四个离散档位
    /// 便于 `sendActions` 自检、也便于人眼确认——这是一处刻意偏差。
    /// 下标对应 Self.rateOptions。
    private let rateControl = UISegmentedControl(items: ["不限", "256 KB/s", "1 MB/s", "4 MB/s"])
    private let rateLabel = UILabel()
    private let startButton = UIButton(type: .system)
    private let clearButton = UIButton(type: .system)
    private let sampleButton = UIButton(type: .system)
    /// 预连接演示。对第一行 URL 输入框里当前的地址（用「用本地样例」
    /// 填入的即为样例服务器 URL）发一次 `SYPlayerNetwork.preconnect`。
    /// 预连接没有可观测的结果（尽力而为、静默失败），状态行
    /// 只能如实写"已发出"，不代表连接真的建成或被随后的下载复用。
    private let preconnectButton = UIButton(type: .system)
    private let preconnectStatusLabel = UILabel()
    private let hintLabel = UILabel()
    private let statsLabel = UILabel()
    private let notesLabel = UILabel()
    private let cacheLabel = UILabel()

    /// 四个 preloader 共用同一份缓存配置 ⇒ 同一个目录 ⇒ 同一份缓存索引。
    private let cache: SYPlayerCacheConfiguration
    private let mergedPreloader: SYPlayerPreloader
    private let rowPreloaders: [SYPlayerPreloader]
    private var mode: Mode = .merged

    private var pollTask: Task<Void, Never>?

    /// 缓存目录的一次测量。`total` 是目录里所有文件；`data` 只算 `.dat`，
    /// 也就是**真正下下来的那些字节**（`.idx` 是缓存索引元数据，本页实测
    /// 一轮 714 B）。低报的减法必须用 `data`，否则会把索引元数据算成"低报"。
    private struct CacheUsage {
        var total: Int64 = 0
        var data: Int64 = 0
    }

    private var lastCacheUsageAt = Date.distantPast
    private var lastCacheUsage = CacheUsage()
    /// **本次**预加载开始那一刻的缓存目录占用；nil = 本次还没开始过。
    /// 页面只显示"当前 − 基线"这个增量，理由见 cacheText 的注释。
    /// 「全部移除」与切换统计口径都会把它清掉——那两件事都结束了一次会话。
    private var cacheBaseline: CacheUsage?

    #if DEBUG
    private var sampleServer: PreloadSampleServer?
    private var sampleRoutes: PreloadSampleServer.Routes?
    /// 带上 ok：自检从"跑完就 exit(0)"变成**真的闸门**。上一版无论
    /// 发生什么都退 0，于是"5 条 addTarget 全被注释掉、一个控件都不响应"
    /// 实测仍然 EXIT=0 且输出与基线逐字节相同。
    private var autorunFinish: ((_ ok: Bool) -> Void)?
    /// 自检期间累计的判据结果。任何一条 `expect` 不成立就变 false，最后经
    /// 退出码表达（AppDelegate 那里 `exit(ok ? 0 : 1)`）。
    private var autorunOK = true
    #endif

    init() {
        let cache = SYPlayerCacheConfiguration()
        self.cache = cache
        // 参数一律用默认值（6 个连接总额度、给"正在播的那条流"留 3 个），
        // 两种口径下的 preloader 用同一套参数，免得数字不可比。
        // ⚠️ 注意这里一共建了 **4 个** preloader，而额度是**逐对象**算的：
        // 分行口径同时在跑时聚合上界是 3×（6 或 3）而不是 6，那 3 个
        // reservedForPlaying 也变成三份各留各的。这是演示口径的代价，
        // 真实 App 只该有合并口径那一个（见文件头）。
        self.mergedPreloader = SYPlayerPreloader(cache: cache)
        self.rowPreloaders = (0..<3).map { _ in SYPlayerPreloader(cache: cache) }
        super.init(nibName: nil, bundle: nil)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) {
        fatalError("这个页面只从代码构造（demo 不用 Storyboard）")
    }

    #if DEBUG
    deinit {
        // 页面被释放时这一行必须打出来。打不出来就说明
        // 有人把 self 强捕进了轮询循环（或别的长寿闭包），preloader 的析构
        // 会被一起吊住——而 SYPlayerPreloader 的 deinit 刻意把桥交给后台队列
        // 正是为了不在主线程上等驱动线程 join（实测就地析构 5,205ms）。
        print("[preload-autorun] PreloadViewController.deinit（页面已释放）")
        sampleServer?.stop()
    }
    #endif

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "预加载"
        view.backgroundColor = .systemBackground
        buildLayout()

        modeControl.selectedSegmentIndex = Mode.merged.rawValue
        modeControl.addTarget(self, action: #selector(modeChanged), for: .valueChanged)
        // 按进程当前的限速反推初始段位：限速是进程级的，别的页面或
        // 上一次进这页时可能已经改过，写死 0 会让段位显示「不限」而实际在限速。
        rateControl.selectedSegmentIndex = Self.segment(forRateLimit: SYPlayerNetwork.maximumDownloadRate)
        rateControl.addTarget(self, action: #selector(rateChanged), for: .valueChanged)
        rateLabel.font = .monospacedDigitSystemFont(ofSize: 12, weight: .regular)
        rateLabel.textColor = .secondaryLabel
        rateLabel.text = Self.rateStatusText(SYPlayerNetwork.maximumDownloadRate)
        startButton.setTitle("开始预加载", for: .normal)
        startButton.addTarget(self, action: #selector(startPreloading), for: .touchUpInside)
        clearButton.setTitle("全部移除", for: .normal)
        clearButton.addTarget(self, action: #selector(clearAll), for: .touchUpInside)
        sampleButton.setTitle("用本地样例", for: .normal)
        sampleButton.addTarget(self, action: #selector(fillLocalSample), for: .touchUpInside)
        preconnectButton.setTitle("预连接", for: .normal)
        preconnectButton.addTarget(self, action: #selector(preconnectSample), for: .touchUpInside)
        preconnectStatusLabel.numberOfLines = 0
        preconnectStatusLabel.font = .monospacedDigitSystemFont(ofSize: 11, weight: .regular)
        preconnectStatusLabel.textColor = .secondaryLabel
        preconnectStatusLabel.text = "尚未预连接"

        for (i, row) in rows.enumerated() {
            row.field.borderStyle = .roundedRect
            row.field.placeholder = "URL \(i + 1)（http/https，或 .m3u8）"
            row.field.autocapitalizationType = .none
            row.field.autocorrectionType = .no
            row.field.keyboardType = .URL
            row.priority.selectedSegmentIndex = 1
            row.status.numberOfLines = 0
            row.status.font = .monospacedDigitSystemFont(ofSize: 11, weight: .regular)
            row.status.textColor = .secondaryLabel
            row.removeButton.setTitle("移除本行", for: .normal)
            row.removeButton.tag = i
            row.removeButton.addTarget(self, action: #selector(removeRow(_:)), for: .touchUpInside)
        }

        hintLabel.numberOfLines = 0
        hintLabel.font = .systemFont(ofSize: 12)
        hintLabel.textColor = .secondaryLabel
        #if DEBUG
        hintLabel.text = "点「用本地样例」：在 127.0.0.1 上起一台只读的样例服务器（不打外网），"
            + "并填入 MP4 / HLS / 一条必然 404 的地址各一条。"
        #else
        sampleButton.isEnabled = false
        sampleButton.setTitle("用本地样例（仅 DEBUG）", for: .normal)
        hintLabel.text = "Release 构建不内置样例服务器，请自己填 http/https 地址。"
        #endif

        statsLabel.numberOfLines = 0
        statsLabel.font = .monospacedDigitSystemFont(ofSize: 13, weight: .regular)
        notesLabel.numberOfLines = 0
        notesLabel.font = .systemFont(ofSize: 11)
        notesLabel.textColor = .secondaryLabel
        notesLabel.text = Self.notesText
        cacheLabel.numberOfLines = 0
        cacheLabel.font = .monospacedDigitSystemFont(ofSize: 11, weight: .regular)
        cacheLabel.textColor = .secondaryLabel

        render()
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        // 预加载没有回调，统计只能拉。用 async 循环而不是 Timer：它天然绑在
        // @MainActor 上，页面一走就随 Task 取消，不会漏一个定时器。
        pollTask?.cancel()
        pollTask = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                // **刻意不写 `guard let self = self else { return }`**：
                // 那个绑定的作用域一直延伸到 while 体结束，把 self 强持有跨过
                // 下面那次 await，于是页面释放最多被推迟一个轮询周期。用可选
                // 链调一次方法，强引用只存活于这一次调用期间；返回 nil 就说明
                // 页面已经没了，循环自己退出——所以即使 viewDidDisappear 没被
                // 调到（例如被直接从 tab 里摘掉），也不会留下一个永久空转的
                // 循环。
                //
                // 【这个写法有多少证据，如实记】变异实测：把 `[weak self]` 换成
                // 强捕获**并**删掉 viewDidDisappear 里的 `cancel()` ⇒ deinit
                // 那行消失（被杀）；但只把本行换回 `guard let self = self else
                // { return }`（保留 [weak self] 与 cancel）⇒ **变异存活**。
                // 所以有鉴别力的是 `[weak self]` + `cancel()` 这一对，**不是**
                // 本行这个惯用法。本行声称的收益是"最多早一个 300ms 周期放手"，
                // 那是个时间差，而自检唯一的判据是"deinit 那行在不在"，收尾的
                // 1.0 秒窗口把 300ms 整个吞掉了。留着它是因为它确实更早放手，
                // 不是因为有测试钉着它。
                if self?.render() == nil { return }
                try? await Task.sleep(nanoseconds: 300_000_000)
            }
        }
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        pollTask?.cancel()
        pollTask = nil
    }

    // MARK: - Actions

    @objc private func modeChanged() {
        let next = Mode(rawValue: modeControl.selectedSegmentIndex) ?? .merged
        guard next != mode else { return }
        // 切口径 = 换一套 preloader。不把旧口径的条目留着：留着的话页面上显示的
        // 是新口径的数，而旧口径的驱动线程还在偷偷下东西，是在骗人。
        mergedPreloader.removeAll()
        rowPreloaders.forEach { $0.removeAll() }
        for row in rows {
            row.registered = nil
            row.note = "换统计口径后已清空登记，重新点「开始预加载」"
        }
        // 换口径 = 上一次会话结束了，缓存基线作废：不清掉的话新口径的"本次
        // 新增"会把旧口径下的字节也算进去。
        cacheBaseline = nil
        mode = next
        render()
    }

    /// 进程级限速是全局状态，与「口径」「行」无关，不需要 render() 兜底——
    /// 状态行只由这一个方法写，写完立即刷新，不等下一轮轮询。
    @objc private func rateChanged() {
        SYPlayerNetwork.maximumDownloadRate = Self.rateLimit(forSegment: rateControl.selectedSegmentIndex)
        rateLabel.text = Self.rateStatusText(SYPlayerNetwork.maximumDownloadRate)
    }

    /// 对第一行 URL 输入框里当前的地址发一次预连接。「用本地样例」会把
    /// 样例服务器的 MP4 地址填进第一行，所以点「用本地样例」之后再点这个
    /// 按钮，打的正是当前样例服务器 URL；也可以手填任意 http/https 地址。
    @objc private func preconnectSample() {
        guard let text = rows.first?.field.text?.trimmingCharacters(in: .whitespacesAndNewlines),
              !text.isEmpty, let url = URL(string: text) else {
            preconnectStatusLabel.text = "预连接：第一行没有可用的 URL"
            return
        }
        SYPlayerNetwork.preconnect(url)
        preconnectStatusLabel.text = "已发出预连接：\(url.absoluteString)"
    }

    @objc private func startPreloading() {
        // 【基线】在任何一次 add **之前**量一次目录占用，之后
        // 页面只显示"本次新增"。走不节流的那一份：1 秒的陈旧读数会把本次刚
        // 下的字节算进基线里，基线一偏，低报那个数就又不可信了。
        let before = measureCacheUsage()
        var added = 0
        for (i, row) in rows.enumerated() {
            guard let text = row.field.text?.trimmingCharacters(in: .whitespacesAndNewlines),
                  !text.isEmpty else {
                row.note = "未填写"
                continue
            }
            guard let url = URL(string: text) else {
                // URL(string:) 自己就解析不出来的串，连 add 都不用调。
                row.note = "URL 解析失败（连 add 都没调）"
                continue
            }
            let priority = Self.priority(forSegment: row.priority.selectedSegmentIndex)
            // seconds: 3 —— 按时间暖 3 秒。估不出时长的 URL 会退化成按字节暖
            // 1 MiB，并让 providerMiss 加一（见页面下方的说明）。
            let ok = preloader(forRow: i).add(.url(url), priority: priority, seconds: 3)
            if ok {
                row.registered = url
                row.note = "add → true（已登记，不代表下得下来：404/DNS/无后端 scheme"
                    + " 一律 true 后异步失败，只体现在「失败」上）"
                added += 1
            } else {
                row.registered = nil
                row.note = "add → false（只有三种：.file 源 / 空 URL 串 / 底层没建起来）"
            }
        }
        if added == 0 {
            hintLabel.text = "没有可预加载的 URL —— 本地文件不需要预加载，请填 http/https 地址"
                + "（或点「用本地样例」）。"
        }
        // 一次会话只取第一次的基线：再点一次「开始预加载」是往同一次会话里
        // 追加条目，重新取基线会把前面已经下的字节当成"开始前就在的"。
        if added > 0, cacheBaseline == nil { cacheBaseline = before }
        render()
    }

    @objc private func clearAll() {
        mergedPreloader.removeAll()
        rowPreloaders.forEach { $0.removeAll() }
        for row in rows {
            row.registered = nil
            row.note = "已请求全部移除（异步生效，条目数可能慢一拍才归零）"
        }
        // 同上：一次会话结束，基线作废。
        cacheBaseline = nil
        render()
    }

    @objc private func removeRow(_ sender: UIButton) {
        let i = sender.tag
        guard i >= 0, i < rows.count else { return }
        guard let url = rows[i].registered else {
            rows[i].note = "本行没有已登记的条目，移除是空操作"
            render()
            return
        }
        preloader(forRow: i).remove(.url(url))
        rows[i].registered = nil
        rows[i].note = "已请求移除（异步生效；已下的字节留在缓存里，下次续传）"
        render()
    }

    #if DEBUG
    @objc private func fillLocalSample() {
        Task { @MainActor [weak self] in
            guard let self = self else { return }
            do {
                let routes = try await self.ensureSampleServer()
                self.fill(with: routes)
            } catch {
                self.hintLabel.text = "本地样例服务器起不来：\(error)"
            }
        }
    }

    private func fill(with routes: PreloadSampleServer.Routes) {
        rows[0].field.text = routes.mp4.absoluteString
        rows[0].priority.selectedSegmentIndex = 2      // 正在播
        rows[1].field.text = routes.hlsMaster.absoluteString
        rows[1].priority.selectedSegmentIndex = 1      // 下一个
        rows[2].field.text = routes.missing.absoluteString
        rows[2].priority.selectedSegmentIndex = 0      // 背景
        hintLabel.text = "已填入本地样例：① MP4（可估时长）② HLS master（会展开成多条条目）"
            + "③ 必然 404（演示 add→true 之后异步失败）。素材是 bundle 里的 sample.mp4，"
            + "不打外网。"
        render()
    }

    private func ensureSampleServer() async throws -> PreloadSampleServer.Routes {
        if let routes = sampleRoutes { return routes }
        let server = try PreloadSampleServer.withBundledSample()
        let routes = try await server.start()
        sampleServer = server
        sampleRoutes = routes
        return routes
    }
    #else
    @objc private func fillLocalSample() {}
    #endif

    // MARK: - Render

    /// 轮询循环靠 `self?.render() == nil` 判断"页面还在不在"——可选链求值出来的
    /// 是 `Void?`，页面没了就是 nil。见 viewWillAppear 的注释。
    private func render() {
        let perRowStats = rowPreloaders.map { $0.statistics }
        // 当前口径报出来的「条目源已下」。缓存那一段要拿它跟磁盘增量做减法，
        // 所以必须取**正在显示的那一份**，不能两处各算各的。
        var reported: Int64 = 0
        switch mode {
        case .merged:
            let s = mergedPreloader.statistics
            reported = s.downloadedBytes
            statsLabel.text = "【合并口径】三行共用一个 SYPlayerPreloader\n" + Self.statsText(s)
            for (i, row) in rows.enumerated() {
                row.status.text = "第 \(i + 1) 行　\(row.note)"
            }
        case .perRow:
            var total = SYPlayerPreloadStatistics()
            for s in perRowStats {
                total.entries += s.entries
                total.activeTasks += s.activeTasks
                total.downloadedBytes += s.downloadedBytes
                total.completed += s.completed
                total.failed += s.failed
                total.providerMiss += s.providerMiss
            }
            reported = total.downloadedBytes
            statsLabel.text = "【分行口径·仅演示，别照抄】三行合计"
                + "（每行一个 SYPlayerPreloader）\n"
                + "⚠️ 额度是逐对象算的：第 1 行有「正在播」条目 ⇒ 额度 6，"
                + "第 2/3 行各 3，聚合上界 12（单个 preloader 是 6）；"
                + "reservedForPlaying 那 3 个本该留给本对象之外真正在播的那条流，"
                + "现在三个池子各留各的 ⇒ 在播的流面对的竞争连接 3 → 9。\n"
                + "⚠️ 优先级也不跨行竞争。这一栏只为看清「每条 URL 各自下了多少」。\n"
                + Self.statsText(total)
            for (i, row) in rows.enumerated() {
                row.status.text = "第 \(i + 1) 行　\(row.note)\n" + Self.statsText(perRowStats[i])
            }
        }
        cacheLabel.text = cacheText(reported: reported)
    }

    /// 直接量一次缓存目录占了多少字节。**不走节流**——基线必须是精确的那一刻。
    ///
    /// 【为什么值得多写这十行】`downloadedBytes` 低报是写在头文件注释里的一句话，
    /// 没人会因为一句注释就相信它。把磁盘上真正新增的字节摆在旁边，两个数一对照，
    /// 低报这件事就从"文档声明"变成了页面上看得见的差值。
    private func measureCacheUsage() -> CacheUsage {
        var usage = CacheUsage()
        let keys: [URLResourceKey] = [.fileSizeKey]
        if let walker = FileManager.default.enumerator(at: cache.directory,
                                                       includingPropertiesForKeys: keys) {
            for case let url as URL in walker {
                let size = Int64((try? url.resourceValues(forKeys: Set(keys)).fileSize) ?? 0)
                usage.total += size
                if url.pathExtension == "dat" { usage.data += size }
            }
        }
        return usage
    }

    /// 给渲染用的那份，节流到 1 秒一次：轮询是 300ms 一轮，每轮都去遍历目录
    /// 纯属浪费。
    private func cacheUsageThrottled() -> CacheUsage {
        if Date().timeIntervalSince(lastCacheUsageAt) < 1.0 { return lastCacheUsage }
        lastCacheUsageAt = Date()
        lastCacheUsage = measureCacheUsage()
        return lastCacheUsage
    }

    /// 缓存目录那一段文字。
    ///
    /// 【为什么必须显示"本次增量"而不是"目录总占用"】这一页原先印的是
    /// "目录实际占用 X KiB……两者的差就是低报的那部分"——那句话**只在缓存
    /// 目录刚被清空时成立**。而这个目录默认 TTL
    /// 7 天、上限 512 MiB，还与两个播放页共用，里面装的是**历次会话**的字节。
    /// 实测同一台机器不清缓存连跑两次（样例服务器每次换端口 ⇒ cache key 变 ⇒
    /// 真的重下一份）：
    ///     第 1 次  目录 0 → 1,070,981 B，页面声称低报 524,679 B（真值，对）
    ///     第 2 次  目录 1,070,981 → 2,141,962 B，照老写法页面会声称低报
    ///              1,594,946 B，而**真实低报仍是 524,679 B——夸大 3.0×**，
    ///              而且每跑一次、每在播放页放一个视频还要再涨约 1 MB，无上界。
    /// 所以：开始预加载时记一个基线，只用增量做减法。目录总量照样显示，但明确
    /// 标成"含历次会话、不是本次下了多少"——把"总量为什么不是答案"也一起演示
    /// 出来，这比换个标签更有教学价值。
    private func cacheText(reported: Int64) -> String {
        let now = cacheUsageThrottled()
        let kib = { (b: Int64) in String(format: "%.1f", Double(b) / 1024) }
        var text = "缓存目录（四个 preloader 与播放页共用）：\(cache.directory.path)\n"
            + "目录当前总占用 \(kib(now.total)) KiB —— 一个缓存目录里装的是"
            + "**历次会话**的字节（默认 TTL 7 天、上限 512 MiB，两个播放页也往"
            + "里写），所以这个总数不等于本次下了多少，**不能**直接拿它去减"
            + "「条目源已下」。\n"
        guard let base = cacheBaseline else {
            return text + "还没有本次基线（没开始预加载，或刚「全部移除」/换过口径）"
                + " ⇒ 算不出本次增量。"
        }
        let grown = now.data - base.data
        text += "本次开始前目录已占用 \(kib(base.total)) KiB ⇒ 本次新增数据文件 "
            + "\(kib(grown)) KiB。\n"
        guard grown != 0 || reported != 0 else {
            return text + "（本次一个字节都没走网络——要的都命中了以前暖好的缓存，"
                + "这正是预加载存在的意义 ⇒ 这一轮没有可算的低报。）"
        }
        guard grown >= reported else {
            return text + "（新增比「条目源已下」还少：本次多半命中了以前暖好的"
                + "缓存——统计只增不减，磁盘却不会重复写 ⇒ 这一轮算不出低报。）"
        }
        let under = grown - reported
        let pct = grown > 0 ? Double(under) / Double(grown) * 100 : 0
        return text + "而「条目源已下」只报了 \(kib(reported)) KiB ⇒ **本次低报 "
            + "\(kib(under)) KiB = \(under) 字节**（占新增的 "
            + "\(String(format: "%.1f", pct))%）：provider 只读 header 那条探测源 + "
            + "每张 HLS 播放列表，都不计入那个字段。"
    }

    private func preloader(forRow i: Int) -> SYPlayerPreloader {
        mode == .merged ? mergedPreloader : rowPreloaders[i]
    }

    /// rateControl 的段位 → `SYPlayerNetwork.maximumDownloadRate` 该写的值。
    /// 下标必须与 `rateControl` 的四个 item 逐一对应。
    private static func rateLimit(forSegment index: Int) -> Int? {
        switch index {
        case 1:  return 256 * 1024        // 256 KB/s
        case 2:  return 1024 * 1024       // 1 MB/s
        case 3:  return 4 * 1024 * 1024   // 4 MB/s
        default: return nil               // 不限
        }
    }

    /// `rateLimit(forSegment:)` 的反推：`SYPlayerNetwork.maximumDownloadRate` → 段位。
    /// 规则：`nil`（不限）→ 0；恰为某一档 → 那一档；不在四档内的正数 → 按**比值**
    /// 最接近的一档（|ln(v / 档位)| 最小；三档成几何级数，按差值会偏向大档），
    /// 并列取较小的一档。只影响段位显示——状态行读的是实际值，两者不一致时
    /// 看状态行。不在档内的值不会被这一步改写，只有用户点选才会写回。
    private static func segment(forRateLimit v: Int?) -> Int {
        guard let v, v > 0 else { return 0 }
        var best = 1
        var bestDist = Double.infinity
        for i in 1...3 {
            guard let tier = rateLimit(forSegment: i) else { continue }
            let d = abs(log(Double(v) / Double(tier)))
            if d < bestDist {
                best = i
                bestDist = d
            }
        }
        return best
    }

    /// 状态行文字：直接读 `SYPlayerNetwork.maximumDownloadRate`（消费者的
    /// getter，经 C 层），不是回显 rateControl 自己的段位——两者若不一致
    /// （例如 setter 接线断了），这一行会如实露出来。
    private static func rateStatusText(_ v: Int?) -> String {
        guard let v else { return "进程级下行限速：不限" }
        return "进程级下行限速：\(v) 字节/秒（约 \(String(format: "%.0f", Double(v) / 1024)) KB/s）"
    }

    private static func priority(forSegment index: Int) -> SYPlayerPreloader.Priority {
        switch index {
        case 0:  return .background
        case 2:  return .playing
        default: return .next
        }
    }

    /// 六个字段全显示。**一个都不省**：漏掉 providerMiss 就等于把"按时间预加载
    /// 到底生效没有"这个唯一信号藏起来了。
    static func statsText(_ s: SYPlayerPreloadStatistics) -> String {
        let kib = Double(s.downloadedBytes) / 1024
        return "条目 \(s.entries)　在下载 \(s.activeTasks)　已暖够 \(s.completed)"
            + "　失败 \(s.failed)　估不出时长 \(s.providerMiss)"
            + "　条目源已下 \(String(format: "%.1f", kib)) KiB"
    }

    private static let notesText = """
    · 「条目源已下」不是网络用量，它**低报**，而且漏掉的是**两类**字节（都真的走了网络、\
    也都落进了同一份缓存）：① 按时间预加载先只读 header 探一次容器的那条 provider 探测源，\
    **每探一次**最多少算 2 MiB（本页那份 1.4 MB 的 MP4 实测落盘 524,288 B）；\
    ② **每一张 HLS 播放列表**——抓 m3u8 用的是另开的一次性源，每张最多 8 MiB\
    （本页实测 master 120 B + media 271 B）。实测一轮：报 545,588 B，实际新增 1,070,267 B，\
    低报 49.0%。页面最下面那段按「本次增量」把这个差额实时算给你看。
    · ①那条的口径是「每探一次」，**不是「每个 URL」**：失败的探测不被记忆，同一个 URL \
    会被反复探测。实测 moov 在尾部的素材连探三次 = 2,215,936 + 49,669 + 0 B，\
    前两次合计 2,265,605 B，**超过一个 2 MiB 上界**。
    · 「条目源已下」同样**不是**缓存目录的占用量：目录里装的是历次会话的字节\
    （默认 TTL 7 天、上限 512 MiB，两个播放页也往里写），所以拿目录总量去减它会把\
    低报夸大好几倍——正因为如此，下面显示的是"本次开始以来新增了多少"。
    · 「估不出时长」= providerMiss。它涨说明那些条目退化成按字节暖 1 MiB；常见原因是 \
    moov 在文件尾、探测够到的字节超过上界，或者服务端根本没响应。\
    ⚠️ 它**依赖缓存冷热**，别拿它判断"某个 URL 支不支持按时间预加载"：moov 在尾部的容器\
    只退化**一次**——第一次探测拉下来的 ~2 MiB 前缀留在缓存里，第二次探测从暖缓存起步\
    只需再新下 49,669 B 就成功了。同一个 URL 冷态让它涨、热态就不涨。
    · 「失败」是异步失败的唯一观测口：add 返回 true 只代表条目进表了。
    · HLS（.m3u8）会先暖 master 与 media 播放列表，再暖 EXT-X-MAP 与前几个分片，\
    所以条目数会比填进去的 URL 多；遇到非 METHOD=NONE 的 EXT-X-KEY 会整条放弃，\
    一次密钥请求都不发。
    · 「已暖够」只增不减，移除条目不会让它回落。
    · 同一份缓存已经暖过的 URL 再 add 一次，会几乎立刻变成「已暖够」而「条目源已下」\
    一个字节都不涨——实测就是这样（见下面缓存目录的实际占用）。这不是统计坏了，\
    正是"预加载暖好的字节播放时直接命中"这件事本身。
    """

    private func buildLayout() {
        let rowViews: [UIView] = rows.map { row in
            let top = UIStackView(arrangedSubviews: [row.field, row.priority, row.removeButton])
            top.axis = .horizontal
            top.spacing = 8
            top.alignment = .center
            row.priority.setContentHuggingPriority(.required, for: .horizontal)
            row.priority.setContentCompressionResistancePriority(.required, for: .horizontal)
            row.removeButton.setContentHuggingPriority(.required, for: .horizontal)

            let box = UIStackView(arrangedSubviews: [top, row.status])
            box.axis = .vertical
            box.spacing = 4
            return box
        }
        let buttons = UIStackView(arrangedSubviews:
            [sampleButton, startButton, clearButton, preconnectButton])
        buttons.axis = .horizontal
        buttons.spacing = 16
        buttons.distribution = .fillEqually

        let stack = UIStackView(arrangedSubviews:
            [modeControl, rateControl, rateLabel] + rowViews
            + [buttons, preconnectStatusLabel, hintLabel, statsLabel, notesLabel, cacheLabel])
        stack.axis = .vertical
        stack.spacing = 12
        stack.translatesAutoresizingMaskIntoConstraints = false

        // 内容比一屏高（三行 + 说明），套一层 UIScrollView，免得小屏上被裁掉。
        let scroll = UIScrollView()
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.addSubview(stack)
        view.addSubview(scroll)
        NSLayoutConstraint.activate([
            scroll.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            scroll.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor),
            scroll.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor),
            stack.topAnchor.constraint(equalTo: scroll.contentLayoutGuide.topAnchor, constant: 16),
            stack.leadingAnchor.constraint(equalTo: scroll.contentLayoutGuide.leadingAnchor,
                                           constant: 16),
            stack.trailingAnchor.constraint(equalTo: scroll.contentLayoutGuide.trailingAnchor,
                                            constant: -16),
            stack.bottomAnchor.constraint(equalTo: scroll.contentLayoutGuide.bottomAnchor,
                                          constant: -16),
            stack.widthAnchor.constraint(equalTo: scroll.frameLayoutGuide.widthAnchor,
                                         constant: -32),
        ])
    }

    // MARK: - 无人值守自检（仅 DEBUG）

    #if DEBUG
    /// 把"人点三下按钮"这套操作自动跑一遍并把**页面上实际渲染出来的那几行字**
    /// 打到 stdout。
    ///
    /// 存在的理由不是自动化测试，而是这一页最可能的失败形态：**编译通过、
    /// 但从来没有人真的跑过它**。此前一批 Swift 测试就整整七轮没被编进
    /// pbxproj，没人发现，因为没人看过它跑。有了这个入口，
    /// "跑一遍并把数贴进报告"就是一条命令的事：
    ///
    ///   SYPLAYER_DEMO_PRELOAD_AUTORUN=1 ./syplayer-mac.app/Contents/MacOS/syplayer-mac
    ///
    /// 打印的文本直接取自 **五个 label**（hint / stats / notes / cache + 各行
    /// status）的 `text`，**不另算一份**——另算一份就又是"两份实现悄悄漂移"
    /// 那个老问题。
    ///
    /// 【自检现在经**控件**驱动，不再直接调 @objc 方法】
    /// 上一版这里调的是 `startPreloading()` / `clearAll()` / `modeChanged()` /
    /// `fill(with:)` 本身，**一次都不经过控件**。实测把 `viewDidLoad` 里那
    /// 5 条 `addTarget` 全注释掉（于是屏幕上没有一个按钮/分段控件响应点击）：
    /// 构建成功、自检 **EXIT=0、输出与基线逐字节相同**，连那句招牌的低报
    /// 结论都一字不差。也就是说这一页**所有控件的 target/action 接线**从来
    /// 没有被任何东西验过。
    /// 现在改成 `sendActions(for:)`：走的是 UIControl 真正的分发路径，
    /// 少一条 `addTarget` 就必然有一个阶段不动、输出立刻不同。
    ///
    /// ⚠️ **这个自检仍然看不见屏幕，别把它当成端到端验证。** 它读的是 label
    /// 的 `text` 属性，而 `text` 只由 `render()` 写——所以它能证明"这些数出自
    /// 页面的渲染代码、而且是经控件触发的"，但它对**渲染、约束、视图层级**
    /// 这一整类故障仍然是部分盲的。已知的一个盲区：在 `buildLayout()`
    /// 开头直接 return（一个控件都不上屏、页面全白），`buildLayout()` 里
    /// return 会让控件不进视图层级，但 `sendActions` 走的是 UIControl 自己
    /// 的 target 列表、不要求在窗口里，所以它**仍然**打得出同样的
    /// 数——真正关掉它要靠人眼或截图。这里只诚实地把这条记在这里，没有
    /// 假装它被修了。
    /// - Parameter phases: `"merged"` / `"perrow"` / 其它（= 两种都跑）。
    ///   之所以要这个开关：两种口径**共用同一份缓存**，先跑的那一种会把字节
    ///   全暖进去，后跑的那一种于是一个字节都不走网络（实测确实如此）。要看
    ///   某一种口径的冷缓存数字，就得能单独跑它（配合先删缓存目录）。
    func startAutorunProbe(seconds: Double, phases: String,
                           onFinish: @escaping (_ ok: Bool) -> Void) {
        autorunFinish = onFinish
        Task { @MainActor [weak self] in
            guard let self = self else { return }
            print("[preload-autorun] 开始。缓存目录：\(self.cache.directory.path)")
            let atStart = self.measureCacheUsage()
            print("[preload-autorun] 起始时缓存目录占用 \(atStart.total) 字节"
                  + "（其中数据文件 \(atStart.data) 字节）"
                  + "——**不为零是正常的**：缓存目录装的是历次会话的字节，"
                  + "页面显示的低报只按本次增量算")
            // 经**按钮**填样例，不再自己 ensureSampleServer + fill：
            // `fillLocalSample` 是异步的（它要起服务器），所以点完之后轮询
            // "第一行的 field 有没有被填上"，而不是睡一个拍脑袋的时长。
            print("[preload-autorun] 点「用本地样例」（经 sampleButton.sendActions）")
            self.sampleButton.sendActions(for: .touchUpInside)
            let filled = await self.waitUntil(timeout: 10.0) {
                !(self.rows[0].field.text ?? "").isEmpty
            }
            guard filled else {
                print("[preload-autorun] 样例服务器起不来或按钮没接线："
                      + "点了「用本地样例」之后 10 秒内第一行仍然是空的。"
                      + "hint=\(self.hintLabel.text ?? "")")
                self.autorunFinish?(false)
                return
            }
            print("[preload-autorun] 本地样例已填入：\(self.rows[0].field.text ?? "")")
            self.dump(tag: "填入样例后")

            // 限速档位（经 rateControl.sendActions）：选「1 MB/s」应立即
            // 落到 SYPlayerNetwork.maximumDownloadRate；再选回「不限」应变回
            // nil。读的是 C 层的消费者（SYPlayerNetwork.maximumDownloadRate 的
            // getter 经桥读 syp_rate_limit_get），不是 rateControl 自己的段位——
            // 断言的是"控件真的驱动了限速"而不是"控件自己记得选了哪一档"。
            print("[preload-autorun] 限速档位切到「1 MB/s」（经 rateControl.sendActions）")
            self.rateControl.selectedSegmentIndex = 2   // 1 MB/s
            self.rateControl.sendActions(for: .valueChanged)
            self.expect(SYPlayerNetwork.maximumDownloadRate == 1_048_576,
                        "选中「1 MB/s」之后 SYPlayerNetwork.maximumDownloadRate = "
                        + "\(String(describing: SYPlayerNetwork.maximumDownloadRate))，"
                        + "不是 1048576 —— rateControl 的 addTarget 断了？")
            print("[preload-autorun] 限速档位切回「不限」（经 rateControl.sendActions）")
            self.rateControl.selectedSegmentIndex = 0   // 不限
            self.rateControl.sendActions(for: .valueChanged)
            self.expect(SYPlayerNetwork.maximumDownloadRate == nil,
                        "选回「不限」之后 SYPlayerNetwork.maximumDownloadRate = "
                        + "\(String(describing: SYPlayerNetwork.maximumDownloadRate))，"
                        + "不是 nil —— rateControl 的 addTarget 断了？")
            self.dump(tag: "限速档位切换后")

            let wanted = phases.lowercased()
            if wanted != "perrow" {
                await self.autorunPhase(named: "合并", mode: .merged, seconds: seconds)
            }
            if wanted != "merged" {
                await self.autorunPhase(named: "分行", mode: .perRow, seconds: seconds)
            }

            // `removeRow(_:)` 与 `rows[i].registered` 这套记账此前**一次
            // 都没被跑过**。这里把两条分支都走一遍：先点一行**有**已登记条目
            // 的（真的 remove），再点同一行第二下（此时 registered 已经是 nil，
            // 走"空操作"那条分支）。两次都经按钮。
            print("[preload-autorun] ── 单行移除（经 rows[0].removeButton.sendActions）──")
            let before = self.rows[0].registered
            print("[preload-autorun] 移除前 rows[0].registered = "
                  + "\(before?.absoluteString ?? "nil")")
            // startButton 的接线断了就不会有任何一行被登记——这是 startPreloading
            // 那条接线的判据，单靠"输出变了"看不出来（要有人去 diff 日志）。
            self.expect(before != nil,
                        "开始预加载之后 rows[0].registered 仍是 nil —— "
                        + "startButton 的 addTarget 断了？")
            self.rows[0].removeButton.sendActions(for: .touchUpInside)
            self.dump(tag: "移除第 1 行之后")
            print("[preload-autorun] 移除后 rows[0].registered = "
                  + "\(self.rows[0].registered?.absoluteString ?? "nil")")
            self.expect(self.rows[0].registered == nil,
                        "点了「移除本行」之后 rows[0].registered 还在 —— "
                        + "removeButton 的 addTarget 断了？")
            self.rows[0].removeButton.sendActions(for: .touchUpInside)
            self.dump(tag: "再点一次第 1 行（应为空操作）")

            print("[preload-autorun] 全部移除（经 clearButton.sendActions）")
            self.clearButton.sendActions(for: .touchUpInside)
            try? await Task.sleep(nanoseconds: 700_000_000)
            self.render()
            self.dump(tag: "清空后")
            // clearAll 那条接线的判据：除了刚被单行移除的第 1 行，第 2/3 行的
            // registered 只可能由 clearAll 清掉。断了的话它们还在。
            self.expect(self.rows.allSatisfy { $0.registered == nil },
                        "「全部移除」之后仍有行留着 registered —— "
                        + "clearButton 的 addTarget 断了？")
            self.autorunFinish?(self.autorunOK)
        }
    }

    /// 把一条判据记进自检的成败。**不 assert、不崩**：这是个演示程序，
    /// 崩掉会盖住后面还没跑的阶段；把失败累积起来、最后用退出码表达。
    private func expect(_ condition: Bool, _ message: @autoclosure () -> String) {
        if condition { return }
        autorunOK = false
        print("[preload-autorun] ❌ 自检判据不成立：\(message())")
    }

    /// 轮询一个谓词直到成立或超时。**不睡一个拍脑袋的时长**——按钮触发的动作
    /// 有的是异步的（起服务器），睡固定时长要么慢要么脆。
    private func waitUntil(timeout: TimeInterval,
                           _ predicate: () -> Bool) async -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if predicate() { return true }
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
        return predicate()
    }

    /// 100ms 采一次，但**只在渲染出来的文字变了的时候打印**。
    ///
    /// 两个理由：① 环回上整件事 0.5 秒内就结束了，500ms 的采样会整个错过
    /// `activeTasks > 0` 那一瞬；② 逐帧打印会刷出几百行一模一样的日志，
    /// 真正的状态跃迁反而找不到。去重之后每一行都是一次真实的变化。
    private func autorunPhase(named name: String, mode target: Mode, seconds: Double) async {
        if mode != target {
            // 改段位之后经**控件**发 .valueChanged，不直接调 modeChanged()：
            // 直接调等于绕过 `modeControl.addTarget(...)` 那条接线。
            modeControl.selectedSegmentIndex = target.rawValue
            modeControl.sendActions(for: .valueChanged)
            // modeChanged 那条接线的判据：断了的话 `mode` 不动，页面会顶着
            // 「分行」的段位显示合并口径的数——而那种输出看起来完全正常。
            expect(mode == target,
                   "把段位切到 \(target) 之后 mode 仍是 \(mode) —— "
                   + "modeControl 的 addTarget 断了？")
        }
        print("[preload-autorun] ── 口径：\(name)（开始前缓存目录占用 "
              + "\(measureCacheUsage().total) 字节）──")
        // 同上，经 startButton 而不是直接调 startPreloading()。
        startButton.sendActions(for: .touchUpInside)
        let start = Date()
        var last = ""
        while true {
            let elapsed = Date().timeIntervalSince(start)
            render()
            // 去重快照里也带上 hint / notes：它们变了同样要打一行。
            let snapshot = ([hintLabel.text, statsLabel.text]
                            + rows.map { $0.status.text }
                            + [cacheLabel.text, notesLabel.text])
                .map { $0 ?? "" }.joined(separator: "\u{1}")
            if snapshot != last {
                last = snapshot
                dump(tag: String(format: "%@ t=%.2fs", name, elapsed))
            }
            if elapsed >= seconds { break }
            try? await Task.sleep(nanoseconds: 100_000_000)
        }
        dump(tag: "\(name) 末态")
    }

    /// **五个 label 全打**。上一版只打 statsLabel / rows[*].status /
    /// cacheLabel 三个，于是 `hintLabel` 与 `notesLabel`——屏幕上**整个说明
    /// 块**，包括那段告诉用户怎么读这些数的正文——一次都没有被验过：把
    /// `notesText` 删空、或者把 `hintLabel.text` 改错，自检输出一个字都不变。
    private func dump(tag: String) {
        let flat = { (s: String?) -> String in
            (s ?? "").replacingOccurrences(of: "\n", with: " ⏎ ")
        }
        print("[preload-autorun] [\(tag)] \(flat(statsLabel.text))")
        for row in rows {
            print("[preload-autorun] [\(tag)]   \(flat(row.status.text))")
        }
        // 缓存那一段也要打出来：低报那个数是这一页的招牌结论，它原先只出现在
        // 屏幕上、进不了自检日志，于是"页面算的低报"与"报告里写的低报"是两笔
        // 账（报告那笔是事后拿 du 手算的）。打的就是 cacheLabel.text 本身。
        print("[preload-autorun] [\(tag)]   \(flat(cacheLabel.text))")
        // hint 是"页面此刻在对用户说什么"（填样例成功 / 没有可预加载的 URL /
        // 服务器起不来），notes 是整段说明正文。两者都只由这一页自己写。
        print("[preload-autorun] [\(tag)]   hint: \(flat(hintLabel.text))")
        print("[preload-autorun] [\(tag)]   notes: \(flat(notesLabel.text))")
    }
    #endif
}
