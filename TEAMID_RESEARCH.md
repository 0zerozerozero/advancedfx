# mirv_pov TeamID 研究记录

## 目标行为

在 CS2 / HLAE 的 `mirv_pov` 下修复关闭 X 光时的 TeamID overhead 显示：

- 关闭 X 光时：显示当前 POV 队友头顶 TeamID，隐藏敌人 TeamID。
- 开启 X 光时：保持游戏默认行为，所有玩家 TeamID 都显示。
- 用户希望只需要执行 `mirv_pov 1`，不需要额外 TeamID 命令即可启用 patch 和 debug。

## 当前代码位置

- 模块：`AfxHookSource2/MirvPovTeamID.cpp`
- 头文件：`AfxHookSource2/MirvPovTeamID.h`
- 构建接入：`AfxHookSource2/CMakeLists.txt`
- 启用链路接入：`AfxHookSource2/MirvCommands.cpp`

当前分支 / worktree：

- `/mnt/d/Edu/Python/advancedfx-build-src/.claude/worktrees/teamid-fix`

## 参考实现：MulNX_CS2

参考仓库：`https://github.com/Co1Swet/MulNX_CS2`

关键文件：

- `/tmp/MulNX_CS2/source/MulNXExtensions/CS2/Feature/Visual/TeamIDController/TeamIDController.cpp`
- `/tmp/MulNX_CS2/source/MulNXExtensions/CS2/Intro/Signatures.hpp`

MulNX 的 TeamID 显示过滤核心是 client.dll 内部 hook，不是 Panorama hook。

关键 signature：

```cpp
inline const static MulNX::Memory::Pattern PosTeamID_CmpForHide(
    "0F 5B FF 0F 2F FE 0F 82 ?? ?? ?? 00 F3 0F 10 44 24"
);

inline const static MulNX::Memory::Pattern PosTeamID_xxIt(
    "41 BC FF FF 00 00 48 8B ?? ?? 33 DB 48 8B FB 66 44 ?? ?? ?? ?? 0F 84 ?? ?? ?? 00"
);
```

MulNX 逻辑：

- 在距离比较点附近 hook。
- `rdi` 是当前遍历到的 `C_CSPlayerPawn*`。
- 如果目标和当前观察者同队，则继续原始距离规则。
- 如果目标是敌人，则跳到 loop tail / next player，跳过该敌人的 TeamID。

MulNX 同文件里的 Panorama hooks 主要是 TeamID 颜色 / `hudreticle.xml` / `CStylePropertyWashColor` 相关，不是隐藏/显示 TeamID 的主逻辑。

## IDA / MCP 研究结论

### TeamID 主函数

MCP 定位到 TeamID overhead 主逻辑函数：

- `sub_180DF9780`

当前用于基线 patch 的距离比较点：

```asm
180df9e44  cvtdq2ps xmm7, xmm7
180df9e47  comiss xmm7, xmm6
180df9e4a  jb loc_180DFA5F7
180df9e50  movss xmm0, dword ptr [rsp+78h]
```

对应 signature：

```text
0F 5B FF 0F 2F FE 0F 82 ?? ?? ?? 00 F3 0F 10 44 24
```

### X 光状态

MCP 反汇编显示 `sub_180DF9780` 内部使用 `r13b` 表示游戏内部“显示全部 / X 光相关”的状态：

```asm
180df9859  mov r13b, 1
180df985e  xor r13b, r13b
```

后续多个 hidden / visibility 分支会检查 `r13b`。

因此用户观察“开启 X 光后不会被遮挡影响”与代码逻辑一致：开启 X 光时，TeamID 主函数内部有路径绕过或弱化部分 hidden / visibility 判断。

### 遮挡导致 TeamID 消失

仅 hook 距离比较点不够覆盖所有隐藏路径。TeamID 主函数中还有多个 early skip / hidden / projection / visibility 相关判断。关闭 X 光时，队友被遮挡后可能在到达距离比较点之前或之后被其他判断跳过。

已确认过的相关函数 / 判断包括：

