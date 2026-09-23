# FFmpeg 源码补丁

本目录放对 FFmpeg 官方源码的修改。**默认为空**——本项目优先只靠 configure
开关裁剪，不改源码；确实非改不可时才往这里加补丁。

## 构建脚本怎么用它

`tools/build-ffmpeg.sh` 解压官方 tarball（校验过 sha256）之后：

1. 取本目录下所有 `*.patch`，按文件名的 C locale 字典序排序；
2. 逐个在源码根目录执行 `patch -p1 --forward --fuzz=0`（上下文必须逐行对上），**任何一个打不上就整个构建失败**，
   并删掉半打补丁的源码树；
3. 把"打了哪些补丁（sha256 + 文件名）"记进源码树的 `.syp-applied-patches`，
   打包时拷到 `build-ffmpeg/out/SYFFmpeg.applied-patches`。

源码树是缓存：补丁集有任何变化（增、删、改任一补丁），下次构建会自动删掉旧源码树
重新解压再打，不会在旧树上叠补丁。

`tools/build-ffmpeg.sh --print-notice` 生成随分发包附带的 NOTICE：没有补丁时写
"No source file was modified or patched."；有补丁时列出每个补丁的文件名、sha256
与它改动的文件——这是 LGPL 要求的"显著标明修改"。产物里记录的补丁集与本目录
当前内容不一致时，`--print-notice` 拒绝输出（产物过期，先重编）。

## 写补丁的规矩

- 文件名：`NNNN-简短英文描述.patch`（如 `0001-fix-hls-seek.patch`），用四位序号
  控制顺序；文件名里不要有空格。
- 格式：统一 diff，路径带一级前缀（`a/libavformat/hls.c` / `b/libavformat/hls.c`），
  即 `git diff` 或 `git format-patch` 在 FFmpeg 源码根目录的默认输出，适用 `-p1`。
- 每个补丁开头写清楚：改了什么、为什么不能靠 configure 解决、上游是否已有对应修复
  （有的话升级版本时删掉本补丁）。
- 升级 FFmpeg 版本时逐个确认补丁还打得上、还需要。
