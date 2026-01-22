// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/commands/command.h"

#include "app/context.h"
#include "app/i18n/strings.h"
#include "app/online/online_session_manager.h"
#include "app/ui/online_session_window.h"
#include "ui/box.h"
#include "ui/button.h"
#include "ui/entry.h"
#include "ui/label.h"
#include "ui/window.h"

#include <cstdlib>

namespace app {

namespace {

int parse_port(const std::string& s, const int fallback)
{
  if (s.empty())
    return fallback;
  const int v = int(std::strtol(s.c_str(), nullptr, 10));
  if (v <= 0 || v > 65535)
    return fallback;
  return v;
}

app::online::OnlineSessionWindow* g_window = nullptr;

void open_session_window()
{
  if (!g_window) {
    g_window = new app::online::OnlineSessionWindow();
    g_window->Close.connect([](ui::CloseEvent&) {
      auto* w = g_window;
      g_window = nullptr;
      delete w;
    });
    g_window->openWindow();
  }
  else {
    g_window->openWindowInForeground();
  }
}

// Show initial choice dialog for hosting
// Returns: 0 = cancelled, 1 = PartyKit, 2 = Manual
int showHostConnectionTypeDialog()
{
  ui::Window win(ui::Window::WithTitleBar, "Host Online Session");
  auto* root = new ui::VBox;
  win.addChild(root);

  auto* label = new ui::Label("How would you like to create your session?");
  root->addChild(label);

  auto* buttons = new ui::HBox;
  root->addChild(buttons);

  int result = 0;
  auto* partyBtn = new ui::Button("On the house");
  auto* manualBtn = new ui::Button("Manually");
  auto* cancelBtn = new ui::Button("Cancel");

  partyBtn->Click.connect([&] { result = 1; win.closeWindow(partyBtn); });
  manualBtn->Click.connect([&] { result = 2; win.closeWindow(manualBtn); });
  cancelBtn->Click.connect([&] { result = 0; win.closeWindow(cancelBtn); });

  buttons->addChild(partyBtn);
  buttons->addChild(manualBtn);
  buttons->addChild(cancelBtn);

  win.centerWindow();
  win.openWindowInForeground();
  return result;
}

// Show initial choice dialog for joining
// Returns: 0 = cancelled, 1 = PartyKit (room name), 2 = Manual (address)
int showJoinConnectionTypeDialog()
{
  ui::Window win(ui::Window::WithTitleBar, "Join Online Session");
  auto* root = new ui::VBox;
  win.addChild(root);

  auto* label = new ui::Label("How would you like to join?");
  root->addChild(label);

  auto* buttons = new ui::HBox;
  root->addChild(buttons);

  int result = 0;
  auto* roomBtn = new ui::Button("Room name");
  auto* addrBtn = new ui::Button("Address");
  auto* cancelBtn = new ui::Button("Cancel");

  roomBtn->Click.connect([&] { result = 1; win.closeWindow(roomBtn); });
  addrBtn->Click.connect([&] { result = 2; win.closeWindow(addrBtn); });
  cancelBtn->Click.connect([&] { result = 0; win.closeWindow(cancelBtn); });

  buttons->addChild(roomBtn);
  buttons->addChild(addrBtn);
  buttons->addChild(cancelBtn);

  win.centerWindow();
  win.openWindowInForeground();
  return result;
}

} // namespace

class HostOnlineSessionCommand : public Command {
public:
  HostOnlineSessionCommand() : Command(CommandId::HostOnlineSession()) {}

protected:
  bool onEnabled(Context* context) override
  {
    return (context && context->isUIAvailable() &&
            context->checkFlags(ContextFlags::HasActiveDocument | ContextFlags::HasActiveSprite));
  }

