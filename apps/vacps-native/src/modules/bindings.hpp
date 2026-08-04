#pragma once

/**
 * C module loaders for vacps:* (registered via ModuleCatalog).
 * Runtime-core ships pure modules (crypto, host) plus async-capable log,
 * store, fs, http, and process.
 *
 * Loaders recover runtime-scoped composition (Runtime::Async /
 * Runtime::Callbacks /
 * ProcessRuntime / data_dir / ca_bundle) via
 * js::async_runtime_from_context(ctx) /
 * js::callbacks_runtime_from_context(ctx) /
 * js::process_runtime_from_context(ctx) /
 * js::composition_from_context(ctx) instead.
 * Pure synchronous modules construct Env from the live ctx alone.
 *
 * Module load/init callbacks are Narrow for live ctx/module/name and the
 * installed composition opaque. Unknown module specifiers remain a Wide
 * loader ReferenceError from ModuleCatalog::load_module.
 */

#include <quickjs.h>

namespace vacps::js {

JSModuleDef* init_module_crypto(JSContext* ctx, const char* name);
JSModuleDef* init_module_host(JSContext* ctx, const char* name);
JSModuleDef* init_module_log(JSContext* ctx, const char* name);
JSModuleDef* init_module_store(JSContext* ctx, const char* name);
JSModuleDef* init_module_fs(JSContext* ctx, const char* name);
JSModuleDef* init_module_http(JSContext* ctx, const char* name);
JSModuleDef* init_module_process(JSContext* ctx, const char* name);

}  // namespace vacps::js
