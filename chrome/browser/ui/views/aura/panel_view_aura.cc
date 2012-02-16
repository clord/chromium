// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/aura/panel_view_aura.h"

#include "ash/wm/panel_frame_view.h"
#include "base/utf_string_conversions.h"
#include "chrome/browser/extensions/extension_function_dispatcher.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/common/chrome_view_type.h"
#include "chrome/common/extensions/extension_messages.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"
#include "googleurl/src/gurl.h"
#include "ipc/ipc_message.h"
#include "ipc/ipc_message_macros.h"
#include "ui/aura/window.h"
#include "ui/views/widget/widget.h"

namespace {
const int kMinWidth = 100;
const int kMinHeight = 100;
const int kDefaultWidth = 200;
const int kDefaultHeight = 300;
}

////////////////////////////////////////////////////////////////////////////////
// PanelHost

namespace internal {

class PanelHost : public content::WebContentsDelegate,
                  public content::WebContentsObserver,
                  public ExtensionFunctionDispatcher::Delegate {
 public:
  explicit PanelHost(PanelViewAura* panel_view, Profile* profile);
  virtual ~PanelHost();

  void Init(const GURL& url);

  content::WebContents* web_contents() const { return web_contents_.get(); }

  // ExtensionFunctionDispatcher::Delegate overrides.
  virtual Browser* GetBrowser() OVERRIDE;
  virtual content::WebContents* GetAssociatedWebContents() const OVERRIDE;

  // content::WebContentsDelegate implementation:
  virtual void CloseContents(content::WebContents* source) OVERRIDE;
  virtual void HandleMouseDown() OVERRIDE;
  virtual void UpdatePreferredSize(content::WebContents* source,
                                   const gfx::Size& pref_size) OVERRIDE;
  virtual void AddNewContents(content::WebContents* source,
                              content::WebContents* new_contents,
                              WindowOpenDisposition disposition,
                              const gfx::Rect& initial_pos,
                              bool user_gesture) OVERRIDE;

  // content::WebContentsObserver implementation:
  virtual void RenderViewCreated(RenderViewHost* render_view_host) OVERRIDE;
  virtual void RenderViewReady() OVERRIDE;
  virtual void RenderViewGone(base::TerminationStatus status) OVERRIDE;
  virtual bool OnMessageReceived(const IPC::Message& message) OVERRIDE;

 protected:
  // Message handlers
  void OnRequest(const ExtensionHostMsg_Request_Params& params);

 private:
  PanelViewAura* panel_view_;
  Profile* profile_;
  ExtensionFunctionDispatcher extension_function_dispatcher_;
  scoped_ptr<content::WebContents> web_contents_;
  // Site instance to be used for opening new links.
  scoped_refptr<content::SiteInstance> site_instance_;
};

PanelHost::PanelHost(PanelViewAura* panel_view, Profile* profile)
    : panel_view_(panel_view),
      profile_(profile),
      ALLOW_THIS_IN_INITIALIZER_LIST(
          extension_function_dispatcher_(profile, this)) {
}

PanelHost::~PanelHost() {
}

void PanelHost::Init(const GURL& url) {
  site_instance_ = content::SiteInstance::CreateForURL(profile_, url);

  web_contents_.reset(content::WebContents::Create(
      profile_, site_instance_.get(), MSG_ROUTING_NONE, NULL, NULL));
  web_contents_->SetViewType(chrome::VIEW_TYPE_PANEL);
  web_contents_->SetDelegate(this);
  Observe(web_contents_.get());

  web_contents_->GetController().LoadURL(
      url, content::Referrer(), content::PAGE_TRANSITION_LINK, std::string());
}

Browser* PanelHost::GetBrowser() {
  return NULL;
}

content::WebContents* PanelHost::GetAssociatedWebContents() const {
  return web_contents_.get();
}

void PanelHost::CloseContents(content::WebContents* source) {
  panel_view_->CloseView();
}

void PanelHost::HandleMouseDown() {
}

void PanelHost::UpdatePreferredSize(content::WebContents* source,
                                    const gfx::Size& pref_size) {
  panel_view_->SetContentPreferredSize(pref_size);
}

// This handles launching a new page from within the panel.
// TODO(stevenjb): Determine whether or not this is the desired/expected
// behavior for panels.
void PanelHost::AddNewContents(content::WebContents* source,
                               content::WebContents* new_contents,
                               WindowOpenDisposition disposition,
                               const gfx::Rect& initial_pos,
                               bool user_gesture) {
  Browser* browser = BrowserList::GetLastActiveWithProfile(
      Profile::FromBrowserContext(new_contents->GetBrowserContext()));
  if (!browser)
    return;
  browser->AddWebContents(new_contents, disposition, initial_pos, user_gesture);
}

void PanelHost::RenderViewCreated(RenderViewHost* render_view_host) {
}

void PanelHost::RenderViewReady() {
}

void PanelHost::RenderViewGone(base::TerminationStatus status) {
  CloseContents(web_contents_.get());
}

bool PanelHost::OnMessageReceived(const IPC::Message& message) {
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP(PanelHost, message)
    IPC_MESSAGE_HANDLER(ExtensionHostMsg_Request, OnRequest)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  return handled;
}

void PanelHost::OnRequest(const ExtensionHostMsg_Request_Params& params) {
  extension_function_dispatcher_.Dispatch(params,
                                          web_contents_->GetRenderViewHost());
}

}  // namespace internal

