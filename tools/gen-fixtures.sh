#!/usr/bin/env bash
# gen-fixtures.sh — 随机生成 headless 验证用的 mp4 素材。
#
# 素材不进 git（落 build/ 下，被 .gitignore 挡住）。随机参数由 seed 决定，
# **失败时把 seed 塞回来即可复现同一份素材**——无种子的随机等于没测。
#
# 两份产物内容完全一致，只有 moov 布局不同：
#   moovend.mp4    默认布局，moov 在文件尾 —— 开流时必须先 seek 到尾部，
#                  这条才真正压 dl 层的 seek 补洞
#   faststart.mp4  由 moovend.mp4 -c copy 重排而来，保证媒体数据逐字节相同
#
# 另两份产物，跟前两份是完全独立的编码（不是 -c copy 出来的），彼此之间
# 才是 -c copy 重排关系（跟 moovend/faststart 同一个模式，只是换了编码
# 参数）：
#   bframes.mp4            -preset medium -bf 3 —— 带 B 帧，moov 在文件尾。
#   bframes_faststart.mp4  由 bframes.mp4 -c copy 重排而来。
# moovend/faststart 用 -preset ultrafast，libx264 在这个 preset 下不产 B 帧
# （bframes 参数被 ultrafast 强制清零），全程验证矩阵因此在一个真实
# H.264/HEVC 素材几乎必带 B 帧的场景上留了个结构性盲区：Pipeline 在带
# B 帧内容上会永久活锁丢帧（见 pipeline.cpp），但
# 没有任何素材能让它出现在测试里。bframes*.mp4 补的就是这个盲区，本脚本
# 生成后会用 ffprobe 现场断言 bframes.mp4 真的产出了 B 帧（pts!=dts 的
# 包数 > 0）——素材本身不带 B 帧的话，靠它做的回归用例就是空的，不能只靠
# "编码参数应该会产出 B 帧"这种一厢情愿。bframes_faststart.mp4 是 -c copy
# 重排（媒体数据逐字节不变），断言不需要在它身上重跑一遍。
set -euo pipefail

SEED=""
OUT=""
usage() { echo "用法: $0 [--seed N] --out DIR" >&2; }

while [ $# -gt 0 ]; do
  case "$1" in
    # set -u 之下，--seed/--out 是最后一个参数（没有跟值）时直接引用 $2
    # 会被 bash 判成 unbound variable，报的是解释器层面的内部错误而不是
    # 用法提示。先检查 $# 够不够，缺值时打印同一条用法提示再 exit 2。
    --seed)
      if [ $# -lt 2 ]; then usage; echo "缺 --seed 的值" >&2; exit 2; fi
      SEED="$2"; shift 2 ;;
    --out)
      if [ $# -lt 2 ]; then usage; echo "缺 --out 的值" >&2; exit 2; fi
      OUT="$2"; shift 2 ;;
    *) usage; exit 2 ;;
  esac
done
[ -n "$OUT" ] || { echo "缺 --out" >&2; exit 2; }
[ -n "$SEED" ] || SEED="$(date +%s)"

# --seed 必须是非负整数：它会被裸值嵌进 manifest.json 的 "seed": $SEED，
# 非数字（如 "abc"）会产出非法 JSON，后续按字段名解析 manifest 的任务会被埋雷。
case "$SEED" in
  ''|*[!0-9]*) echo "非法 --seed：必须是非负整数，收到 '$SEED'" >&2; exit 2 ;;
esac

