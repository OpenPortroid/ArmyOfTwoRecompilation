// dlc_folder.h - Where the downloadable content lives (see dlc_folder.cpp).
#pragma once

#include <filesystem>

namespace ao2::dlc
{
  // The folder the game reads the downloadable content from (<folder>\<package>), after Setup:
  // <game>\dlc, unless --marketplace_content_root says otherwise.
  std::filesystem::path Dir();

  // Before the game starts (OnPostSetup): moves the packages that earlier versions put in the user
  // data folder (<user data>\0000000000000000\4541084C\00000002) into the game folder's dlc\ and
  // points the SDK's marketplace_content_root at it.
  void Setup(const std::filesystem::path &user_data_root);
} // namespace ao2::dlc
