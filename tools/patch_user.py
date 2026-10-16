p = 'C:/aot2/sdk/src/kernel/xam/xam_user.cpp'
s = open(p, encoding='utf-8').read()


def sub(old, new, count=1):
    global s
    assert old in s, old[:80]
    s = s.replace(old, new, count)


sub('#include <rex/system/xtypes.h>\n',
    '#include <rex/system/xtypes.h>\n\n#include "xlive_net.h"\n')

# Online identity: signed in to LIVE with a per-machine XUID and gamertag.
sub("""      if (type & (2 | 4)) {
        // maybe online profile?
        xuid = user_profile->xuid();
        result = X_E_SUCCESS;
      } else if (type & 1) {
        // maybe offline profile?
        xuid = user_profile->xuid();
        result = X_E_SUCCESS;
      }""", """      if (type & (1 | 2 | 4)) {
        xuid = live::LocalXuid(0);
        result = X_E_SUCCESS;
      }""")

sub("""    if (user_index == 0) {
      const auto& user_profile = REX_KERNEL_STATE()->user_profile();
      signin_state = user_profile->signin_state();
    }""", """    if (user_index == 0) {
      signin_state = 2;  // eXUserSigninState_SignedInToLive
    }""")

sub("""  info->xuid = user_profile->xuid();
  info->signin_state = user_profile->signin_state();
  rex::string::copy_truncating(info->name, user_profile->name(), rex::countof(info->name));""",
    """  (void)user_profile;
  info->xuid = live::LocalXuid(0);
  info->signin_state = 2;  // signed in to LIVE
  rex::string::copy_truncating(info->name, live::LocalGamertag(), rex::countof(info->name));""")

sub("""  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  const auto& user_name = user_profile->name();
  rex::string::copy_truncating(buffer, user_name, std::min(buffer_len, uint32_t(16)));""",
    """  const auto user_name = live::LocalGamertag();
  rex::string::copy_truncating(buffer, user_name, std::min(buffer_len, uint32_t(16)));""")

sub("""  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  auto user_name = rex::string::to_utf16(user_profile->name());""",
    """  auto user_name = rex::string::to_utf16(live::LocalGamertag());""")

sub("""  // If we deny everything, games should hopefully not try to do stuff.
  *out_value = 0;
  return X_ERROR_SUCCESS;""", """  // Grant everything (multiplayer, communications, content...).
  *out_value = 1;
  return X_ERROR_SUCCESS;""")

sub("""u32 XamSessionCreateHandle_entry(mapped_u32 handle_ptr) {
  *handle_ptr = 0xCAFEDEAD;
  return X_ERROR_SUCCESS;
}

u32 XamSessionRefObjByHandle_entry(u32 handle, mapped_u32 obj_ptr) {
  assert_true(handle == 0xCAFEDEAD);
  // TODO(PermaNull): Implement this properly,
  // For the time being returning 0xDEADF00D will prevent crashing.
  *obj_ptr = 0xDEADF00D;
  return X_ERROR_SUCCESS;
}""", """u32 XamSessionCreateHandle_entry(mapped_u32 handle_ptr) {
  auto session = object_ref<live::XSession>(new live::XSession(REX_KERNEL_STATE()));
  X_STATUS result = session->Initialize();
  if (XFAILED(result)) {
    return result;
  }
  *handle_ptr = session->handle();
  return X_ERROR_SUCCESS;
}

u32 XamSessionRefObjByHandle_entry(u32 handle, mapped_u32 obj_ptr) {
  auto session = REX_KERNEL_OBJECTS()->LookupObject<live::XSession>(handle);
  if (!session) {
    return X_STATUS_INVALID_HANDLE;
  }
  session->RetainHandle();
  *obj_ptr = session->guest_object();
  return X_ERROR_SUCCESS;
}""")

open(p, 'w', encoding='utf-8').write(s)
print('ok')