# CMakeLists.txt 用 string(RANDOM LENGTH 8 ALPHABET "0123456789" ...) 抽种子，
# 产物带前导零（如 "00062260"）。上面的 case 只挡非数字，挡不住前导零——
# 而 JSON 数字语法不允许前导零（"0" 本身除外），"seed": 00062260 是非法 JSON。
# 10# 强制按十进制解释并去掉前导零，同时避免 bash 算术把前导零串当八进制解析。
SEED=$((10#$SEED))

echo "seed=$SEED"

command -v ffmpeg >/dev/null || { echo "需要 ffmpeg CLI：brew install ffmpeg" >&2; exit 3; }

# awk 的 srand(seed) 保证同 seed 同参数。每次 rand() 取值顺序固定。
read -r DUR W H FPS VBR GOP ARATE <<EOF
$(awk -v seed="$SEED" 'BEGIN{
  srand(seed);
  dur = 40 + int(rand()*61);                 # 40..100 秒
  split("640x360 854x480 1280x720", RES, " ");
  res = RES[1 + int(rand()*3)];
  split(res, wh, "x");
  split("24 25 30", F, " "); fps = F[1 + int(rand()*3)];
  vbr_cap = int(60 * 8000 / dur) - 128;      # 让估算体积不超过 60MB
  if (vbr_cap > 5000) vbr_cap = 5000;
  # 兜底：当前 dur 取值范围 40..100 秒内这行永远不会触发——dur 越大
  # vbr_cap 越小，最紧的 dur=100 时 vbr_cap 约 4672，仍然远高于 2500。
  # 有意保留而不是删掉：它是给 vbr_cap 兜的下界地板，防止以后有人把
  # 上面 `dur = 40 + int(rand()*61)` 的取值范围调大（duration 越长，
  # `60*8000/dur` 越小）时 vbr_cap 塌到不合理的低值甚至负数却没人察觉——
  # 到时候这行会真的触发，而不是像现在这样是死代码。
  if (vbr_cap < 2500) vbr_cap = 2500;
  vbr = 2500 + int(rand() * (vbr_cap - 2500 + 1));
  gop = fps * (1 + int(rand()*4));           # 1..4 秒一个 I 帧
  split("44100 48000", A, " "); arate = A[1 + int(rand()*2)];
  print dur, wh[1], wh[2], fps, vbr, gop, arate;
}')
EOF

mkdir -p "$OUT"
MOOVEND="$OUT/moovend.mp4"
FASTSTART="$OUT/faststart.mp4"

# -ac 2：sine 源本身是单声道，这里强制升到立体声（左右声道内容相同，
# 内容本身不重要——只有"声道数 > 1"这件事重要）。实测：
# 单声道素材会让 FFmpeg 的 planar 音频（AV_SAMPLE_FMT_FLTP）解码出 1 个
# 平面，frame_digest.cpp 里"按声道拼接多个平面"那段循环只跑 1 次迭代，
# channels>1 的分支从未被真实素材执行过——一条把"按声道拼接"这段代码
# 整体作废的变异体（memset 常量填充）在这份素材上 5/5 全绿地漏过了。
# 立体声（或以上）是让这段代码真正被验证到的前提，不是锦上添花。
#
# -fflags/-flags +bitexact：去掉编码器版本串与时间戳，让同 seed 的产物 sha256 稳定。
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "testsrc2=size=${W}x${H}:rate=${FPS}:duration=${DUR}" \
  -f lavfi -i "sine=frequency=440:sample_rate=${ARATE}:duration=${DUR}" \
  -fflags +bitexact -flags +bitexact \
  -c:v libx264 -preset ultrafast -b:v "${VBR}k" -g "${GOP}" -pix_fmt yuv420p \
  -c:a aac -ar "${ARATE}" -ac 2 -b:a 128k \
  "$MOOVEND"

# -c copy：只重排容器，媒体数据逐字节不变，两份素材的 packet 序列必然相同。
# -flags +bitexact 在这条命令里不生效（remux 不走编码器，这个 flag 只影响
# 编码器输出），跟上面第一遍编码那条命令保持对称补上，不留没道理的不一致。
ffmpeg -hide_banner -loglevel error -y -i "$MOOVEND" \
  -c copy -fflags +bitexact -flags +bitexact -movflags +faststart "$FASTSTART"

# bframes.mp4：独立编码（不是 -c copy 出来的），同一份 lavfi 源、同一套
# 分辨率/帧率/码率/GOP/采样率参数，唯一的区别是 -preset medium -bf 3——
# 让 libx264 真的产出 B 帧。moov 布局用默认（跟 moovend 一致）即可：这份
# 素材只喂给 Pipeline::create_file（本地文件路径），不测 dl 层/seek 补洞，
# moov 位置不影响它要验证的东西。
BFRAMES="$OUT/bframes.mp4"
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "testsrc2=size=${W}x${H}:rate=${FPS}:duration=${DUR}" \
  -f lavfi -i "sine=frequency=440:sample_rate=${ARATE}:duration=${DUR}" \
  -fflags +bitexact -flags +bitexact \
  -c:v libx264 -preset medium -bf 3 -b:v "${VBR}k" -g "${GOP}" -pix_fmt yuv420p \
  -c:a aac -ar "${ARATE}" -ac 2 -b:a 128k \
  "$BFRAMES"

# 硬断言，不是"生成参数里写了 -bf 3 应该就有 B 帧"式的一厢情愿：现场用
# ffprobe 数视频轨里 pts!=dts 的包数（B 帧重排序的直接后果——B 帧的
# 编码顺序（dts）晚于显示顺序（pts）里排在它前面的参照帧，I/P 帧在没有
# 后续 B 帧参照它们时 pts==dts）。数不出费文本处理独立工具的依赖：只用
# ffprobe + awk，跟本脚本其余部分风格一致。数值为 0 直接 exit，不把一个
# "看起来带 B 帧但其实没有"的空验证素材悄悄放出去。
command -v ffprobe >/dev/null || { echo "需要 ffprobe CLI：brew install ffmpeg" >&2; exit 3; }
BFRAMES_PTS_DTS_MISMATCH="$(
  ffprobe -v error -select_streams v:0 -show_entries packet=pts,dts -of csv=p=0 "$BFRAMES" \
    | awk -F',' '$1 != $2 { c++ } END { print c + 0 }'
)"
if [ "$BFRAMES_PTS_DTS_MISMATCH" -lt 1 ]; then
  echo "错误: bframes.mp4 没有产出任何 pts!=dts 的包（即没有真正的 B 帧）——" \
       "回归测试的前提不成立，不能继续" >&2
  exit 4
fi
echo "bframes: pts!=dts packets=$BFRAMES_PTS_DTS_MISMATCH"

# bframes_faststart.mp4：跟 faststart.mp4 是 moovend.mp4 的关系完全对称——
# -c copy 重排，媒体数据（因而 pts!=dts 的包集合）逐字节不变。经 dl 层的
# 场景（test_decode_e2e.cpp 场景 B）用它而不是 bframes.mp4 本身，跟这份
# 场景里其它三个场景（A/C/D）保持"同一份素材，只是访问路径不同"的既有
# 用法一致——这四个场景到今天为止一直共用 faststart.mp4 这一份文件，不是
# moovend.mp4；换成带 B 帧的素材后延续同一个选择，不引入跟 moov 布局
# 相关的、跟本次任务无关的变量。
BFRAMES_FASTSTART="$OUT/bframes_faststart.mp4"
ffmpeg -hide_banner -loglevel error -y -i "$BFRAMES" \
  -c copy -fflags +bitexact -flags +bitexact -movflags +faststart "$BFRAMES_FASTSTART"

# hevc.mp4：HEVC Main 8-bit，带 B 帧（x265 默认 bframes=4），-tag:v hvc1（Apple
# 平台 mp4 里 HEVC 的标准 tag）。同一套 lavfi 源与分辨率/帧率/GOP 参数。
HEVC="$OUT/hevc.mp4"
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i "testsrc2=size=${W}x${H}:rate=${FPS}:duration=${DUR}" \
  -f lavfi -i "sine=frequency=440:sample_rate=${ARATE}:duration=${DUR}" \
  -fflags +bitexact -flags +bitexact \
  -c:v libx265 -preset fast -x265-params "log-level=error:keyint=${GOP}" \
  -b:v "${VBR}k" -pix_fmt yuv420p -tag:v hvc1 \
  -c:a aac -ar "${ARATE}" -ac 2 -b:a 128k -movflags +faststart \
  "$HEVC"
HEVC_PTS_DTS_MISMATCH="$(
  ffprobe -v error -select_streams v:0 -show_entries packet=pts,dts -of csv=p=0 "$HEVC" \
    | awk -F',' '$1 != $2 { c++ } END { print c + 0 }'
)"
if [ "$HEVC_PTS_DTS_MISMATCH" -lt 1 ]; then
  echo "错误: hevc.mp4 没有 B 帧（pts!=dts 包数为 0），硬解重排序验证的前提不成立" >&2
  exit 4
