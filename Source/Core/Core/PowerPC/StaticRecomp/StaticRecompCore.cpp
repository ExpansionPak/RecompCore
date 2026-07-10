// RecompCore: StaticRecomp CPU core.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Common/Config/Config.h"
#include "Common/DynamicLibrary.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/PowerPC/StaticRecomp/lockstep/StaticRecompLockstep.h"
#include "Core/System.h"

#ifdef _M_X86_64
#include "Core/PowerPC/Jit64/Jit.h"
#endif
#ifdef _M_ARM_64
#include "Core/PowerPC/JitArm64/Jit.h"
#endif


StaticRecompCore* g_static_recomp_core = nullptr;

bool StaticRecompCore::IsModuleActive() const
{
  return m_module_active;
}

StaticRecompCore::StaticRecompCore(Core::System& system) : JitBase(system)
{
}

StaticRecompCore::~StaticRecompCore() = default;

void StaticRecompCore::Init()
{
  g_static_recomp_core = this;
  RefreshConfig();
  jo.enableBlocklink = false;
  jo.fastmem = false;
  jo.fastmem_arena = false;

  m_block_cache.Init();

  m_guest = CPUState{};
  m_guest.external_read = HookExternalRead;
  m_guest.external_write = HookExternalWrite;
  m_guest.external_read32 = HookExternalRead32;
  m_guest.external_write32 = HookExternalWrite32;
  m_guest.external_pointer = HookExternalPointer;
  m_guest.instruction_fallback = HookInstructionFallback;
  m_guest.host_call = nullptr;
  m_guest.external_user_data = this;

  std::fprintf(stderr, "[staticrecomp] core init (chassis built " __DATE__ " " __TIME__ ")\n");

  LoadModule();
  m_idle_pc = Config::Get(Config::MAIN_STATICRECOMP_IDLE_PC);
  m_lockstep_verifier = std::make_unique<StaticRecompLockstep::StaticRecompLockstepVerifier>(*this);
  m_lockstep_verifier->Init();

#ifdef _M_ARM_64
  m_fallback_jit = std::make_unique<JitArm64>(m_system);
#elif defined(_M_X86_64)
  m_fallback_jit = std::make_unique<Jit64>(m_system);
#endif
  if (m_fallback_jit)
    m_fallback_jit->Init();
}

void StaticRecompCore::Shutdown()
{
  g_static_recomp_core = nullptr;
  std::fprintf(stderr,
               "[staticrecomp] shutdown: native=%llu fallback=%llu native_exc=%llu hook_fb=%llu "
               "smc_failed=%u verifications=%llu reverify_events=%llu\n",
               (unsigned long long)m_native_dispatches, (unsigned long long)m_fallback_steps,
               (unsigned long long)m_native_exceptions,
               (unsigned long long)m_hook_fallback_instructions, m_failed_chunks,
               (unsigned long long)m_verifications, (unsigned long long)m_reverify_events);
  NOTICE_LOG_FMT(POWERPC,
                 "StaticRecomp: shutdown. native_dispatches={} fallback_steps={} "
                 "native_exceptions={} hook_fallback_instructions={} smc_failed_chunks={} "
                 "verifications={} reverify_events={}",
                 m_native_dispatches, m_fallback_steps, m_native_exceptions,
                 m_hook_fallback_instructions, m_failed_chunks, m_verifications,
                 m_reverify_events);
  m_lockstep_verifier.reset();
  m_block_cache.Shutdown();
  m_module = nullptr;
  if (m_library.IsOpen())
    m_library.Close();

  if (m_fallback_jit)
  {
    m_fallback_jit->Shutdown();
    m_fallback_jit.reset();
  }
}

