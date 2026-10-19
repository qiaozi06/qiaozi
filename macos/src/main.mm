// CrossRemote macOS 被控端（Objective-C++）
// 信令：NSURLSessionWebSocketTask
// 抓屏：CGDisplayStream（需「屏幕录制」权限）
// 输入：CGEventPost
// 媒体：WebRTC.framework（GoogleWebRTC / libwebrtc ObjC 接口）
//
// 构建：见 README.md（xcodebuild 或 clang++ 直接编译）
#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <WebRTC/WebRTC.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "cr_e2ee.h"
#include "cr_proto.h"

static NSString *const kDefaultServer = @"wss://desk.qiaozi06.icu/ws?role=agent";

// ---------------------------------------------------------------- 配置
@interface CRConfig : NSObject
@property(nonatomic, copy) NSString *server;
@property(nonatomic, copy) NSString *deviceCode;
@property(nonatomic, copy) NSString *password;
@property(nonatomic, copy) NSString *deviceName;
@property(nonatomic, assign) BOOL autoAccept;
@end

@implementation CRConfig
+ (instancetype)loadOrCreate {
  CRConfig *c = [CRConfig new];
  c.server = kDefaultServer;
  c.deviceCode = @"";
  c.password = @"";
  c.deviceName = [[NSHost currentHost] localizedName] ?: @"Mac";
  c.autoAccept = YES;
  NSString *path = [CRConfig path];
  NSData *data = [NSData dataWithContentsOfFile:path];
  if (data) {
    NSDictionary *j = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (j) {
      if (j[@"server"]) c.server = j[@"server"];
      if (j[@"device_code"]) c.deviceCode = j[@"device_code"];
      if (j[@"password"]) c.password = j[@"password"];
      if (j[@"device_name"]) c.deviceName = j[@"device_name"];
      if (j[@"auto_accept"]) c.autoAccept = [j[@"auto_accept"] boolValue];
    }
  }
  return c;
}
+ (NSString *)path {
  NSString *dir = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES) firstObject];
  dir = [dir stringByAppendingPathComponent:@"CrossRemote"];
  [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
  return [dir stringByAppendingPathComponent:@"agent.json"];
}
- (void)save {
  NSDictionary *j = @{@"server": self.server ?: @"", @"device_code": self.deviceCode ?: @"",
                      @"password": self.password ?: @"", @"device_name": self.deviceName ?: @"",
                      @"auto_accept": @(self.autoAccept)};
  NSData *d = [NSJSONSerialization dataWithJSONObject:j options:NSJSONWritingPrettyPrinted error:nil];
  [d writeToFile:[CRConfig path] atomically:YES];
}
@end

// ---------------------------------------------------------------- VK -> CGKeyCode
static CGKeyCode CGCodeForVK(uint32_t vk) {
  // 浏览器端发送的是 Windows 虚拟键码，这里做最小可用映射。
  static const std::map<uint32_t, CGKeyCode> m = {
      {8, 51},   {9, 48},   {13, 36},  {16, 56},  {17, 59},  {18, 58},  {19, 10},
      {20, 57},  {27, 53},  {32, 49},  {33, 116}, {34, 121}, {35, 119}, {36, 115},
      {37, 123}, {38, 126}, {39, 124}, {40, 125}, {45, 114}, {46, 117}, {91, 55},
      {92, 55},  {93, 110}, {106, 67}, {107, 69}, {109, 78}, {110, 65}, {111, 75},
      {186, 41}, {187, 24}, {188, 43}, {189, 27}, {190, 47}, {191, 44}, {192, 50},
      {219, 33}, {220, 42}, {221, 30}, {222, 39},
  };
  auto it = m.find(vk);
  if (it != m.end()) return it->second;
  if (vk >= 'A' && vk <= 'Z') return (CGKeyCode)(vk - 'A' + 0);  // kVK_ANSI_A = 0..25
  if (vk >= '0' && vk <= '9') return (CGKeyCode)(vk - '0' + 29);
  if (vk >= 112 && vk <= 123) return (CGKeyCode)(vk - 112 + 122);  // F1..F12（近似）
  return 0;
}

// ---------------------------------------------------------------- 抓屏
@interface CRScreenSource : NSObject
@property(nonatomic, strong) RTCVideoSource *source;
@property(nonatomic, assign) CGDirectDisplayID displayId;
@property(nonatomic, assign) size_t width;
@property(nonatomic, assign) size_t height;
- (instancetype)initWithSource:(RTCVideoSource *)source;
- (BOOL)start;
- (void)stop;
@end