fi
echo "hevc: pts!=dts packets=$HEVC_PTS_DTS_MISMATCH"

# ---------------------------------------------------------------------------
# HLS fixture。用 ffmpeg 的 hls muxer 切 fMP4 分片，三套：
#   vod_single  单码率。媒体数据来自同一份 source.mp4 的 -c copy remux——
#               不是重新编码——所以"HLS 播出来的帧数与直接播源
#               素材一致"这条断言天然成立（media.m3u8 里的 packet 就是
#               source.mp4 的 packet，不是"应该差不多"）。
#   vod_multi   三档码率（800k/400k/200k），ffmpeg 不写 master，手写一个
#               把三档 BANDWIDTH 分别标成对应值。
#   vod_demuxed 音视频分轨 + fMP4——Twitter amplify_video 的真实形状。用
#               -var_stream_map 一次调用切出两条独立播放列表，master 由
#               ffmpeg 自己生成（EXT-X-MEDIA:TYPE=AUDIO 引用音轨）。
#
# 分片时长 1 秒，4 秒源素材切出 4 个分片——够测"跨分片连续播放"和"跨
# 分片 seek"，又不会让 fixture 变大。这三套的分辨率/帧率/时长用固定值，
# 不随 --seed 变化：它们验证的是 HLS 播放路径的结构性行为（多档切换、
# 分轨合并、fMP4 分片边界），不是像 moovend/bframes 那样要在"真实素材的
# 分布"上验证解码算法——随机化这里只会让下游用例的断言变脆，换不来
# 对应的收益。GOP 取跟帧率相同的值，让关键帧每 1 秒一个，跟 1 秒分片
# 对齐（segment 边界必须落在关键帧上，ffmpeg 的 hls muxer 靠这个切分片）。
HLS_W=640
HLS_H=360
HLS_FPS=25
HLS_DUR=4
HLS_ARATE=44100
HLS_GOP=$HLS_FPS

