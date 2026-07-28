将官方 WebView2 Runtime 安装器放到这个目录，构建后会自动复制到输出目录。

推荐做法：
- `x64` 构建放 `MicrosoftEdgeWebView2RuntimeInstallerX64.exe`
- `Win32` 构建放 `MicrosoftEdgeWebView2RuntimeInstallerX86.exe`

兼容做法：
- 也可以放 `MicrosoftEdgeWebView2Setup.exe`，应用会优先尝试它

应用启动时的处理顺序：
1. 检测系统是否已安装 WebView2 Runtime
2. 如果缺失，优先尝试应用目录中的离线安装器
3. 如果仍然没有，再尝试临时目录缓存
4. 最后才回退到在线下载

适用场景：
- 客户网络禁止访问微软下载地址
- 目标机器无法通过 URLMon 成功下载 bootstrapper
- 需要把运行时安装能力随程序一起分发
