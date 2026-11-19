# v1.0.2

第一个公开版本，基于 [m417z](https://github.com/m417z) 的 Windhawk 插件
[Slick Window Arrangement](https://windhawk.net/mods/slick-window-arrangement) 1.0.2。

## 包含

- 单文件 x64 exe，不需要安装 Windhawk。
- 拖动窗口时自动吸附到其它窗口和屏幕工作区边缘。
- 松开鼠标后的惯性滑动动画。
- 中文托盘菜单：总开关、拖动吸附、滑动吸附、滑动动画、开机自启、退出。
- 开机自启支持注册表、启动文件夹快捷方式、启动脚本和计划任务多种方式。
- 配置保存在 exe 同目录的 `config.ini`。

## 下载

`SlickWindowArrangement-1.0.2-x64.zip`

解压后双击 `SlickWindowArrangement.exe`。

## 已知问题

- 仅支持 64 位 Windows 10 / 11。
- 管理员权限运行的程序需要以管理员身份启动本工具。
- 带反作弊保护的程序默认跳过。
- Windows 10 可能把新托盘图标放进 `^` 隐藏区。
- 开机自启可能被安全软件拦截，程序会尝试多种方式并显示错误码。
- 程序没有代码签名，SmartScreen 可能提示。

## 说明

这是个人使用的小工具，基本功能可用，但后续不保证继续维护。源码在仓库里，欢迎自行 fork。