gen_hls_vod_single() {
  local out="$1/hls/vod_single"
  mkdir -p "$out"
  local src="$out/source.mp4"
  ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "testsrc2=size=${HLS_W}x${HLS_H}:rate=${HLS_FPS}:duration=${HLS_DUR}" \
    -f lavfi -i "sine=frequency=440:sample_rate=${HLS_ARATE}:duration=${HLS_DUR}" \
    -fflags +bitexact -flags +bitexact \
    -c:v libx264 -preset ultrafast -b:v 800k -g "${HLS_GOP}" -pix_fmt yuv420p \
    -c:a aac -ar "${HLS_ARATE}" -ac 2 -b:a 128k \
    "$src"
  ffmpeg -hide_banner -loglevel error -y \
    -i "$src" \
    -c copy \
    -f hls -hls_time 1 -hls_playlist_type vod \
    -hls_segment_type fmp4 \
    -hls_fmp4_init_filename init.mp4 \
    -hls_segment_filename "$out/seg%d.m4s" \
    "$out/media.m3u8"
  # ffmpeg 不生成 master；手写一个只有一档的 master 指向它。VERSION 用 7
  # 而不是常见的 6——fMP4 分片靠 EXT-X-MAP 引用 init.mp4，这个标签要求
  # EXT-X-VERSION >= 7；ffmpeg 给 media.m3u8 自己写的版本号就是 7，master
  # 跟它保持一致，不留一个没道理的不一致。
  cat > "$out/master.m3u8" <<EOF
#EXTM3U
#EXT-X-VERSION:7
#EXT-X-INDEPENDENT-SEGMENTS
#EXT-X-STREAM-INF:BANDWIDTH=800000,CODECS="avc1.64001f,mp4a.40.2"
media.m3u8
EOF

  # 硬断言，不是"手写的 master 应该没问题"式的一厢情愿：master 能被
  # ffprobe 认成 hls，且从它数出来的视频帧数跟 source.mp4 逐字节相同。
  # 数值不对就是 fixture 本身错了，现在就地挡住——不要让它变成后面
  # 某个 e2e 用例里"莫名其妙差几帧"，那种错误极难查到根子在 fixture。
  local fmt_name
  fmt_name="$(ffprobe -v error -show_entries format=format_name -of csv=p=0 "$out/master.m3u8")"
  if [ "$fmt_name" != "hls" ]; then
    echo "错误: vod_single/master.m3u8 的 format_name 是 '$fmt_name'，不是 'hls'" >&2
    exit 5
  fi
  # 数帧数用 -show_entries frame=... 数行数，不用 -show_entries
  # stream=nb_read_frames：后者在探测 master.m3u8 这种走 HLS 分路复用器
  # 的输入时，ffprobe 会把同一条流按 program 分组再重复输出一遍（实测
  # 见证：单档 master 会把 video 流的 nb_read_frames 打印两遍），拿它
  # 直接跟 source.mp4 的单份输出做字符串比较必然判不等，不是 fixture
  # 真的有问题，是数错了指标。按帧/按包数行数不受这个问题影响。
  HLS_VOD_SINGLE_FRAMES="$(ffprobe -v error -select_streams v:0 \
    -show_entries frame=stream_index -of csv=p=0 "$out/master.m3u8" | wc -l | tr -d ' ')"
  local frames_src
  frames_src="$(ffprobe -v error -select_streams v:0 \
    -show_entries frame=stream_index -of csv=p=0 "$src" | wc -l | tr -d ' ')"
  if [ "$HLS_VOD_SINGLE_FRAMES" != "$frames_src" ]; then
    echo "错误: vod_single 帧数不一致——master.m3u8=$HLS_VOD_SINGLE_FRAMES," \
         "source.mp4=$frames_src" >&2
    exit 5
  fi
  HLS_VOD_SINGLE_DIR="$out"
  HLS_VOD_SINGLE_MASTER="$out/master.m3u8"
  HLS_VOD_SINGLE_SOURCE="$src"
  echo "vod_single: format=$fmt_name frames=${HLS_VOD_SINGLE_FRAMES} (与 source.mp4 一致)"
}

