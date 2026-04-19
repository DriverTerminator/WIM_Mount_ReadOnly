# WIM_Mount_ReadOnly
只读挂载WIM，初始版本只为测试功能

此项目中使用的第三方库列表：

Dokan v0.6.0

https://github.com/dokan-dev/dokany


winfsp v2.1.25156

https://github.com/winfsp/winfsp


wimlib v1.13.6

https://github.com/ebiggers/wimlib


YY-Thunks

https://github.com/Chuyu-Team/YY-Thunks

VC-LTL5

https://github.com/Chuyu-Team/VC-LTL5


原理：
使用 wimlib 解析 WIM 映像文件
优先使用 Dokan 驱动挂载，如果没有 Dokan 则使用 winfsp（ARM64 目前只使用 winfsp）

初始目标兼容： Windows XP至Windows 11，x86、x64、arm64

下一阶段目标：希望高手能帮忙改造 wimlib 使项目能支持 Windows 2000