  void onExecute(Context* context) override
  {
    const int choice = showHostConnectionTypeDialog();
    if (choice == 0)
      return;

    if (choice == 1) {
      // PartyKit hosting
      ui::Window win(ui::Window::WithTitleBar, "Host via PartyKit");
      auto* root = new ui::VBox;
      win.addChild(root);

      auto* roomRow = new ui::HBox;
      root->addChild(roomRow);
      roomRow->addChild(new ui::Label("Room name:"));
      auto* room = new ui::Entry(256, "");
      room->setExpansive(true);
      roomRow->addChild(room);

      auto* userRow = new ui::HBox;
      root->addChild(userRow);
      userRow->addChild(new ui::Label("Username:"));
      auto* user = new ui::Entry(256, "Host");
      user->setExpansive(true);
      userRow->addChild(user);

      auto* passRow = new ui::HBox;
      root->addChild(passRow);
      passRow->addChild(new ui::Label("Password:"));
      auto* pass = new ui::Entry(256, "");
      pass->setExpansive(true);
      passRow->addChild(pass);

      auto* buttons = new ui::HBox;
      root->addChild(buttons);
      auto* ok = new ui::Button("OK");
      auto* cancel = new ui::Button("Cancel");
      ok->Click.connect([&] { win.closeWindow(ok); });
      cancel->Click.connect([&] { win.closeWindow(cancel); });
      buttons->addChild(ok);
      buttons->addChild(cancel);

      win.centerWindow();
      win.openWindowInForeground();
      if (win.closer() != ok)
        return;

      auto* doc = context->activeDocument();
      app::online::OnlineSessionManager::instance()->startHostPartyKit(
        context, doc, room->text(), user->text(), pass->text());
      open_session_window();
    }
    else {
      // Manual hosting (existing flow)
      ui::Window win(ui::Window::WithTitleBar, "Host Manually");
      auto* root = new ui::VBox;
      win.addChild(root);

      auto* addrRow = new ui::HBox;
      root->addChild(addrRow);
      addrRow->addChild(new ui::Label("Bind:"));
      auto* addr = new ui::Entry(256, "127.0.0.1");
      addr->setExpansive(true);
      addrRow->addChild(addr);

      auto* portRow = new ui::HBox;
      root->addChild(portRow);
      portRow->addChild(new ui::Label("Port:"));
      auto* port = new ui::Entry(16, "5000");
      port->setExpansive(true);
      portRow->addChild(port);

      auto* userRow = new ui::HBox;
      root->addChild(userRow);
      userRow->addChild(new ui::Label("Username:"));
      auto* user = new ui::Entry(256, "Host");
      user->setExpansive(true);
      userRow->addChild(user);

      auto* passRow = new ui::HBox;
      root->addChild(passRow);
      passRow->addChild(new ui::Label("Password:"));
      auto* pass = new ui::Entry(256, "");
      pass->setExpansive(true);
      passRow->addChild(pass);

      auto* buttons = new ui::HBox;
      root->addChild(buttons);
      auto* ok = new ui::Button("OK");
      auto* cancel = new ui::Button("Cancel");
      ok->Click.connect([&] { win.closeWindow(ok); });
      cancel->Click.connect([&] { win.closeWindow(cancel); });
      buttons->addChild(ok);
      buttons->addChild(cancel);

      win.centerWindow();
      win.openWindowInForeground();
      if (win.closer() != ok)
        return;

      const int p = parse_port(port->text(), 5000);
      auto* doc = context->activeDocument();
      app::online::OnlineSessionManager::instance()->startHost(context, doc, p, user->text(), pass->text(), addr->text());
      open_session_window();
    }
  }
};

class JoinOnlineSessionCommand : public Command {
public:
  JoinOnlineSessionCommand() : Command(CommandId::JoinOnlineSession()) {}

protected:
  bool onEnabled(Context* context) override { return (context && context->isUIAvailable()); }