gen_hls_vod_multi() {
  local out="$1/hls/vod_multi"
  mkdir -p "$out"
  local names=(high mid low)
  local kbps=(800 400 200)
  local i
  for i in 0 1 2; do
    ffmpeg -hide_banner -loglevel error -y \
      -f lavfi -i "testsrc2=size=${HLS_W}x${HLS_H}:rate=${HLS_FPS}:duration=${HLS_DUR}" \
      -f lavfi -i "sine=frequency=440:sample_rate=${HLS_ARATE}:duration=${HLS_DUR}" \
      -fflags +bitexact -flags +bitexact \
      -c:v libx264 -preset ultrafast -b:v "${kbps[$i]}k" -g "${HLS_GOP}" -pix_fmt yuv420p \
      -c:a aac -ar "${HLS_ARATE}" -ac 2 -b:a 128k \
      -f hls -hls_time 1 -hls_playlist_type vod \
      -hls_segment_type fmp4 \
      -hls_fmp4_init_filename "init_${names[$i]}.mp4" \
      -hls_segment_filename "$out/seg_${names[$i]}%d.m4s" \
      "$out/media_${names[$i]}.m3u8"
  done
  # ffmpeg 每次调用只知道自己那一档，不会写 master；手写一个把三档的
  # BANDWIDTH 分别标成对应值（跟 kbps 数组一一对应，单位从 kbps 换算成 bps）。
  cat > "$out/master.m3u8" <<EOF
#EXTM3U
#EXT-X-VERSION:7
#EXT-X-INDEPENDENT-SEGMENTS
#EXT-X-STREAM-INF:BANDWIDTH=800000,CODECS="avc1.64001f,mp4a.40.2"
media_high.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=400000,CODECS="avc1.64001f,mp4a.40.2"
media_mid.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=200000,CODECS="avc1.64001f,mp4a.40.2"
media_low.m3u8
EOF

  # 硬断言：master 是 hls、真有三档、BANDWIDTH 正好是写进去的那三个值，
  # 且三档各自的 media playlist 都能被 ffprobe 单独打开（不是只挂在
  # master 底下看起来对，自己单独打开时才是它会被消费的方式）。
  local fmt_name
  fmt_name="$(ffprobe -v error -show_entries format=format_name -of csv=p=0 "$out/master.m3u8")"
  if [ "$fmt_name" != "hls" ]; then
    echo "错误: vod_multi/master.m3u8 的 format_name 是 '$fmt_name'，不是 'hls'" >&2
    exit 6
  fi
  local bw_count
  bw_count="$(grep -c '^#EXT-X-STREAM-INF:' "$out/master.m3u8")"
  if [ "$bw_count" != "3" ]; then
    echo "错误: vod_multi/master.m3u8 里 EXT-X-STREAM-INF 条数是 $bw_count，不是 3" >&2
    exit 6
  fi
  for i in 0 1 2; do
    local want=$((${kbps[$i]} * 1000))
    if ! grep -q "BANDWIDTH=${want}," "$out/master.m3u8"; then
      echo "错误: vod_multi/master.m3u8 里找不到 BANDWIDTH=${want}（档位 ${names[$i]}）" >&2
      exit 6
    fi
    if ! ffprobe -v error -show_entries format=format_name -of csv=p=0 \
        "$out/media_${names[$i]}.m3u8" >/dev/null; then
      echo "错误: vod_multi/media_${names[$i]}.m3u8 无法被 ffprobe 单独打开" >&2
      exit 6
    fi
  done
  HLS_VOD_MULTI_DIR="$out"
  HLS_VOD_MULTI_MASTER="$out/master.m3u8"
  echo "vod_multi: format=$fmt_name 三档 BANDWIDTH=800000/400000/200000，均可单独 ffprobe 打开"
}

