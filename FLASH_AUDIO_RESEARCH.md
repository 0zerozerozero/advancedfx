# mirv_pov flash 声音致盲研究记录

## 当前目标

`mirv_pov 1` 播放 demo POV 时，希望被观察玩家被 flashbang 闪到后，不仅视觉白屏正确覆盖 HUD，还要触发正常玩家被闪时的声音致盲/失聪效果，让周围脚步声、枪声等明显被压低或过滤。

## 当前状态

### 已解决

- flash 视觉层级已修复。
- 被观察玩家被闪后，白色 flash 会覆盖 HUD。
- 紫色/透明 lower flash layer 已通过关闭 lower/down flash pass 解决。
- 用户已确认视觉效果正确。

### 未解决

- flash 声音致盲仍未实现。
- 多次构建安装后，用户反馈仍然没有声音致盲效果。

## 当前代码实现概况

主要修改在：

- `AfxHookSource2/MirvPovHud.cpp`
- `AfxHookSource2/ClientEntitySystem.cpp`

### 视觉 flash patch

当前视觉方案不是模拟 flash 状态，而是 patch client.dll 中 HUD 前后 flash pass 的 gate。

实现方式：

- upper/up-HUD flash gate：强制开启。
- lower/down-HUD flash gate：强制关闭。
- patch 方式：把目标位置的 `test al, al` (`84 C0`) 改成 `xor al, al` (`30 C0`)。

效果：白色 flash 在 HUD 上方，lower/purple flash layer 不再显示。

这部分已经验证有效，后续不要回退。

## IDA MCP 当前数据库状态

当前使用的 IDA MCP session：

- session id：`cs2-client`
- 文件：`D:\ProgramFile\Steam\steamapps\common\Counter-Strike Global Offensive\game\csgo\bin\win64\client.dll`
- IDB：`client.dll.i64`
- imagebase：`0x180000000`
- 函数数：`65477`
- 字符串数：`63547`
- Hex-Rays：ready
- auto-analysis：ready

最近一次重新连接后，client.dll 已重新完整 auto-analysis，并完成 Hex-Rays / string cache warmup。

## 关键 IDA 发现

### 1. flash update 函数

新版 `client.dll` 中 flash update 函数当前定位为：

- `sub_180BC8270`

旧数据库或旧版本中对应地址曾是：

- `sub_180BC7F10`

两者逻辑结构一致，但新版地址整体后移。

### 2. flash update 关键字段

`sub_180BC8270` 中观察到的 pawn 内部字段：

| 偏移 | 作用推测 |
| --- | --- |
| `+0x13EC` | flash end / bang time candidate |
| `+0x13F0` | flash snapshot / screenshot alpha |
| `+0x13F4` | flash overlay alpha |
| `+0x13F8` | flash buildup bool |
| `+0x13F9` | `m_bFlashDspHasBeenCleared` schema 字段 |
| `+0x13FA` | flash screenshot grabbed bool |
| `+0x13FC` | `m_flFlashMaxAlpha` |
| `+0x1400` | `m_flFlashDuration` |
| `+0x1404` | flash-adjacent internal field |
| `+0x1408` | flash-adjacent internal field |
| `+0x140C` | periodic update timestamp |
| `+0x1410` | flash / music / DSP-ish state |
| `+0x1414` | state timestamp |
| `+0x1418` | state-2 enter timestamp |
| `+0x1420` | flash effect scalar updated by flash update |
| `+0x1424` | previous/secondary scalar updated by flash update |

其中 `+0x13FC`, `+0x1400`, `+0x13F9` 是 schema 可解析字段；`+0x1410` 之后属于 IDA 发现的 internal 状态，不应假装加入 schema lookup。

### 3. flash update 中的声音相关候选逻辑

在 `sub_180BC8270` 中：

```asm
180bc8530  cmp cs:dword_18234C638, r14d
180bc8537  xorps xmm6, xmm6
180bc853a  jle short loc_180BC85B5
180bc853c  mov rax, [rdi]
180bc853f  mov rcx, rdi
180bc8542  call qword ptr [rax+358h]
180bc8548  test al, al
180bc854a  jz short loc_180BC85B5
180bc854c  call sub_18075B160
180bc8551  ucomiss xmm0, xmm7
180bc8554  jp short loc_180BC8562
180bc8556  jnz short loc_180BC8562
180bc8558  mov rcx, rdi
180bc855b  call sub_180BF8670
180bc8560  jmp short loc_180BC8572
180bc8562  mov rcx, rdi
180bc8565  call sub_180BFD820
180bc856a  movss xmm6, dword ptr [rdi+1424h]
180bc8572  movaps xmm7, xmm0
180bc8575  movss xmm0, dword ptr [rdi+1420h]
180bc857d  comiss xmm0, xmm7
...
180bc85b5  movss dword ptr [rdi+1420h], xmm7
180bc85c5  movss dword ptr [rdi+1424h], xmm6
```

