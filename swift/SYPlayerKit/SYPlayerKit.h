// SYPlayerKit.h — SYPlayerKit framework 的公开伞头（umbrella header）。
//
// **这里刻意什么都不暴露。** SYPlayerKit 的公开 API 全部是 Swift（SYPlayer /
// SYPlayerState / SYPlayerError / SYPlayerView …），业务侧 `import SYPlayerKit`
// 拿到的就是那些 Swift 类型；ObjC++ 桥（SYPBridge.h）是实现细节，走
// PrivateHeaders + module.private.modulemap，**绝不能**在这里 #import 进来——
// 一旦进来，`int32_t*` 出参和裸 syp_status 就永久变成公开 API 的一部分。
//
// DEFINES_MODULE=YES 时 Xcode 按这个伞头自动生成 Modules/module.modulemap，
// 并把 Swift 半边挂成 `module SYPlayerKit.Swift`，所以本文件不需要（也不应该）
// 手写 modulemap。
#import <Foundation/Foundation.h>