gen_hls_vod_demuxed() {
  local out="$1/hls/vod_demuxed"
  mkdir -p "$out"
  # -var_stream_map "v:0,agroup:aud,name:video a:0,agroup:aud,name:audio"：
  # 把视频轨（v:0）和音频轨（a:0）各自切成一条独立播放列表，用同一个
  # audio group "aud" 关联——ffmpeg 因此会在 master 里写一条
  # EXT-X-MEDIA:TYPE=AUDIO 指向音频播放列表。name: 属性把 %v 占位符替换
  # 成 "video"/"audio" 而不是数字下标，产物文件名可读。
  ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "testsrc2=size=${HLS_W}x${HLS_H}:rate=${HLS_FPS}:duration=${HLS_DUR}" \
    -f lavfi -i "sine=frequency=440:sample_rate=${HLS_ARATE}:duration=${HLS_DUR}" \
    -fflags +bitexact -flags +bitexact \
    -c:v libx264 -preset ultrafast -b:v 800k -g "${HLS_GOP}" -pix_fmt yuv420p \
    -c:a aac -ar "${HLS_ARATE}" -ac 2 -b:a 128k \
    -map 0:v -map 1:a \
    -f hls -hls_time 1 -hls_playlist_type vod \
    -hls_segment_type fmp4 \
    -master_pl_name master.m3u8 \
    -var_stream_map "v:0,agroup:aud,name:video a:0,agroup:aud,name:audio" \
    -hls_fmp4_init_filename "init_%v.mp4" \
    -hls_segment_filename "$out/seg_%v_%d.m4s" \
    "$out/media_%v.m3u8"

  # 硬断言：master 是 hls、真有 EXT-X-MEDIA:TYPE=AUDIO，且视频/音频播放
  # 列表是两个不同的文件，各自打开后只有一条轨（不是"两条轨都在，只是
  # 音频轨被标了 default"这种看起来像分轨、其实没分开的情况）。
  local fmt_name
  fmt_name="$(ffprobe -v error -show_entries format=format_name -of csv=p=0 "$out/master.m3u8")"
  if [ "$fmt_name" != "hls" ]; then
    echo "错误: vod_demuxed/master.m3u8 的 format_name 是 '$fmt_name'，不是 'hls'" >&2
    exit 7
  fi
  if ! grep -q '^#EXT-X-MEDIA:TYPE=AUDIO' "$out/master.m3u8"; then
    echo "错误: vod_demuxed/master.m3u8 里没有 EXT-X-MEDIA:TYPE=AUDIO" >&2
    exit 7
  fi
  if [ ! -f "$out/media_video.m3u8" ] || [ ! -f "$out/media_audio.m3u8" ]; then
    echo "错误: vod_demuxed 期望的 media_video.m3u8/media_audio.m3u8 缺失" >&2
    exit 7
  fi
  # ffprobe 探测 HLS 播放列表时会把流按 program 分组再重复输出一遍
  # （跟上面数帧数踩到的是同一个 ffprobe 行为——参见 vod_single 里的
  # 注释），-of csv=p=0 因此会掺一行空行进来；sort -u 前先用 grep -v
  # '^$' 滤掉，不然空字符串会被当成"第二种 codec_type"，把本该相等
  # 的比较判成不等。
  local video_types audio_types
  video_types="$(ffprobe -v error -select_streams v -show_entries stream=codec_type \
    -of csv=p=0 "$out/media_video.m3u8" | grep -v '^$' | sort -u)"
  audio_types="$(ffprobe -v error -show_entries stream=codec_type -of csv=p=0 \
    "$out/media_audio.m3u8" | grep -v '^$' | sort -u)"
  if [ "$video_types" != "video" ]; then
    echo "错误: vod_demuxed/media_video.m3u8 不是纯视频（含: ${video_types}）" >&2
    exit 7
  fi
  if [ "$audio_types" != "audio" ]; then
    echo "错误: vod_demuxed/media_audio.m3u8 不是纯音频（含: ${audio_types}）" >&2
    exit 7
  fi
  HLS_VOD_DEMUXED_DIR="$out"
  HLS_VOD_DEMUXED_MASTER="$out/master.m3u8"
  HLS_VOD_DEMUXED_VIDEO_PL="$out/media_video.m3u8"
  HLS_VOD_DEMUXED_AUDIO_PL="$out/media_audio.m3u8"
  echo "vod_demuxed: format=$fmt_name EXT-X-MEDIA:TYPE=AUDIO 存在，音视频各在独立文件里各一条轨"
}