- `sub_1808582E0`：observer / target hidden 相关判断，可能影响遮挡或可见性。
- `sub_180BE7230`：spatial / projection 相关判断。
- `sub_180E133F0`：TeamID panel visible 状态写入函数，调用点附近 `edx` 是 visible bool。

重要结论：直接 hook `sub_180E133F0` 的 visible 参数太晚，部分 early skip 路径不会走到这个 visible 写入点，因此不能救回已经被跳过的队友 TeamID。

## 已尝试方案

### 1. 独立模块拆分

最初误把逻辑放到 `DeathMsg.cpp`，后来按要求拆成：

- `MirvPovTeamID.cpp`
- `MirvPovTeamID.h`

并接入 `CMakeLists.txt`。

### 2. MulNX 风格基线 patch

实现逻辑：

- 找距离比较点 signature。
- 找 next-player / loop-tail signature。
- 在距离比较点写 `E9` 跳到 code cave。
- code cave 调用 `MirvPovTeamID_ShouldHide(CEntityInstance* playerPawn)`。
- 如果 helper 返回 true，跳到 next-player，隐藏敌人。
- 如果 helper 返回 false，执行原始被覆盖指令并返回。

该方案曾达到过“队友 TeamID 能显示，但被遮挡后消失”的状态。

当前已按用户要求切回这个方向。

### 3. 多点 hidden / projection / visibility patch

曾尝试增加多个分支 patch，覆盖遮挡相关路径：

- hidden spatial
- hidden projection
- projection front
- cvar / mode skip
- entity check
- visibility check
- visibility flag

结果：复杂度高、风险高，并且出现过触发遮挡后闪退。结论是不应继续在这个基础上叠加多点 patch。

### 4. visible override / 强制显示

曾尝试在 `sub_180E133F0` 调用前 hook `movzx edx, sil`，对队友强制设置 visible。

问题：

- 该点太晚，无法覆盖 early skip。
- 曾出现 helper 返回值被 `pop rax` 覆盖的问题。
- 即使修复返回值保存，用户测试仍然“关闭 X 光时队友敌人都没 TeamID”。

结论：visible override 不是稳定方向。

### 5. 使用 `spec_show_xray` 判断 X 光

曾尝试读取 `spec_show_xray` / 相关 cvar 判断 X 光。

问题：

- 日志显示 `xray=1` 时关闭 X 光场景也会被误判。
- MCP 显示 TeamID 函数实际内部状态是 `r13b`。

结论：判断 TeamID 内部“是否显示全部”应优先用 `r13b`，而不是外部 cvar。

### 6. near code cave / rel32 范围检查

因为 x64 `E9 rel32` / `0F 8? rel32` 只能跳 ±2GB，曾怀疑闪退来自普通 `MdtAllocExecuteableMemory(128)` 分配的 cave 太远。

已尝试：

- 实现 near allocation：`VirtualAlloc` 在 patch 点附近搜索可执行内存。
- 对 cave jump、return jump、hide jump、original jump 增加 rel32 范围检查。

结果：开启 X 光闪退仍存在，说明闪退不只是 cave 距离问题。

### 7. 栈对齐修复

曾怀疑 code cave 调用 C++ helper 前 Windows x64 栈对齐错误。尝试调整过 shadow space / `sub rsp`。

后续读到当前文件状态时，cave code 仍是基线结构：

```cpp
0x50, 0x51, 0x52,
0x41, 0x50,
0x41, 0x51,
0x41, 0x52,
0x41, 0x53,
0x48, 0x83, 0xEC, 0x28,
...
0x48, 0x83, 0xC4, 0x28,
```

注意：这里涉及调用约定和栈对齐，仍是潜在崩溃点之一。

### 8. 崩溃隔离版

曾临时在 `MirvPovTeamID_ApplyPatches` 中 early return：

```cpp
advancedfx::Warning("[mirv_pov_teamid] TeamID patch disabled for crash isolation.\n");
return;
```

用于确认开启 X 光闪退是否来自 TeamID hook。

用户随后要求“还是先切为之前的被遮挡版本”，因此已去掉该 early return，恢复基线 patch。

## 当前版本状态

当前已切回“之前的被遮挡版本”方向：

