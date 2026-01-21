// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_UI_ONLINE_SESSION_WINDOW_H_INCLUDED
#define APP_UI_ONLINE_SESSION_WINDOW_H_INCLUDED
#pragma once

#include "ui/window.h"

namespace ui {
class Button;
class CheckBox;
class Entry;
class Label;
class ListBox;
class TextBox;
class View;
} // namespace ui

namespace app::online {

class OnlineSessionWindow : public ui::Window {
public:
  OnlineSessionWindow();
  ~OnlineSessionWindow() override;

  void refresh();

private:
  void refreshPeers();
  void refreshChat();
  void refreshSelection();

  uint32_t selectedPeerId() const;
  void applySelectedPermsToHost();
  void onSendChat();
  void onLeave();
  void onKick();

private:
  ui::Label* m_status = nullptr;
  ui::Button* m_leave = nullptr;

  ui::ListBox* m_peers = nullptr;
  ui::View* m_peersView = nullptr;
  ui::CheckBox* m_canEditCanvas = nullptr;
  ui::CheckBox* m_canEditLayers = nullptr;
  ui::CheckBox* m_canLockLayers = nullptr;
  ui::CheckBox* m_canEditTimeline = nullptr;
  ui::Button* m_kick = nullptr;

  ui::View* m_chatView = nullptr;
  ui::TextBox* m_chatText = nullptr;
  ui::Entry* m_chatEntry = nullptr;
  ui::Button* m_chatSend = nullptr;
};

} // namespace app::online

#endif

