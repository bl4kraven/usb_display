USB_Display
=========

**这是什么**

一句话概况是树莓派(raspberry pi 1)的外界显示器(TFT-LCD)Linux USB驱动。

本人手头有一个Atmel A5(SAMA5D3)的ARM板子，自带TFT-LCD电容触摸液晶屏和USB接口，刚好作为树莓派1代的显示器，两者之间用
USB传输图像数据（不求性能，能显示就行）。

网上找到[RoboPeak](http://www.robopeak.net)有一个树莓派的[液晶屏](http://www.robopeak.com/blog/?p=480)在卖,而且开源树[莓派端的驱动](https://github.com/robopeak/rpusbdisp)和协议，我就在这个基础上改。

为了快速开发, 代码没遵守kernel代码规则, 有可能有BUG, 要用自己小心。

这就涉及到两个USB驱动：

* ARM液晶屏的USB gadget驱动:

    就是本项目

* 树莓派的USB HOST驱动:

    刷RoboPeak的树莓派固件，重新编译树莓派的Linux kernel, 编译修改原来的RoboPeak的开源树莓派驱动。

**原理简介**

ARM液晶屏的USB gadget驱动新建一个function接口，里面有一个OUT endpoint传输Robopeak液晶屏的图像拷贝协议, 还有一个IN endpoint传输触摸屏的输入。