- `MirvPovTeamID_ApplyPatches` 不再 early return。
- 使用 MulNX 风格距离比较点 hook。
- 使用 near cave 和 rel32 范围检查。
- 保留 debug counters。
- `mirv_pov 1` 会启用 TeamID debug、reset counters、apply patch、打印 status。

当前预期行为：

- 关闭 X 光时：目标是恢复队友 TeamID，但队友被遮挡后仍可能消失。
- 开启 X 光：如果仍闪退，需要继续从当前基线排查。

### 2026-07-03 继续修复记录

本轮已在 `AfxHookSource2/MirvPovTeamID.cpp` 做稳定性和 X 光默认行为修复：

- code cave 不再用手写固定偏移数组，改为按指令 emit，减少偏移维护错误。
- cave 调用 helper 前保存全部 GPR，并保存 / 恢复 caller-saved `xmm0-xmm5`。
- cave 将 TeamID 主函数内部的 `r13` 作为第二参数传入 helper。
- helper 签名改为 `MirvPovTeamID_ShouldHide(CEntityInstance* playerPawn, bool showAllTeamIDs)`。
- 当 `showAllTeamIDs` 为 true 时直接返回 false，保持 X 光开启时游戏默认显示所有 TeamID。
- 当前观察队伍不再把 `GetRealSplitScreenPlayer(0)` 误当 pawn 使用，而是复用 `GetFakePovRadarController()` 的 auto-sync controller 结果取队伍。
- helper 的实体访问包进 `__try / __except`，坏指针或偏移异常时返回 false，避免热路径直接崩溃。
- patch 成功后才标记 `bPatched = true`，失败时后续可重试。
- debug status 新增 `showAll` 和 `exceptions` 计数。

验证结果：

- Windows Release 构建通过：`AfxHookSource2.dll` 已生成。
- DLL 已复制到：
  - `D:\Edu\Python\CS_AutoHighlight\tools\hlae\x64\AfxHookSource2.dll`
  - `D:\Edu\Python\advancedfx\build\x64\AfxHookSource2\Release\AfxHookSource2.dll`
- 三个 DLL 当前大小一致：`5,552,128` bytes，时间为 `2026/07/03 20:50`。

仍需进 CS2 / HLAE 手动验证：

1. `mirv_pov 1` 后不开 X 光不闪退。
2. 开启 X 光不闪退，且 `mirv_teamid status` 中 `showAll` 会增长。
3. 关闭 X 光无遮挡场景下，`sameTeam / differentTeam / hidden` 正常增长，队友显示、敌人隐藏。
4. 队友被遮挡消失问题仍按“第三阶段：处理遮挡消失”推进；本轮未重新引入多点 hidden / projection patch。

### 2026-07-04 最新 client.dll 校验记录

当前磁盘文件：

- `D:\ProgramFile\Steam\steamapps\common\Counter-Strike Global Offensive\game\csgo\bin\win64\client.dll`
- 文件大小：`37,272,728` bytes
- 修改时间：`2026-07-04 06:15:12 +0800`

MCP 当前旧 session `cs2-client` 创建于 `2026-07-03 15:11`，不能直接视为最新 IDB。尝试打开 fresh worker / 临时副本时 MCP `idalib_open` 返回 schema 错误，未创建新 session。因此本次用磁盘字节扫描 + `objdump` 直接校验最新版 DLL。

最新版磁盘字节扫描结果：

```text
cmpForHide count=1 raw=0xdfb3c4 rva=0xdfbfc4 va=0x180dfbfc4
nextPlayer count=1 raw=0xdfbb77 rva=0xdfc777 va=0x180dfc777
```

最新版 TeamID 函数入口约为：

```asm
180dfb900  mov rax, rsp
180dfb903  push rbp
180dfb904  push rbx
180dfb905  push rdi
180dfb906  push r15
```

最新版 `r13b` 状态仍在函数开头按 show-all / X 光相关路径置 1 或清 0：

```asm
180dfb9d9  mov r13b, 1
180dfb9de  xor r13b, r13b
```

最新版 loop 中 `rdi` 仍是当前目标实体 / pawn 指针：