@implementation CRScreenSource {
  CGDisplayStreamRef _stream;
  dispatch_queue_t _queue;
  RTCVideoCapturer *_capturer;
}

- (instancetype)initWithSource:(RTCVideoSource *)source {
  if ((self = [super init])) {
    _source = source;
    _capturer = [[RTCVideoCapturer alloc] init];
    _displayId = CGMainDisplayID();
    _width = CGDisplayPixelsWide(_displayId);
    _height = CGDisplayPixelsHigh(_displayId);
    _queue = dispatch_queue_create("crossremote.capture", DISPATCH_QUEUE_SERIAL);
    [_source adaptOutputFormatToWidth:(int32_t)_width height:(int32_t)_height fps:30];
  }
  return self;
}

- (BOOL)start {
  __weak CRScreenSource *weakSelf = self;
  _stream = CGDisplayStreamCreate(_displayId, _width, _height, 'BGRA', nil,
                                  ^(CGDisplayStreamFrameStatus status, uint64_t time,
                                    IOSurfaceRef frame, CGDisplayStreamUpdateRef update) {
                                    if (status == kCGDisplayStreamFrameStatusFrameComplete && frame) {
                                      [weakSelf pushSurface:frame time:time];
                                    }
                                  });
  if (!_stream) return NO;
  return CGDisplayStreamStart(_stream) == kCGErrorSuccess;
}

- (void)pushSurface:(IOSurfaceRef)surface time:(uint64_t)time {
  CVPixelBufferRef pb = nullptr;
  NSDictionary *attrs = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{}};
  if (CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, surface,
                                       (__bridge CFDictionaryRef)attrs, &pb) != kCVReturnSuccess) {
    return;
  }
  RTCCVPixelBuffer *buffer = [[RTCCVPixelBuffer alloc] initWithPixelBuffer:pb];
  RTCVideoFrame *frame = [[RTCVideoFrame alloc] initWithBuffer:buffer
                                                      rotation:RTCVideoRotation_0
                                                   timeStampNs:(int64_t)(time * 1000)];
  [self.source capturer:_capturer didCaptureVideoFrame:frame];
  CVPixelBufferRelease(pb);
}

- (void)stop {
  if (_stream) {
    CGDisplayStreamStop(_stream);
    CFRelease(_stream);
    _stream = NULL;
  }
}
@end

// ---------------------------------------------------------------- 输入注入
static void InjectInput(const cr::InputEvent &ev) {
  switch (ev.kind) {
    case cr::InputEvent::kMove:
      CGWarpMouseCursorPosition(CGPointMake(ev.x, ev.y));
      break;
    case cr::InputEvent::kButton: {
      CGEventType t = kCGEventOtherMouseDown;
      CGMouseButton b = kCGMouseButtonLeft;
      if (ev.button == 1) { b = kCGMouseButtonRight; t = ev.pressed ? kCGEventRightMouseDown : kCGEventRightMouseUp; }
      else if (ev.button == 2) { b = kCGMouseButtonCenter; t = ev.pressed ? kCGEventOtherMouseDown : kCGEventOtherMouseUp; }
      else { t = ev.pressed ? kCGEventLeftMouseDown : kCGEventLeftMouseUp; }
      CGEventRef e = CGEventCreateMouseEvent(NULL, t, CGPointMake(ev.x, ev.y), b);
      if (e) { CGEventPost(kCGHIDEventTap, e); CFRelease(e); }
      break;
    }
    case cr::InputEvent::kWheel: {
      CGEventRef e = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitLine, 1, -ev.delta_y / 120.0);
      if (e) { CGEventPost(kCGHIDEventTap, e); CFRelease(e); }
      break;
    }
    case cr::InputEvent::kKey: {
      CGKeyCode code = CGCodeForVK(ev.keycode);
      CGEventRef e = CGEventCreateKeyboardEvent(NULL, code, ev.pressed);
      if (e) { CGEventPost(kCGHIDEventTap, e); CFRelease(e); }
      break;
    }
    default:
      break;
  }
}

