/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/logging.h>
#include <map>
#include <mutex>

#include <rex/cvar.h>
#include <rex/thread.h>

#include "../xlive_net.h"

REXCVAR_DECLARE(uint32_t, net_search_timeout_ms);

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XgiApp::XgiApp(KernelState* kernel_state) : App(kernel_state, 0xFB) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  {
    static std::mutex trace_mutex;
    static std::map<uint32_t, int> trace_counts;
    std::lock_guard<std::mutex> trace_lock(trace_mutex);
    if (++trace_counts[message] <= 500) {
      live::NetLog("XgiApp message {:08X} (buf={:08X} len={})", message, buffer_ptr, buffer_length);
    }
  }
  switch (message) {
    case 0x000B0006: {
      // XGIUserSetContextEx: {user_index, unk, xuid, context_id, value}
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t context_value = memory::load_and_swap<uint32_t>(buffer + 20);
      REXKRNL_DEBUG("XGIUserSetContextEx({:08X}, {:08X}, {:08X})", user_index, context_id,
                    context_value);
      live::SetUserContext(user_index, context_id, context_value);
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t property_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t value_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t value_ptr = memory::load_and_swap<uint32_t>(buffer + 24);
      REXKRNL_DEBUG("XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})", user_index, property_id,
                    value_size, value_ptr);
      if (value_ptr) {
        live::SetUserProperty(user_index, property_id,
                              memory_->TranslateVirtual<uint8_t*>(value_ptr), value_size);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0008: {
      // Raw dump so we can confirm the actual buffer layout the game sends.
      uint32_t raw0 = buffer_length >= 4 ? memory::load_and_swap<uint32_t>(buffer + 0) : 0;
      uint32_t raw4 = buffer_length >= 8 ? memory::load_and_swap<uint32_t>(buffer + 4) : 0;
      REXKRNL_INFO("XGIUserWriteAchievements called: buf_len={} raw[0]={:08X} raw[4]={:08X}",
                   buffer_length, raw0, raw4);

      assert_true(!buffer_length || buffer_length == 8);
      uint32_t achievement_count = raw0;
      uint32_t achievements_ptr = raw4;

      // Empirically confirmed from log: each entry is {u32 padding/user_index, u32 id, ...}.
      // The achievement ID sits at offset 4, not 0. Stride 8 covers the observed fields.
      constexpr uint32_t kEntryIdOffset = 4;
      constexpr uint32_t kEntryStride = 8;
      constexpr uint32_t kMaxAchievements = 1000;

      if (achievements_ptr && achievement_count > 0) {
        if (achievement_count > kMaxAchievements) {
          REXKRNL_WARN("XGIUserWriteAchievements: count={} unreasonable, ignoring",
                       achievement_count);
          return X_E_FAIL;
        }
        uint32_t span_end = achievements_ptr + achievement_count * kEntryStride - 1;
        if (!memory_->LookupHeap(achievements_ptr) || !memory_->LookupHeap(span_end)) {
          REXKRNL_WARN("XGIUserWriteAchievements: ptr {:08X} OOB", achievements_ptr);
          return X_E_FAIL;
        }
        auto* base = memory_->TranslateVirtual(achievements_ptr);
        for (uint32_t i = 0; i < achievement_count; ++i) {
          uint32_t id = memory::load_and_swap<uint32_t>(base + i * kEntryStride + kEntryIdOffset);
          REXKRNL_INFO("XGIUserWriteAchievements: id={} ({})", id, i);
          kernel_state_->UnlockAchievement(id);
        }
      } else {
        REXKRNL_INFO("XGIUserWriteAchievements: skipped (count={} ptr={:08X})", achievement_count,
                     achievements_ptr);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      auto data = reinterpret_cast<const live::XGI_SESSION_CREATE*>(buffer);
      auto session = live::XSession::FromGuest(kernel_state_, data->obj_ptr);
      if (!session) {
        return X_E_INVALIDARG;
      }
      return session->Create(data);
    }
    case 0x000B0011: {
      auto data = reinterpret_cast<const live::XGI_SESSION_STATE*>(buffer);
      auto session = live::XSession::FromGuest(kernel_state_, data->obj_ptr);
      if (!session) {
        return X_E_INVALIDARG;
      }
      return session->Delete();
    }
    case 0x000B0012: {
      auto data = reinterpret_cast<const live::XGI_SESSION_MANAGE*>(buffer);
      auto session = live::XSession::FromGuest(kernel_state_, data->obj_ptr);
      if (!session) {
        return X_E_INVALIDARG;
      }
      return session->Join(data);
    }
    case 0x000B0013: {
      auto data = reinterpret_cast<const live::XGI_SESSION_MANAGE*>(buffer);
      auto session = live::XSession::FromGuest(kernel_state_, data->obj_ptr);
      if (!session) {
        return X_E_INVALIDARG;
      }
      return session->Leave(data);
    }
    case 0x000B0014: {
      auto data = reinterpret_cast<const live::XGI_SESSION_STATE*>(buffer);
      auto session = live::XSession::FromGuest(kernel_state_, data->obj_ptr);
      if (!session) {
        return X_E_INVALIDARG;
      }
      return session->Start();
    }
    case 0x000B0015: {
      auto data = reinterpret_cast<const live::XGI_SESSION_STATE*>(buffer);
      auto session = live::XSession::FromGuest(kernel_state_, data->obj_ptr);
      if (!session) {
        return X_E_INVALIDARG;
      }
      return session->End();
    }
    case 0x000B0016: {
      auto data = reinterpret_cast<const live::XGI_SESSION_SEARCH*>(buffer);
      live::NetLog("XSessionSearch(proc={}, user={}, max={})", data->proc_index.get(),
                   data->user_index.get(), data->num_results.get());
      auto sessions = live::SearchSessions(REXCVAR_GET(net_search_timeout_ms));
      return live::WriteSearchResults(kernel_state_, sessions, data->num_results,
                                      data->search_results_ptr, data->results_buffer_size);
    }
    case 0x000B0018: {
      auto data = reinterpret_cast<const live::XGI_SESSION_MODIFY*>(buffer);
      auto session = live::XSession::FromGuest(kernel_state_, data->obj_ptr);
      if (!session) {
        return X_E_INVALIDARG;
      }
      return session->Modify(data);
    }
    case 0x000B001C: {
      auto data = reinterpret_cast<const live::XGI_SESSION_SEARCH*>(buffer);
      live::NetLog("XSessionSearchEx(proc={}, user={}, max={})", data->proc_index.get(),
                   data->user_index.get(), data->num_results.get());
      auto sessions = live::SearchSessions(REXCVAR_GET(net_search_timeout_ms));
      return live::WriteSearchResults(kernel_state_, sessions, data->num_results,
                                      data->search_results_ptr, data->results_buffer_size);
    }
    case 0x000B001D: {
      auto data = reinterpret_cast<const live::XGI_SESSION_DETAILS*>(buffer);
      auto session = live::XSession::FromGuest(kernel_state_, data->obj_ptr);
      if (!session) {
        return X_E_INVALIDARG;
      }
      return session->GetDetails(data);
    }
    case 0x000B001E: {
      auto data = reinterpret_cast<const live::XGI_SESSION_MIGRATE*>(buffer);
      auto session = live::XSession::FromGuest(kernel_state_, data->obj_ptr);
      if (!session) {
        return X_E_INVALIDARG;
      }
      if (!data->session_info_ptr) {
        return X_E_FAIL;
      }
      return session->Migrate(data);
    }
    case 0x000B0019: {
      assert_true(!buffer_length || buffer_length == 8);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 4);

      REXKRNL_DEBUG("XSessionGetInvitationData - unimplemented({}, {:08X})", user_index,
                    session_info_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B001A: {
      auto data = reinterpret_cast<const live::XGI_SESSION_ARBITRATION*>(buffer);
      auto session = live::XSession::FromGuest(kernel_state_, data->obj_ptr);
      if (!session) {
        return X_E_INVALIDARG;
      }
      return session->RegisterArbitration(data);
    }
    case 0x000B001B: {
      auto data = reinterpret_cast<const live::XGI_SESSION_SEARCH_BYID*>(buffer);
      const live::XNKID* ids = &data->session_id;
      uint32_t count = 1;
      uint32_t results_ptr = data->search_results_ptr;
      uint32_t results_size = data->results_buffer_size;
      live::NetLog("XSessionSearchByID({:016X})", live::XnkidToU64(data->session_id));
      std::vector<live::AdvertisedSession> found;
      for (uint32_t i = 0; i < count; i++) {
        if (auto s = live::QuerySessionById(live::XnkidToU64(ids[i]), 1000)) {
          found.push_back(*s);
        }
      }
      return live::WriteSearchResults(kernel_state_, found, count, results_ptr, results_size);
    }
    case 0x000B001F: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t array_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuid_array_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionModifySkill({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr, array_count,
                    xuid_array_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0020: {
      assert_true(!buffer_length || buffer_length == 8);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t view_id = memory::load_and_swap<uint32_t>(buffer + 4);

      REXKRNL_DEBUG("XUserResetStatsView({:08X}, {})", user_index, view_id);

      return X_E_SUCCESS;
    }
    case 0x000B0021: {
      assert_true(!buffer_length || buffer_length == 28);

      uint32_t title_id = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t xuids_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t specs_count = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t specs_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t results_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG("XUserReadStats({}, {}, {:08X}, {}, {:08X}, {}, {:08X})", title_id, xuids_count,
                    xuids_ptr, specs_count, specs_ptr, results_size, results_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0025: {
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 4);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_DEBUG("XSessionWriteStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0026: {
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 4);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_DEBUG("XSessionFlushStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0036: {
      // Called after opening xbox live arcade and clicking on xbox live v5759
      // to 5787 and called after clicking xbox live in the game library from
      // v6683 to v6717
      // Does not get sent a buffer
      REXKRNL_DEBUG("XInvalidateGamerTileCache, unimplemented");
      return X_E_FAIL;
    }
    case 0x000B003D: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t AnId_buffer_size = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t AnId_buffer_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t block = memory::load_and_swap<uint32_t>(buffer + 12);

      REXKRNL_DEBUG("XUserGetANID({:08X}, {:08X}, {:08X}, {:08X})", user_index, AnId_buffer_size,
                    AnId_buffer_ptr, block);

      return X_E_SUCCESS;
    }
    case 0x000B0041: {
      // XGIUserGetContext
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      auto context = context_ptr ? memory_->TranslateVirtual(context_ptr) : nullptr;
      if (!context) {
        return X_E_INVALIDARG;
      }
      uint32_t context_id = memory::load_and_swap<uint32_t>(context + 0);
      memory::store_and_swap<uint32_t>(context + 4,
                                       live::GetUserContext(user_index, context_id, 0));
      return X_E_SUCCESS;
    }
    case 0x000B0060: {
      auto data = reinterpret_cast<const live::XGI_SESSION_SEARCH_BYIDS*>(buffer);
      auto ids = memory_->TranslateVirtual<const live::XNKID*>(data->session_ids_ptr);
      uint32_t count = std::min<uint32_t>(data->num_session_ids, 100);
      uint32_t results_ptr = data->search_results_ptr;
      uint32_t results_size = data->results_buffer_size;
      live::NetLog("XSessionSearchByIds({})", count);
      std::vector<live::AdvertisedSession> found;
      for (uint32_t i = 0; i < count; i++) {
        if (auto s = live::QuerySessionById(live::XnkidToU64(ids[i]), 1000)) {
          found.push_back(*s);
        }
      }
      return live::WriteSearchResults(kernel_state_, found, count, results_ptr, results_size);
    }
    case 0x000B0065: {
      auto data = reinterpret_cast<const live::XGI_SESSION_SEARCH_WEIGHTED*>(buffer);
      live::NetLog("XSessionSearchWeighted(proc={}, user={}, max={})", data->proc_index.get(),
                   data->user_index.get(), data->num_results.get());
      auto sessions = live::SearchSessions(REXCVAR_GET(net_search_timeout_ms));
      return live::WriteSearchResults(kernel_state_, sessions, data->num_results,
                                      data->search_results_ptr, data->results_buffer_size);
    }
    case 0x000B0071: {
      REXKRNL_DEBUG("XGI 0x000B0071, unimplemented");
      return X_E_SUCCESS;
    }
  }
  live::NetLog("Unimplemented XGI message {:08X}", message);
  REXKRNL_ERROR(
      "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
