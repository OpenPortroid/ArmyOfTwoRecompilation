import re

p = 'C:/aot2/sdk/src/kernel/xam/apps/xgi_app.cpp'
s = open(p, encoding='utf-8').read()

s = s.replace('#include <rex/thread.h>\n',
              '#include <rex/cvar.h>\n#include <rex/thread.h>\n\n#include "../xlive_net.h"\n\n'
              'REXCVAR_DECLARE(uint32_t, net_search_timeout_ms);\n', 1)


def replace_case(msg, body):
    global s
    start = s.index('    case %s: {' % msg)
    m = re.compile(r'\n    case 0x|\n  }\n  REXKRNL_ERROR').search(s, start + 10)
    s = s[:start] + body.rstrip('\n') + s[m.start():]


LOOKUP = """      auto session = live::XSession::FromGuest(kernel_state_, data->obj_ptr);
      if (!session) {
        return X_E_INVALIDARG;
      }
"""


def session_case(msg, struct, call, extra=''):
    return ('    case %s: {\n      auto data = reinterpret_cast<const live::%s*>(buffer);\n'
            % (msg, struct)) + LOOKUP + extra + '      return session->%s;\n    }\n' % call


replace_case('0x000B0006', """    case 0x000B0006: {
      // XGIUserSetContextEx: {user_index, unk, xuid, context_id, value}
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t context_value = memory::load_and_swap<uint32_t>(buffer + 20);
      REXKRNL_DEBUG("XGIUserSetContextEx({:08X}, {:08X}, {:08X})", user_index, context_id,
                    context_value);
      live::SetUserContext(user_index, context_id, context_value);
      return X_E_SUCCESS;
    }
""")
replace_case('0x000B0007', """    case 0x000B0007: {
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
""")
replace_case('0x000B0010', session_case('0x000B0010', 'XGI_SESSION_CREATE', 'Create(data)'))
replace_case('0x000B0011', session_case('0x000B0011', 'XGI_SESSION_STATE', 'Delete()'))
replace_case('0x000B0012', session_case('0x000B0012', 'XGI_SESSION_MANAGE', 'Join(data)') +
             session_case('0x000B0013', 'XGI_SESSION_MANAGE', 'Leave(data)'))
replace_case('0x000B0014', session_case('0x000B0014', 'XGI_SESSION_STATE', 'Start()'))
replace_case('0x000B0015', session_case('0x000B0015', 'XGI_SESSION_STATE', 'End()'))
replace_case('0x000B0018', session_case('0x000B0018', 'XGI_SESSION_MODIFY', 'Modify(data)'))
replace_case('0x000B001D', session_case('0x000B001D', 'XGI_SESSION_DETAILS', 'GetDetails(data)'))
replace_case('0x000B001E', session_case('0x000B001E', 'XGI_SESSION_MIGRATE', 'Migrate(data)',
                                        '      if (!data->session_info_ptr) {\n'
                                        '        return X_E_FAIL;\n      }\n'))
replace_case('0x000B001A', session_case('0x000B001A', 'XGI_SESSION_ARBITRATION',
                                        'RegisterArbitration(data)'))

SEARCH = """      auto sessions = live::SearchSessions(REXCVAR_GET(net_search_timeout_ms));
      return live::WriteSearchResults(kernel_state_, sessions, data->num_results,
                                      data->search_results_ptr, data->results_buffer_size);
    }
"""
for msg, struct, name in [('0x000B0016', 'XGI_SESSION_SEARCH', 'XSessionSearch'),
                          ('0x000B001C', 'XGI_SESSION_SEARCH', 'XSessionSearchEx'),
                          ('0x000B0065', 'XGI_SESSION_SEARCH_WEIGHTED', 'XSessionSearchWeighted')]:
    replace_case(msg, ("""    case %s: {
      auto data = reinterpret_cast<const live::%s*>(buffer);
      REXKRNL_INFO("%s(proc={}, user={}, max={})", data->proc_index.get(),
                   data->user_index.get(), data->num_results.get());
""" % (msg, struct, name)) + SEARCH)

BYID = """      std::vector<live::AdvertisedSession> found;
      for (uint32_t i = 0; i < count; i++) {
        if (auto s = live::QuerySessionById(live::XnkidToU64(ids[i]), 1000)) {
          found.push_back(*s);
        }
      }
      return live::WriteSearchResults(kernel_state_, found, count, results_ptr, results_size);
    }
"""
replace_case('0x000B001B', """    case 0x000B001B: {
      auto data = reinterpret_cast<const live::XGI_SESSION_SEARCH_BYID*>(buffer);
      const live::XNKID* ids = &data->session_id;
      uint32_t count = 1;
      uint32_t results_ptr = data->search_results_ptr;
      uint32_t results_size = data->results_buffer_size;
      REXKRNL_INFO("XSessionSearchByID({:016X})", live::XnkidToU64(data->session_id));
""" + BYID)
replace_case('0x000B0060', """    case 0x000B0060: {
      auto data = reinterpret_cast<const live::XGI_SESSION_SEARCH_BYIDS*>(buffer);
      auto ids = memory_->TranslateVirtual<const live::XNKID*>(data->session_ids_ptr);
      uint32_t count = std::min<uint32_t>(data->num_session_ids, 100);
      uint32_t results_ptr = data->search_results_ptr;
      uint32_t results_size = data->results_buffer_size;
      REXKRNL_INFO("XSessionSearchByIds({})", count);
""" + BYID)
replace_case('0x000B0041', """    case 0x000B0041: {
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
""")
open(p, 'w', encoding='utf-8').write(s)
print('ok')