// ---------------------------------------------------------------- Agent
@interface CRAgent : NSObject <NSURLSessionWebSocketDelegate, RTCPeerConnectionDelegate, RTCDataChannelDelegate>
@property(nonatomic, strong) CRConfig *cfg;
@property(nonatomic, strong) NSURLSession *session;
@property(nonatomic, strong) NSURLSessionWebSocketTask *ws;
@property(nonatomic, strong) RTCPeerConnectionFactory *factory;
@property(nonatomic, strong) RTCPeerConnection *pc;
@property(nonatomic, strong) RTCDataChannel *dc;
@property(nonatomic, strong) RTCVideoSource *videoSource;
@property(nonatomic, strong) RTCVideoTrack *videoTrack;
@property(nonatomic, strong) CRScreenSource *screen;
@property(nonatomic, copy) NSString *sessionId;
@property(nonatomic, copy) NSString *deviceCode;
@property(nonatomic, copy) NSString *password;
@property(nonatomic, strong) NSMutableArray *iceServers;
- (void)send:(NSString *)type payload:(NSDictionary *)payload;
@end

@implementation CRAgent {
  cr::E2ee _e2ee;
}

- (instancetype)initWithConfig:(CRConfig *)cfg {
  if ((self = [super init])) {
    _cfg = cfg;
    _deviceCode = cfg.deviceCode;
    _password = cfg.password;
    _factory = [[RTCPeerConnectionFactory alloc] init];
    _iceServers = [NSMutableArray array];
  }
  return self;
}

- (void)start {
  NSURL *url = [NSURL URLWithString:self.cfg.server];
  NSURLSessionConfiguration *c = [NSURLSessionConfiguration defaultSessionConfiguration];
  self.session = [NSURLSession sessionWithConfiguration:c delegate:self delegateQueue:nil];
  self.ws = [self.session webSocketTaskWithURL:url];
  [self.ws resume];
  [self send:@"register" payload:@{
    @"device_code": self.cfg.deviceCode ?: @"",
    @"password": self.cfg.password ?: @"",
    @"os_type": @2,
    @"device_name": self.cfg.deviceName ?: @"",
    @"version": @"1.0.0",
    @"screen_resolution": [NSString stringWithFormat:@"%zux%zu",
                           CGDisplayPixelsWide(CGMainDisplayID()),
                           CGDisplayPixelsHigh(CGMainDisplayID())],
    @"e2ee": @YES
  }];
  [self receiveLoop];
  NSLog(@"CrossRemote macOS agent 启动，信令 %@", url);
}

- (void)receiveLoop {
  __weak CRAgent *weakSelf = self;
  [self.ws receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage *msg, NSError *err) {
    if (err) {
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        [weakSelf start];
      });
      return;
    }
    if (msg.type == NSURLSessionWebSocketMessageTypeString) {
      [weakSelf handleSignal:msg.string];
    } else if (msg.type == NSURLSessionWebSocketMessageTypeData) {
      // DataChannel 走 WebRTC，不走信令；此处忽略
    }
    [weakSelf receiveLoop];
  }];
}

- (void)send:(NSString *)type payload:(NSDictionary *)payload {
  NSDictionary *msg = @{@"msg_type": type ?: @"", @"payload": payload ?: @{}};
  NSData *d = [NSJSONSerialization dataWithJSONObject:msg options:0 error:nil];
  NSString *s = [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding];
  [self.ws sendMessage:[[NSURLSessionWebSocketMessage alloc] initWithString:s]
     completionHandler:^(NSError *e) {}];
}

- (void)handleSignal:(NSString *)text {
  NSData *d = [text dataUsingEncoding:NSUTF8StringEncoding];
  NSDictionary *j = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
  if (!j) return;
  NSString *type = j[@"msg_type"] ?: @"";
  NSDictionary *p = j[@"payload"] ?: @{};

  if ([type isEqualToString:@"register_ok"]) {
    self.deviceCode = p[@"device_code"] ?: self.deviceCode;
    self.password = p[@"password"] ?: self.password;
    self.cfg.deviceCode = self.deviceCode;
    self.cfg.password = self.password;
    [self.cfg save];
    [self.iceServers removeAllObjects];
    for (NSDictionary *s in (p[@"ice_servers"] ?: @[])) {
      RTCIceServer *is = [[RTCIceServer alloc] initWithURLStrings:s[@"urls"] ?: @[]
                                                         username:s[@"username"] ?: @""
                                                       credential:s[@"credential"] ?: @""];
      [self.iceServers addObject:is];
    }
    NSLog(@"已注册 设备码=%@ 密码=%@", self.deviceCode, self.password);
  } else if ([type isEqualToString:@"call"]) {
    self.sessionId = p[@"session_id"] ?: @"";
    BOOL accept = self.cfg.autoAccept;
    [self send:@"call_resp" payload:@{@"session_id": self.sessionId ?: @"", @"accept": @(accept)}];
    if (accept) [self startSession];
  } else if ([type isEqualToString:@"sdp_answer"]) {
    NSString *sdp = p[@"sdp"] ?: @"";
    RTCSessionDescription *desc = [[RTCSessionDescription alloc] initWithType:RTCSdpTypeAnswer sdp:sdp];
    [self.pc setRemoteDescription:desc completionHandler:^(NSError *e) {
      if (e) NSLog(@"setRemoteDescription 失败: %@", e);
    }];
  } else if ([type isEqualToString:@"ice_candidate"]) {
    NSDictionary *c = p[@"candidate"];
    if (![c isKindOfClass:[NSDictionary class]]) return;
    RTCIceCandidate *cand = [[RTCIceCandidate alloc] initWithSdp:c[@"candidate"] ?: @""
                                                   sdpMLineIndex:[c[@"sdpMLineIndex"] intValue]
                                                          sdpMid:c[@"sdpMid"] ?: @""];
    [self.pc addIceCandidate:cand completionHandler:^(NSError *e) {}];
  } else if ([type isEqualToString:@"session_close"]) {
    [self endSession];
  }
}

