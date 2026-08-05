# vacps::binding::detail

Internal headers for the phase-1 binding DSL. Not part of the stable public API.

| Header              | Role                                                        |
| ------------------- | ----------------------------------------------------------- |
| `concepts.hpp`      | Callable traits, Result detection                           |
| `invoke.hpp`        | Arg decode (no T{} required) + return encode dispatch       |
| `native_slot.hpp`   | Heap `move_only_function` storage for `JS_NewCFunctionData` |
| `class_storage.hpp` | Per-T `JSClassID` + `shared_ptr` opaque; one name per T     |