这说明 `+0x1420/+0x1424` 是 flash update 后半段计算出来并写回的状态，很可能参与 flash 声音/效果衰减。但同步这些字段后，用户仍反馈无效。

### 4. flash 状态切换函数

发现函数：

- `sub_180BF16C0`

旧分析中的伪代码：

```cpp
void *__fastcall sub_180BF16C0(_DWORD *a1, int a2)
{
    a1[1284] = a2;        // +0x1410
    if (a2 == 2)
        a1[1286] = currentTime; // +0x1418
    a1[1285] = currentTime;     // +0x1414
}
```

对应字段：

- `+0x1410`：状态值。
- `+0x1414`：状态更新时间。
- `+0x1418`：进入状态 2 的时间。

`sub_180BC8270` / 旧 `sub_180BC7F10` 会检查 `+0x1410 == 2`，满足条件时触发一段 `Music.StartAction` 相关逻辑，然后把 `+0x1410` 改成 `3`。

但后续确认：这条 `Music.StartAction` 并不是脚步/枪声被压低的最终声音致盲路径。

### 5. `sub_180706A40` 不是最终声音致盲

旧路径中：

```asm
mov edx, 3
call sub_180706A40
```

经 IDA 分析，`edx=3` 对应的是：

- `Music.StartAction`

这更像音乐事件/动作音乐触发，不是 flashbang 造成的脚步、枪声 muffling / deafness 本体。

因此后续不要再把 `sub_180706A40` 当成最终 hook 点。

### 6. demo blind check 证明 blind 状态基于 local pawn 字段

函数：

- `sub_180C41300`

相关字符串：

- `Cannot record demos while blind.`

伪代码核心：

```cpp
v4 = sub_180BDB8B0();
if (*(float *)(v4 + 5120) > 0.0 && *(float *)(v4 + 5100) > currentTime)
{
    CBufferString::Insert(..., "Cannot record demos while blind.", ...);
}
```

偏移换算：

- `5120 = 0x1400`：`m_flFlashDuration`
- `5100 = 0x13EC`：flash end / bang time candidate

这证明至少某些 blind 判断只看 local pawn 的 flash 字段。

但把 target pawn 的这些字段同步到 real pawn 后，声音致盲仍无效，说明最终声音 muffling 不只依赖这些 netvar/schema flash 字段。

### 7. sound EQ / headphone 相关路径

曾重点分析函数：

- `sub_180C6C700`

它引用：

- `snd_headphone_eq`
- `snd_headphone_eq_active`
- `snd_vol_arms_race`
- `snd_vol_deathmatch`
- `snd_vol_competitive`
- `snd_vol_casual`
- `snd_vol_spectator`
- `snd_vol_warmup`
- `snd_eq_arms_race`
- `snd_eq_deathmatch`
- `snd_eq_competitive`
- `snd_eq_casual`
- `snd_eq_spectator`
- `snd_eq_warmup`

它会刷新 audio UI / EQ / volume 状态。该函数更像全局音频状态刷新，而不是 flashbang 命中时的瞬时 deafness 消费端。

`sub_1800FE980` 注册：

```asm
"snd_headphone_eq"
"Select Headphone EQ Preset"
```

`sub_1800FEA30` 注册：

```asm
"snd_headphone_eq_active"
"Select Headphone EQ Preset"
```

目前没有证据表明 flashbang 的脚步/枪声 muffling 是直接通过这两个 cvar 实现。

### 8. UI DSP restore 不是目标路径

字符串：

- `snd_refresh_ui_audio_state`
- `Restores audio DSP state for the UI.`
- `reverb_29_UI`

对应函数：

- `sub_180CAD130 -> sub_180CAD430`

结论：这更像 UI reverb/DSP restore，不是 flashbang deafness 本体。

## 已尝试代码方案

### 1. 同步 schema flash 字段

尝试同步：

- `m_flFlashMaxAlpha`
- `m_flFlashDuration`
- `m_bFlashDspHasBeenCleared`

结果：无效。

### 2. 同步 `0x13EC..0x1400` flash block

尝试同步 flash end time、snapshot alpha、overlay alpha、duration 等字段。

结果：无效。

### 3. hook flash update 函数并调用 real pawn

旧实现思路：当 target pawn flash update 时，额外调用 real pawn 的 flash update，或把 target 状态复制到 real pawn。

结果：无效。

### 4. 同步 `+0x1410/+0x1414/+0x1418`

尝试把 flash internal state 同步给 real pawn。

结果：无效。

### 5. 同步 `+0x1420/+0x1424`

根据新版 IDA 分析，增加同步 `m_flFlashDuration + 0x0c` 到 `m_flFlashDuration + 0x28`，覆盖：

