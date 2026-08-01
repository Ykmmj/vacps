#pragma once

/**
 * C module loaders for vacps:* (registered via ModuleCatalog).
 *
 * `binding` is ModuleDescriptor.binding from instance composition:
 *   - nullptr: pure modules (log, crypto)
 *   - FsBindingContext* / HostBindingContext* / StoreBindingContext* /
 *     HttpBindingContext* / ProcessBindingContext*: non-owning into
 *     ScriptServices via ModuleBindings
 *
 * Call-time Promise/job work still uses script_runtime_from(ctx); binding
 * carries I/O config so modules need not take full ScriptServices*.
 */

#include <quickjs.h>

namespace vacps::js {

JSModuleDef* init_module_log(JSContext* ctx, const char* name, void* binding);
JSModuleDef* init_module_store(JSContext* ctx, const char* name, void* binding);
JSModuleDef* init_module_host(JSContext* ctx, const char* name, void* binding);
JSModuleDef* init_module_http(JSContext* ctx, const char* name, void* binding);
JSModuleDef* init_module_fs(JSContext* ctx, const char* name, void* binding);
JSModuleDef* init_module_process(JSContext* ctx, const char* name, void* binding);
JSModuleDef* init_module_crypto(JSContext* ctx, const char* name, void* binding);

}  // namespace vacps::js