////////////////////////////////////////////////////////////////////////////////
// PanelViewAura

PanelViewAura::PanelViewAura(const std::string& title)
    : title_(title),
      preferred_size_(kMinWidth, kMinHeight),
      widget_(NULL) {
}

PanelViewAura::~PanelViewAura() {
}

views::Widget* PanelViewAura::Init(Profile* profile,
                                   const GURL& url,
                                   const gfx::Rect& bounds) {
  widget_ = new views::Widget;
  views::Widget::InitParams params(views::Widget::InitParams::TYPE_PANEL);
  params.delegate = this;

  params.bounds = bounds;
  if (params.bounds.width() == 0)
    params.bounds.set_width(kDefaultWidth);
  else if (params.bounds.width() < kMinWidth)
    params.bounds.set_width(kMinWidth);

  if (params.bounds.height() == 0)
    params.bounds.set_height(kDefaultHeight);
  else if (params.bounds.height() < kMinHeight)
    params.bounds.set_height(kMinHeight);

  widget_->Init(params);
  widget_->GetNativeView()->SetName(title_);

  host_.reset(new internal::PanelHost(this, profile));
  host_->Init(url);

  Attach(host_->web_contents()->GetNativeView());

  widget_->Show();

  return widget_;
}

content::WebContents* PanelViewAura::WebContents() {
  return host_->web_contents();
}

void PanelViewAura::CloseView() {
  widget_->CloseNow();
}

void PanelViewAura::SetContentPreferredSize(const gfx::Size& size) {
  if (size.width() > kMinWidth)
    preferred_size_.set_width(size.width());
  if (size.height() > kMinHeight)
    preferred_size_.set_height(size.height());
}

// views::View implementation:

gfx::Size PanelViewAura::GetPreferredSize() {
  return preferred_size_;
}

// views::WidgetDelegate implementation:

bool PanelViewAura::CanResize() const {
  // TODO(stevenjb): Can/should panels be able to prevent resizing?
  return true;
}

string16 PanelViewAura::GetWindowTitle() const {
  return UTF8ToUTF16(title_);
}

views::View* PanelViewAura::GetContentsView() {
  return this;
}

views::View* PanelViewAura::GetInitiallyFocusedView() {
  return this;
}

bool PanelViewAura::ShouldShowWindowTitle() const {
  return true;
}

views::Widget* PanelViewAura::GetWidget() {
  return View::GetWidget();
}

const views::Widget* PanelViewAura::GetWidget() const {
  return View::GetWidget();
}

views::NonClientFrameView* PanelViewAura::CreateNonClientFrameView() {
  return new ash::PanelFrameView();
}