gen_hls_vod_single "$OUT"
gen_hls_vod_multi "$OUT"
gen_hls_vod_demuxed "$OUT"

sha_of() { shasum -a 256 "$1" | awk '{print $1}'; }
bytes_of() { stat -f %z "$1"; }
# manifest 里唯一的自由文本字段是 path：转义反斜杠与双引号，防止路径本身
# 含这两个字符时破坏 JSON（同源问题：manifest 的 JSON 合法性从没被真正
# 保证过，seed 的前导零是一处，这是另一处）。
json_escape() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  printf '%s' "$s"
}

# 八次调用先落到变量里：heredoc 内容里的命令替换失败不会被外层 cat 的退出码
# 反映出来，set -e 管不住；提到 heredoc 之前赋值，失败才能立刻让脚本退出。
MOOVEND_BYTES="$(bytes_of "$MOOVEND")"
MOOVEND_SHA="$(sha_of "$MOOVEND")"
FASTSTART_BYTES="$(bytes_of "$FASTSTART")"
FASTSTART_SHA="$(sha_of "$FASTSTART")"
BFRAMES_BYTES="$(bytes_of "$BFRAMES")"
BFRAMES_SHA="$(sha_of "$BFRAMES")"
BFRAMES_FASTSTART_BYTES="$(bytes_of "$BFRAMES_FASTSTART")"
BFRAMES_FASTSTART_SHA="$(sha_of "$BFRAMES_FASTSTART")"
HEVC_BYTES="$(bytes_of "$HEVC")"
HEVC_SHA="$(sha_of "$HEVC")"
MOOVEND_PATH_JSON="$(json_escape "$MOOVEND")"
FASTSTART_PATH_JSON="$(json_escape "$FASTSTART")"
BFRAMES_PATH_JSON="$(json_escape "$BFRAMES")"
BFRAMES_FASTSTART_PATH_JSON="$(json_escape "$BFRAMES_FASTSTART")"
HEVC_PATH_JSON="$(json_escape "$HEVC")"

# HLS fixture 的路径同样要转义——跟上面四份素材同一个理由（manifest 的
# JSON 合法性从没被真正保证过，这是又一处自由文本字段）。
HLS_VOD_SINGLE_DIR_JSON="$(json_escape "$HLS_VOD_SINGLE_DIR")"
HLS_VOD_SINGLE_MASTER_JSON="$(json_escape "$HLS_VOD_SINGLE_MASTER")"
HLS_VOD_SINGLE_SOURCE_JSON="$(json_escape "$HLS_VOD_SINGLE_SOURCE")"
HLS_VOD_MULTI_DIR_JSON="$(json_escape "$HLS_VOD_MULTI_DIR")"
HLS_VOD_MULTI_MASTER_JSON="$(json_escape "$HLS_VOD_MULTI_MASTER")"
HLS_VOD_DEMUXED_DIR_JSON="$(json_escape "$HLS_VOD_DEMUXED_DIR")"
HLS_VOD_DEMUXED_MASTER_JSON="$(json_escape "$HLS_VOD_DEMUXED_MASTER")"
HLS_VOD_DEMUXED_VIDEO_PL_JSON="$(json_escape "$HLS_VOD_DEMUXED_VIDEO_PL")"
HLS_VOD_DEMUXED_AUDIO_PL_JSON="$(json_escape "$HLS_VOD_DEMUXED_AUDIO_PL")"

