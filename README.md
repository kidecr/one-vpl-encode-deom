# VPL Encode Module

## 简介
该包功能为，使用oneVPL接口调用软/硬编解码器，对视频进行编码。
输入格式为OpenCV的Mat类型。

## 使用
使用参照`vpl-encode-module-demo.cpp`。
### 调用
模块提供了一个输入接口`void push(cv::Mat image)`，向待编码队列中添加一帧，编码循环函数会不断访问队列，当队列不为空时进行编码。
在构造函数中必须设置`输出文件`和`图像大小`，而且图像大小和实际`push`进的图像大小必须一致，否则会导致内存访问逻辑出现问题。
### 改参数
1. 改参数前，一定要使用`vlp-inspect`程序查看一下你的电脑都支持什么格式。
2. 需要调整的参数主要在`mfxVideoParam SetEncodeParam(int w, int h)`和`mfxVideoParam SetVPPParam(int w, int h)`两个函数中直接改。首先，两者都需要输入参数`w`和`h`，为图像宽高。VPP不太需要改，主要可能要改的应该是Encode，详细查看[参数含义](https://spec.oneapi.io/versions/latest/elements/oneVPL/source/API_ref/VPL_structs_cross_component.html?highlight=mfxvideoparam#mfxvideoparam)。注意，VPP输出格式和Encode输入格式需要相同。
3. 改软硬编码在构造函数里，找注释`2.1.设置编码方式`；注意，注释`2.2`位置的编码器一定要支持软或者硬编码（用vpl-inspect查）。
4. 总之，整个参数需要自恰，而且电脑支持，否则都会报错。

## 配环境
1. 软编解码:安一个oneVPL就行，这个不在oneAPI那个安装包里，需要单独安装。
2. 硬编解码：尝试在NUCi710上尝试安装VAAPI，结果没有找到可用的编解码器。新核显应该安装onevpl-intel-gpu,旧核显使用VAAPI或MSDK（查得时候发现VAAPI还能访问nvidia显卡的硬编解码器，但环境貌似不是很容易配）（oneVPL是一个顶层接口，这些东西都是作为底层的，所以不需要改代码）
3. [安装教程](https://www.intel.com/content/www/us/en/developer/articles/guide/onevpl-installation-guide.html), oneVPL选择单独安装就行（oneAPI安装包包本身不包含oneVPL了），GPU安装照着教程后两个都行（设置用户组那块写的不对，可以不用管）。