void StaticRecompCore::LoadModule()
{
  if (!Config::Get(Config::MAIN_STATICRECOMP_MODULE))
  {
    std::fprintf(stderr,
                 "[staticrecomp] native module disabled by config "
                 "(Main.Core.StaticRecompModule=false); interpreter-only (invariant path).\n");
    NOTICE_LOG_FMT(POWERPC, "StaticRecomp: module disabled by config; interpreter-only.");
    return;
  }

  const std::string game_id = SConfig::GetInstance().GetGameID();
  std::string path;
  if (const char* env = std::getenv("STATICRECOMP_MODULE"))
  {
    path = env;
  }
  else if (!game_id.empty())
  {
    path = Common::DynamicLibrary::GetUnprefixedFilename(
        (File::GetUserPath(D_USER_IDX) + "StaticRecompModules/g" + game_id + "_recomp").c_str());
  }

  if (path.empty() || !File::Exists(path))
  {
    NOTICE_LOG_FMT(POWERPC, "StaticRecomp: no module for game '{}' (looked at '{}'); "
                            "running interpreter-only.",
                   game_id, path);
    return;
  }

  if (!m_library.Open(path.c_str()))
  {
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: failed to dlopen module '{}'; interpreter-only.", path);
    return;
  }

  const auto get_module = reinterpret_cast<StaticRecompGetModuleFn>(
      m_library.GetSymbolAddress(STATICRECOMP_GET_MODULE_SYMBOL));
  const StaticRecompModuleDesc* desc = get_module ? get_module() : nullptr;

  const auto reject = [&](const std::string& why) {
    ERROR_LOG_FMT(POWERPC, "StaticRecomp: rejecting module '{}': {}. Interpreter-only.", path, why);
    m_module = nullptr;
    m_library.Close();
  };

  if (!desc)
    return reject("missing or null " STATICRECOMP_GET_MODULE_SYMBOL);
  if (desc->abi_version != STATICRECOMP_ABI_VERSION)
    return reject(fmt::format("abi_version {} != {}", desc->abi_version, STATICRECOMP_ABI_VERSION));
  if (desc->cpu_abi_version != GXRUNTIME_CPU_ABI_VERSION)
    return reject(fmt::format("cpu_abi_version {} != {}", desc->cpu_abi_version,
                              GXRUNTIME_CPU_ABI_VERSION));
  if (desc->cpu_state_size != sizeof(CPUState))
    return reject(fmt::format("cpu_state_size {} != sizeof(CPUState) {}", desc->cpu_state_size,
                              sizeof(CPUState)));
  if (!desc->dispatch || !desc->code_ranges || desc->num_code_ranges == 0)
    return reject("no dispatch entry or empty code ranges");
  if (!desc->chunk_ranges || desc->num_chunk_ranges == 0 || !desc->chunk_hashes)
    return reject("no chunk ranges/hashes (required for the SMC guard)");
  if (!std::getenv("STATICRECOMP_MODULE") && !game_id.empty() && game_id != desc->game_id)
    return reject(fmt::format("module game_id '{}' != running game '{}'", desc->game_id, game_id));

  m_module = desc;
  m_module_active = (desc != nullptr);
  m_chunk_state.assign(desc->num_chunk_ranges, CHUNK_UNVERIFIED);
  m_failed_chunks = 0;
  m_lookup_ram_size = 0;
  m_lookup_exram_size = 0;
  m_chunk_lookup_table.clear();

  std::fprintf(stderr,
               "[staticrecomp] module loaded: %s entry=0x%08X (chassis built " __DATE__
               " " __TIME__ ")\n",
               path.c_str(), desc->entry_point);
  NOTICE_LOG_FMT(POWERPC,
                 "StaticRecomp: loaded module '{}' (game_id={} entry=0x{:08X} "
                 "code_ranges={} smc_ranges={})",
                 path, desc->game_id, desc->entry_point, desc->num_code_ranges,
                 desc->num_smc_ranges);
}

void StaticRecompCore::ClearCache()
{
  if (m_fallback_jit)
    m_fallback_jit->ClearCache();

  if (!m_module)
    return;
  std::fill(m_chunk_state.begin(), m_chunk_state.end(), u8{CHUNK_UNVERIFIED});
  m_failed_chunks = 0;
  ++m_reverify_events;
}
