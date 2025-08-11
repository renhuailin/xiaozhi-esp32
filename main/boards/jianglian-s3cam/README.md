# 匠联未来ESP32S3/CAM

## 简介

<div align="center">
    <a href="http://xz.jlfuture.top/"><b>项目介绍</b></a>
    |
    <a href="https://item.taobao.com/item.htm?id=940139592072"><b>套件购买地址</b></a>
</div>
## 介绍

匠联未来ESP32S3CAM 是 基于小度音响改装而来，内置双麦克风、双音频芯片，自升压功放、2寸SPI屏，AXP173电源管理芯片，带有一个充电指示灯和RGB 指示灯和三个按钮，一个电源按钮，一个说话按钮，一个用户按钮。

本产品是适用于 小度蓝牙音箱自改 小智AI聊天机器人控制板套件。
使用方法，直接替换原有PCB，切割外壳放置屏幕，打两个麦克风孔（注意：PCB比原来的大，一定要处理平突出来的位置，否者压坏PCB，无法提供免费售后）
注：套件手工改造，难免有改造痕迹和轻微磨损，实际效果视个人的动手能力。
立创有开源版本，可复刻自用，开源协议：CC BY-NC-SA 4.0
2025/7/15后已升级为兼容摄像头版本，IO变更固件不在与立创版兼容，板子暂时不开源，但可提供板级代码自行修改固件。
具体配置：
1.主控 ESP32S3R8
2.外置储存 16M NOR FLASH
3.音频 ES8311+ES7210
4.双全指向MEMS麦克风ZTS6216
5.电源 AXP173
6.显示屏 2.0寸TFT液晶IPS屏 240*320分辨率
7.支持摄像头GC0308/其它接口相同摄像头应该要修改代码
8.固件同步官方OTA更新（自编译固件需要自己更新）
3个实体按键（从左到右，电源/唤醒按键/音量+ 长按静音）
支持4欧5W喇叭
3.7V锂电池供电
语音唤醒：你好小智
WIFI版，需要链接WIFI网络
板级代码只提供基础固件功能：同官方固件

## 移植说明
自编译固件：

1.下载附件中的代码，覆盖到完整项目代码中，

2.编辑main/CMakeLists.txt文件在  # 根据 BOARD_TYPE 配置添加对应的板级文件  下添加如下代码

elseif(CONFIG_BOARD_TYPE_JIANGLIAN_S3CAM)
    set(BOARD_TYPE "jianglian-s3cam")

3.编辑main/Kconfig.projbuild文件在  #Board type. 开发板类型  下添加如下代码

    config BOARD_TYPE_JIANGLIAN_S3CAM
        bool "匠联未来·ESP32-S3CAM"

在config USE_DEVICE_AEC 中添加

BOARD_TYPE_JIANGLIAN_S3CAM

4.按照官方编译方式进行编译，版子进入menuconfi选择匠联未来·ESP32-S3CAM

## 配置、编译命令

**配置编译目标为 ESP32S3**

```bash
idf.py set-target esp32s3
```

**打开 menuconfig 并配置**

```bash
idf.py menuconfig
```

分别配置如下选项：

- `Xiaozhi Assistant` → `Board Type` → 选择 `匠联未来·ESP32-S3CAM`

按 `S` 保存，按 `Q` 退出。

**编译**

```bash
idf.py build
```

**烧录**

```bash
idf.py flash
```

> [!TIP]
>
