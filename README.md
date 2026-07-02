# Icon Sphere

桌面图标 3D 球形旋转工具，将 Windows 桌面图标排列成一个可交互的立体球体。

## 功能

- 桌面图标自动排列为 3D 球体，持续旋转
- 鼠标拖拽旋转球体，双击打开图标
- 多种旋转模式：水平 / 垂直 / 对角线 / 摇摆
- 球体大小、图标大小、透明度、帧率等均可调
- 支持文件名显示（按类型着色：文件夹 / 应用 / 文件）
- 脉动动画（缓慢 / 心跳模式）
- 开机自启，系统托盘常驻
- 鼠标悬停暂停旋转

## 使用

直接运行 `icon_sphere.exe`，桌面图标会自动飞入球体。右键托盘图标可暂停、设置或退出。

## 配置

编辑同目录下的 `icon_sphere.ini` 可自定义参数，或通过托盘菜单中的「设置」对话框调整。

## 编译

使用 MinGW g++ 编译：

```bash
g++ -O2 -std=c++17 -o icon_sphere.exe main.cpp resource.rc -lgdiplus -ld2d1 -ldwrite -ldwmapi -lcomctl32 -mwindows
```

## 技术栈

- Win32 API + GDI+ + Direct2D + DirectWrite
- Fibonacci 球面分布算法
- 分层窗口（Layered Window）透明渲染
