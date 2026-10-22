# KZLadderOff 0.1.3-candidate

Windows x64 原生 Metamod:Source 插件，用于显示 CS2KZ 梯子碰撞面的 **0off 候选状态**。0.1.1 已修复上梯崩溃；本版根据玩家在 grotto LJ room 同一梯子的两次诊断数据，将成功卡梯距时测得的 1/16 单位间隙计入接触判定。玩家已在该梯子确认普通爬梯显示红色、0off 爬梯显示绿色；当前仅有一个梯面的实测数据，`0 OFF` **不能视为所有地图上已验证的卡梯距条件**。

## 安装与使用

1. 从 GitHub Releases 下载 `KZLadderOff-Windows-x64-0.1.3-candidate.zip` 并解压。停止服务器，移除旧版 DLL，再将包内 `KZLadderOff/install/addons` 目录复制到服务器的 `game/csgo`，合并其中的 `addons`。最终文件为：
   - `game/csgo/addons/metamod/kzladderoff.vdf`
   - `game/csgo/addons/kzladderoff/bin/win64/kzladderoff.dll`
2. 启动 Windows x64 CS2 专服，确认 `meta list` 中出现 `KZLadderOff (0.1.3-candidate)`。VDF 会自动加载；也可在控制台执行 `meta load kzladderoff`。
3. 玩家在聊天中输入 `!ladderoff` 切换显示。默认关闭，再次输入立即关闭。

仅在命中有效梯子时显示一行：候选零间隙显示绿色 `0 OFF`，有效但非零显示红色 `NOT 0 OFF`；信息不足时清空。原生彩色事件不可用时自动使用纯文本中央提示。未安装任何客户端文件，也不依赖 CounterStrikeSharp、数据库或 Workshop HUD。彩色事件使用 `show_survival_respawn_status`；CS2KZ 传统 HUD 也会刷新同一中心区域，可能覆盖本提示，实际视觉效果和并用情况仍须客户端检查。

开关按 SteamID64 保存到 `game/csgo/addons/kzladderoff/data/<SteamID64>.txt`，内容只有 `1` 或 `0` 加换行；缺失或无效文件按关闭处理。尚未认证的玩家可以临时切换，获得 SteamID64 后才会保存。`example-preference.txt` 是格式示例，无需安装。设置变化时写入临时文件并原子替换目标文件。

## 判定原理与边界

代码从玩家 `CCSPlayer_MovementServices::m_vecLadderNormal` 读取并验证梯子法线，读取 Pawn 当前碰撞盒和位置。处于 `MOVETYPE_LADDER` 时，沿 `-normal` 对实际 Hull 执行 Source 2 `TraceShape`，仅接受法线一致且具有 `CONTENTS_LADDER` 的碰撞形状。然后以点 Trace 在相同形状上重获碰撞面，计算玩家 AABB 靠近该面的支撑点至碰撞平面的有符号距离。切回 WALK 时保留已验证的法线与形状，但每帧检查是否仍命中同一形状；死亡、换 Pawn、跳离、传送或地图切换时清理相关缓存。

`Unknown` 用于无效法线、非梯子命中、起始穿透、无命中、超出接触范围等情况，不显示红色。`Zero` 表示有效梯子间隙不超过 1/16 单位加浮点舍入容差；超出的有效间隙为 `NonZero`。这个阈值来自一次玩家确认成功的卡梯距实测，而非 CKZ 的 1/32 常量本身：诊断记录中 Hull Trace 距梯面还剩 1/32 单位，成功帧的玩家 AABB 距梯面为 1/16 单位。仅靠 `CONTENTS_LADDER` 也可能漏掉仅以 climbable 材质定义的有效梯子。尚不能宣称准确检测所有 0off。

游戏 SDK 不提供公开的 Nav Trace 接口，因此插件在实际游戏 `server.dll` 中用 MSVC RTTI 定位 `CNavPhysicsInterface` 虚表；找不到时拒绝加载。0.1.0 错把接收 `CTraceFilter*` 的 TraceLine/TraceShape 槽位写成了接收实体指针的槽位，首次执行时会访问冲突；0.1.1 使用已经在隔离服执行验证的槽位 1/3，并只允许已检查的 Windows 游戏 `server.dll` 构建加载（PE 时间戳 `0x6ad7b41c`、映像大小 `0x2572000`）。若游戏更新后出现 `Unsupported CS2 server.dll build`，应先复验虚表 ABI 再更新插件。彩色 HUD 需在同一模块扫描当前 `GameEventManager` 签名，失效时退回纯文本。插件仅读取移动与碰撞数据，不修改玩家位置、速度、碰撞参数或 CS2KZ 移动逻辑。

