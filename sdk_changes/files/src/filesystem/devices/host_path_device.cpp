/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/filesystem/devices/host_path_entry.h>

#include <algorithm>
#include <map>
#include <mutex>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/string/utf8.h>

namespace rex::filesystem {

HostPathDevice::HostPathDevice(const std::string_view mount_path,
                               const std::filesystem::path& host_path, bool read_only,
                               bool allow_share_delete)
    : Device(mount_path),
      name_("STFS"),
      host_path_(host_path),
      read_only_(read_only),
      allow_share_delete_(allow_share_delete) {}

HostPathDevice::~HostPathDevice() {
  SetHostPathOverlay(this, {});
}

// Files of an overlay folder (at the same relative path) stand in for a
// device's own: a translated copy of a package's text without touching the
// package. Kept here, not in the class: the port builds against the released
// SDK headers.
namespace {
std::mutex overlay_mutex;
std::map<const HostPathDevice*, std::filesystem::path> overlays;
}  // namespace

void SetHostPathOverlay(const HostPathDevice* device, const std::filesystem::path& dir) {
  std::lock_guard lock(overlay_mutex);
  if (dir.empty()) {
    overlays.erase(device);
  } else {
    overlays[device] = dir;
  }
}

static std::filesystem::path OverlayOf(const HostPathDevice* device) {
  std::lock_guard lock(overlay_mutex);
  auto it = overlays.find(device);
  return it == overlays.end() ? std::filesystem::path() : it->second;
}

bool HostPathDevice::Initialize() {
  if (!std::filesystem::exists(host_path_)) {
    if (!read_only_) {
      // Create the path.
      std::filesystem::create_directories(host_path_);
    } else {
      REXFS_ERROR("Host path does not exist");
      return false;
    }
  }

  auto root_entry = new HostPathEntry(this, nullptr, "", host_path_);
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);
  PopulateEntry(root_entry);

  return true;
}

void HostPathDevice::Dump(string::StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* HostPathDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  auto* resolved = root_entry_->ResolvePath(path);
  if (resolved) {
    return resolved;
  }

  // Fallback to a lazy case-insensitive host lookup when an entry is missing
  // from the in-memory tree (for example because casing differs on Linux).
  auto* current_entry = static_cast<HostPathEntry*>(root_entry_.get());
  for (const auto& part : rex::string::utf8_split_path(path)) {
    if (part.empty()) {
      continue;
    }

    auto* child = current_entry->GetChild(part);
    if (!child) {
      // Stat the exact name first: enumerating a directory of hundreds of
      // archives costs milliseconds a walk. Only casing mismatch reaches
      // the scan below.
      const auto exact_path = current_entry->host_path() / rex::to_path(part);
      rex::filesystem::FileInfo exact_info;
      if (rex::filesystem::GetInfo(exact_path, &exact_info)) {
        auto* exact_child = HostPathEntry::Create(this, current_entry, exact_path, exact_info);
        if (!exact_child) {
          return nullptr;
        }
        current_entry->children_.push_back(std::unique_ptr<Entry>(exact_child));
        current_entry = static_cast<HostPathEntry*>(exact_child);
        continue;
      }

      auto child_infos = rex::filesystem::ListFiles(current_entry->host_path());
      auto match = std::find_if(child_infos.begin(), child_infos.end(), [&](const auto& info) {
        return rex::string::utf8_equal_case(rex::path_to_utf8(info.name), part);
      });
      if (match == child_infos.end()) {
        return nullptr;
      }

      auto new_child = HostPathEntry::Create(this, current_entry,
                                             current_entry->host_path() / match->name, *match);
      if (!new_child) {
        return nullptr;
      }
      child = new_child;
      current_entry->children_.push_back(std::unique_ptr<Entry>(new_child));
    }

    current_entry = static_cast<HostPathEntry*>(child);
  }

  return current_entry;
}

void HostPathDevice::PopulateEntry(HostPathEntry* parent_entry) {
  auto child_infos = rex::filesystem::ListFiles(parent_entry->host_path());
  auto overlay = OverlayOf(this);
  auto overlay_dir = overlay.empty()
                         ? std::filesystem::path()
                         : overlay / parent_entry->host_path().lexically_relative(host_path_);
  for (auto& child_info : child_infos) {
    auto child_path = parent_entry->host_path() / child_info.name;
    if (!overlay_dir.empty() && child_info.type == rex::filesystem::FileInfo::Type::kFile) {
      // The overlay's file of that name, if there is one.
      rex::filesystem::FileInfo overlay_info;
      auto overlay_file = overlay_dir / child_info.name;
      if (rex::filesystem::GetInfo(overlay_file, &overlay_info) &&
          overlay_info.type == rex::filesystem::FileInfo::Type::kFile) {
        overlay_info.name = child_info.name;
        child_info = overlay_info;
        child_path = overlay_file;
      }
    }
    auto child = HostPathEntry::Create(this, parent_entry, child_path, child_info);
    parent_entry->children_.push_back(std::unique_ptr<Entry>(child));

    if (child_info.type == rex::filesystem::FileInfo::Type::kDirectory) {
      PopulateEntry(child);
    }
  }
}

}  // namespace rex::filesystem
