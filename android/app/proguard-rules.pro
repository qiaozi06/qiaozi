# 保留 WebRTC / OkHttp 反射使用到的类
-keep class org.webrtc.** { *; }
-dontwarn org.webrtc.**
-keep class okhttp3.** { *; }
-dontwarn okhttp3.**
-dontwarn okio.**