  void onExecute(Context* context) override
  {
    const int choice = showJoinConnectionTypeDialog();
    if (choice == 0)
      return;

    if (choice == 1) {
      // PartyKit join via room name
      ui::Window win(ui::Window::WithTitleBar, "Join via Room Name");
      auto* root = new ui::VBox;
      win.addChild(root);

      auto* roomRow = new ui::HBox;
      root->addChild(roomRow);
      roomRow->addChild(new ui::Label("Room name:"));
      auto* room = new ui::Entry(256, "");
      room->setExpansive(true);
      roomRow->addChild(room);

      auto* userRow = new ui::HBox;
      root->addChild(userRow);
      userRow->addChild(new ui::Label("Username:"));
      auto* user = new ui::Entry(256, "Guest");
      user->setExpansive(true);
      userRow->addChild(user);

      auto* passRow = new ui::HBox;
      root->addChild(passRow);
      passRow->addChild(new ui::Label("Password:"));
      auto* pass = new ui::Entry(256, "");
      pass->setExpansive(true);
      passRow->addChild(pass);

      auto* buttons = new ui::HBox;
      root->addChild(buttons);
      auto* ok = new ui::Button("OK");
      auto* cancel = new ui::Button("Cancel");
      ok->Click.connect([&] { win.closeWindow(ok); });
      cancel->Click.connect([&] { win.closeWindow(cancel); });
      buttons->addChild(ok);
      buttons->addChild(cancel);

      win.centerWindow();
      win.openWindowInForeground();
      if (win.closer() != ok)
        return;

      app::online::OnlineSessionManager::instance()->joinPartyKit(
        context, room->text(), user->text(), pass->text());
      open_session_window();
    }
    else {
      // Manual join via address (existing flow)
      ui::Window win(ui::Window::WithTitleBar, "Join via Address");
      auto* root = new ui::VBox;
      win.addChild(root);

      auto* addrRow = new ui::HBox;
      root->addChild(addrRow);
      addrRow->addChild(new ui::Label("Address:"));
      auto* addr = new ui::Entry(256, "127.0.0.1");
      addr->setExpansive(true);
      addrRow->addChild(addr);

      auto* portRow = new ui::HBox;
      root->addChild(portRow);
      portRow->addChild(new ui::Label("Port:"));
      auto* port = new ui::Entry(16, "5000");
      port->setExpansive(true);
      portRow->addChild(port);

      auto* userRow = new ui::HBox;
      root->addChild(userRow);
      userRow->addChild(new ui::Label("Username:"));
      auto* user = new ui::Entry(256, "Guest");
      user->setExpansive(true);
      userRow->addChild(user);

      auto* passRow = new ui::HBox;
      root->addChild(passRow);
      passRow->addChild(new ui::Label("Password:"));
      auto* pass = new ui::Entry(256, "");
      pass->setExpansive(true);
      passRow->addChild(pass);

      auto* buttons = new ui::HBox;
      root->addChild(buttons);
      auto* ok = new ui::Button("OK");
      auto* cancel = new ui::Button("Cancel");
      ok->Click.connect([&] { win.closeWindow(ok); });
      cancel->Click.connect([&] { win.closeWindow(cancel); });
      buttons->addChild(ok);
      buttons->addChild(cancel);

      win.centerWindow();
      win.openWindowInForeground();
      if (win.closer() != ok)
        return;

      const int p = parse_port(port->text(), 5000);
      app::online::OnlineSessionManager::instance()->join(context, addr->text(), p, user->text(), pass->text());
      open_session_window();
    }
  }
};

class LeaveOnlineSessionCommand : public Command {
public:
  LeaveOnlineSessionCommand() : Command(CommandId::LeaveOnlineSession()) {}

protected:
  bool onEnabled(Context* context) override
  {
    return context && app::online::OnlineSessionManager::instance()->isActive();
  }

  void onExecute(Context* context) override
  {
    (void)context;
    app::online::OnlineSessionManager::instance()->leave();
  }
};

class OnlineSessionCommand : public Command {
public:
  OnlineSessionCommand() : Command(CommandId::OnlineSession()) {}

protected:
  bool onEnabled(Context* context) override { return (context && context->isUIAvailable()); }
  void onExecute(Context* context) override
  {
    (void)context;
    open_session_window();
  }
};

Command* CommandFactory::createHostOnlineSessionCommand()
{
  return new HostOnlineSessionCommand;
}

Command* CommandFactory::createJoinOnlineSessionCommand()
{
  return new JoinOnlineSessionCommand;
}

Command* CommandFactory::createLeaveOnlineSessionCommand()
{
  return new LeaveOnlineSessionCommand;
}

Command* CommandFactory::createOnlineSessionCommand()
{
  return new OnlineSessionCommand;
}

} // namespace app