- `+0x140C`
- `+0x1410`
- `+0x1414`
- `+0x1418`
- `+0x1420`
- `+0x1424`

并调整 hook 顺序：

1. real pawn 原始 flash update 先执行。
2. target pawn 原始 flash update 随后执行。
3. target pawn flash audio state 覆盖回 real pawn。

结果：构建和安装成功，但用户反馈仍无声音致盲效果。

## 当前结论

目前可以确定：

1. 视觉 flash 和声音 deafness 是分离路径。
2. `m_flFlashMaxAlpha/m_flFlashDuration/m_bFlashDspHasBeenCleared` 足够影响部分 blind 判断和视觉，但不足以触发声音致盲。
3. `+0x1410..+0x1424` 是 flash update 的 internal 状态，但同步后仍不触发最终声音 muffling。
4. `Music.StartAction` 不是脚步/枪声 muffling 的最终路径。
5. `snd_headphone_eq` / `snd_headphone_eq_active` 目前没有被证明是 flashbang deafness 的最终机制。
6. 真正的 deafness 很可能发生在 sound system / mixer / sound operator 层，而不是 pawn flash update 本身。

## 当前最可能遗漏的方向

### 方向 A：寻找 flashbang sound operator / DSP graph

字符串候选：

- `sound_dsp_effect`
- `dsp_volume`
- `dsp_player`
- `RoomDSP`
- `sv_smoke_volume_blind_start`

其中 `sound_dsp_effect` 字符串在当前 IDA `xref_query` 没拿到直接 xref，可能需要用更底层的 data ref / bytes / text search 继续追。

### 方向 B：寻找实际写入 sound DSP cvar / engine interface 的函数

需要追踪：

- 谁写 `sound_dsp_effect`
- 谁写 `dsp_player`
- 谁写 `dsp_volume`
- 谁向 engine sound system 设置 listener DSP / mix layer

### 方向 C：从正常玩家实时被闪时的声音调用链入手

比继续从 flash field 往下猜更可靠的方式：

1. 在 IDA 中找所有 flashbang / blind / dsp / sound operator 相关字符串。
2. 找到实际向 sound system 提交 DSP / mix / lowpass 参数的调用点。
3. 判断该调用点依赖的是 local pawn、local controller、view target，还是 sound listener entity。
4. hook 最终消费端，让它在 `mirv_pov` 时使用 POV target pawn，而不是 real local pawn。

### 方向 D：比较正常 live local player 与 demo POV 的 listener entity 差异

声音致盲可能绑定在 listener / local player object，而不是 pawn flash fields。

需要进一步查：

- sound listener 当前 entity 如何取。
- demo spectator / observer target 是否影响 listener。
- `mirv_pov` 只改相机和 HUD target，是否没有改 sound listener target。

## 下一步建议

不要继续盲目扩大 pawn flash block 同步范围。

推荐下一步只做 IDA 研究：

1. 以 `sound_dsp_effect`, `dsp_player`, `dsp_volume`, `RoomDSP` 为入口继续追 xref。
2. 如果普通 xref 不足，改用 bytes / data ref / rendered text search 定位引用。
3. 找 sound system 的最终 setter，而不是 flash update 的中间状态。
4. 确认最终 setter 的输入来源后，再决定 hook 点。

## 最近一次无效实现摘要

当前 `ClientEntitySystem.cpp` 中的最新思路是：

- hook flash update 函数。
- real pawn 原始 update 后，主动 update target pawn。
- copy target pawn 的 flash visual/audio internal fields 到 real pawn。

该方案可以编译安装，但用户反馈仍无声音致盲，所以它不是最终答案。

## 本次继续处理记录（2026-07-03）

### 新增结论：`+0x1420/+0x1424` 不是 flashbang deafness 本体

继续分析 `sub_180BF8670` / `sub_180BFD820` 后确认：

- 这两个函数只由 `sub_180BC8270` 调用。
- 两者都会遍历 grenade projectile 列表，并动态检查 `C_SmokeGrenadeProjectile`。
- 相关逻辑读取 smoke blind / smoke volume 类 cvar，例如 `sv_smoke_volume_blind_start` 附近的配置。
- `+0x1420/+0x1424/+0x1428/+0x1430` 更像 smoke volume blind / smoke occlusion 状态，不是 flashbang 脚步枪声 muffling 的最终状态。

因此不要再扩大或依赖 `+0x1420` 之后的同步范围；之前同步 `+0x1410..+0x1424` 无效是合理结果。

### 新增候选 hook 点：flash data ingress

本次用 IDA 和本地 `objdump` 找到 flash 数据入口函数。注意：早先从 `objdump --start-address` 的反汇编起点误判过一次地址，后续用 IDA 函数边界和 raw bytes 已校正：

