# WIM_Mount_ReadOnly
只读挂载WIM到一个目录
只读挂载UD(fbinst)分区到一个目录
参数说明：
```
wim.exe [/TempPath DIR] <archive.wim> <mount-directory> [image-index]
       wim.exe /ud <\\.\PhysicalDriveN|image.fba> <mount-directory>
       wim.exe /unmount <mount-directory>       // 卸载挂载
  /TempPath: 临时工作目录，不指定则为%Temp%环境变量，指定的话，目录必须已存在
  /ud: 只读挂载fbinst UD (自动判断1.6/1.7);
```
注意：挂载目录若不存在则wim.exe自动创建，若已存在则必须为空目录。卸载挂载后，程序不负责清理挂载目录，需要用户自行清理挂载目录。

此项目中使用的第三方库列表：

Dokan v0.6.0（由于Dokan v0.6.0版官方安装包里未带x64的dokan.dll，所以本项目中的x64版dokan.dll为自编译版本）

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
