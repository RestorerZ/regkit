// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "frame/command_detail.h"

#include "editors/decoder_dialog.h"

namespace regkit {
using namespace command_detail;

bool MainWindow::Impl::HandleToolsCommand(
    int command_id
) {
  switch (command_id) {
  case cmd::kEditDecodeValue:
    {
      const RegistryNode* node = browse_.current_node();
      if (!node) {
        return true;
      }
      std::vector<ListRow> selected_rows = SelectedListRows(browse_.values());
      if (selected_rows.size() != 1 ||
          selected_rows.front().kind != rowkind::kValue) {
        return true;
      }
      ValueEntry entry;
      if (!RegistryStore::QueryValue(*node, selected_rows.front().extra, &entry)) {
        ui::ShowError(hwnd_, L"Failed to read value.");
        return true;
      }
      editors::DecodeRequest request;
      request.value_name = entry.name;
      request.key_path = registry_path::Build(*node);
      request.type = entry.type;
      request.data = std::move(entry.data);
      editors::ShowValueDecoder(hwnd_, request);
      return true;
    }
  default:
    return false;
  }
}

} // namespace regkit