- (void)startSession {
  [self endSession];
  // 每个会话重新生成 ECDH 密钥对；控制端在 DataChannel open 后先发 KEY 帧。
  if (!_e2ee.Init()) NSLog(@"E2EE 初始化失败，将回退明文帧");
  RTCConfiguration *cfg = [[RTCConfiguration alloc] init];
  cfg.iceServers = self.iceServers;
  cfg.sdpSemantics = RTCSdpSemanticsUnifiedPlan;
  self.pc = [self.factory peerConnectionWithConfiguration:cfg constraints:[self offerConstraints] delegate:self];

  self.videoSource = [self.factory videoSource];
  self.videoTrack = [self.factory videoTrackWithSource:self.videoSource trackId:@"screen"];
  [self.pc addTrack:self.videoTrack streamIds:@[@"stream0"]];

  RTCDataChannelConfiguration *dcc = [[RTCDataChannelConfiguration alloc] init];
  dcc.isOrdered = YES;
  self.dc = [self.pc dataChannelForLabel:@"control-channel" configuration:dcc];
  self.dc.delegate = self;

  self.screen = [[CRScreenSource alloc] initWithSource:self.videoSource];
  [self.screen start];

  __weak CRAgent *weakSelf = self;
  [self.pc offerForConstraints:[self offerConstraints] completionHandler:^(RTCSessionDescription *sdp, NSError *e) {
    if (e || !sdp) { NSLog(@"createOffer 失败: %@", e); return; }
    [weakSelf.pc setLocalDescription:sdp completionHandler:^(NSError *err) {
      if (err) { NSLog(@"setLocalDescription 失败: %@", err); return; }
      [weakSelf send:@"sdp_offer" payload:@{@"session_id": weakSelf.sessionId ?: @"",
                                            @"sdp": sdp.sdp ?: @"", @"type": @"offer"}];
    }];
  }];
}

- (RTCMediaConstraints *)offerConstraints {
  return [[RTCMediaConstraints alloc]
      initWithMandatoryConstraints:@{ @"OfferToReceiveAudio": @"false", @"OfferToReceiveVideo": @"false" }
               optionalConstraints:nil];
}

- (void)endSession {
  [self.screen stop];
  self.screen = nil;
  [self.dc close];
  self.dc = nil;
  [self.pc close];
  self.pc = nil;
  self.sessionId = nil;
}

#pragma mark - DataChannel

- (void)dataChannelDidChangeState:(RTCDataChannel *)dataChannel {
  if (dataChannel.readyState == RTCDataChannelStateOpen) {
    [self sendScreenInfo];
  }
}