- wrapper：`sub_180BEBD00`
- inner data handler：`sub_180BEBD40`

`sub_180BEBD40` 的行为：

```asm
180bebd56  mov byte ptr [rdx+13F8h], 0
...
180bebd8d  mov qword ptr [rbx+13F0h], rbp
180bebd94  mov byte ptr [rbx+13F8h], bpl
180bebd9b  mov byte ptr [rbx+13FAh], bpl
180bebdb1  mov dword ptr [rbx+1400h], ebp
180bebdb7  mov dword ptr [rbx+13ECh], ebp
...
180bebe8c  mov dword ptr [rbx+13F0h], 3F800000h
180bebe96  mov dword ptr [rbx+13F4h], 3F800000h
180bebea0  mov byte ptr [rbx+13F8h], 1
180bebea7  mov byte ptr [rbx+13FAh], bpl
180bebeb5  movss dword ptr [rbx+1400h], xmm6
180bebecc  movss dword ptr [rbx+13ECh], xmm1
180bebed4  mov byte ptr [rbx+13F9h], bpl
180bebee0  mov byte ptr [rsi+13F9h], bpl
```

这说明 `m_bFlashDspHasBeenCleared` 的清零发生在 flash 数据进入 pawn 时，而不是 flash update 后半段。相比在 tick 后复制 pawn 字段，这个入口更接近声音 DSP 可能依赖的状态边沿。

Wrapper `sub_180BEBD00` 的伪代码核心：

```cpp
sub_180BEBD40(context, pawn, flashDuration);
if (flashDuration && *flashDuration > 0.0f)
    sub_180BC5E20(context + 0x1BD0);
```

`sub_180BC5E20` 只有两个 direct code xrefs，其中一个就是该 wrapper。进一步看伪代码，它更像插值/随机相位刷新 helper：读取 `*(a1)`，调用 `sub_180164C70` / `sub_180172180`，并写 `a1+0x10` 与 `a1+0x0C`。当前 hook inner handler 时，调用顺序是：wrapper 调 inner detour -> detour 先处理 target pawn 再处理 real pawn -> 返回 wrapper -> wrapper 再执行 `sub_180BC5E20(context + 0x1BD0)`。因此当前 inner hook 并没有漏掉 wrapper side-effect，不需要因为这个 helper 盲目切到 wrapper hook。

### 本次代码改动

`AfxHookSource2/ClientEntitySystem.cpp` 已从旧的 flash update detour 改为 flash data update detour：

- 不再 hook `sub_180BC8270` 后手动 update target pawn。
- 新 hook 定位 inner handler `sub_180BEBD40`，pattern 当前唯一匹配该地址。
- 当 `mirv_pov` 开启且 engine 正在对 POV target pawn 应用 flash 数据时，额外对真实 local pawn 调用同一个原始 flash data handler。
- 周期性兜底同步只保留 `+0x13EC..+0x1400` 的真实 flash 字段，不再复制 `+0x140C..+0x1424` smoke blind 状态。

### 当前验证状态

- `git diff --check -- AfxHookSource2/ClientEntitySystem.cpp FLASH_AUDIO_RESEARCH.md`：clean。
- `ClientEntitySystem.cpp` LSP diagnostics 在当前 WSL 环境被 Windows SDK 依赖挡住：`Windows.h` not found，另有 `SOURCESDK::CS2::ISource2EngineToClient` 级联类型错误。
- 旧 hook 名称和旧 `MirvPov_CopyFlashAudioState` 引用已清空。
- 已按主工作区旧布局构建 `build/Release/advancedfx-x64` 的 Release `AfxHookSource2` target。
- 已安装到 `build/Release/advancedfx-x64-install/bin/x64/AfxHookSource2.dll`，并同步更新 staging：`build/staging-release/bin/x64/AfxHookSource2.dll`。
- 旧布局 build 产物、旧布局 install 产物、staging 产物 SHA-256 一致。
- 已按 HLAE 实际 CS2 hook 路径安装到软件目录：`D:/Edu/Python/CS_AutoHighlight/tools/hlae/x64/AfxHookSource2.dll`。
- `hlae/LaunchCs2.cs` 使用 `Application.StartupPath + "\\x64\\AfxHookSource2.dll"`，因此没有覆盖软件根目录下的 `AfxHookSource2.dll`。
- 覆盖前已备份旧运行时 DLL/PDB：`AfxHookSource2.dll.backup_20260704_161718`、`AfxHookSource2.pdb.backup_20260704_161718`。
- 仍需要启动 CS2 demo 实测 target 被 flash 时是否出现声音致盲。
- Explore/Oracle 复核结论：当前 Detours 生命周期符合仓库约定；修正地址后不建议切 wrapper，继续保留 inner `sub_180BEBD40` hook 作为下一次 runtime test 的候选实现。
