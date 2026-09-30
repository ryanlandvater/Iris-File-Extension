# TASKS — Iris File Extension

Tickets filed from outside the recovery work orders. **The authoritative task list for
recovery is still `MIGRATION.md`** (`# ▶ RECOVERY WORK ORDERS`); this file holds items
that do not belong to that series. Re-verify every `file:line` with `grep` before
editing — line numbers drift.

---

## VL-1 — The validation layer's `std::string*` diagnostic sink is a data race under a multithreaded writer (P1, latent)

**Filed 2026-09-15** from FastFHIR, where the same design was ported for its conformance
layer (FastFHIR `TASKS.md` Block K, decision D4) and changed for exactly this reason.
Ryan's direction: IFE writes with multiple threads too, so the sink has to be a parallel
logger that threads drop entries into simultaneously, claiming space by CAS.

### The defect

`ValidationHooks::diagnostic` is a caller-owned `std::string*`
(`generated_source/IFE_Blocks.hpp:670`), and every failing check writes it through one
helper:

```cpp
// generated_source/IFE_Validation.cpp:25
if (__hooks != nullptr && __hooks->diagnostic != nullptr)
    *__hooks->diagnostic = __message;
```

`std::string::operator=` is not thread-safe. The hooks struct is attached **once, at
writer creation** (the header's own comment), so every thread that calls `store()` on
that writer shares the same `std::string`. Two concurrent failing stores assign it at
the same time: undefined behaviour, in practice a torn string, a double free during
reallocation, or a crash inside the layer. The layer is `noexcept`, so a crash there
takes the process down.

The documented usage contract fails under concurrency too, and fails silently:
"clear it before a store to detect whether one fired" (`IFE_Blocks.hpp`, the comment on
`diagnostic`). With several writers, thread A's clear erases thread B's message, and
thread A then reads B's message as its own. The `Status` is still correct because it is
returned per call, but the sink attributes diagnostics to the wrong store.

### Where the concurrency comes from

Iris-Codec encodes tiles on `hardware_concurrency()` workers and hands each stream to
`Builder::append_tile` after compression returns (since 2026-09-30; it used to claim
with its own `offset.fetch_add`), which claims lock-free from any thread. A writer that
validates `TILE_PIXEL_DATA` or the per-layer tables from those workers reaches
`__report` from many threads at once.

**Current exposure: latent.** In the checkouts read on 2026-09-15, Iris-Codec does not
reference `ValidationHooks`, so no shipped path attaches the layer to the parallel
encoder yet. The bug fires the first time one does, and nothing in the API warns
against it. `examples/validation_layer.cpp` and every test drive it from one thread,
which is why the suite is green.

### Fix

- [ ] VL-1.1 **Replace the sink with a parallel log.** A caller-owned, fixed-capacity
      buffer; a writer claims `offset..offset+len` with a `compare_exchange_weak` loop
      that computes the end first and **refuses** (without moving the head) when the
      entry does not fit, counting the refusal in an atomic dropped counter; then
      copies its message into the claimed range. Messages are already string literals
      emitted by the generator (`generator/emit/cpp.py:1810`, `:1923`), so the failure
      path still allocates nothing. Use CAS rather than `fetch_add`: `fetch_add` cannot
      refuse a claim, so an entry that overflows leaves unwritten bytes inside the
      readable range (the defect FastFHIR filed as LOG-1).
- [ ] VL-1.2 **Add a caller-owned `std::atomic<uint64_t>* failures`** beside the sink,
      incremented on every failing check, so a caller can detect "did anything fire"
      without the clear-then-read pattern. Remove that pattern from the docs.
- [ ] VL-1.3 **Fix it in the emitter, not the generated file.** The struct and its
      comment are emitted at `generator/emit/cpp.py:1026–1038`, `__report` at
      `:1846–1850`, and the usage example in the doc comment at `:1979`. Regenerate,
      and update `examples/validation_layer.cpp` (`:91`, `:107`, `:127`, `:146` assign
      `hooks.diagnostic = &why`).
- [ ] VL-1.4 **Readers:** document that reading the log is valid once writers have
      stopped (or add a committed watermark advanced after each copy). Pick one; say
      which in the header.
- [ ] VL-1.5 **This is an ABI change** to `ValidationHooks` (the IFE header says the
      call site was emitted early precisely so the ABI would not move later). Record
      it in `MIGRATION.md`.
- [ ] VL-1.6 **Test under ThreadSanitizer:** one hooks struct, N threads each storing a
      spec-violating block through it, a log sized to overflow partway. Assert no
      TSan report, every line read back is a complete message, no NUL bytes, and
      `lines + dropped == failures == N × stores`.

**Acceptance:** VL-1.6 green under `-fsanitize=thread` in CMake and Bazel;
`examples/validation_layer.cpp` still runs; wire witness unchanged (the layer emits no
wire constant).

**Reference implementation to compare against:** FastFHIR `include/FF_Conformance.hpp`
(`ValidationHooks::diagnostic` / `failures`), `src/FF_Builder.cpp`
(`_check_conformance`), and, once LOG-1 lands, `include/FF_Logger.hpp`.