cat > "$OUT/manifest.json" <<JSON
{
  "seed": $SEED,
  "duration_s": $DUR,
  "width": $W,
  "height": $H,
  "fps": $FPS,
  "video_bitrate_kbps": $VBR,
  "gop": $GOP,
  "audio_rate": $ARATE,
  "audio_channels": 2,
  "files": {
    "moovend":           { "path": "$MOOVEND_PATH_JSON",           "bytes": $MOOVEND_BYTES,           "sha256": "$MOOVEND_SHA" },
    "faststart":         { "path": "$FASTSTART_PATH_JSON",         "bytes": $FASTSTART_BYTES,         "sha256": "$FASTSTART_SHA" },
    "bframes":           { "path": "$BFRAMES_PATH_JSON",           "bytes": $BFRAMES_BYTES,           "sha256": "$BFRAMES_SHA",
                           "pts_dts_mismatches": $BFRAMES_PTS_DTS_MISMATCH },
    "bframes_faststart": { "path": "$BFRAMES_FASTSTART_PATH_JSON", "bytes": $BFRAMES_FASTSTART_BYTES, "sha256": "$BFRAMES_FASTSTART_SHA" },
    "hevc":              { "path": "$HEVC_PATH_JSON",              "bytes": $HEVC_BYTES,              "sha256": "$HEVC_SHA" }
  },
  "hls": {
    "duration_s": $HLS_DUR,
    "width": $HLS_W,
    "height": $HLS_H,
    "fps": $HLS_FPS,
    "audio_rate": $HLS_ARATE,
    "vod_single": {
      "dir":         "$HLS_VOD_SINGLE_DIR_JSON",
      "master":      "$HLS_VOD_SINGLE_MASTER_JSON",
      "source":      "$HLS_VOD_SINGLE_SOURCE_JSON",
      "segment_type": "fmp4",
      "frame_count": $HLS_VOD_SINGLE_FRAMES
    },
    "vod_multi": {
      "dir":    "$HLS_VOD_MULTI_DIR_JSON",
      "master": "$HLS_VOD_MULTI_MASTER_JSON",
      "segment_type": "fmp4",
      "variants": [
        { "name": "high", "bandwidth": 800000, "playlist": "media_high.m3u8" },
        { "name": "mid",  "bandwidth": 400000, "playlist": "media_mid.m3u8" },
        { "name": "low",  "bandwidth": 200000, "playlist": "media_low.m3u8" }
      ]
    },
    "vod_demuxed": {
      "dir":            "$HLS_VOD_DEMUXED_DIR_JSON",
      "master":         "$HLS_VOD_DEMUXED_MASTER_JSON",
      "video_playlist": "$HLS_VOD_DEMUXED_VIDEO_PL_JSON",
      "audio_playlist": "$HLS_VOD_DEMUXED_AUDIO_PL_JSON",
      "segment_type": "fmp4"
    }
  }
}
JSON

echo "moovend=$MOOVEND ($MOOVEND_BYTES bytes)"
echo "faststart=$FASTSTART ($FASTSTART_BYTES bytes)"
echo "bframes=$BFRAMES ($BFRAMES_BYTES bytes)"
echo "bframes_faststart=$BFRAMES_FASTSTART ($BFRAMES_FASTSTART_BYTES bytes)"
echo "hevc=$HEVC ($HEVC_BYTES bytes)"
echo "hls/vod_single=$HLS_VOD_SINGLE_MASTER"
echo "hls/vod_multi=$HLS_VOD_MULTI_MASTER"
echo "hls/vod_demuxed=$HLS_VOD_DEMUXED_MASTER"
