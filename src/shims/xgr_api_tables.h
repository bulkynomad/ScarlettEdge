// ScarlettX — xgameruntime QueryApiImpl tables that only forward to flat APIs.
#pragma once

#include <string>

namespace scarlettx::xgr {

// Returns a new reference to the guest COM table for `api_id`, or nullptr when
// this file does not provide it (or `riid` is not the table's interface).
void* CreateApiTable(const void* api_id, const void* riid);

// The form in which the save store keeps, and enumerates, a container or
// blob name.
std::string StoredSaveName(const std::string& name);

}  // namespace scarlettx::xgr
