# mirv_pov TeamID reverse-engineering notes

## Required behavior

- `mirv_pov` disabled: leave TeamID behavior untouched.
- `mirv_pov` enabled and `spec_show_xray 0`: use the selected POV player as the
  native TeamID context. The game then applies its own teammate, distance,
  visibility, and panel rules.
- `mirv_pov` enabled and `spec_show_xray 1`: leave the observer/X-ray behavior
  untouched.

`mirv_pov` intentionally does not set `spec_show_xray`; it remains under user
control and can be changed while a demo is playing.

## Local binary analysis

The implementation was derived from the locally installed 2026-07-25
`client.dll` using `dumpbin /disasm`.

The TeamID update routine starts at approximately `0x180E2B7F0`. Early in the
routine it obtains two player objects:

```asm
180E2B847  call 180C10C40
180E2B84C  mov  r14,rax
180E2B853  call 180C10EF0
180E2B858  mov  rsi,rax
```

The second result (`rsi`) is subsequently passed as the first argument to the
game's relationship predicate for each candidate pawn (`rbx`):

```asm
180E2BC70  mov  rdx,rbx
180E2BC73  mov  rcx,rsi
180E2BC76  call 180898420
180E2BC7B  test al,al
```

`sub_180898420` performs native entity validation and ultimately compares the
two players' teams. This makes the context-player call site a smaller and more
stable intervention point than changing a later per-player branch.

## Implementation

`AfxHookSource2/MirvPovTeamID.cpp` patches only the five-byte call at the second
getter site. Its wrapper always calls the original getter first, then:

1. returns the original result when `mirv_pov` is disabled;
2. returns the original result when `spec_show_xray` is enabled or unavailable;
3. otherwise returns `GetCurrentPovPlayerPawn()` when it resolves to a player
   pawn, falling back to the original result on failure.

No candidate pawn is filtered directly and no TeamID loop branch is skipped.
The original call bytes are restored when `mirv_pov` is disabled.

## Test matrix

```text
mirv_pov false                       -> completely native TeamID behavior
mirv_pov true; spec_show_xray 0      -> native teammate rules relative to POV
mirv_pov true; spec_show_xray 1      -> native observer/X-ray behavior
```

Use `mirv_teamid status` to inspect call, override, native-X-ray, fallback, and
exception counters.