源码研究依据：[CS:GO LadderMove/OnLadder](https://github.com/perilouswithadollarsign/cstrike15_src/blob/master/game/shared/gamemovement.cpp)、[CS:GO CS 梯子实现](https://github.com/perilouswithadollarsign/cstrike15_src/blob/master/game/shared/cstrike15/cs_gamemovement.cpp)、[CS2KZ NavPhysics 接口](https://github.com/KZGlobalTeam/cs2kz-metamod/blob/5fe5811f997fbfc64f32859008f286fee68e03f9/src/sdk/navphysicsinterface.h)、[CS2KZ 玩家移动过滤器](https://github.com/KZGlobalTeam/cs2kz-metamod/blob/5fe5811f997fbfc64f32859008f286fee68e03f9/src/sdk/tracefilter.h)、[CKZ 碰撞分离常量](https://github.com/KZGlobalTeam/cs2kz-metamod/blob/5fe5811f997fbfc64f32859008f286fee68e03f9/src/kz/mode/kz_mode_ckz.h#L10)、[CKZ 的 1/32 单位说明](https://github.com/KZGlobalTeam/cs2kz-metamod/blob/5fe5811f997fbfc64f32859008f286fee68e03f9/src/kz/mode/kz_mode_ckz.cpp#L820-L823)。`THIRD_PARTY.md` 记录许可证。

## 构建

需要 Visual Studio 2022 C++ Build Tools、Windows SDK、Python、AMBuild 源码以及匹配目标 CS2 构建的 Metamod:Source 和 hl2sdk-cs2 源码。本仓库不捆绑第三方 SDK。运行：

```powershell
python build_windows.py --deps-root C:\path\to\deps --ambuild-path C:\path\to\ambuild
```

`deps` 目录内必须有 `metamod-source` 和 `hl2sdk-cs2`。若 AMBuild 已作为 Python 包安装，可省略 `--ambuild-path`。输出 DLL 位于构建目录的 `kzladderoff/windows-x86_64/`；默认中间文件写到项目上两级目录的 `work/kzladderoff-build-win64`。构建同时生成 `kzladderoff-prefs-test.exe`。

## 已执行验证（2026-10-22）

- 0.1.3 Windows x64 MSVC 编译与链接通过；最终安装包中的 DLL 在隔离专服执行 `meta list` 时显示 `KZLadderOff (0.1.3-candidate)`，卸载成功且进程正常退出。
- 在使用本机完整 CS2 游戏文件建立的隔离专服中，Metamod 启动时报告 `[META] Loaded 1 plugin.`，`meta list` 显示 `KZLadderOff`；`meta unload kzladderoff` 返回 `Plugin 1 unloaded.`。0.1.0 的错误虚表调用在隔离服复现为 `0xC0000005`，修正槽位后 `TraceLine` 和 `TraceShape` 均已实际返回，进程继续运行。该隔离服**没有加载 CS2KZ**。
- 地图加载后彩色事件和网络消息均可获取，诊断版调用 `SerializeEvent` 成功返回并生成 92 字节；尚无真实客户端画面验证。
- 设置测试程序通过：缺失文件默认关闭、两个 SteamID 相互独立、开启/关闭状态持久化、损坏文件回退关闭、覆盖保存。测试代码位于 `tests/preferences_test.cpp`。
- 玩家提供的 `diagnostic.csv` 有两次同一梯面的梯上记录：普通爬梯 85 帧（527.5625–528.875 秒）间隙均为 0.347167969；玩家确认的 0off 爬梯 195 帧（530.875–533.90625 秒）间隙均为 0.0625。两次的点 Trace 都精确命中 X=7782.5 的同一梯面；新阈值对该 CSV 的回放判定为普通爬梯 85 帧全红、0off 爬梯 195 帧全绿。旧阈值对后者 195 帧全红。
- 玩家已在 grotto LJ room 用 0.1.3 实测确认：普通爬梯显示红色，成功完成卡梯距的 0off 爬梯显示绿色。

## 后续验证

还需检查其他梯面是否适用 1/16 间隙。原需求的 A–N 场景尚未全部完成：普通移动、挂梯、撞梯下落、重新挂梯、卡梯距失败、离梯、检查点传送、不同朝向、蹲站变化、多人、重连、关闭、无效 Trace、移动时序。还需确认彩色提示不会覆盖 CS2KZ HUD，因此保留 `candidate` 标识。
