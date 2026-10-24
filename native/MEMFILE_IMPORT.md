# Experimental existing MemFile replacement

`memfile_import.hpp/.cpp` implements exact replacement of registered, closed HD
MemFiles, plus explicit cold-scaffold preparation of missing known roles described
below. Its synthetic test is included in `test.ps1`. It does not load a world, reconstruct network state,
restore ST metadata, complete a turn or prove cold recovery.

The three accepted roles have fixed names:

| Role | Destination |
|---|---|
| `BaselineDayOne` | `orig0.bin` |
| `BaselineLater` | `orig.dat` |
| `LocalSnapshot` | `RMTGEXBS.BIN` |

Missing streams return `MissingStream`; no registry entry is created. Incoming and
outgoing transport streams are deliberately unsupported pending proof of fresh
session naming/admission. The caller must choose the baseline appropriate to the
actual world date and establish that the bytes belong to the correct world/epoch.
This module neither authenticates checkpoints nor accepts filenames from them.

## Native admission and address validation

`InitializeNative(hd_base, game_thread_id, admission, context)` must execute on the
explicitly supplied game thread. It verifies that `hd_base` is an actual loaded
module base, hashes that module's disk file against
`0a2c193d0c510d77080b669a710694ef282761b0ec6f6ac5b86f3166b8038550`, checks executable
entry prefixes for all five called native functions, and checks the first three
MemFile vtable entries. `Replace` repeats fingerprint validation and enforces the
same thread. Unsupported/modified entry points fail closed; there is no alternate
ABI or address fallback. This is disk identity plus selected live-byte validation,
not a hash of the complete relocated in-memory module.

| Preferred address | Actual ABI used |
|---|---|
| `011C5227` | cdecl allocation(size), returns pointer |
| `01046760` | thiscall constructor(object, name), ret 4 |
| `01046830` | thiscall write(object, bytes, signed count), ret 8 |
| `01046970` | thiscall clear(object), no stack args |
| `011C55BA` | cdecl sized deletion(object, 0x11D) |

All are relocated from preferred HD base `01000000`. The object layout, registry
at `0121872C`, vtable `01202B14`, and allocator tag `F00DBEEF` follow
`../re/memfile-import-and-completed-resume.md` and its static evidence. The sized
delete prefix was additionally read from the same pinned disk image:
`55 8b ec ff 75 08 e8 43 06 00 00 59 5d c3`.

The required admission callback must certify an exclusively held, quiescent game
thread boundary outside save/load/merge/receive/battle transitions. It is checked
before staging and immediately before commit. This callback is an integration
obligation, not a discovered native quiescence detector. There is currently no
runtime wiring that can supply this proof. Open flags alone are insufficient.

## Publication and ownership

The importer checks bounded registry count/capacity, unique names and objects,
non-overlapping registry/object/payload ranges, native vtables, terminated names,
length/cursor/capacity, payload tags, and memory accessibility. Any busy object in
the registry is conservatively rejected. Requested objects also need writable
headers. Default bounds are 256 registry entries, 64 MiB per replacement and
192 MiB total. Existing object capacities must also be at most 64 MiB.

All requested replacements are first allocated as unregistered native objects,
constructed with the original bounded name, and populated through native Write.
The importer verifies the returned length, object invariants and every payload
byte; it then rewinds staged cursors. Empty input uses an empty constructor and
skips Write. Allocation/write exceptions or validation failures release staging
objects without changing registered streams.

Immediately before publishing, it rechecks registry entries and complete object
headers for changes. Publication swaps only each object's ownership/scalar tail
starting at `+108`, preserving its existing address, name and vtable. All opens
were validated zero. There are no allocations, callbacks or native calls inside
this bounded commit loop. Once all swaps finish, Clear and sized Delete release
the retired buffers through their original native allocator.

The all-or-none guarantee applies to detected failures before commit and assumes
the caller actually holds the exclusive boundary, stable mapped memory and an
unaltered supported allocator. It is not process-crash atomic, concurrent-memory
protection, or a transaction spanning subsequent world loads. Process memory
corruption/access violations and a later engine-load failure have no rollback
claim. Native code's C++ allocation exceptions are caught; SEH faults are not
suppressed. The test backend is a trusted development seam, never a transport
option or production fingerprint bypass.

## Synthetic verification

`memfile_import_test.cpp` runs without loading game binaries. It injects logical
32-bit memory, native allocation/write/cleanup substitutes, thread identity and
fingerprint/admission checks. Covered cases include two-stream publication,
shorter exact truncation, empty replacement, identity preservation, release of
retired ownership, second-allocation/second-write failure preserving both old
streams, short/corrupt writes, revoked admission, busy/malformed/aliased objects,
missing roles, duplicate/unknown roles, wrong thread/build, bounded requests,
state changes during staging, and retry after failure.

Built with MSVC x86 `/std:c++17 /EHsc /W4 /O2 /MT`, linking `common.cpp`, and passed:

```text
artifacts/native/memfile-import-x86/Native.MemFileImportTest.exe
MemFile import synthetic tests passed (no game execution).
```

The isolated generated build command is
`artifacts/native/memfile-import-x86/build-import-test.cmd`; no shared native build
or adapter files were edited. Passing this suite validates the importer algorithm
and seams, not execution of the original allocator or a restored multiplayer game.
# Cold-scaffold stream preparation

`Importer::EnsureRoles` now prepares missing known roles at the same exclusively
held game-thread boundary. It validates the whole registry and refuses any busy,
invalid or aliased object. Existing registered roles are not opened or modified.
For a missing role, it invokes the installed engine file constructor004D6EB0
with ECX=8-byte stack file object and(name,"rb"), RET8, then installed close
004D6FC0 with ECX=file, RET. HD's installed constructor performs native object
allocation and registry insertion; no application code writes registry pointers.
The returned wrapper must have HD vtable RVA20EC60, and the resulting registry
must contain a valid closed stream. Read mode prevents disk truncation if native
interception is unexpectedly absent. Preparation also pins the actual deployed
engine executable hash and constructor/close body tails.

Creation is deliberately separate from transactional payload replacement.
If preparing one role succeeds and another fails, the earlier empty object may
remain. Native constructor failure can leave uncertain registry state; callers
must keep the disposable recovery client held. `Replace` still never creates a
missing role and still stages all requested payloads before publishing any.