- (void)dataChannel:(RTCDataChannel *)dataChannel didReceiveMessageWithBuffer:(RTCDataBuffer *)buffer {
  const uint8_t *bytes = (const uint8_t *)buffer.data.bytes;
  NSUInteger len = buffer.data.length;
  if (!len) return;
  uint8_t type = bytes[0];
  if (type == cr::kKey) {
    // [8][65B 未压缩 P-256 公钥]：派生密钥后立即回自己的公钥帧。
    if (len == 66 && _e2ee.AcceptPeerKey(bytes + 1, 65)) {
      auto kf = _e2ee.KeyFrame();
      if (!kf.empty()) [self sendBinary:kf];
    }
    return;
  }
  std::vector<uint8_t> body;
  if (type == cr::kEnc) {
    uint8_t inner = 0;
    if (!_e2ee.Ready() || !_e2ee.Decrypt(bytes, len, &inner, &body)) return;
    type = inner;
  } else {
    body.assign(bytes + 1, bytes + len);
  }
  if (type == cr::kInput) {
    InjectInput(cr::decodeInput(body));
  } else if (type == cr::kClipboard) {
    cr::Reader r(body);
    std::string text;
    while (!r.eof()) {
      uint64_t t = r.varint();
      int f = (int)(t >> 3), w = (int)(t & 7);
      if (f == 1 && w == 2) text = r.string(); else r.skip(w);
    }
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:[NSString stringWithUTF8String:text.c_str()] forType:NSPasteboardTypeString];
  } else if (type == cr::kFile) {
    cr::FileChunk f = cr::decodeFile(body);
    [self saveChunk:f];
  } else if (type == cr::kCtrl) {
    if (cr::decodeCtrl(body).cmd == cr::kCtrlClose) [self endSession];
  }
}

- (void)sendScreenInfo {
  size_t w = CGDisplayPixelsWide(CGMainDisplayID());
  size_t h = CGDisplayPixelsHigh(CGMainDisplayID());
  NSDictionary *meta = @{@"action": @"screen_info", @"width": @(w), @"height": @(h),
                         @"monitors": @[@{@"index": @0, @"width": @(w), @"height": @(h)}]};
  NSData *d = [NSJSONSerialization dataWithJSONObject:meta options:0 error:nil];
  std::vector<uint8_t> payload((const uint8_t *)d.bytes, (const uint8_t *)d.bytes + d.length);
  [self sendFrame:cr::kMeta body:payload];
}

// 已协商 E2EE 时发加密帧，否则回退明文帧。
- (void)sendFrame:(uint8_t)type body:(const std::vector<uint8_t> &)body {
  std::vector<uint8_t> out;
  if (_e2ee.Ready() && _e2ee.Encrypt(type, body.data(), body.size(), &out)) {
    [self sendBinary:out];
    return;
  }
  out.reserve(body.size() + 1);
  out.push_back(type);
  out.insert(out.end(), body.begin(), body.end());
  [self sendBinary:out];
}

- (void)sendBinary:(const std::vector<uint8_t> &)data {
  if (!self.dc || self.dc.readyState != RTCDataChannelStateOpen) return;
  NSData *d = [NSData dataWithBytes:data.data() length:data.size()];
  [self.dc sendData:[[RTCDataBuffer alloc] initWithData:d isBinary:YES]];
}

- (void)saveChunk:(const cr::FileChunk &)f {
  NSString *dir = [NSSearchPathForDirectoriesInDomains(NSDownloadsDirectory, NSUserDomainMask, YES) firstObject];
  NSString *path = [dir stringByAppendingPathComponent:[NSString stringWithUTF8String:f.file_name.c_str()]];
  NSFileHandle *fh = [NSFileHandle fileHandleForWritingAtPath:path];
  if (!fh) {
    [[NSFileManager defaultManager] createFileAtPath:path contents:nil attributes:nil];
    fh = [NSFileHandle fileHandleForWritingAtPath:path];
  }
  [fh seekToEndOfFile];
  [fh writeData:[NSData dataWithBytes:f.chunk_data.data() length:f.chunk_data.size()]];
  [fh closeFile];
}

#pragma mark - PeerConnection delegate

- (void)peerConnection:(RTCPeerConnection *)peerConnection
    didGenerateIceCandidate:(RTCIceCandidate *)candidate {
  NSDictionary *c = @{@"candidate": candidate.sdp ?: @"",
                      @"sdpMid": candidate.sdpMid ?: @"",
                      @"sdpMLineIndex": @(candidate.sdpMLineIndex)};
  [self send:@"ice_candidate" payload:@{@"session_id": self.sessionId ?: @"", @"candidate": c,
                                        @"sdpMid": candidate.sdpMid ?: @"",
                                        @"sdpMLineIndex": @(candidate.sdpMLineIndex)}];
}

@end

int main(int argc, const char *argv[]) {
  @autoreleasepool {
    CRConfig *cfg = [CRConfig loadOrCreate];
    CRAgent *agent = [[CRAgent alloc] initWithConfig:cfg];
    [agent start];
    [[NSRunLoop currentRunLoop] run];
  }
  return 0;
}
