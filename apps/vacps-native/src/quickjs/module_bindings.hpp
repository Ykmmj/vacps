#pragma once

/**
 * Non-owning binding contexts for ModuleDescriptor.binding.
 *
 * Composition root (install_default_modules) fills ModuleBindings from
 * ScriptServices, then register_modules points each descriptor.binding at
 * the matching context (or nullptr for pure modules: log / crypto).
 *
 * Lifetime: owned by ScriptRuntime; pointers into ScriptServices fields.
 * Catalog descriptors hold void* into these structs — ModuleBindings must
 * outlive ModuleCatalog usage (both destroyed with ScriptRuntime).
 *
 * Keep this header free of heavy domain includes (http/server, nlohmann, …)
 * so ScriptRuntime (quickjs_core) can include it without domain link edges.
 */

#include <string>

namespace boost::asio {
class thread_pool;
}

namespace vacps::bootstrap {
class EnvironmentSnapshot;
}

namespace vacps::process {
class ProcessRuntime;
}

namespace vacps::js {

class ScriptServices;

/** vacps:fs — path root + offload pool + File backend probe. */
struct FsBindingContext {
  std::string* data_dir{nullptr};
  boost::asio::thread_pool* pool{nullptr};
  bool* use_asio_file{nullptr};
};

/** vacps:host — dataDir + getenv snapshot. */
struct HostBindingContext {
  std::string* data_dir{nullptr};
  vacps::bootstrap::EnvironmentSnapshot* environment{nullptr};
};

/** vacps:store — SQL offload pool (paths come from JS / host.dataDir). */
struct StoreBindingContext {
  boost::asio::thread_pool* pool{nullptr};
};

/** vacps:http — CA bundle for outbound TLS (Servers are JS-owned, not registered). */
struct HttpBindingContext {
  std::string* ca_bundle{nullptr};
};

/** vacps:process — ProcessRuntime (executor + budget). */
struct ProcessBindingContext {
  vacps::process::ProcessRuntime* processes{nullptr};
};

/**
 * Composition bag of binding contexts.
 * Owned by ScriptRuntime; wire_from() fills non-owning refs into ScriptServices.
 * Pure modules (log, crypto) stay nullptr — no entry here.
 */
struct ModuleBindings {
  FsBindingContext fs;
  HostBindingContext host;
  StoreBindingContext store;
  HttpBindingContext http;
  ProcessBindingContext process;

  void wire_from(ScriptServices& services) noexcept;
};

}  // namespace vacps::js
