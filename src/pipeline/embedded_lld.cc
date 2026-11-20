// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "pipeline/embedded_lld.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/build_config.h"
#include "debug/dcheck.h"
#include "diag/bag.h"
#include "fpag/debug/profiler/profile_scope.h"
#include "fpag/io/io_util.h"
#include "path/path.h"

#if defined(ALCY_EMBEDDED_LLD)
#include "codegen/target.h"
#include "fpag/base/numeric.h"
#include "i18n/messages.h"
#include "lld/Common/Driver.h"
#include "llvm/Support/raw_ostream.h"
#include "pipeline/diag_code.h"
#include "pipeline/link_options.h"
#include "pipeline/pipeline_context.h"

#if BUILD_FLAG(IS_OS_WIN)
LLD_HAS_DRIVER(coff)
#elif BUILD_FLAG(IS_OS_MAC)
LLD_HAS_DRIVER(macho)
#else
LLD_HAS_DRIVER(elf)
#endif
#endif

#if BUILD_FLAG(IS_OS_WIN)
// windows.h first, not the handful of SDK headers the directory listing
// needs: a bare <fileapi.h> reaches winnt.h before anything has defined
// _AMD64, and winnt.h rejects that.
#include <windows.h>
#else
#include <dirent.h>
#endif

namespace pipeline {

#if defined(ALCY_EMBEDDED_LLD)

namespace {

// The spellings this file opens with are plain and NUL-free, so the
// conversion cannot fail.
path::Path as_path(std::string_view text) {
  base::Result<path::Path, path::PathError> result =
      path::Path::from_native(text);
  DCHECK(result.is_ok());
  return std::move(result).unwrap();
}

std::string as_string(const path::Path& p) {
  return std::string(p.as_view());
}

#if BUILD_FLAG(IS_OS_WIN) || BUILD_FLAG(IS_OS_MAC)
// The immediate entry names of a directory, unsorted. An unreadable or
// absent directory is an empty list.
std::vector<std::string> list_directory(const path::Path& dir) {
  std::vector<std::string> names;
#if BUILD_FLAG(IS_OS_WIN)
  WIN32_FIND_DATAA found;
  HANDLE handle = ::FindFirstFileA(dir.join("*").c_str(), &found);
  if (handle == INVALID_HANDLE_VALUE) {
    return names;
  }
  do {
    const std::string_view name(found.cFileName);
    if (name != "." && name != "..") {
      names.emplace_back(name);
    }
  } while (::FindNextFileA(handle, &found) != 0);
  ::FindClose(handle);
#else
  DIR* handle = ::opendir(dir.c_str());
  if (handle == nullptr) {
    return names;
  }
  while (dirent* entry = ::readdir(handle)) {
    const std::string_view name(entry->d_name);
    if (name != "." && name != "..") {
      names.emplace_back(name);
    }
  }
  ::closedir(handle);
#endif
  return names;
}

std::vector<u64> version_parts(std::string_view version) {
  std::vector<u64> parts;
  usize start = 0;
  while (start <= version.size()) {
    const usize dot = version.find('.', start);
    const std::string_view part = version.substr(
        start,
        dot == std::string_view::npos ? version.size() - start : dot - start);
    u64 value = 0;
    for (const char c : part) {
      if (c >= '0' && c <= '9') {
        value = value * 10 + static_cast<u64>(c - '0');
      }
    }
    parts.push_back(value);
    if (dot == std::string_view::npos) {
      break;
    }
    start = dot + 1;
  }
  return parts;
}

bool version_less(std::string_view a, std::string_view b) {
  const std::vector<u64> left = version_parts(a);
  const std::vector<u64> right = version_parts(b);
  for (usize i = 0; i < std::max(left.size(), right.size()); ++i) {
    const u64 l = i < left.size() ? left[i] : 0;
    const u64 r = i < right.size() ? right[i] : 0;
    if (l != r) {
      return l < r;
    }
  }
  return false;
}
#endif  // BUILD_FLAG(IS_OS_WIN) || BUILD_FLAG(IS_OS_MAC)

// The host's architecture, which decides the multiarch directory and the
// loader's file name. Owned, because `host_triple()` hands back a string
// and a view into it would not outlive this call.
std::string host_arch() {
  const std::string triple = codegen::host_triple();
  return triple.substr(0, triple.find('-'));
}

#if !BUILD_FLAG(IS_OS_WIN) && !BUILD_FLAG(IS_OS_MAC)
// What a dynamically linked program starts from: the three startup
// objects, the loader the kernel hands control to, and the directories
// the C library is searched in. lld is a linker and not a driver, so
// what the spawned driver used to add around the object has to be named
// here.
struct Startup {
  path::Path crt1;
  path::Path crti;
  path::Path crtn;
  path::Path dynamic_linker;
  std::vector<path::Path> lib_dirs;
};

// Probes the usual Linux locations, once per process. The host's C
// library is not a behavioral input the way an environment variable
// would be: it is the platform the compiler already targets.
const std::optional<Startup>& startup_inputs() {
  static const std::optional<Startup> FOUND = []() -> std::optional<Startup> {
    const std::string arch = host_arch();
    std::vector<path::Path> dirs;
    dirs.push_back(as_path("/usr/lib64"));
    if (arch == "x86_64") {
      dirs.push_back(as_path("/usr/lib/x86_64-linux-gnu"));
    } else if (arch == "aarch64") {
      dirs.push_back(as_path("/usr/lib/aarch64-linux-gnu"));
    } else if (arch == "riscv64") {
      dirs.push_back(as_path("/usr/lib/riscv64-linux-gnu"));
    }
    dirs.push_back(as_path("/usr/lib"));

    std::optional<path::Path> loader;
    if (arch == "x86_64") {
      loader = as_path("/lib64/ld-linux-x86-64.so.2");
    } else if (arch == "aarch64") {
      loader = as_path("/lib/ld-linux-aarch64.so.1");
    } else if (arch == "riscv64") {
      loader = as_path("/lib/ld-linux-riscv64-lp64d.so.1");
    }
    if (!loader.has_value() || !io::is_file(as_string(*loader))) {
      return std::nullopt;
    }
    for (const path::Path& dir : dirs) {
      const path::Path crt1 = dir.join("crt1.o");
      const path::Path crti = dir.join("crti.o");
      const path::Path crtn = dir.join("crtn.o");
      if (!io::is_file(as_string(crt1)) || !io::is_file(as_string(crti)) ||
          !io::is_file(as_string(crtn))) {
        continue;
      }
      return Startup{.crt1 = crt1,
                     .crti = crti,
                     .crtn = crtn,
                     .dynamic_linker = *loader,
                     .lib_dirs = std::move(dirs)};
    }
    return std::nullopt;
  }();
  return FOUND;
}
#endif  // !BUILD_FLAG(IS_OS_WIN) && !BUILD_FLAG(IS_OS_MAC)

#if BUILD_FLAG(IS_OS_WIN)
// What a Windows link needs beyond the objects: the kernel import
// library. A Windows program references nothing else, so this one file
// is the whole discovery. The kits put it under Lib/<version>/um/<arch>;
// vcvars hands the location down in WindowsSdkDir and WindowsSDKVersion,
// and a host without them is answered with the default install roots,
// highest version first.
struct WindowsStartup {
  path::Path kernel32;
};

std::string windows_sdk_arch() {
  const std::string arch = host_arch();
  return (arch == "arm64" || arch == "aarch64") ? "arm64" : "x64";
}

std::optional<WindowsStartup> kit_kernel32(const path::Path& lib_root,
                                           std::string_view arch) {
  std::optional<WindowsStartup> best;
  std::string best_version;
  for (const std::string& name : list_directory(lib_root)) {
    if (!name.starts_with("10.")) {
      continue;
    }
    const path::Path candidate =
        lib_root.join(name).join("um").join(arch).join("kernel32.lib");
    if (!io::is_file(as_string(candidate))) {
      continue;
    }
    if (best.has_value() && !version_less(best_version, name)) {
      continue;
    }
    best = WindowsStartup{candidate};
    best_version = name;
  }
  return best;
}

const std::optional<WindowsStartup>& windows_startup_inputs() {
  static const std::optional<WindowsStartup> FOUND =
      []() -> std::optional<WindowsStartup> {
    const std::string arch = windows_sdk_arch();
    auto from_env = [&arch](const char* dir_name, const char* version_name) {
      const char* dir_value = std::getenv(dir_name);
      if (dir_value == nullptr) {
        return std::optional<WindowsStartup>{};
      }
      const path::Path dir = as_path(dir_value);
      const char* version_value = std::getenv(version_name);
      if (version_value != nullptr) {
        std::string trimmed = version_value;
        while (!trimmed.empty() &&
               (trimmed.back() == '/' || trimmed.back() == '\\')) {
          trimmed.pop_back();
        }
        const path::Path candidate =
            dir.join("Lib").join(trimmed).join("um").join(arch).join(
                "kernel32.lib");
        if (io::is_file(as_string(candidate))) {
          return std::optional<WindowsStartup>{WindowsStartup{candidate}};
        }
      }
      return kit_kernel32(dir.join("Lib"), arch);
    };
    if (std::optional<WindowsStartup> found =
            from_env("WindowsSdkDir", "WindowsSDKVersion");
        found.has_value()) {
      return found;
    }
    if (std::optional<WindowsStartup> found =
            from_env("WindowsSdk10Dir", "WindowsSDKVersion");
        found.has_value()) {
      return found;
    }
    for (const char* root : {"C:/Program Files (x86)/Windows Kits/10/Lib",
                             "C:/Program Files/Windows Kits/10/Lib"}) {
      if (std::optional<WindowsStartup> found =
              kit_kernel32(as_path(root), arch);
          found.has_value()) {
        return found;
      }
    }
    return std::nullopt;
  }();
  return FOUND;
}
#endif  // BUILD_FLAG(IS_OS_WIN)

#if BUILD_FLAG(IS_OS_MAC)
// What a macOS link needs beyond the objects: the C runtime startup
// object and the SDK it and libSystem live in. lld is a linker and not
// a driver, so the SDK the compiler was built without is discovered
// from SDKROOT, then DEVELOPER_DIR, then the usual SDK locations,
// highest version first. A Mach-O link has no crti/crtn pair: Mach-O
// registers constructors in the binary itself, so the startup is the
// single crt1.o.
struct MacStartup {
  path::Path sysroot;
  path::Path crt1;
  std::string sdk_version;
};

std::string macos_arch() {
  const std::string arch = host_arch();
  return arch == "x86_64" ? "x86_64" : "arm64";
}

struct Sdk {
  path::Path root;
  std::string version;
};

// The version the SDK's directory name carries, or the arm64 floor
// when the name carries none (the MacOSX.sdk symlink, an SDKROOT that
// points straight at a versioned directory).
std::string sdk_version_of(const path::Path& sdk) {
  const std::string value = as_string(sdk);
  std::string_view name = value;
  while (name.size() > 1 && name.back() == '/') {
    name.remove_suffix(1);
  }
  const usize slash = name.find_last_of('/');
  if (slash != std::string_view::npos) {
    name = name.substr(slash + 1);
  }
  if (name.starts_with("MacOSX") && name.ends_with(".sdk") &&
      name.size() > 10) {
    return std::string(name.substr(6, name.size() - 10));
  }
  return "11.0";
}

std::optional<Sdk> newest_macos_sdk(const path::Path& sdks_root) {
  // The command-line tools and Xcode both keep a MacOSX.sdk symlink next
  // to the versioned directories; it names the same newest SDK the
  // versioned scan would find, but without a version in the name, so
  // the scan answers first and the symlink stands in when nothing is
  // versioned.
  std::optional<Sdk> best;
  for (const std::string& name : list_directory(sdks_root)) {
    if (!name.starts_with("MacOSX") || !name.ends_with(".sdk")) {
      continue;
    }
    const path::Path full = sdks_root.join(name);
    if (!io::is_dir(as_string(full))) {
      continue;
    }
    const std::string version = name.substr(6, name.size() - 6 - 4);
    if (version.empty()) {
      continue;
    }
    if (best.has_value() && !version_less(best->version, version)) {
      continue;
    }
    best = Sdk{.root = full, .version = version};
  }
  if (best.has_value()) {
    return best;
  }
  const path::Path unversioned = sdks_root.join("MacOSX.sdk");
  if (io::is_dir(as_string(unversioned))) {
    return Sdk{.root = unversioned, .version = sdk_version_of(unversioned)};
  }
  return std::nullopt;
}

const std::optional<MacStartup>& macos_startup_inputs() {
  static const std::optional<MacStartup> FOUND =
      []() -> std::optional<MacStartup> {
    std::vector<Sdk> sdks;
    if (const char* sdkroot = std::getenv("SDKROOT"); sdkroot != nullptr) {
      const path::Path root = as_path(sdkroot);
      sdks.push_back(Sdk{.root = root, .version = sdk_version_of(root)});
    }
    if (const char* developer_dir = std::getenv("DEVELOPER_DIR");
        developer_dir != nullptr) {
      // DEVELOPER_DIR names an Xcode Developer directory, whose SDKs
      // live under Platforms/MacOSX.platform/Developer/SDKs; a value
      // that names a root with an SDKs directory directly (the Command
      // Line Tools layout) is honored too.
      const path::Path root = as_path(developer_dir);
      const path::Path xcode =
          root.join("Platforms/MacOSX.platform").join("Developer").join("SDKs");
      const path::Path plain = root.join("SDKs");
      if (io::is_dir(as_string(xcode))) {
        if (std::optional<Sdk> sdk = newest_macos_sdk(xcode); sdk.has_value()) {
          sdks.push_back(std::move(*sdk));
        }
      } else if (std::optional<Sdk> sdk = newest_macos_sdk(plain);
                 sdk.has_value()) {
        sdks.push_back(std::move(*sdk));
      }
    }
    for (const char* root : {"/Library/Developer/CommandLineTools/SDKs",
                             "/Applications/Xcode.app/Contents/Developer/"
                             "Platforms/MacOSX.platform/Developer/SDKs"}) {
      if (std::optional<Sdk> sdk = newest_macos_sdk(as_path(root));
          sdk.has_value()) {
        sdks.push_back(std::move(*sdk));
      }
    }
    for (const Sdk& sdk : sdks) {
      const path::Path crt1 = sdk.root.join("usr").join("lib").join("crt1.o");
      const path::Path libsystem =
          sdk.root.join("usr").join("lib").join("libSystem.tbd");
      if (!io::is_file(as_string(crt1)) || !io::is_file(as_string(libsystem))) {
        continue;
      }
      return MacStartup{.sysroot = sdk.root,
                        .crt1 = std::move(crt1),
                        .sdk_version = sdk.version};
    }
    return std::nullopt;
  }();
  return FOUND;
}
#endif  // BUILD_FLAG(IS_OS_MAC)

}  // namespace

bool embedded_lld_ready(bool freestanding) {
#if BUILD_FLAG(IS_OS_WIN)
  // A Windows program references nothing but the kernel, freestanding or
  // not: the one input is the kernel import library.
  (void)freestanding;
  return windows_startup_inputs().has_value();
#elif BUILD_FLAG(IS_OS_MAC)
  // A macOS hosted program starts from the SDK's C runtime object and
  // links libSystem; the SDK is the one thing to find. A macOS
  // freestanding link is refused by the pipeline before it gets here.
  (void)freestanding;
  return macos_startup_inputs().has_value();
#else
  // The freestanding path names everything it links and asks nothing of
  // the host.
  return freestanding || startup_inputs().has_value();
#endif
}

base::Result<void, diag::Reported> link_with_embedded_lld(
    PipelineContext& ctx,
    const LinkOptions& link,
    std::span<const std::string> object_paths,
    const std::string& exe_path) {
  std::vector<std::string> args;
#if BUILD_FLAG(IS_OS_WIN)
  const std::optional<WindowsStartup>& found = windows_startup_inputs();
  if (!found.has_value()) {
    // The probe runs once per process, so only a caller that skipped
    // `embedded_lld_ready()` gets here.
    DCHECK(false);
    return base::make_err(diag::Reported{});
  }
  // The kernel is the only library: no startup objects, no C runtime.
  // The entry is the object's `_start`, exactly as a spawned MSVC-style
  // driver would be told.
  args.emplace_back("lld-link");
  args.emplace_back("/nodefaultlib");
  args.emplace_back("/entry:_start");
  args.emplace_back("/subsystem:console");
  args.reserve(4 + object_paths.size() + 1 + link.args.size());
  for (const std::string& object_path : object_paths) {
    args.push_back(object_path);
  }
  args.push_back(as_string(found->kernel32));
  for (std::string_view argument : link.args) {
    args.emplace_back(argument);
  }
#elif BUILD_FLAG(IS_OS_MAC)
  const std::optional<MacStartup>& found = macos_startup_inputs();
  if (!found.has_value()) {
    // The probe runs once per process, so only a caller that skipped
    // `embedded_lld_ready()` gets here.
    DCHECK(false);
    return base::make_err(diag::Reported{});
  }
  const MacStartup& startup = *found;
  args.emplace_back("ld64.lld");
  args.emplace_back("-arch");
  args.emplace_back(macos_arch());
  args.emplace_back("-syslibroot");
  args.emplace_back(as_string(startup.sysroot));
  // ld64 takes the platform, the deployment floor and the SDK in one
  // flag, and nothing of the version-min spellings feeds its platform
  // state; the arm64 floor of 11.0 keeps the load command usable on
  // every SDK a runner names, and the SDK version is the one the load
  // command records.
  args.emplace_back("-platform_version");
  args.emplace_back("macos");
  args.emplace_back("11.0");
  args.emplace_back(startup.sdk_version);
  args.push_back(as_string(startup.crt1));
  for (const std::string& object_path : object_paths) {
    args.push_back(object_path);
  }
  for (std::string_view argument : link.args) {
    args.emplace_back(argument);
  }
  args.emplace_back("-lSystem");
#else
  args.emplace_back("ld.lld");
  // A plain crt1.o is the non-PIE startup, and lld does not default the
  // loader the way the system linker does, so every link here is
  // non-PIE.
  args.emplace_back("-no-pie");
  if (link.freestanding) {
    // The object defines `_start` and the program owns every symbol it
    // calls: no startup objects, no loader, no C library.
    args.emplace_back("-e");
    args.emplace_back("_start");
    for (const std::string& object_path : object_paths) {
      args.push_back(object_path);
    }
    for (std::string_view argument : link.args) {
      args.emplace_back(argument);
    }
  } else {
    const std::optional<Startup>& found = startup_inputs();
    if (!found.has_value()) {
      // The probe runs once per process, so only a caller that skipped
      // `embedded_lld_ready()` gets here.
      DCHECK(false);
      return base::make_err(diag::Reported{});
    }
    const Startup& startup = *found;
    args.reserve(args.size() + 6 + startup.lib_dirs.size() +
                 object_paths.size() + link.args.size());
    args.emplace_back("--dynamic-linker=" + as_string(startup.dynamic_linker));
    args.push_back(as_string(startup.crt1));
    args.push_back(as_string(startup.crti));
    for (const path::Path& dir : startup.lib_dirs) {
      args.push_back("-L" + as_string(dir));
    }
    for (const std::string& object_path : object_paths) {
      args.push_back(object_path);
    }
    for (std::string_view argument : link.args) {
      args.emplace_back(argument);
    }
    args.emplace_back("-lc");
    args.push_back(as_string(startup.crtn));
  }
#endif
#if BUILD_FLAG(IS_OS_WIN)
  args.emplace_back("/OUT:" + exe_path);
#else
  args.emplace_back("-o");
  args.push_back(exe_path);
#endif

  std::vector<const char*> argv;
  argv.reserve(args.size());
  for (const std::string& argument : args) {
    argv.push_back(argument.c_str());
  }
  // The driver's own output goes to the process streams, exactly as the
  // spawned driver's would. The call is its own row in a time trace, so
  // a link's cost reads apart from the object write before it.
  const lld::Result linked = [&] {
    PROFILE_SCOPE_WITH_CATEGORY_AND_PROFILER(ctx.profiler, "lld-link",
                                             "backend");
#if BUILD_FLAG(IS_OS_WIN)
    return lld::lldMain(llvm::ArrayRef<const char*>(argv.data(), argv.size()),
                        llvm::outs(), llvm::errs(),
                        {{lld::WinLink, &lld::coff::link}});
#elif BUILD_FLAG(IS_OS_MAC)
    return lld::lldMain(llvm::ArrayRef<const char*>(argv.data(), argv.size()),
                        llvm::outs(), llvm::errs(),
                        {{lld::Darwin, &lld::macho::link}});
#else
    return lld::lldMain(llvm::ArrayRef<const char*>(argv.data(), argv.size()),
                        llvm::outs(), llvm::errs(),
                        {{lld::Gnu, &lld::elf::link}});
#endif
  }();
  if (linked.retCode != 0) {
    const u32 index = ctx.bag.emit<i18n::Key::PipelineLinkFailed>(
        DiagCode::LinkError, exe_path);
    (void)index;
    return base::make_err(diag::Reported{});
  }
  return base::make_ok();
}

#else  // !defined(ALCY_EMBEDDED_LLD)

bool embedded_lld_ready(bool) {
  return false;
}

base::Result<void, diag::Reported> link_with_embedded_lld(
    PipelineContext&,
    const LinkOptions&,
    std::span<const std::string>,
    const std::string&) {
  // The caller asks embedded_lld_ready() first, so only a mistake in the
  // caller reaches this.
  DCHECK(false);
  return base::make_err(diag::Reported{});
}

#endif

}  // namespace pipeline
