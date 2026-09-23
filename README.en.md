# syplayer

English · [中文](README.md)

**A video player SDK for Apple platforms, with its own download-and-cache layer.**

One SwiftUI view and one `open` call play local files, progressive HTTP(S) video and
HLS. Underneath sits a purpose-built caching layer: seeking to an uncached position
fetches **only the missing byte ranges**, already downloaded bytes are never re-fetched,
and the index is written to disk crash-safely so a restarted process **resumes instead
of re-downloading**.

```swift
import SYPlayerKit

let player = SYPlayer()
try await player.open(.url(URL(string: "https://example.com/movie.m3u8")!))
player.play()
```

Integration is one SwiftPM dependency — two prebuilt xcframeworks, no linker settings to
configure. Already bundling FFmpeg? That works too: ours is symbol-isolated (see
[Coexisting with your own FFmpeg](#coexisting-with-your-own-ffmpeg)).

## What it does

- **Playback**: local files, progressive HTTP(S) MP4, HLS (VOD, live, multi-variant with track selection)
- **Caching**: seek fetches only the gaps, resumes across process restarts, shared between playback and preloading, configurable size and TTL
- **Preloading**: warm up upcoming videos by duration or bytes, three priority levels; warmed bytes are cache hits during playback
- **Video**: VideoToolbox hardware decoding with zero-copy Metal presentation, three gravity modes, SAR and rotation normalized
- **Audio**: AudioUnit output (macOS HALOutput / iOS RemoteIO), volume and mute, audio master clock with fallback to the system clock
- **Flow control**: process-wide download rate cap (playback first), preconnect, automatic replacement of stalled or unusually slow connections
- **Coexistence**: apps that already bundle any FFmpeg version can integrate as-is; the two copies never mix
- **Compliance**: privacy manifests, plus FFmpeg's LGPL license text and NOTICE, shipped inside the package

## Quick start

### 1. Add the dependency

In Xcode: File → Add Package Dependencies…, enter
`https://github.com/swlfigo/syplayer.git`, choose Up to Next Major `0.1.0`, and add
`SYPlayerKit` to your app target.

In a `Package.swift`:

```swift
dependencies: [
    .package(url: "https://github.com/swlfigo/syplayer.git", from: "0.1.0"),
],
targets: [
    .target(name: "MyApp", dependencies: [
        .product(name: "SYPlayerKit", package: "syplayer"),
    ]),
]
```

### 2. Play something

```swift
import SwiftUI
import SYPlayerKit

// The player is created and owned by the caller (e.g. a property on your App
// or an ancestor view). This uses @ObservedObject rather than @StateObject,
// which is an iOS 14 API — this package's floor is iOS 13.
struct PlayerScreen: View {
    @ObservedObject var player: SYPlayer

    var body: some View {
        VStack {
            SYPlayerViewRepresentable(player: player)
                .aspectRatio(player.state.videoSize.map { $0.width / $0.height } ?? 16.0 / 9.0,
                             contentMode: .fit)

            Text(statusText)

            HStack {
                Button("Play") { player.play() }
                Button("Pause") { player.pause() }
                Button("-10s") { player.seek(to: max(0, player.state.position - 10)) }
            }
        }
        .onAppear {
            Task {
                do {
                    try await player.open(
                        .url(URL(string: "https://example.com/movie.mp4")!),
                        decoding: SYPlayer.isHardwareDecodeAvailable ? .hardwarePreferred : .software)
                    player.play()
                } catch {
                    print("open failed: \(error)")
                }
            }
        }
        .onDisappear { player.close() }
    }

    private var statusText: String {
        switch player.state.playback {
        case .idle:              return "idle"
        case .buffering:         return "buffering"
        case .playing:           return "playing"
        case .paused:            return "paused"
        case .ended:             return "ended"
        case .failed(let error): return "failed: \(error)"
        }
    }
}
```

Two things worth knowing up front:

- A successful `open()` leaves the player `.paused`; call `play()` explicitly. That is
  deliberate — integrators often want to wait until the first frame is ready.
- `decoding` defaults to `.hardwarePreferred`, and **on platforms without hardware
  decoding `open()` throws `.notImplemented`** (Mac Catalyst never has it, nor do some
  simulators). Hence the `SYPlayer.isHardwareDecodeAvailable` check above, which is also
  what the bundled example apps do.

Full example projects live in `examples/` (one iOS, one native macOS, both integrated
purely through SwiftPM).

## Usage

`SYPlayer` and `SYPlayerPreloader` are `@MainActor`; `SYPlayerNetwork` can be called
from any thread. Everything below works on iOS, Mac Catalyst and native macOS.

### Sources

```swift
try await player.open(.url(URL(string: "https://example.com/movie.mp4")!))   // remote, cached
try await player.open(.url(URL(string: "https://example.com/live.m3u8")!))   // path ends in .m3u8 ⇒ HLS
try await player.open(.file(URL(fileURLWithPath: "/path/to/local.mp4")))     // local file, no cache
try await player.open(.url(remote), decoding: .software)                      // force software decoding
```

### Transport control and volume

```swift
player.play()
player.pause()
player.seek(to: 42)                  // seconds
try player.setRate(1.5)              // [0.5, 2.0]; outside that range throws .invalidArgument
player.volume = 0.5                  // 0...1 (NaN → 0, +inf → 1, negative → 0)
player.isMuted = true
player.close()                       // releases decode and network resources; you may open() again
```

### Observing state

Three equivalent ways:

```swift
// 1. SwiftUI: SYPlayer is an ObservableObject and `state` is @Published
//    (receive it with @ObservedObject in the view — see Quick start)

// 2. Async sequence
for await state in player.stateStream {
    print(state.position, state.duration ?? -1, state.buffered ?? -1)
}

// 3. Delegate (typical for UIKit; all three methods have default empty implementations)
player.delegate = self
```

`SYPlayerState` carries:

| Field | Meaning |
|---|---|
| `playback` | `.idle` / `.buffering(reason)` / `.playing` / `.paused` / `.ended` / `.failed(error)` |
| `hasMedia` | whether media is open |
| `position` / `duration` | current time / total duration (`nil` = unknown or live) |
| `buffered` | buffered-up-to timestamp (`nil` = all playing tracks fully read) |
| `hasVideo` / `videoSize` | video track present / display size (SAR and rotation applied; `nil` for audio-only) |
| `isHardwareDecoding` | whether hardware decoding is currently in use |
| `rate` | current playback rate |
| `clock` / `clockSwitchReason` | master clock source (audio / system) and why it switched |
| `statistics` | dropped frames, rebuffer count, startup duration, audio underruns, … |

The polling interval defaults to 0.1 s (floor 1/60): `player.stateUpdateInterval = 0.25`.

### UIKit

```swift
final class PlayerViewController: UIViewController, SYPlayerDelegate {
    private let player = SYPlayer()
    private let playerView = SYPlayerView()

    override func viewDidLoad() {
        super.viewDidLoad()
        playerView.frame = view.bounds
        playerView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        playerView.videoGravity = .aspectFit      // .aspectFill / .resize
        view.addSubview(playerView)

        player.delegate = self
        player.attach(to: playerView)
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        player.close()
    }

    func player(_ player: SYPlayer, didFailWith error: SYPlayerError) {
        print("playback failed: \(error)")
    }
}
```

On native macOS `SYPlayerView` is an `NSView` with the same members (`playerLayer`,
`videoGravity`); swap `UIViewController` for `NSViewController` and the code is identical.

### Preloading

```swift
let preloader = SYPlayerPreloader()    // also takes cache: / maxTotalTasks: / reservedForPlaying:

preloader.add(.url(nextVideoURL), priority: .next, seconds: 3)   // warm the first 3 seconds
preloader.add(.url(laterURL), priority: .background)

preloader.setPriority(.playing, for: .url(nextVideoURL))          // user opened it — raise priority
preloader.remove(.url(laterURL))
preloader.removeAll()

let s = preloader.statistics    // entries / activeTasks / downloadedBytes / completed / failed …
```

- Only `.url(...)` sources are accepted; local files need no preloading (`add` returns `false`).
- Preloading and playback **share one cache**: warmed bytes are hits, never re-downloaded.
- The three priorities only affect the concurrency budget: `.playing` first, then `.next`,
  with `.background` using whatever is left.
- With `seconds: nil` a default byte count is warmed. When time→byte can be estimated from
  the media the request is duration-based; otherwise it falls back to bytes.

### Cache configuration

```swift
let cache = SYPlayerCacheConfiguration(
    directory: FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("my-video-cache"),
    maxBytes: 512 * 1024 * 1024,
    minFreeSpaceBytes: 256 * 1024 * 1024,
    timeToLive: 7 * 24 * 60 * 60        // nil = no time-based eviction
)

let player = SYPlayer(cache: cache)
let preloader = SYPlayerPreloader(cache: cache)   // same directory ⇒ same cache
```

The default directory is `Caches/syplayer-http-cache`. Configuration is per instance, but
**the same directory means the same cache index** — pass the same directory when a player
and a preloader should hit each other's bytes.

### Process-wide network settings

```swift
// Download rate cap in bytes per second for the whole process; nil = unlimited.
// Playback has priority: preloading only uses the leftover budget.
SYPlayerNetwork.maximumDownloadRate = 2 * 1024 * 1024
SYPlayerNetwork.maximumDownloadRate = nil

// Preconnect: warm DNS/TCP/TLS into the connection pool. Downloads no content,
// writes no cache, ignores the rate limit. Good for "the user will probably tap this
// soon, but it isn't worth spending bandwidth yet".
SYPlayerNetwork.preconnect(URL(string: "https://cdn.example.com/movie.mp4")!)
```

Repeated preconnects to the same origin within 30 seconds are deduplicated; at most 4 are
in flight at a time and the rest are dropped silently.

### Error handling

A failed `open()` surfaces on three paths — pick the one that fits; they are **the same
failure, not three**: `try/catch`, the delegate's `didFailWith`, and
`state.playback == .failed(error)`.

`SYPlayerError` cases: `.invalidArgument`, `.canceled`, `.timeout`, `.outOfMemory`,
`.io`, `.noSpace`, `.cacheCorrupt`, `.network`, `.httpStatus(Int)`, `.tooManyRedirects`,
`.rangeUnsupported`, `.contentChanged`, `.notImplemented`, `.unknown(Int)`. It conforms
to `LocalizedError` (descriptions are in Chinese).

## Tech stack

| Layer | Technology |
|---|---|
| Download-and-cache layer (the core asset) | C++23, **zero platform dependencies** (enforced by a build target), talks only to an HTTP function table |
| Playback core | C++23 + FFmpeg 8.1.2 (minimal build: mov / hls / mpegts demuxers, H.264 / HEVC / AAC decoders) |
| Public interface | C11 ABI (`include/syplayer/*.h`): primitives, opaque pointers, PODs, function pointers |
| Platform implementations | Objective-C++: NSURLSession (HTTP), VideoToolbox (hardware decode), Metal (presentation), AudioUnit (output) |
| App-facing API | Swift (`SYPlayerKit`): `SYPlayer` / `SYPlayerView` / SwiftUI wrappers / preloading / network settings |
| Build and distribution | CMake (desktop and tests), Xcode projects emitted by a Ruby generator, SwiftPM binary distribution (xcframeworks) |
| Testing | in-house `tiny_test` + ctest (in-memory stubs, injected clocks, a dependency-free loopback HTTP server), XCTest for the Swift layer |

Three layers, three seams:

```
┌──────────────────────────────────────────┐
│  SYPlayerKit (Swift) / your UI            │
└──────────────────┬───────────────────────┘
                   │  C ABI                     ← seam ①
┌──────────────────────────────────────────┐
│  Playback core (C++)                      │
│    demux / decode / A-V sync / buffering  │
│    IVideoRenderer / IVideoDecoder /       │  ← seam ② (pure virtual, per-platform impls)
│    IAudioSink                             │
└──────────────────┬───────────────────────┘
                   │  C ABI                     ← seam ③
┌──────────────────────────────────────────┐
│  Download-and-cache layer (platform-free) │
│    HoleSet / CacheIndex / CacheStore /    │
│    DLTask / Scheduler / Preloader         │
│    syp_http_backend (function table)      │  ← platform hook: NSURLSession ✅
└──────────────────────────────────────────┘
```

Why only these three seams (and why UI, threading, time and file IO are deliberately not
abstracted) is written up in `docs/architecture.md` (Chinese).

## Performance and engineering trade-offs

These are **design properties plus numbers measured by this repository's own test
rigs**. ⚠️ There is no benchmark against AVPlayer, ijkplayer or anything else, and no
field data from real devices or a real CDN — each item states where its number comes
from, so read them at that scale.

- **Seeking never re-downloads.** "Which bytes are still missing" is a set of ranges
  (`HoleSet`); a seek only issues requests for the gaps. The end-to-end matrix compares
  packet by packet (including payload hashes) that reading through the cache layer yields
  exactly what FFmpeg reads straight from `file:`, across eight scenarios: sequential
  read, seek, Range fallback, reconnect-and-resume, restart-and-resume and more.
- **Resume after a process restart.** The index is a custom binary format (magic,
  version, CRC32, length-prefixed strings), written atomically and validated against the
  etag; a restarted process continues instead of starting over.
- **No more double downloads when the server ignores Range.** This claim was once
  **disproved** by the project's own matrix: measured traffic was about 4.04× the file
  size with a concurrency peak of 3. After the fix, traffic dropped to ~1.1×, Range
  support became tri-state and concurrency is pinned to 1 until it is known (peak 4 → 1,
  request count 5 → 3), with no regression for well-behaved servers.
- **Concurrent connections with adaptive segmentation.** Segment size follows the
  remaining bytes and the concurrency level; when a slot frees up, the scheduler first
  decides whether waiting for an almost-finished task beats opening a new connection.
- **Bad connections are replaced automatically.** A stalled attempt (no progress for the
  backend timeout plus a 2 s grace period) is cancelled and re-issued from the last byte;
  a connection markedly slower than the baseline (4× slower on two consecutive checks) is
  swapped out. Measured end to end: a slow-loris connection was replaced at about 3.2 s
  and the whole read finished in about 4.8 s.
- **Zero-copy hardware decoding.** `VideoToolbox → CVPixelBuffer → CVMetalTextureCache →
  MTLTexture`, never touching CPU memory; the YUV→RGB matrix is chosen per frame from the
  frame's real colorspace and color range.
- **A minimal FFmpeg.** Only the demuxers and decoders actually needed are compiled in:
  roughly 4.5–4.8 MB per architecture, unstripped, and with no TLS backend (HTTPS is
  NSURLSession's job; a gate checks the binary for TLS symbols).
- **Rate limiting is process-wide**, prioritises playback over preloading, caps the
  average rate only, and never blocks a callback thread.

## Platform support

| Platform | Minimum | Architectures |
|---|---|---|
| iOS (device / simulator) | 13.0 | device arm64; simulator arm64 + x86_64 |
| Mac Catalyst | 14.0 | **Apple Silicon only** (arm64) |
| macOS (native, AppKit) | 11.0 | **Apple Silicon only** (arm64) |

Not supported: Intel Macs (no x86_64 slice for native macOS or Catalyst), tvOS,
visionOS, watchOS.

Native macOS apps must set `ARCHS = arm64`. iOS apps that also build for Mac Catalyst
need `ARCHS[sdk=macosx*] = arm64` (Catalyst only; iOS device and simulator builds are
unaffected — `examples/iOSExample` is set up that way). Otherwise a Release build also
compiles x86_64 and fails to link.

**Toolchain**: the `0.1.0` xcframeworks are built with **Xcode 26.6 (Apple Swift
6.3.3)**. Library evolution guarantees that *newer* compilers can read the shipped
`.swiftinterface`; older Xcode versions are neither guaranteed nor tested — use the same
or a newer Xcode.

## Coexisting with your own FFmpeg

**Nothing to configure on your side.** No link-order changes, no `-force_load` /
`-all_load` exceptions, no changes to your own FFmpeg build.

**How it works**: the FFmpeg shipped here is named `SYFFmpeg` and **exports
`syp_`-prefixed symbols only** — at link time every global symbol gets a `syp_` alias
(`ld -alias_list`) and only the aliases are exported (`-exported_symbols_list`), so the
original names become library-local. SYPlayerKit is compiled with a mapping header
generated from the same symbol list, so its source still says `av_read_frame` while the
binary calls `syp_av_read_frame`. Your FFmpeg — any version, static or dynamic, any link
order, with or without `-all_load` — therefore shares no symbols with ours: no
duplicate-symbol errors, and neither side can be bound to the other's implementation.
Global state is separate too (`av_log` level and callback, network init, allocators), and
FFmpeg objects never cross between the copies — the public API exposes no FFmpeg types.

**The cost**: two FFmpeg copies in the app. Ours is a minimal build, roughly 4.5–4.8 MB
per architecture unstripped. Sharing one copy across major versions is not possible for a
binary SDK — the ABI changes between them.

**Regression coverage**: `ffmpeg_coexist_*` links a fake "integrator FFmpeg" that exports
real FFmpeg names, returns recognizable values and records calls, together with our code
in five link configurations, and asserts that a direct `avformat_version()` call returns
the fake value, that we still open the sample at 640×360, and that the fake library was
never called by us. `tools/check-ffmpeg-symbols.sh` additionally watches the export
tables and our libraries' undefined references.

## License and compliance

- **FFmpeg is a dynamic framework distributed under LGPL v2.1 or later** (unmodified
  sources; built with `--disable-gpl`, without `--enable-version3`, no nonfree
  components). For the binary shipped with this repository's releases, the distributor
  obligations are met here: the root of `SYFFmpeg.xcframework.zip` contains
  `COPYING.LGPLv2.1` and a `NOTICE` (FFmpeg version, source URL, the full configure flag
  list, and the fact that it is built by `tools/build-ffmpeg.sh`). **When you
  redistribute it inside your app you still have to** include FFmpeg's LGPL notice in
  your app, and keep `SYFFmpeg.framework` dynamically linked and replaceable — which is
  what SwiftPM does by default; do not link it statically or merge it into another
  binary. A replacement build must export the same `syp_`-prefixed symbols; see the
  `NOTICE` for details.
- **Privacy manifests are bundled**: `SYPlayerKit.framework` and `SYFFmpeg.framework`
  each ship a `PrivacyInfo.xcprivacy` — no tracking, no data collection. The declared
  required-reason APIs are file timestamp (`C617.1`, both) and disk space (`E174.1`,
  SYPlayerKit only, a free-space check before writing to the cache). Xcode rolls them
  into your app's privacy report at archive time.
- **The audio session is yours**: the framework never sets an `AVAudioSession` category
  or mode and never activates it (it only reads output latency and observes route
  changes). Configure it yourself if you need audio with the ringer switch off, or mixing
  with other apps.

## Building from source

Requires CMake ≥ 3.20, a clang with C++23 support, and Xcode for the Apple targets.

```bash
bash tools/build-ffmpeg.sh                     # produces build-ffmpeg/out/SYFFmpeg.xcframework (4 slices)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure --no-tests=error
```

FFmpeg artifacts are not checked in. CI should add `-DSYP_REQUIRE_FULL_MATRIX=ON` so that
"this machine lacks FFmpeg" turns the build red instead of silently skipping matrices
while `ctest` still exits 0.

**The two demo projects (`demo/ios`, `demo/mac`) will not open in a fresh clone, by
design**: their `project.pbxproj` embeds absolute paths from the machine that generated
them. Run `tools/build-ffmpeg.sh`, then `ruby demo/generate_xcodeprojects.rb` to
regenerate. The two projects under `examples/` are different — relative paths only, so
they open straight after a clone.

Distribution artifacts and examples:

```bash
tools/build-xcframework.sh --version 0.1.0   # both xcframeworks + zips + checksums, backfills Package.swift
tools/check-spm.sh                            # builds and tests the example apps on three destinations + verifies the release zips
SYPLAYER_LOCAL_BINARIES=1 xed examples/iOSExample     # open an example against local binaries (launch Xcode from a terminal)
```

Release order (**do not reorder**): build → run the gate → commit the backfilled
`Package.swift` → tag that commit → push commit and tag → create the GitHub Release **on
the existing tag** and upload **exactly the two zips whose checksums were computed in
step one**. Creating the Release first makes GitHub tag the pre-backfill commit, and the
checksums integrators resolve will not match the uploaded zips.

### Gates

Run these after any change (CI and local are the same):

```bash
bash tools/check-abi.sh              # public headers stay C-only + Swift binding check
bash tools/check-deploy-target.sh    # strict availability check at the iOS 13 deployment target
bash tools/check-demo-no-c-types.sh  # no C / bridge types leak into app-side sources
bash tools/check-ffmpeg-no-tls.sh    # no TLS backend inside the FFmpeg slices
bash tools/check-ffmpeg-symbols.sh --expect-slices 4 build-ffmpeg/out/SYFFmpeg.xcframework
cmake --build build --target syp_dl_purity_check      # cache layer: zero FFmpeg, zero platform symbols
ruby demo/generate_xcodeprojects.rb --check           # demo pbxproj files match the generator
bash tools/check-spm.sh                                # SwiftPM integration (run build-xcframework.sh first)
```

⚠️ `xcodebuild` Release builds must pass `ARCHS=arm64` explicitly, or they also compile
x86_64 and fail to link.

### Tests

There is more test code than production code. The download-and-cache layer is tested
entirely with in-memory stubs, a synchronous pump and injected clocks — **deterministic,
no sleeps, no network**; the platform backend and the end-to-end matrices use a
dependency-free loopback HTTP server with real media. Current size: **43 ctest targets,
829 test cases** (fully green in both `build` and `build-tsan`), plus **109 XCTest cases**
on the Swift side (`SYPlayerKitTests`, run under Mac Catalyst).

The verification rigs are held to the same standard: `syp_probe` diffs packet by packet
(payload hashes included) against "FFmpeg reading straight from `file:`", and with
`--decode` it diffs frame by frame (SHA-256 of pixels and samples) against "FFmpeg
decoding directly". New tests must be mutation-verified — break the implementation and
confirm the test goes red. That is a rule here, not a suggestion; `CLAUDE.md` explains
why, with the traps this repository has actually hit.

### Maintainers: upgrading FFmpeg and patching it

- **Upgrading**: change `FFMPEG_VERSION` at the top of `tools/build-ffmpeg.sh` and re-run
  it. The symbol list is regenerated from the static libraries with `nm` on every build,
  and the mapping header, export list and `NOTICE` all come from that same list, so
  symbols added or removed by a new version are picked up automatically — no hand-written
  lists to maintain. Then rebuild the xcframeworks and backfill the checksums.
- **Patching FFmpeg sources**: put patches in `tools/ffmpeg-patches/` (`*.patch`, `patch
  -p1` format). The script applies them in filename order after extracting the official
  tarball and aborts if any fails; `NOTICE` then lists the patch names and the files they
  touch (the "prominently marked changes" LGPL asks for). With an empty directory the
  `NOTICE` says the sources are unmodified.
- **Never** edit `build-ffmpeg/src` directly — it is not checked in, so edits are lost and
  nobody can reproduce your build.

## Status and known limits

Functional and structural correctness is backed by hard evidence, but a few boundaries
have to be stated plainly — do not read "all green" as more than it is:

- **Everything was verified against a loopback server on 127.0.0.1.** There is no field
  data from real devices or a real CDN.
- **Nothing has ever been installed on a physical iOS device** (no device or signing setup
  here); iOS conclusions come from the simulator, Mac Catalyst and code-level checks.
- **HLS currently runs in the test suite only**: `Pipeline::create_hls()` (VOD and live,
  multi-variant selection, separate audio/video tracks) is delivered and green, but the
  in-repo demo shells use the file / AVIO path.
- **Mac Catalyst has no hardware decoding** (VideoToolbox is disabled in that FFmpeg
  slice) — software decoding only.
- **Rate-limit accuracy and the buffering thresholds have not been measured on a real
  network**; the defaults are experience-based starting points.
- Per-milestone verdicts (including the conditions attached to "conditional pass") are in
  `docs/roadmap.md`; numbered known gaps in `docs/known-gaps.md`; registered technical
  debt in `docs/tech-debt.md`. All three are in Chinese.

## Repository layout

```
include/syplayer/   public C ABI (primitives, opaque pointers, PODs, function pointers)
src/dl/             download-and-cache layer, fully platform-independent
src/media/          demuxing / decoding / sync core / HLS
src/platform/apple/ NSURLSession, AudioUnit, Metal, VideoToolbox (Objective-C++)
swift/SYPlayerKit/  the Swift public API and the ObjC++ bridge
demo/               iOS + Mac Catalyst development shells (compiled from source, not a public ABI)
examples/           iOS and native macOS sample apps: SwiftPM-only integration, open after a clone
tests/              unit tests + in-memory backend stub + dependency-free loopback HTTP server
tools/              build scripts (FFmpeg / xcframework), gate scripts, the syp_probe verification tool
docs/               architecture, roadmap, known gaps, technical debt, toolchain
```

## Documentation

The detailed documents are in Chinese:

- `docs/architecture.md` — layers, the three seams, how HLS IO is taken over
- `docs/roadmap.md` — milestones, deliverables and verdicts (including conditional ones)
- `docs/known-gaps.md` — numbered known gaps, each with its evidence
- `docs/tech-debt.md` — registered debt and the false-green traps this repo has hit
- `CLAUDE.md` — contributor rules: gates, testing discipline, pitfalls
