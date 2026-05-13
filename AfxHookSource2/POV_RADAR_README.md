# mirv_pov_radar_local — Runtime POV Radar for CS2

## 概述

通过 AfxHookSource2 runtime hooks 实现 CS2 离线 demo 回放时的 POV 风格雷达和 HUD，无需加载任何 VPK 修改。

## 构建

### 前置条件

- Visual Studio 2022 Build Tools (含 C++ 桌面开发工作负载)
- CMake (已通过 advancedfx 构建系统生成 sln)

### 构建命令

```powershell
"/mnt/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe" `
  "build/advancedfx-x64/AfxHookSource2/AfxHookSource2.sln" `
  /p:Configuration=Release /p:Platform=x64 /v:minimal
```

### 部署

```powershell
cp "build/advancedfx-x64/AfxHookSource2/Release/AfxHookSource2.dll" `
   "D:\Edu\Python\CS_AutoHighlight\tools\hlae\x64\AfxHookSource2.dll"
```

## 启动游戏

使用 HLAE 启动 CS2，需要 `-insecure` 参数（离线 demo 回放，无 VAC）：

```powershell
# 通过 HLAE 启动 CS2
& "D:\Edu\Python\CS_AutoHighlight\tools\hlae\x64\hlae.exe" `
  -csgoLauncher `
  -csgoExe "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game\bin\win64\cs2.exe" `
  -gfxEnabled true `
  -customLaunchOptions "-insecure -netconport 2121"
```

## 测试步骤

### 1. 加载 Demo

通过 netcon 或控制台：
```
playdemo match730_003816530305267794434_0253272208_397
```

### 2. 启用 POV Radar

```
mirv_pov_radar_local auto
mirv_pov_radar_local experiments be
```

### 3. 验证效果

- **雷达居中**：雷达应以当前观察的玩家为中心（不是 (0,0,0)）
- **队友可见**：队友始终显示在雷达上
- **敌人可见性**：敌人仅在被发现时显示（正常行为）
- **HUD**：无观战面板，显示血量/弹药/道具（POV 风格）
- **切换玩家**：`spec_next` / `spec_prev` 正常切换，雷达跟随

### 4. 切换玩家测试

```
spec_next    # 切换到下一个玩家
spec_prev    # 切换到上一个玩家
```

跨队伍切换也正常工作。

## 实验标志 (experiments)

| Flag | 名称 | 状态 | 说明 |
|------|------|------|------|
| **b** | ForceSpotted | ✅ 必需 | 强制队友在雷达上始终可见 |
| **e** | PatchShowAll | ✅ 必需 | POV 风格 HUD（隐藏观战面板、显示血量弹药） |
| a | LocalPointer | ⚠️ 冗余 | Frame context 期间修改全局本地玩家指针，当前不需要 |
| c | ControllerFlags | ❌ 崩溃 | 修改 IsLocalPlayerController 标志，会导致崩溃 |
| d | ObserverMode | ⚠️ 冗余 | Frame context 期间修改 pawn observer mode，当前不需要 |

**推荐配置**：`mirv_pov_radar_local experiments be`

## 已知问题

1. **队友被烟雾隔开后显示为 '?'**：游戏战争迷雾机制导致，烟雾阻隔视线时队友图标变为问号
2. **玩家颜色固定为蓝色**：队友及自己在雷达上的颜色固定为观战蓝色，而非游戏分配的序号对应颜色（红/蓝/黄/绿/橙）

## 技术架构

### 核心 Hooks

- **GetLocalPlayerController** (`sub_180BD7DE0`): 始终返回 fake controller（当前观察的玩家）
- **GetObserverMode / GetObserverTarget**: 仅在 frame context 内生效（FRAME_RENDER_PASS），防止崩溃
- **Auto-sync**: 监听 spec_next/spec_prev 切换，自动更新 fake controller

### Byte Patches (experiment 'e')

- Patch 1: **已禁用** — 曾导致雷达居中在 (0,0,0)
- Patch 2: NOP show-all flag — 阻止所有玩家无条件显示
- Patch 3: HUD spectator check `01`→`FF` — 启用 POV HUD 元素
- Patch 4: 隐藏观战面板 `mov sil,1`→`xor sil,sil`

### 关键设计决策

- **不锁死观战镜头**：用户手动切换玩家时不会被强制拉回
- **Frame-context-only hooks**：GetObserverMode/Target 仅在渲染帧内生效，避免相机系统崩溃
- **Patch 1 禁用原因**：该 patch 强制 `xor eax,eax` 使雷达使用 DemoRecorder 位置 (0,0,0)，禁用后雷达自然使用观战目标位置

## 文件结构

- `ClientEntitySystem.cpp`: 所有 POV radar 逻辑（hooks、byte patches、frame context）
- `ClientEntitySystem.h`: 函数声明
- `MirvCommands.cpp`: 命令处理（experiments 解析）
- `main.cpp`: FrameStageNotify hook 入口
