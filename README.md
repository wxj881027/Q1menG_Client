
# Q1menG Client  
*A customized DDNet client based on TaterClient*  
[![Build status](https://github.com/TaterClient/TClient/workflows/Build/badge.svg)](https://github.com/TaterClient/TClient/actions/workflows/build.yaml)


## 📝 Introduction 

Q1menG_Client is a customized client built upon the foundations of TaterClient and the DDNet client.  
It provides additional visual enhancements, personalized features, and small quality-of-life improvements while remaining compatible with DDNet gameplay.

## ✨ Features (Added / Modified)  

- Improved UI elements and custom visual adjustments  
- New effect and animation options  
- Additional configuration toggles  
- Client-side quality-of-life optimizations  
- Personal custom features depending on your modifications  

## 🔧 Build Instructions

- CMake 3.16+
- Visual Studio 2022 (Desktop C++ Development workload)
- Python 3.x (for asset generation)
- Git

## 🏛 Credits 


This project is based on the work of the following open-source projects:
Teeworlds — Magnus Auvinen

DDRace — Shereef Marzouk

DDNet — Dennis Felsing & contributors

TaterClient — community modifications

Their original work remains licensed under the zlib/libpng license, and all original copyright notices have been preserved.

📜 License 

This project follows the zlib/libpng license, identical to the upstream DDNet/TaterClient licensing terms.
Modified versions must be clearly marked and original authorship must not be misrepresented.

## 📮 Notes 

This client is purely a personal/custom modification project and is not affiliated with DDNet or TaterClient developers.
---


# Q1menG Client
*基于 TaterClient 的个性化 DDNet 客户端*

## 📝 介绍 
Q1menG_Client 是一个基于 TaterClient 与 DDNet 客户端开发的个性化客户端。  
它加入了视觉增强、个性化功能以及一些便捷性改动，同时保持与 DDNet 玩法的兼容性。


## ✨ 功能特性（新增 / 修改）

- 改进的 UI 元素与自定义视觉效果  
- 新的粒子特效与动画功能  
- 更多可配置选项  
- 客户端侧的便捷性优化    

## 🔧 编译方式

- CMake 3.16+  
- Visual Studio 2022（需安装 C++ 桌面开发组件）  
- Python 3.x（用于生成部分资源）  
- Git  

## 🏛  致谢

本项目基于以下开源项目的成果进行开发：

Teeworlds — Magnus Auvinen

DDRace — Shereef Marzouk

DDNet — Dennis Felsing 和众多贡献者

TaterClient — 社区修改版本

它们均采用 zlib/libpng 开源协议，本仓库已保留所有原始版权声明。

## 📜 许可证

本项目遵循 zlib/libpng 开源协议，与上游的 DDNet / TaterClient 保持一致。
所有修改版本必须清楚标明，不得误导他人为原作者。

## 📮 说明

本客户端属于个人/非官方的自定义修改项目，与官方 DDNet 或 TaterClient 无任何隶属关系。




TO-DO:

- [x] 落水自动发爱心和发言

  tc_freeze_chat_enabled 1
  tc_freeze_chat_emoticon 1
  tc_freeze_chat_emoticon_id 7
  tc_freeze_chat_message "救命,又冻住了,帮帮我,freeze了"    //支持数组
  tc_freeze_chat_chance 30

- [ ] 延长辅助线到准星位置

- [ ] 辅助线跟随武器激光折射

- [ ] 辅助线模式改为切换(1:跟随武器颜色切换而切换;2:跟随墙体变化而变化)

- [ ] +toggle_exec,用于按下执行cfg1松开执行cfg2

- [ ] 内置一键cfg(例如,读取"C:\Users\User\AppData\Roaming\DDNet\settings_ddnet"内的所有)
- [ ] 仍在收集


