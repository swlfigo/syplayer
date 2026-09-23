// PreloadSampleServer.swift — 演示页专用的**本地**样例 HTTP 服务器（仅 DEBUG）。
//
// 【为什么要有这个文件】
// 预加载页原本的设计是"让用户自己填 URL"，因为这个仓库的纪律是不打外网。
// 那样做的实际后果是：**这一页永远不会有人真的跑过它**，它只会被编译。此前
// 已经出现过同型的失败——Swift 测试长期没被编进 pbxproj，
// 谁都没发现，因为没人真的看过它跑。一页"能编译但没人跑过"的演示等于没有。
//
// 所以这里在 demo 进程内起一个只监听 127.0.0.1 的最小 HTTP/1.1 服务器，
// 演示页上一个按钮就能把三条指向它的 URL 填进去，点"开始预加载"立刻能看到
// 六个统计数在动。**不打任何外网**：素材就是 bundle 里那份 sample.mp4
// （1.4MB，12 秒，640x360 h264+aac），HLS 播放列表是照它现算出来的。
//
// 【为什么不复用 tests/support/loopback_server】
// 那是 C++ 测试代码，只编进 ctest 的目标；把它拉进 demo 的 App 目标意味着
// App 的 Compile Sources 里重新出现 .cpp——之前已经把 .cpp/.mm 全部从
// App 挪进 SYPlayerKit，不能为了一个演示页倒回去。Network.framework 在
// Swift 侧写一份几十行的反而更便宜，也顺便证明了"业务侧零 C 类型"这条纪律
// 连 demo 的脚手架都守得住。
//
// 【实测口径】这个服务器支持 Range（dl 层是按区间取的，不支持 Range 的话
// 预加载会退化成整文件下载，演示的数就不对了），每个连接答完即关
// （`Connection: close`）——不做 keep-alive 状态机是有意的取舍：预加载一个
// URL 也就几条请求，省下的那点连接开销换不来一份需要维护的状态机。
#if DEBUG
import Foundation
import Network

/// `@unchecked Sendable` 是**证明过的**，不是敷衍：除 `init` 外所有可变状态
/// （`listener` / `didFinishStart`）都只在 `queue` 这条串行队列上读写，
/// `mp4Bytes` 是 `let` 且构造后只读。对照 SYPlayerPreloaderBox 的同类标注。
final class PreloadSampleServer: @unchecked Sendable {
    /// 演示页要用的三条路径。刻意包含一条**必然 404** 的：`SYPlayerPreloader.add`
    /// 对它返回 `true`（登记成功），失败只会异步地体现在 `statistics.failed` 上。
    /// 这条语义不演示出来，使用者就会以为 `add == true` 等于"下得下来"。
    struct Routes {
        let mp4: URL
        let hlsMaster: URL
        let missing: URL
    }

    enum StartError: Error, CustomStringConvertible {
        case sampleNotFound
        case listenerFailed(String)

        var description: String {
            switch self {
            case .sampleNotFound:            return "bundle 里找不到 sample.mp4"
            case .listenerFailed(let why):   return "监听失败：\(why)"
            }
        }
    }

    private let queue = DispatchQueue(label: "com.syplayer.demo.preload-sample-server")
    private let mp4Bytes: Data
    /// **刻意把每个响应压慢**。环回上不加这个延迟，三条 URL（展开成 6 个条目）
    /// 从 add 到全部「已暖够」实测只要 **110ms**——比页面 300ms 的轮询周期还短，
    /// 于是「在下载」这一栏永远显示 0，演示页最该展示的"正在暖"状态一次都看不到。
    /// 250ms/次是照"一条普通移动网络上单次请求的量级"取的，纯为可观测性。
    private let responseDelay: TimeInterval = 0.25
    /// 切成几段。6 段 × 2 秒 ≈ 12 秒，与样片时长对得上；预加载默认只暖前几段，
    /// 段数少了看不出"条目数比填进去的 URL 多"这件事。
    private let segmentCount = 6
    private var listener: NWListener?
    private var didFinishStart = false

    private init(mp4Bytes: Data) {
        self.mp4Bytes = mp4Bytes
    }

    /// 从 App bundle 里的 sample.mp4 建一台。找不到样片直接抛——这时候
    /// 起一台只会 404 的服务器毫无意义，还会让演示页上的数字骗人。
    static func withBundledSample() throws -> PreloadSampleServer {
        guard let url = Bundle.main.url(forResource: "sample", withExtension: "mp4"),
              let data = try? Data(contentsOf: url), !data.isEmpty else {
            throw StartError.sampleNotFound
        }
        return PreloadSampleServer(mp4Bytes: data)
    }

    /// 起监听并返回三条可用 URL。端口交给系统分配（`.any`），不写死——
    /// 写死的端口在开发机上被别的进程占了就会静默失败。
    func start() async throws -> Routes {
        let port = try await startListener()
        let base = "http://127.0.0.1:\(port)"
        // 强制解包在这里是安全的：base 由我们自己拼、端口是数字。
        return Routes(mp4: URL(string: "\(base)/sample.mp4")!,
                      hlsMaster: URL(string: "\(base)/hls/master.m3u8")!,
                      missing: URL(string: "\(base)/does-not-exist.mp4")!)
    }

