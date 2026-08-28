// RecompCore: StaticRecomp CPU core - Main execution loop.
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "Core/AnimaniacsSettings.h"
#include "Core/System.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompLockstep.h"
#include "Core/CoreTiming.h"
#include "Core/HW/CPU.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/ConfigManager.h"
#include "Core/HW/SystemTimers.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>

namespace
{
constexpr u32 SYNC_EXCEPTION_MASK = ~static_cast<u32>(
    EXCEPTION_EXTERNAL_INT | EXCEPTION_DECREMENTER | EXCEPTION_PERFORMANCE_MONITOR);
}

void StaticRecompCore::Run()
{
  auto& core_timing = m_system.GetCoreTiming();
  auto& power_pc = m_system.GetPowerPC();
  auto& ppc = power_pc.GetPPCState();
  auto& interpreter = m_system.GetInterpreter();
  auto& memory = m_system.GetMemory();
  const CPU::State* state_ptr = m_system.GetCPU().GetStatePtr();

  m_guest.ram = memory.GetRAM();
  m_guest.ram_size = memory.GetRamSizeReal();
  m_guest.exram = memory.GetEXRAM();
  m_guest.exram_size = memory.GetExRamSizeReal();
  m_l1_cache = memory.GetL1Cache();
  m_l1_cache_size = memory.GetL1CacheSize();


  // GANE7U enhancements: 60 FPS + 16:9
  //
  // Gecko equivalents:
  //
  // $60 FPS
  // 0023EBCB 00000001
  //
  // $16:9 Widescreen
  // 041C15E4 41800000
  // 041C15E8 41100000
  //
  // Reapplied from the main CPU loop because these are normally
  // persistent Gecko memory writes.
  const auto apply_gane_enhancements = [&]()
  {
    constexpr u32 mem1_base = 0x80000000u;

    const auto write8 = [&](u32 address, u8 value)
    {
      if (address < mem1_base)
        return;

      const u32 offset = address - mem1_base;
      if (offset >= m_guest.ram_size)
        return;

      m_guest.ram[offset] = value;
    };

    const auto write32_be = [&](u32 address, u32 value)
    {
      if (address < mem1_base)
        return;

      const u32 offset = address - mem1_base;
      if (offset > m_guest.ram_size || m_guest.ram_size - offset < 4)
        return;

      m_guest.ram[offset + 0] = static_cast<u8>(value >> 24);
      m_guest.ram[offset + 1] = static_cast<u8>(value >> 16);
      m_guest.ram[offset + 2] = static_cast<u8>(value >> 8);
      m_guest.ram[offset + 3] = static_cast<u8>(value);
    };

    // 60+ FPS mode: the game waits this many VI retraces per rendered frame.
    // Retail gameplay normally uses 2 (~30 FPS); the known 60 FPS code forces 1.
    // Values below 1 do not exist, so >60 FPS is achieved by increasing the VI
    // retrace rate while keeping this divisor at 1.
    write32_be(0x8023EBC8u, 0x00000001u);

    // Live widescreen ratio. These are the same GANE7U guest globals used by
    // the known Gecko 16:9 patch, but the pair now follows the PC menu.
    const auto [aspect_width, aspect_height] = AnimaniacsPC::GuestAspectPair();
    write32_be(0x801C15E4u, std::bit_cast<u32>(aspect_width));
    write32_be(0x801C15E8u, std::bit_cast<u32>(aspect_height));

    /*
     * GANE7U native horizontal FOV, recovered from the retail main.dol.
     *
     * 0x801C15DC = 0x3F1C61AA = 0.610865235 rad = 35 degrees
     *                ^ half of the native ~70 degree horizontal FOV
     *
     * func 0x8005D754 loads that value, calls the game's tanf wrapper at
     * 0x8018583C, then computes:
     *
     *   x_scale = 1 / tan(hFOV / 2)
     *   y_scale = x_scale * aspect_width / aspect_height
     *
     * and stores the live projection coefficients at:
     *
     *   0x8026B978 = x_scale
     *   0x8026B97C = y_scale
     *
     * Updating all three values makes Ctrl+F10 FOV changes live immediately,
     * without touching Dolphin's generic perspective matrices (which also
     * drive shadow/reflection/auxiliary passes).
     */
    constexpr float pi = 3.14159265358979323846f;
    constexpr float native_hfov = 70.0f;

    const float requested_hfov = AnimaniacsPC::FovDegrees();
    const float hfov =
        requested_hfov > 0.0f ? std::clamp(requested_hfov, 45.0f, 120.0f) : native_hfov;
    const float half_fov_radians = hfov * pi / 360.0f;

    if (std::isfinite(half_fov_radians) && half_fov_radians > 0.0f)
    {
      const float tangent = std::tan(half_fov_radians);
      const float aspect =
          (aspect_height != 0.0f) ? (aspect_width / aspect_height) : (4.0f / 3.0f);

      if (std::isfinite(tangent) && tangent > 0.00001f &&
          std::isfinite(aspect) && aspect > 0.0f)
      {
        const float x_scale = 1.0f / tangent;
        const float y_scale = x_scale * aspect;

        // Keep the game's source constant synchronized for any code path that
        // rebuilds the projection later.
        write32_be(0x801C15DCu, std::bit_cast<u32>(half_fov_radians));

        // And update the already-derived live coefficients so the slider takes
        // effect immediately, even if func_8005D754 is not called this frame.
        write32_be(0x8026B978u, std::bit_cast<u32>(x_scale));
        write32_be(0x8026B97Cu, std::bit_cast<u32>(y_scale));
      }
    }

    /*
     * ANIM_PC_V10_TIMING_ASPECT
     *
     * 3D:
     * GANE7U stores half of its horizontal FOV at 0x801C15DC.  The retail
     * value is 35 degrees (70 degree hFOV) at 4:3.  With no manual FOV override
     * selected, derive Hor+ hFOV from the chosen aspect while preserving the
     * original vertical FOV.  16:9 lands at ~86.07 degrees, matching the known
     * PS2 widescreen correction.
     *
     * Timing:
     * The retail DOL contains exactly six aligned 1/60 second constants used by
     * frame-driven game/UI systems.  A VI rate above 60 makes those call sites
     * execute more often.  Scale all six to 1/target so real-time gameplay,
     * animation and UI rates stay at their 60 Hz behavior while presentation
     * can run at 90/120/144/165/240 Hz.  CPU, timebase, DSP and AI clocks are
     * deliberately untouched.
     */
    {
      constexpr float pi = 3.14159265358979323846f;
      constexpr float native_aspect = 4.0f / 3.0f;
      constexpr float native_half_hfov_rad = 35.0f * pi / 180.0f;

      const float target_aspect =
          (aspect_height != 0.0f) ? (aspect_width / aspect_height) : native_aspect;
      const float requested_hfov = AnimaniacsPC::FovDegrees();

      float hfov = 70.0f;
      if (requested_hfov > 0.0f)
      {
        hfov = std::clamp(requested_hfov, 45.0f, 130.0f);
      }
      else if (std::isfinite(target_aspect) && target_aspect > 0.0f)
      {
        const float half_hfov =
            std::atan(std::tan(native_half_hfov_rad) * (target_aspect / native_aspect));
        hfov = std::clamp(2.0f * half_hfov * 180.0f / pi, 45.0f, 130.0f);
      }

      const float half_fov_radians = hfov * pi / 360.0f;
      const float tangent = std::tan(half_fov_radians);
      if (std::isfinite(tangent) && tangent > 0.00001f &&
          std::isfinite(target_aspect) && target_aspect > 0.0f)
      {
        const float x_scale = 1.0f / tangent;
        const float y_scale = x_scale * target_aspect;
        write32_be(0x801C15DCu, std::bit_cast<u32>(half_fov_radians));
        write32_be(0x8026B978u, std::bit_cast<u32>(x_scale));
        write32_be(0x8026B97Cu, std::bit_cast<u32>(y_scale));
      }

      const int fps_target = std::clamp(AnimaniacsPC::FpsTarget(), 60, 360);
      const float frame_dt = 1.0f / static_cast<float>(fps_target);
      constexpr u32 FRAME_DT_GLOBALS[] = {
          0x801C383Cu,
          0x801C3FF0u,
          0x801C6F74u,
          0x801D0010u,
          0x801D005Cu,
          0x801D049Cu,
      };
      const u32 frame_dt_bits = std::bit_cast<u32>(frame_dt);
      for (const u32 address : FRAME_DT_GLOBALS)
        write32_be(address, frame_dt_bits);
    }

  };

  apply_gane_enhancements();
  InitLookupTable(m_guest.ram_size, m_guest.exram_size);

  const std::string initial_game_id = SConfig::GetInstance().GetGameID();
  m_module_active = m_module && (initial_game_id.empty() || initial_game_id == m_module->game_id);

  if (!m_module_active && m_fallback_jit && !m_guest.host_call)
  {
    m_fallback_jit->Run();
    return;
  }

  int anim_last_fps_target = -1;

  while (*state_ptr == CPU::State::Running)
  {
    // GANE7U >60 FPS: the guest frame divisor bottoms out at 1, so increase
    // the VI retrace cadence instead. Core/DSP/audio clocks remain untouched.
    const int anim_fps_target = AnimaniacsPC::FpsTarget();
    if (anim_fps_target != anim_last_fps_target)
    {
      constexpr double NTSC_VPS = 59.94005994005994;

      if (anim_fps_target > 60)
      {
        const float vi_factor =
            static_cast<float>(static_cast<double>(anim_fps_target) / NTSC_VPS);
        Config::SetCurrent(Config::MAIN_VI_OVERCLOCK, vi_factor);
        Config::SetCurrent(Config::MAIN_VI_OVERCLOCK_ENABLE, true);
        Config::SetCurrent(Config::MAIN_PRECISION_FRAME_TIMING, true);
        std::fprintf(stderr,
                     "[ANIM-FPS] target=%d VI factor=%.6f (CPU/DSP/audio clock unchanged)\n",
                     anim_fps_target, vi_factor);
      }
      else
      {
        Config::SetCurrent(Config::MAIN_VI_OVERCLOCK_ENABLE, false);
        Config::SetCurrent(Config::MAIN_VI_OVERCLOCK, 1.0f);
        std::fprintf(stderr, "[ANIM-FPS] 60 FPS native VI cadence\n");
      }

      anim_last_fps_target = anim_fps_target;
    }

    apply_gane_enhancements();
    core_timing.Advance();
    const std::string current_game_id = SConfig::GetInstance().GetGameID();
    m_module_active = m_module && (current_game_id.empty() || current_game_id == m_module->game_id);

    do
    {
      // MSR.FP needs no gate here: generated FPU instructions raise the
      // FP-unavailable exception themselves (ppc_fp_available).
      if (m_module_active && DispatchableAt(ppc.pc) &&
          !(m_guest.host_call && IsHostCallAddress(ppc.pc)))
      {
        SyncIn();
        ++m_bursts;
        do
        {
          const bool do_ls = m_lockstep_verifier->ShouldCheck(m_guest.pc);
          if (do_ls)
          {
            m_lockstep_verifier->Prepare(m_guest);
          }

          const u32 runtime_dispatch_address = m_guest.pc;
          u32 linked_dispatch_address = runtime_dispatch_address;

          // HPCOS DOL fast path: DOL runtime and linked addresses are identical.
          if (!m_active_rel_sections.empty())
            ResolveNativeAddress(runtime_dispatch_address, &linked_dispatch_address, nullptr);

          m_guest.pc = linked_dispatch_address;

          /*
           * One public native dispatch may execute an in-chunk cycle, but it
           * must not run past either the configured quantum or Dolphin's
           * current CoreTiming deadline. The second counter is a termination
           * backstop for cycles made entirely from zero-cycle helper/data
           * blocks. Both fields live in CPUState so helper/MMIO callbacks
           * cannot accidentally reset the budget.
           */
          const u64 remaining_slice =
              ppc.downcount > 0 ? static_cast<u64>(ppc.downcount) : 1u;
          m_guest.native_cycle_budget = static_cast<s64>(
              std::min<u64>(m_native_cycle_quantum, remaining_slice));
          m_guest.native_guard_budget = 4096;

          u32 dispatched_blocks = 0;

          /*
           * ABI v4 native burst:
           *
           * Keep lockstep and REL execution on the old one-segment path.
           * For the normal DOL gameplay path, execute multiple verified chunks
           * inside the native module before returning to the C++ chassis.
           */
          if (m_native_burst_enabled && !do_ls &&
              m_module->dispatch_burst &&
              m_active_rel_sections.empty() &&
              !m_native_chain_state.empty())
          {
            /*
             * Never execute past Dolphin's current CoreTiming slice.
             *
             * The old dispatcher returned to this loop after every native
             * segment and stopped chaining as soon as ppc.downcount <= 0.
             * Give the module exactly that remaining budget so native chaining
             * cannot run through a pending CoreTiming event.
             */
            const u64 burst_cycle_budget =
                ppc.downcount > 0 ? static_cast<u64>(ppc.downcount) : 1u;

            dispatched_blocks = m_module->dispatch_burst(
                &m_guest,
                linked_dispatch_address,
                m_native_chain_state.data(),
                static_cast<u32>(m_native_chain_state.size()),
                burst_cycle_budget,
                m_burst_tb_base,
                m_burst_tb_cycles,
                static_cast<u32>(SystemTimers::TIMER_RATIO));
          }

          // Safety fallback for non-chainable/legacy paths.
          if (dispatched_blocks == 0)
          {
            m_module->dispatch(&m_guest, linked_dispatch_address);
            dispatched_blocks = 1;
          }

          if (!m_active_rel_sections.empty())
            m_guest.pc = TranslateRelAddress(m_guest.pc);

          m_native_dispatches += dispatched_blocks;

          if (do_ls)
          {
            m_lockstep_verifier->Verify(m_guest);
          }

          // Flush the module's per-block cycle charges into Dolphin's
          // downcount. A dispatch that charged nothing (PC-switch default,
          // pure embedded data) still costs 1 so the burst always makes
          // downcount progress; this per-dispatch flush is also the
          // dispatcher back-edge timing check — CoreTiming regains control
          // with at least CachedInterpreter's per-block frequency, so
          // external-interrupt latency matches stock.
          const s64 charge = -m_guest.downcount;
          m_guest.downcount = 0;
          ppc.downcount -= static_cast<int>(charge > 0 ? charge : 1);
          m_charged_cycles += static_cast<u64>(charge > 0 ? charge : 1);
          m_burst_tb_cycles += static_cast<u64>(charge > 0 ? charge : 1);
          m_guest.timebase = m_burst_tb_base + m_burst_tb_cycles / SystemTimers::TIMER_RATIO;

          // Idle loop skipping for configured target loops (e.g. Wii Menu OSIdleThread)
          if (m_guest.pc == m_idle_pc && m_idle_pc != 0)
          {
            m_system.GetCoreTiming().Idle();
          }

          // ctx->timebase is refreshed at burst start (SyncIn), and here we
          // incrementally advance it by the exact block cycle charges to
          // prevent guest busy-wait loops from spinning on a stale timebase.
          if (m_guest.exception)
          {
            // DolRecomp's runtime already redirected pc/msr/srr to the guest
            // exception vector; the flag only signals that it happened.
            m_guest.exception = 0;
            m_guest.program_exception = 0;
            ++m_native_exceptions;
          }
          if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
            break;  // Hook-raised synchronous exception: deliver via Dolphin below.
        } while (m_module_active && FastDispatchableAt(m_guest.pc) &&
                 !(m_guest.host_call && IsHostCallAddress(m_guest.pc)) && ppc.downcount > 0 &&
                 *state_ptr == CPU::State::Running);
        SyncOut();
        if ((ppc.Exceptions & SYNC_EXCEPTION_MASK) != 0)
          power_pc.CheckExceptions();
      }
      else
      {
        if (m_guest.host_call && IsHostCallAddress(ppc.pc))
        {
          SyncIn();
          bool handled = m_guest.host_call(&m_guest, m_guest.pc);
          if (!handled && m_guest.pc < m_guest.ram_size)
            handled = m_guest.host_call(&m_guest, m_guest.pc | 0x80000000u);
          if (m_fallback_jit && IsHostCallAddress(m_guest.lr))
            m_fallback_jit->GetBlockCache()->InvalidateICache(m_guest.lr, 4, true);
          if (handled)
          {
            const s64 charge = -m_guest.downcount;
            m_guest.downcount = 0;
            ppc.downcount -= static_cast<int>(charge > 0 ? charge : 1);
            m_burst_tb_cycles += static_cast<u64>(charge > 0 ? charge : 1);
            m_guest.timebase = m_burst_tb_base + m_burst_tb_cycles / SystemTimers::TIMER_RATIO;
            SyncOut();
            continue;
          }
          SyncOut();
          if (m_fallback_jit)
          {
            m_host_call_passthrough_pc = ppc.pc;
            m_host_call_passthrough = true;
          }
        }
        // SingleStepInner delivers synchronous exceptions itself; external
        // interrupts are delivered at slice start, as in Interpreter::Run.
        // A failed verification retires module code specifically to Dolphin's
        // interpreter. Do not let the ordinary fallback JIT hide SMC execution
        // from fallback telemetry. Non-module code retains the configured JIT
        // fallback policy.
        const bool smc_failed_module_pc = m_module_active && IsFailedModuleAddress(ppc.pc);
        const bool forced_fallback_pc = m_module_active && IsForcedFallbackAddress(ppc.pc);

        // A real SMC/hash mismatch must stay on Dolphin's interpreter so every
        // instruction fetch observes the modified guest code exactly.
        if (smc_failed_module_pc)
        {
          ppc.downcount -= interpreter.SingleStepInner();
          ++m_fallback_steps;
          ++m_smc_interpreter_steps;
        }
        // Forced compatibility ranges are static DOL code, not failed SMC.
        // Run them through Dolphin's fallback JIT instead of SingleStepInner.
        // The fallback JIT is configured with SetStaticRecompFallback(true):
        // its dispatcher calls StaticRecompShouldYieldAt() before each block,
        // so it returns here as soon as execution reaches native recomp code or
        // a host-call address. This preserves the compatibility range while
        // removing instruction fetch/decode/opinfo overhead from every step.
        else if (forced_fallback_pc && m_fallback_jit)
        {
          m_fallback_jit->Run();
        }
        else if (forced_fallback_pc)
        {
          // Non-x86/non-arm builds may not provide a fallback JIT.
          ppc.downcount -= interpreter.SingleStepInner();
          ++m_fallback_steps;
        }
        else if (m_fallback_jit)
        {
          m_fallback_jit->Run();
        }
        else
        {
          do
          {
            ppc.downcount -= interpreter.SingleStepInner();
            ++m_fallback_steps;
          } while (!(m_module_active && DispatchableAt(ppc.pc)) &&
                   !IsHostCallAddress(ppc.pc) && ppc.downcount > 0 &&
                   *state_ptr == CPU::State::Running);
        }
      }
    } while (ppc.downcount > 0 && *state_ptr == CPU::State::Running);
  }
}

void StaticRecompCore::SingleStep()
{
  // Debugger stepping runs through the interpreter; state outside Run() lives
  // in PowerPCState, so no sync is needed.
  auto& system = m_system;
  system.GetCoreTiming().Advance();
  system.GetPPCState().downcount -= system.GetInterpreter().SingleStepInner();
}