```asm
180dfbc7d  mov rdi, rax
...
180dfbe18  mov rax, [rdi]
180dfbe1b  mov rcx, rdi
180dfbe1e  call qword ptr [rax+4C0h]
...
180dfbe46  mov rdx, rdi
180dfbe49  mov rcx, rsi
180dfbe4c  call 1808582f0h
```

最新版距离比较 patch 点和 next-player tail 仍保持预期结构：

```asm
180dfbfc0  movd xmm7, dword ptr [rax]
180dfbfc4  cvtdq2ps xmm7, xmm7
180dfbfc7  comiss xmm7, xmm6
180dfbfca  jb 180dfc777h
180dfbfd0  movss xmm0, dword ptr [rsp+78h]
...
180dfc777  mov r12d, 0FFFFh
180dfc77d  mov rsi, [rbp-60h]
180dfc781  xor ebx, ebx
180dfc783  mov rdi, rbx
```

结论：当前 `MirvPovTeamID.cpp` 使用的两个 signature 在最新版 `client.dll` 中仍唯一匹配；`r13b` 和 `rdi` 假设仍成立。本轮不需要修改代码或重新构建 DLL。此前已安装的 DLL 仍是当前代码对应版本。

## 当前已知风险点

### 1. code cave 调用 helper 的寄存器保存不完整

当前 cave 保存了：

- `rax`
- `rcx`
- `rdx`
- `r8`
- `r9`
- `r10`
- `r11`

但 helper 是普通 C++ 函数，会按 Windows x64 ABI 保证保留 non-volatile registers。理论上 `rbx/rbp/rsi/rdi/r12-r15` 应由被调用函数保存，但 inline hook 在游戏函数中间调用复杂 C++ 逻辑，仍需谨慎。

### 2. XMM / flags 未完整保存

hook 点附近原始逻辑依赖：

```asm
cvtdq2ps xmm7, xmm7
comiss xmm7, xmm6
jb ...
```

当前 cave 会重新执行这些被覆盖指令，因此 flags 会由 `comiss` 重新生成。

但 helper 调用前后的 XMM 状态没有保存。如果 helper 或编译器生成代码改写 caller-saved XMM 寄存器，而原函数后续依赖这些寄存器，则可能导致异常行为或崩溃。

### 3. 当前观察者 pawn 获取逻辑可能仍不稳定

历史上尝试过两种方式：

- `GetSplitScreenPlayer(0)`：baseline 旧代码中使用，但当前代码不可直接用。
- `GetRealSplitScreenPlayer(0)`：当前可用，但它可能返回 controller 或 pawn，需结合实际实现确认。

当前读取到的 `ClientEntitySystem.h` 暴露：

```cpp
class CEntityInstance * GetRealSplitScreenPlayer(int slot);
class CEntityInstance * GetEntityFromIndex(int index);
```

如果 `GetObservedPlayerPawn()` 获取不到当前观察者，debug 中 `noObserved` 会增加，TeamID 过滤会失效或表现异常。

### 4. helper 内部逻辑较重

`MirvPovTeamID_ShouldHide` 会调用：

- `MirvPov_IsEnabled()`
- `GetObservedPlayerPawn()`
- `GetRealSplitScreenPlayer(0)`
- `GetEntityFromIndex(...)`
- `CEntityInstance::GetObserverTarget()`
- `CEntityInstance::GetTeam()`
- debug 打印 throttled

在 TeamID 渲染热路径中调用这些逻辑有风险。MulNX 的 hook 框架可能对上下文保存更完整，而当前手写 cave 更容易破坏现场。

## Debug counters

`mirv_teamid status` / `mirv_pov 1` 会打印类似：

```text
mirv_teamid debug=1 calls=0 hidden=0 noObserved=0 sameTeam=0 differentTeam=0 lastTargetTeam=-1 lastObservedTeam=-1
```

字段含义：

- `calls`：helper 被调用次数。
- `hidden`：helper 判定应隐藏敌人的次数。
- `noObserved`：没有找到当前观察者 pawn 的次数。
- `sameTeam`：目标和观察者同队次数。
- `differentTeam`：目标和观察者不同队次数。
- `lastTargetTeam`：最近一次目标 team。
- `lastObservedTeam`：最近一次观察者 team。