    func stop() {
        queue.async {
            self.listener?.cancel()
            self.listener = nil
        }
    }

    // MARK: - 监听

    private func startListener() async throws -> UInt16 {
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<UInt16, Error>) in
            queue.async {
                let params = NWParameters.tcp
                // 只绑 127.0.0.1：这台服务器不该对局域网可见。端口 .any 让系统挑。
                params.requiredLocalEndpoint = NWEndpoint.hostPort(host: .ipv4(.loopback),
                                                                   port: .any)
                params.allowLocalEndpointReuse = true
                let listener: NWListener
                do {
                    listener = try NWListener(using: params)
                } catch {
                    cont.resume(throwing: StartError.listenerFailed("\(error)"))
                    return
                }
                self.listener = listener
                listener.stateUpdateHandler = { [weak self] state in
                    guard let self = self else { return }
                    // continuation 只能 resume 一次；.ready 之后还可能来 .failed。
                    switch state {
                    case .ready:
                        guard !self.didFinishStart else { return }
                        self.didFinishStart = true
                        cont.resume(returning: listener.port?.rawValue ?? 0)
                    case .failed(let error), .waiting(let error):
                        guard !self.didFinishStart else { return }
                        self.didFinishStart = true
                        cont.resume(throwing: StartError.listenerFailed("\(error)"))
                    default:
                        break
                    }
                }
                listener.newConnectionHandler = { [weak self] connection in
                    self?.serve(connection)
                }
                listener.start(queue: self.queue)
            }
        }
    }

    // MARK: - 一次请求

    private func serve(_ connection: NWConnection) {
        connection.start(queue: queue)
        readRequest(connection, accumulated: Data())
    }

    /// 只读到头部结束（`\r\n\r\n`）为止。GET/HEAD 没有请求体，读多了也没用。
    private func readRequest(_ connection: NWConnection, accumulated: Data) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 8192) {
            [weak self] chunk, _, isComplete, error in
            guard let self = self else { connection.cancel(); return }
            var buffer = accumulated
            if let chunk = chunk { buffer.append(chunk) }
            if error != nil || (isComplete && buffer.isEmpty) {
                connection.cancel()
                return
            }
            guard let headerEnd = PreloadSampleServer.rangeOfHeaderEnd(in: buffer) else {
                if isComplete || buffer.count > 64 * 1024 {
                    connection.cancel()
                } else {
                    self.readRequest(connection, accumulated: buffer)
                }
                return
            }
            let header = String(decoding: buffer[..<headerEnd], as: UTF8.self)
            let response = self.response(forHeader: header)
            self.queue.asyncAfter(deadline: .now() + self.responseDelay) {
                connection.send(content: response, completion: .contentProcessed { _ in
                    connection.cancel()
                })
            }
        }
    }

    private static func rangeOfHeaderEnd(in data: Data) -> Data.Index? {
        let marker = Data("\r\n\r\n".utf8)
        return data.range(of: marker)?.lowerBound
    }

    private func response(forHeader header: String) -> Data {
        let lines = header.split(separator: "\r\n", omittingEmptySubsequences: false)
        guard let requestLine = lines.first else { return Self.simple(status: "400 Bad Request") }
        let parts = requestLine.split(separator: " ")
        guard parts.count >= 2 else { return Self.simple(status: "400 Bad Request") }
        let method = String(parts[0]).uppercased()
        guard method == "GET" || method == "HEAD" else {
            return Self.simple(status: "405 Method Not Allowed")
        }
        // 去掉查询串；本服务器不认任何参数。
        let path = String(parts[1].split(separator: "?").first ?? "")

        var rangeHeader: String?
        for line in lines.dropFirst() {
            let lower = line.lowercased()
            if lower.hasPrefix("range:") {
                rangeHeader = String(line.dropFirst("range:".count)).trimmingCharacters(in: .whitespaces)
            }
        }

        guard let body = self.body(for: path) else {
            return Self.simple(status: "404 Not Found")
        }
        return Self.payload(body.bytes, contentType: body.contentType,
                            rangeHeader: rangeHeader, includeBody: method == "GET")
    }

    // MARK: - 路由

    private func body(for path: String) -> (bytes: Data, contentType: String)? {
        switch path {
        case "/sample.mp4":
            return (mp4Bytes, "video/mp4")
        case "/hls/master.m3u8":
            return (Data(masterPlaylist.utf8), "application/vnd.apple.mpegurl")
        case "/hls/media.m3u8":
            return (Data(mediaPlaylist.utf8), "application/vnd.apple.mpegurl")
        case "/hls/init.mp4":
            return (slice(index: -1), "video/mp4")
        default:
            guard path.hasPrefix("/hls/seg"), path.hasSuffix(".m4s"),
                  let index = Int(path.dropFirst("/hls/seg".count).dropLast(".m4s".count)),
                  index >= 0, index < segmentCount else {
                return nil
            }
            return (slice(index: index), "video/iso.segment")
        }
    }

    /// 分片内容就是样片字节的一段。**这些"分片"不是能解码的 fMP4**——预加载
    /// 只按字节暖分片、一个字节都不解码，所以这对本页要演示的东西是足够的；
    /// 但也因此**别拿这条 HLS 去播放页打开**，它播不了。这是本页明确的局限。
    private func slice(index: Int) -> Data {
        let total = mp4Bytes.count
        if index < 0 {  // init 段
            return mp4Bytes.prefix(min(64 * 1024, total))
        }
        let each = max(1, total / segmentCount)
        let start = min(index * each, total)
        let end = index == segmentCount - 1 ? total : min(start + each, total)
        return mp4Bytes.subdata(in: start..<end)
    }

    private var masterPlaylist: String {
        // 用相对 URI，顺带让 dl 层的 resolve_url（3cce11e 那轮补的 RFC 3986
        // 点段归一）真的被走一遍。
        """
        #EXTM3U
        #EXT-X-VERSION:7
        #EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS="avc1.4d401e,mp4a.40.2"
        media.m3u8
        """
    }

    private var mediaPlaylist: String {
        var text = """
        #EXTM3U
        #EXT-X-VERSION:7
        #EXT-X-TARGETDURATION:2
        #EXT-X-PLAYLIST-TYPE:VOD
        #EXT-X-MEDIA-SEQUENCE:0
        #EXT-X-MAP:URI="init.mp4"

        """
        for i in 0..<segmentCount {
            text += "#EXTINF:2.0,\nseg\(i).m4s\n"
        }
        text += "#EXT-X-ENDLIST\n"
        return text
    }

    // MARK: - 响应拼装

    private static func simple(status: String) -> Data {
        Data("HTTP/1.1 \(status)\r\nContent-Length: 0\r\nConnection: close\r\n\r\n".utf8)
    }

    /// 支持 `Range: bytes=a-b` / `bytes=a-`。不支持多区间（`a-b,c-d`）——
    /// dl 层一次只要一个区间，实现多区间等于给演示脚手架加一份 multipart
    /// 拼装代码，没人会从中得到任何东西。
    ///
    /// ⚠️ **别把这台服务器当通用 fixture 复用**：还有两个 RFC 7233 的角落
    /// 它是错的。dl 层只会发 `bytes=N-` / `bytes=N-M` 两种形状，所以这两个
    /// 角落在本仓库里**不可达**，演示页上的任何一个数都不受影响；但下一个人
    /// 拿它去测别的 HTTP 客户端就会被咬：
    ///   · **后缀区间 `bytes=-500`**（"最后 500 字节"）：`Int("")` 解析不出
    ///     下界 ⇒ 整个 Range 被当成没看懂 ⇒ 静默退化成 `200` + **全量 body**，
    ///     而不是 `206` + 最后 500 字节。客户端会以为自己拿到了尾部。
    ///   · **倒置区间 `bytes=5-4`**（last-byte-pos < first-byte-pos）：这里返
    ///     `206` + `Content-Range: bytes 5-4/<total>` + **0 字节 body**。
    ///     RFC 7233 要求这种 Range 无效，应当 `416`（或整条忽略 Range 返 200），
    ///     绝不该拿它拼出一个自相矛盾的 Content-Range。
    /// 两个都没修：修它们要给演示脚手架加一份完整的 Range 语法解析，而这一页
    /// 要演示的是预加载统计，不是 HTTP 一致性。
    static func payload(_ bytes: Data, contentType: String,
                        rangeHeader: String?, includeBody: Bool) -> Data {
        let total = bytes.count
        var start = 0
        var end = total - 1
        var isPartial = false

        if let raw = rangeHeader, raw.lowercased().hasPrefix("bytes=") {
            let spec = raw.dropFirst("bytes=".count)
            if !spec.contains(",") {
                let halves = spec.split(separator: "-", omittingEmptySubsequences: false)
                if halves.count == 2, let lower = Int(halves[0]) {
                    if lower >= total {
                        var head = "HTTP/1.1 416 Range Not Satisfiable\r\n"
                        head += "Content-Range: bytes */\(total)\r\n"
                        head += "Content-Length: 0\r\nConnection: close\r\n\r\n"
                        return Data(head.utf8)
                    }
                    start = lower
                    if let upper = Int(halves[1]) { end = min(upper, total - 1) }
                    isPartial = true
                }
            }
        }
        if total == 0 { start = 0; end = -1 }
        let slice = end >= start ? bytes.subdata(in: start..<(end + 1)) : Data()

        var head = isPartial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n"
        head += "Content-Type: \(contentType)\r\n"
        head += "Accept-Ranges: bytes\r\n"
        head += "Content-Length: \(slice.count)\r\n"
        if isPartial {
            head += "Content-Range: bytes \(start)-\(end)/\(total)\r\n"
        }
        head += "Connection: close\r\n\r\n"

        var out = Data(head.utf8)
        if includeBody { out.append(slice) }
        return out
    }
}
#endif
