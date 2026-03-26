# Casprix Language ABI Shim

This directory provides the stable C ABI surface intended for binding the async runtime to higher-level systems languages.

## Files

- `lang_abi.c`: versioned function-table ABI shim implementation
- `include/casprix/lang_abi.h`: public ABI contract

## Notes

- Current implementation is intentionally minimal and stable-first.
- Task and pool APIs are available.
- I/O and timer APIs are capability-gated and currently return `CPX_STATUS_UNSUPPORTED` until runtime internals are fully bridged.

## Next Steps

1. Bind `io_submit` to runtime event loop operations.
2. Bind `timer_arm` to runtime timer wheel/heap.
3. Add async completion callback trampoline API.
4. Add capability bits for cancellation and NUMA affinity.
