// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/ui/online_session_window.h"

#include "app/online/online_session_manager.h"
#include "fmt/format.h"
#include "ui/box.h"
#include "ui/button.h"
#include "ui/entry.h"
#include "ui/label.h"
#include "ui/listbox.h"
#include "ui/listitem.h"
#include "ui/textbox.h"
#include "ui/view.h"

#include "gfx/border.h"
#include "gfx/size.h"
#include "ui/scale.h"
#include "ui/system.h"
#include "ui/ui.h"

#include <cstdlib>

namespace app::online {

using namespace ui;

OnlineSessionWindow::OnlineSessionWindow() : ui::Window(ui::Window::WithTitleBar, "Online Session")
{
  setId("online_session_window");
  setVisible(false);

  auto* root = new VBox;
  root->setExpansive(true);
  addChild(root);

  m_status = new Label("Not connected.");
  m_status->setExpansive(true);
  root->addChild(m_status);

  auto* mid = new HBox;
  mid->setExpansive(true);
  root->addChild(mid);

  auto* peersBox = new VBox;
  peersBox->setExpansive(true);

  auto* peersLabel = new Label("Users");
  peersBox->addChild(peersLabel);

  m_peersView = new View;
  m_peersView->setExpansive(true);
  peersBox->addChild(m_peersView);

  m_peers = new ListBox;
  m_peers->Change.connect([this] { refreshSelection(); });
  m_peersView->attachToView(m_peers);

  mid->addChild(peersBox);

  auto* perms = new VBox;
  perms->setExpansive(true);
  mid->addChild(perms);

  m_canEditCanvas = new CheckBox("Canvas");
  m_canEditLayers = new CheckBox("Layers");
  m_canLockLayers = new CheckBox("Lock Layers");
  m_canEditTimeline = new CheckBox("Timeline");
  m_kick = new Button("Kick");
  m_leave = new Button("Leave");

  auto onPermToggle = [this] { applySelectedPermsToHost(); };
  m_canEditCanvas->Click.connect(onPermToggle);
  m_canEditLayers->Click.connect(onPermToggle);
  m_canLockLayers->Click.connect(onPermToggle);
  m_canEditTimeline->Click.connect(onPermToggle);
  m_kick->Click.connect([this] { onKick(); });
  m_leave->Click.connect([this] { onLeave(); });

  perms->addChild(m_canEditCanvas);
  perms->addChild(m_canEditLayers);
  perms->addChild(m_canLockLayers);
  perms->addChild(m_canEditTimeline);

  auto* actionButtons = new HBox;
  actionButtons->addChild(m_kick);
  actionButtons->addChild(m_leave);
  perms->addChild(actionButtons);

  auto* chatBox = new VBox;
  chatBox->setExpansive(true);
  root->addChild(chatBox);

  m_chatView = new View;
  m_chatView->setExpansive(true);
  chatBox->addChild(m_chatView);

  m_chatText = new TextBox("", LEFT);
  m_chatText->setExpansive(true);
  m_chatView->attachToView(m_chatText);
  m_chatView->makeVisibleAllScrollableArea();

  auto* chatInput = new HBox;
  chatInput->setExpansive(true);
  chatBox->addChild(chatInput);

  m_chatEntry = new Entry(1024, "");
  m_chatEntry->setExpansive(true);
  chatInput->addChild(m_chatEntry);

  m_chatSend = new Button("Send");
  m_chatSend->Click.connect([this] { onSendChat(); });
  chatInput->addChild(m_chatSend);

  InitTheme.connect([this] {
    m_peers->setBorder(gfx::Border(3) * guiscale());
    m_chatView->setMinSize(gfx::Size(0, 450 * guiscale()));
  });

  initTheme();

  OnlineSessionManager::instance()->attachWindow(this);
  refresh();
}

OnlineSessionWindow::~OnlineSessionWindow()
{
  OnlineSessionManager::instance()->detachWindow(this);
}

uint32_t OnlineSessionWindow::selectedPeerId() const
{
  if (!m_peers)
    return 0;
  auto* w = m_peers->getSelectedChild();
  auto* item = dynamic_cast<ListItem*>(w);
  if (!item)
    return 0;
  const std::string& v = item->getValue();
  if (v.empty())
    return 0;
  return uint32_t(std::strtoul(v.c_str(), nullptr, 10));
}

void OnlineSessionWindow::refresh()
{
  auto* mgr = OnlineSessionManager::instance();
  const bool active = mgr->isActive();
  const bool host = mgr->isHost();

  if (!active) {
    m_status->setText("Not connected.");
  }
  else if (host) {
    m_status->setText("Hosting session.");
  }
  else {
    m_status->setText("Connected as guest.");
  }

  m_leave->setEnabled(active);

  refreshPeers();
  refreshSelection();
  refreshChat();
}

void OnlineSessionWindow::refreshPeers()
{
  auto* mgr = OnlineSessionManager::instance();
  auto peers = mgr->peers();

  // Preserve selection
  const uint32_t oldId = selectedPeerId();

  // Clear
  while (m_peers->children().size() > 0) {
    Widget* child = m_peers->children().front();
    m_peers->removeChild(child);
    delete child;
  }

  bool selected = false;
  for (const auto& p : peers) {
    auto* item = new ListItem(fmt::format("{} ({})", p.name, p.id));
    item->setValue(std::to_string(p.id));
    m_peers->addChild(item);

    if (p.id == oldId) {
      m_peers->selectChild(item);
      selected = true;
    }
  }

  if (!selected && m_peers->getItemsCount() > 0)
    m_peers->selectIndex(0);

  m_peersView->updateView();
}

void OnlineSessionWindow::refreshChat()
{
  auto* mgr = OnlineSessionManager::instance();
  const std::string log = mgr->chatLog();
  m_chatText->setText(log.c_str());
  m_chatView->updateView();
  m_chatView->makeVisibleAllScrollableArea();
}

void OnlineSessionWindow::refreshSelection()
{
  auto* mgr = OnlineSessionManager::instance();
  const bool isHost = mgr->isHost();

  const uint32_t peerId = selectedPeerId();
  app::online::Permissions perms = 0;
  for (const auto& p : mgr->peers()) {
    if (p.id == peerId) {
      perms = p.perms;
      break;
    }
  }

  const bool canEditCanvas = (perms & kPermEditCanvas) != 0;
  const bool canEditLayers = (perms & kPermEditLayers) != 0;
  const bool canLockLayers = (perms & kPermLockLayers) != 0;
  const bool canEditTimeline = (perms & kPermEditTimeline) != 0;

  m_canEditCanvas->setSelected(canEditCanvas);
  m_canEditLayers->setSelected(canEditLayers);
  m_canLockLayers->setSelected(canLockLayers);
  m_canEditTimeline->setSelected(canEditTimeline);

  const bool canEditThisPeer = isHost && peerId != 0 && peerId != 1;
  m_canEditCanvas->setEnabled(canEditThisPeer);
  m_canEditLayers->setEnabled(canEditThisPeer);
  m_canLockLayers->setEnabled(canEditThisPeer);
  m_canEditTimeline->setEnabled(canEditThisPeer);
  m_kick->setEnabled(canEditThisPeer);
}

void OnlineSessionWindow::applySelectedPermsToHost()
{
  auto* mgr = OnlineSessionManager::instance();
  if (!mgr->isHost())
    return;

  const uint32_t peerId = selectedPeerId();
  if (peerId == 0 || peerId == 1)
    return;

  Permissions perms = 0;
  if (m_canEditCanvas->isSelected())
    perms |= kPermEditCanvas;
  if (m_canEditLayers->isSelected())
    perms |= kPermEditLayers;
  if (m_canLockLayers->isSelected())
    perms |= kPermLockLayers;
  if (m_canEditTimeline->isSelected())
    perms |= kPermEditTimeline;

  mgr->hostSetGuestPermissions(peerId, perms);
  refresh();
}

void OnlineSessionWindow::onSendChat()
{
  std::string text = m_chatEntry->text();
  if (text.empty())
    return;
  OnlineSessionManager::instance()->sendChat(text);
  m_chatEntry->setText("");
}

void OnlineSessionWindow::onLeave()
{
  OnlineSessionManager::instance()->leave();
  refresh();
}

void OnlineSessionWindow::onKick()
{
  const uint32_t peerId = selectedPeerId();
  if (peerId == 0 || peerId == 1)
    return;
  OnlineSessionManager::instance()->hostKick(peerId, "Kicked by host");
  refresh();
}

} // namespace app::online
