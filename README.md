
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