如果 `calls` 一直是 0：

- patch 点没有被执行；或
- patch 没有成功；或
- TeamID 路径未进入；或
- 执行前已崩溃。

如果 `noObserved` 增长：

- 当前观察者 pawn 获取逻辑有问题。

如果 `sameTeam/differentTeam` 增长：

- helper 正常运行，后续应重点看跳转和原始逻辑恢复。

## 构建和安装命令

构建 x64 Release：

```cmd
cmd.exe /C "cd /D D:\Edu\Python\advancedfx-build-src\.claude\worktrees\teamid-fix && cmake --build build\x64-release --config Release --target AfxHookSource2"
```

安装到两个路径：

```cmd
cmd.exe /C "copy /Y D:\Edu\Python\advancedfx-build-src\.claude\worktrees\teamid-fix\build\x64-release\AfxHookSource2\Release\AfxHookSource2.dll D:\Edu\Python\CS_AutoHighlight\tools\hlae\x64\AfxHookSource2.dll && copy /Y D:\Edu\Python\advancedfx-build-src\.claude\worktrees\teamid-fix\build\x64-release\AfxHookSource2\Release\AfxHookSource2.dll D:\Edu\Python\advancedfx\build\x64\AfxHookSource2\Release\AfxHookSource2.dll"
```

如果复制失败，通常是 HLAE / CS2 正在占用 DLL，需要关闭相关进程后再复制。

## 建议后续方向

### 第一阶段：稳定基线，不解决遮挡

先确保“MulNX 风格基线版本”稳定：

1. `mirv_pov 1` 后不开 X 光不闪退。
2. 开 X 光不闪退。
3. `mirv_teamid status` 中 `calls` 会增长。
4. 关闭 X 光时队友 TeamID 至少在无遮挡时显示。
5. 暂不解决遮挡消失。

如果当前基线仍闪退，建议把 cave 改成更保守的两阶段测试：

- 第一版：只 trampoline 原始 18 字节，不调用 helper，不跳 next-player。
- 第二版：调用一个极简 helper，只计数并返回 false，不访问实体系统。
- 第三版：再逐步加入 `GetObservedPlayerPawn()` 和 team 判断。

这样可以定位崩溃到底来自：

- trampoline / 原始指令恢复；
- C++ helper 调用约定；
- 实体系统访问；
- 跳到 next-player；
- debug 打印。

### 第二阶段：确认 X 光状态

如果需要实现“开启 X 光时所有 TeamID 默认显示”，不要读 `spec_show_xray`，应优先使用 TeamID 函数内部的 `r13b`。

但当前基线 hook 点没有把 `r13b` 传给 helper。后续如果要加，需要在 cave 里额外读取 `r13b` 并作为参数传入 helper，例如传到 `rdx`。

### 第三阶段：处理遮挡消失

只有在基线稳定后再处理遮挡。

建议不要继续大规模多点 patch。更稳妥方向：

1. MCP 精确列出所有跳到 `loc_180DFA5F7` 或 TeamID skip tail 的路径。
2. 逐个判断每条路径是否只影响 TeamID，还是也影响布局 / 状态初始化。
3. 优先寻找和 `r13b` 相关的“X 光时绕过遮挡”的最小判断点。
4. 只 patch 一个最小分支，使队友在关闭 X 光时走类似 X 光的可见路径，敌人仍保持隐藏。

不要再直接 hook final visible write，因为 early skip 会绕过它。

## 当前结论

- MulNX 的核心修复确实是 client.dll TeamID 距离比较点 hook。
- 关闭 X 光时队友被遮挡消失，不是 MulNX signature 本身能完整解决的问题；当前 CS2 TeamID 主函数还有额外 hidden / visibility / projection 分支。
- X 光状态在 TeamID 主函数内部主要体现为 `r13b`，不应依赖外部 cvar 猜测。
- 当前最稳妥策略是先回到“无遮挡可用、遮挡会消失”的基线，再逐步定位遮挡分支。
- 当前版本已按用户要求切回基线方向，并已构建安装。
