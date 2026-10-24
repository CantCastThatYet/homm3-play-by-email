# Experimental inner-world reload

`world_resume.hpp/.cpp` implements a callable native world replacement. It is
not a complete reconnect transaction and does not claim that loading bytes
restores the ST barrier or network membership. It never calls new-day setup,
End Turn, the outer contact rollback, or movement renewal.

Integration on the adapter's installed game thread:

1. `Initialize(hd_base, game_thread, {context, ReadFrames})` checks the pinned
   engine file, pinned HD/importer ABI, and all93 bytes of inner wrapper
   HD RVA09EF00, including its relocated cookie address.
2. Put `IsGameplayBlocked()` in the adventure dispatch gate and transport poll
   gate. The adventure gate must suppress the complete original chain, including
   nested dispatches, idle timeout processing and AI work. Poll blocking must
   prevent gameplay packet delivery during the load and subsequent held state.
3. The installed engine004BEFF0 hook calls `ObserveLoadEnter(world,name,a,b)`
   immediately before its original chain and `ObserveLoadReturn(eax)` immediately
   after. These preserve the actual loader AL; the HD wrapper's EAX is not its
   loader result.
4. Supply real save, turn, merge, receive and battle frame depths in `Frames`.
   Certify gates only when installed and active. Returning defaults without
   installing the corresponding observation/gates does not satisfy the contract.
   Native004CBA00 has five stack arguments/RET14; HD0111EA90 has no arguments/RET.
5. The worker validates the checkpoint's authenticated manifest, hashes, matching
   map identity and audit, then calls `Queue`. This deep-copies at most64MiB and
   binds one request token to the immutable bytes and supplied date/owner/geometry.
6. Invoke `TryAtAdventureBoundary()` from the outermost pre-dispatch idle callback.
   It latches the gate itself, verifies an initialized same-geometry world and
   quiet adventure manager. A disposable single-player scaffold with a selected
   centered/transparent hero is prepared using native4175E0(adventure,0,0), only
   when every other readiness guard passes, the authenticated source audit and
   roster are present, and the selected hero plus live patched ABI are validated.
   This restores the scaffold's renderer/map representation and skips the native
   render/pump branch. It re-reads the full audit and strict readiness afterward.
   This cleanup is never applied to the loaded source world. It then stages the captured187-byte protected-save roster,
   ensures the native `RMTGEXBS.BIN` stream exists, replaces its contents, and calls
   the installed HD inner wrapper using ECX=name, no stack arguments. Missing
   streams are created through installed engine004D6EB0/004D6FC0 using read mode,
   preserving HD's native allocation/registration. Existing streams are untouched
   during preparation. Scaffold owner/date may differ; geometry must match.
   Immediately before the inner call it stages only session-local color006995A4
   from authenticated request metadata. The base reader restores current color
   from the saved header but does not restore this local identity. PlayerE1/E2
   and current color are never written by this preparation. Failure restores the
   prior local identity when possible and retains the gate; snapshot records
   staging and rollback outcomes.
7. Read `GetSnapshot()`. Success requires exactly one observed load call/return,
   matching world/name/(0,1), successful actual AL, balanced native frames,
   unchanged geometry/manager identity, expected owner/date and a complete audit.

The disposable single-player loader can normalize multiplayer membership while
preserving checkpoint gameplay. Cold testv10 observed all215 heroes, positions,
movement, resources and date/current matching, with playerC8 IDs and remoteE2
cleared. The later Blue lobby-to-solo test additionally observed local-controller
E1 reassigned from Blue to the scaffold's Red seat. The helper retains the exact
native result in `loaded_before_membership`. Before any membership write it
compares gameplay against source, excluding exactly C8/E1/E2. Every other audited
field must match. Source membership is validated before preparation: exactly one
E1 controller on the authenticated local seat, that seat human, and each human
with a nonzero source transport ID. Only under held/quiescent single-player state
with the authenticated187-byte roster bound to the source audit does it restore
changed C8/E1/E2. It verifies each seat and requires a fresh FULL source audit match.
`membership_reconstruction_attempted` and `membership_reconstructed` distinguish
this explicit reconstruction from native deserialization. No AI, day or turn
transition is invoked. Any failure leaves input held; partial membership writes
are not represented as an atomic native rollback.

The helper retains its input gate after attempting recovery, even on failure or
a matching source audit. The enclosing transaction must rebind native transport identities, rebuild
ST scheduling using validated semantics, repair/redraw UI and validate those
operations before calling `ReleaseAfterRebind`. Release requires the matching
request token, source-audit success, a live audit exactly matching the supplied
rebound audit, and an explicit read-only caller authorization callback certifying
transport/ST/UI success. Only transport IDs may differ from the source audit;
date, colors, human/local flags, resources and hero movement stay unchanged. A
second live audit after the callback precedes release. On native load failure the world may already
be partially reset: dispose of the experimental client. There is no invented
rollback guarantee for the engine loader. A successful MemFile import followed
by a later refusal also leaves that stream replaced and the client held.
Stream creation is also not transactional: failure preparing a later role may
leave earlier empty native objects registered. Such failures retain the gate.
Partial initialization does not install hooks or set the initialized flag; a
retry is permitted. Successful initialization is one-shot. The executable hash
is the deployed `h3hota HD.exe`5aaab925...18de, checked against the owned copy,
rather than the original unpatched on-disk `h3hota.exe` used for early research.

`ReadAudit` captures date, dimensions, underground level, local/current colors,
all eight human/local flags and native transport IDs, all player resources, and
every hero's ID, owner, coordinates, maximum and remaining movement. Hero array
offset/count are read from HotA's patched engine immediates rather than a fixed
156-hero assumption. The verified32-bit LEA encodes a disp32 relative to world;
address calculation wraps modulo2^32 before checking that the final nonzero
array range fits wholly in the32-bit address space. This admits arrays relocated
below world without admitting a wrapping array span. Individual field reads
still use ReadProcessMemory. `SameWorldContent` compares these values while ignoring
process pointers. It does not claim a complete semantic comparison of every map
object, army, spell or quest; the native checkpoint carries those fields.

`EncodeAudit`/`DecodeAudit` use pointer-free little-endian H3RA format version1:
36-byte header, eight36-byte player records and count20-byte hero records.
Reserved bytes must be zero; count is1..1024, total input must match exactly.
Capture code should read the audit immediately before and after serialization,
require `SameWorldContent`, and persist `world-audit.bin` with its own byte count
and SHA256 bound to the checkpoint. Decode output intentionally contains zero
process pointers. `SerializeAudit` produces readable JSON for diagnostics.

Validation: `artifacts/native/world-resume-x86/build-test.cmd` compiles the actual
x86 module and runs audit roundtrip, pointer exclusion, movement/resource/roster
mismatch, malformed length/version/reserved/count, every truncated input, hex
roundtrip and uninitialized-gate checks, plus positive/negative hero displacement
resolution and actual final-range overflow. Existing MemFile tests separately
cover allocation failure, exact shortening and busy/malformed registry rejection.
They additionally test ensure-existing preservation, native-factory seam
creation, replacement of the newly registered stream and factory failure.
These tests execute no game code. Actual native load has not been exercised by
this module's author; root owns disposable-client integration.

Native evidence: `re/completed-turn-cold-rejoin-path.md`,
`re/cold-load-boundary-static.txt`, `re/scaffold-render-cleanup-live.md`, and
`native/MEMFILE_IMPORT.md`. The prior
read-only live bytes corroborate the pinned branch code; they are not a runtime
validation of a recovery attempt. No access to user Red PID7328 was performed.
