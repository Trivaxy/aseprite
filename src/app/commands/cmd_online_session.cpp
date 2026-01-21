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
    ui::Window win(ui::Window::WithTitleBar, "Host Online Session");
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
    app::online::OnlineSessionManager::instance()->startHost(context, doc, p, addr->text());
    open_session_window();
  }
};

class JoinOnlineSessionCommand : public Command {
public:
  JoinOnlineSessionCommand() : Command(CommandId::JoinOnlineSession()) {}

protected:
  bool onEnabled(Context* context) override { return (context && context->isUIAvailable()); }

  void onExecute(Context* context) override
  {
    ui::Window win(ui::Window::WithTitleBar, "Join Online Session");
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
    app::online::OnlineSessionManager::instance()->join(context, addr->text(), p);
    open_session_window();
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
