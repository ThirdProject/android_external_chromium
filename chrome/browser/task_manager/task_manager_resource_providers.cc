// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/task_manager/task_manager_resource_providers.h"

#include "build/build_config.h"

#include "app/l10n_util.h"
#include "app/resource_bundle.h"
#include "base/basictypes.h"
#include "base/file_version_info.h"
#include "base/i18n/rtl.h"
#include "base/process_util.h"
#include "base/stl_util-inl.h"
#include "base/string_util.h"
#include "base/thread.h"
#include "chrome/app/chrome_dll_resource.h"
#include "chrome/browser/browser_child_process_host.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/browser_list.h"
#include "chrome/browser/chrome_thread.h"
#include "chrome/browser/extensions/extension_host.h"
#include "chrome/browser/extensions/extension_process_manager.h"
#include "chrome/browser/notifications/balloon_collection.h"
#include "chrome/browser/notifications/balloon_host.h"
#include "chrome/browser/notifications/notification_ui_manager.h"
#include "chrome/browser/profile_manager.h"
#include "chrome/browser/renderer_host/render_process_host.h"
#include "chrome/browser/renderer_host/render_view_host.h"
#include "chrome/browser/renderer_host/resource_message_filter.h"
#include "chrome/browser/tab_contents/tab_util.h"
#include "chrome/browser/tab_contents/tab_contents.h"
#include "chrome/common/extensions/extension.h"
#include "chrome/common/notification_service.h"
#include "chrome/common/render_messages.h"
#include "chrome/common/sqlite_utils.h"
#include "grit/generated_resources.h"
#include "grit/theme_resources.h"

#if defined(OS_MACOSX)
#include "skia/ext/skia_utils_mac.h"
#endif
#if defined(OS_WIN)
#include "chrome/browser/app_icon_win.h"
#include "gfx/icon_util.h"
#endif  // defined(OS_WIN)

////////////////////////////////////////////////////////////////////////////////
// TaskManagerTabContentsResource class
////////////////////////////////////////////////////////////////////////////////

TaskManagerTabContentsResource::TaskManagerTabContentsResource(
    TabContents* tab_contents)
    : tab_contents_(tab_contents),
      pending_stats_update_(false),
      v8_memory_allocated_(0),
      v8_memory_used_(0),
      pending_v8_memory_allocated_update_(false) {
  // We cache the process as when the TabContents is closed the process
  // becomes NULL and the TaskManager still needs it.
  process_ = tab_contents_->GetRenderProcessHost()->GetHandle();
  pid_ = base::GetProcId(process_);
  stats_.images.size = 0;
  stats_.cssStyleSheets.size = 0;
  stats_.scripts.size = 0;
  stats_.xslStyleSheets.size = 0;
  stats_.fonts.size = 0;
}

TaskManagerTabContentsResource::~TaskManagerTabContentsResource() {
}

std::wstring TaskManagerTabContentsResource::GetTitle() const {
  // Fall back on the URL if there's no title.
  std::wstring tab_title(UTF16ToWideHack(tab_contents_->GetTitle()));
  if (tab_title.empty()) {
    tab_title = UTF8ToWide(tab_contents_->GetURL().spec());
    // Force URL to be LTR.
    tab_title = UTF16ToWide(base::i18n::GetDisplayStringInLTRDirectionality(
        WideToUTF16(tab_title)));
  } else {
    // Since the tab_title will be concatenated with
    // IDS_TASK_MANAGER_TAB_PREFIX, we need to explicitly set the tab_title to
    // be LTR format if there is no strong RTL charater in it. Otherwise, if
    // IDS_TASK_MANAGER_TAB_PREFIX is an RTL word, the concatenated result
    // might be wrong. For example, http://mail.yahoo.com, whose title is
    // "Yahoo! Mail: The best web-based Email!", without setting it explicitly
    // as LTR format, the concatenated result will be "!Yahoo! Mail: The best
    // web-based Email :BAT", in which the capital letters "BAT" stands for
    // the Hebrew word for "tab".
    base::i18n::AdjustStringForLocaleDirection(tab_title, &tab_title);
  }

  return l10n_util::GetStringF(IDS_TASK_MANAGER_TAB_PREFIX, tab_title);
}

void TaskManagerTabContentsResource::Refresh() {
  if (!pending_stats_update_) {
    tab_contents_->render_view_host()->Send(new ViewMsg_GetCacheResourceStats);
    pending_stats_update_ = true;
  }
  if (!pending_v8_memory_allocated_update_) {
    tab_contents_->render_view_host()->Send(new ViewMsg_GetV8HeapStats);
    pending_v8_memory_allocated_update_ = true;
  }
}

WebKit::WebCache::ResourceTypeStats
    TaskManagerTabContentsResource::GetWebCoreCacheStats() const {
  return stats_;
}

size_t TaskManagerTabContentsResource::GetV8MemoryAllocated() const {
  return v8_memory_allocated_;
}

size_t TaskManagerTabContentsResource::GetV8MemoryUsed() const {
  return v8_memory_used_;
}

void TaskManagerTabContentsResource::NotifyResourceTypeStats(
    const WebKit::WebCache::ResourceTypeStats& stats) {
  stats_ = stats;
  pending_stats_update_ = false;
}

void TaskManagerTabContentsResource::NotifyV8HeapStats(
    size_t v8_memory_allocated, size_t v8_memory_used) {
  v8_memory_allocated_ = v8_memory_allocated;
  v8_memory_used_ = v8_memory_used;
  pending_v8_memory_allocated_update_ = false;
}

SkBitmap TaskManagerTabContentsResource::GetIcon() const {
  return tab_contents_->GetFavIcon();
}

base::ProcessHandle TaskManagerTabContentsResource::GetProcess() const {
  return process_;
}

TabContents* TaskManagerTabContentsResource::GetTabContents() const {
  return static_cast<TabContents*>(tab_contents_);
}

////////////////////////////////////////////////////////////////////////////////
// TaskManagerTabContentsResourceProvider class
////////////////////////////////////////////////////////////////////////////////

TaskManagerTabContentsResourceProvider::
    TaskManagerTabContentsResourceProvider(TaskManager* task_manager)
    :  updating_(false),
       task_manager_(task_manager) {
}

TaskManagerTabContentsResourceProvider::
    ~TaskManagerTabContentsResourceProvider() {
}

TaskManager::Resource* TaskManagerTabContentsResourceProvider::GetResource(
    int origin_pid,
    int render_process_host_id,
    int routing_id) {

  TabContents* tab_contents =
      tab_util::GetTabContentsByID(render_process_host_id, routing_id);
  if (!tab_contents)  // Not one of our resource.
    return NULL;

  base::ProcessHandle process_handle =
      tab_contents->GetRenderProcessHost()->GetHandle();
  if (!process_handle) {
    // We should not be holding on to a dead tab (it should have been removed
    // through the NOTIFY_TAB_CONTENTS_DISCONNECTED notification.
    NOTREACHED();
    return NULL;
  }

  int pid = base::GetProcId(process_handle);
  if (pid != origin_pid)
    return NULL;

  std::map<TabContents*, TaskManagerTabContentsResource*>::iterator
      res_iter = resources_.find(tab_contents);
  if (res_iter == resources_.end())
    // Can happen if the tab was closed while a network request was being
    // performed.
    return NULL;

  return res_iter->second;
}

void TaskManagerTabContentsResourceProvider::StartUpdating() {
  DCHECK(!updating_);
  updating_ = true;

  // Add all the existing TabContents.
  for (TabContentsIterator iterator; !iterator.done(); ++iterator)
    Add(*iterator);

  // Then we register for notifications to get new tabs.
  registrar_.Add(this, NotificationType::TAB_CONTENTS_CONNECTED,
                 NotificationService::AllSources());
  registrar_.Add(this, NotificationType::TAB_CONTENTS_SWAPPED,
                 NotificationService::AllSources());
  registrar_.Add(this, NotificationType::TAB_CONTENTS_DISCONNECTED,
                 NotificationService::AllSources());
  // TAB_CONTENTS_DISCONNECTED should be enough to know when to remove a
  // resource.  This is an attempt at mitigating a crasher that seem to
  // indicate a resource is still referencing a deleted TabContents
  // (http://crbug.com/7321).
  registrar_.Add(this, NotificationType::TAB_CONTENTS_DESTROYED,
                 NotificationService::AllSources());
}

void TaskManagerTabContentsResourceProvider::StopUpdating() {
  DCHECK(updating_);
  updating_ = false;

  // Then we unregister for notifications to get new tabs.
  registrar_.Remove(this, NotificationType::TAB_CONTENTS_CONNECTED,
                    NotificationService::AllSources());
  registrar_.Remove(this, NotificationType::TAB_CONTENTS_SWAPPED,
                    NotificationService::AllSources());
  registrar_.Remove(this, NotificationType::TAB_CONTENTS_DISCONNECTED,
                    NotificationService::AllSources());
  registrar_.Remove(this, NotificationType::TAB_CONTENTS_DESTROYED,
                    NotificationService::AllSources());

  // Delete all the resources.
  STLDeleteContainerPairSecondPointers(resources_.begin(), resources_.end());

  resources_.clear();
}

void TaskManagerTabContentsResourceProvider::AddToTaskManager(
    TabContents* tab_contents) {
  TaskManagerTabContentsResource* resource =
      new TaskManagerTabContentsResource(tab_contents);
  resources_[tab_contents] = resource;
  task_manager_->AddResource(resource);
}

void TaskManagerTabContentsResourceProvider::Add(TabContents* tab_contents) {
  if (!updating_)
    return;

  // Don't add dead tabs or tabs that haven't yet connected.
  // Also ignore tabs which display extension content. We collapse
  // all of these into one extension row.
  if (!tab_contents->GetRenderProcessHost()->GetHandle() ||
      !tab_contents->notify_disconnection() ||
      tab_contents->HostsExtension()) {
    return;
  }

  std::map<TabContents*, TaskManagerTabContentsResource*>::const_iterator
      iter = resources_.find(tab_contents);
  if (iter != resources_.end()) {
    // The case may happen that we have added a TabContents as part of the
    // iteration performed during StartUpdating() call but the notification that
    // it has connected was not fired yet. So when the notification happens, we
    // already know about this tab and just ignore it.
    return;
  }
  AddToTaskManager(tab_contents);
}

void TaskManagerTabContentsResourceProvider::Remove(TabContents* tab_contents) {
  if (!updating_)
    return;
  std::map<TabContents*, TaskManagerTabContentsResource*>::iterator
      iter = resources_.find(tab_contents);
  if (iter == resources_.end()) {
    // Since TabContents are destroyed asynchronously (see TabContentsCollector
    // in navigation_controller.cc), we can be notified of a tab being removed
    // that we don't know.  This can happen if the user closes a tab and quickly
    // opens the task manager, before the tab is actually destroyed.
    return;
  }

  // Remove the resource from the Task Manager.
  TaskManagerTabContentsResource* resource = iter->second;
  task_manager_->RemoveResource(resource);
  // And from the provider.
  resources_.erase(iter);
  // Finally, delete the resource.
  delete resource;
}

void TaskManagerTabContentsResourceProvider::Observe(NotificationType type,
    const NotificationSource& source,
    const NotificationDetails& details) {
  switch (type.value) {
    case NotificationType::TAB_CONTENTS_CONNECTED:
      Add(Source<TabContents>(source).ptr());
      break;
    case NotificationType::TAB_CONTENTS_SWAPPED:
      Remove(Source<TabContents>(source).ptr());
      Add(Source<TabContents>(source).ptr());
      break;
    case NotificationType::TAB_CONTENTS_DESTROYED:
      // If this DCHECK is triggered, it could explain http://crbug.com/7321.
      DCHECK(resources_.find(Source<TabContents>(source).ptr()) ==
             resources_.end()) << "TAB_CONTENTS_DESTROYED with no associated "
                                  "TAB_CONTENTS_DISCONNECTED";
      // Fall through.
    case NotificationType::TAB_CONTENTS_DISCONNECTED:
      Remove(Source<TabContents>(source).ptr());
      break;
    default:
      NOTREACHED() << "Unexpected notification.";
      return;
  }
}

////////////////////////////////////////////////////////////////////////////////
// TaskManagerChildProcessResource class
////////////////////////////////////////////////////////////////////////////////
SkBitmap* TaskManagerChildProcessResource::default_icon_ = NULL;

TaskManagerChildProcessResource::TaskManagerChildProcessResource(
    ChildProcessInfo child_proc)
    : child_process_(child_proc),
      title_(),
      network_usage_support_(false) {
  // We cache the process id because it's not cheap to calculate, and it won't
  // be available when we get the plugin disconnected notification.
  pid_ = child_proc.id();
  if (!default_icon_) {
    ResourceBundle& rb = ResourceBundle::GetSharedInstance();
    default_icon_ = rb.GetBitmapNamed(IDR_PLUGIN);
    // TODO(jabdelmalek): use different icon for web workers.
  }
}

TaskManagerChildProcessResource::~TaskManagerChildProcessResource() {
}

// TaskManagerResource methods:
std::wstring TaskManagerChildProcessResource::GetTitle() const {
  if (title_.empty())
    title_ = UTF16ToWideHack(child_process_.GetLocalizedTitle());

  return title_;
}

SkBitmap TaskManagerChildProcessResource::GetIcon() const {
  return *default_icon_;
}

base::ProcessHandle TaskManagerChildProcessResource::GetProcess() const {
  return child_process_.handle();
}

////////////////////////////////////////////////////////////////////////////////
// TaskManagerChildProcessResourceProvider class
////////////////////////////////////////////////////////////////////////////////

TaskManagerChildProcessResourceProvider::
    TaskManagerChildProcessResourceProvider(TaskManager* task_manager)
    : updating_(false),
      task_manager_(task_manager) {
}

TaskManagerChildProcessResourceProvider::
    ~TaskManagerChildProcessResourceProvider() {
}

TaskManager::Resource* TaskManagerChildProcessResourceProvider::GetResource(
    int origin_pid,
    int render_process_host_id,
    int routing_id) {
  std::map<int, TaskManagerChildProcessResource*>::iterator iter =
      pid_to_resources_.find(origin_pid);
  if (iter != pid_to_resources_.end())
    return iter->second;
  else
    return NULL;
}

void TaskManagerChildProcessResourceProvider::StartUpdating() {
  DCHECK(!updating_);
  updating_ = true;

  // Register for notifications to get new child processes.
  registrar_.Add(this, NotificationType::CHILD_PROCESS_HOST_CONNECTED,
                 NotificationService::AllSources());
  registrar_.Add(this, NotificationType::CHILD_PROCESS_HOST_DISCONNECTED,
                 NotificationService::AllSources());

  // Get the existing child processes.
  ChromeThread::PostTask(
      ChromeThread::IO, FROM_HERE,
      NewRunnableMethod(
          this,
          &TaskManagerChildProcessResourceProvider::RetrieveChildProcessInfo));
}

void TaskManagerChildProcessResourceProvider::StopUpdating() {
  DCHECK(updating_);
  updating_ = false;

  // Unregister for notifications to get new plugin processes.
  registrar_.Remove(this, NotificationType::CHILD_PROCESS_HOST_CONNECTED,
                    NotificationService::AllSources());
  registrar_.Remove(this,
                    NotificationType::CHILD_PROCESS_HOST_DISCONNECTED,
                    NotificationService::AllSources());

  // Delete all the resources.
  STLDeleteContainerPairSecondPointers(resources_.begin(), resources_.end());

  resources_.clear();
  pid_to_resources_.clear();
  existing_child_process_info_.clear();
}

void TaskManagerChildProcessResourceProvider::Observe(
    NotificationType type,
    const NotificationSource& source,
    const NotificationDetails& details) {
  switch (type.value) {
    case NotificationType::CHILD_PROCESS_HOST_CONNECTED:
      Add(*Details<ChildProcessInfo>(details).ptr());
      break;
    case NotificationType::CHILD_PROCESS_HOST_DISCONNECTED:
      Remove(*Details<ChildProcessInfo>(details).ptr());
      break;
    default:
      NOTREACHED() << "Unexpected notification.";
      return;
  }
}

void TaskManagerChildProcessResourceProvider::Add(
    ChildProcessInfo child_process_info) {
  if (!updating_)
    return;
  std::map<ChildProcessInfo, TaskManagerChildProcessResource*>::
      const_iterator iter = resources_.find(child_process_info);
  if (iter != resources_.end()) {
    // The case may happen that we have added a child_process_info as part of
    // the iteration performed during StartUpdating() call but the notification
    // that it has connected was not fired yet. So when the notification
    // happens, we already know about this plugin and just ignore it.
    return;
  }
  AddToTaskManager(child_process_info);
}

void TaskManagerChildProcessResourceProvider::Remove(
    ChildProcessInfo child_process_info) {
  if (!updating_)
    return;
  std::map<ChildProcessInfo, TaskManagerChildProcessResource*>
      ::iterator iter = resources_.find(child_process_info);
  if (iter == resources_.end()) {
    // ChildProcessInfo disconnection notifications are asynchronous, so we
    // might be notified for a plugin we don't know anything about (if it was
    // closed before the task manager was shown and destroyed after that).
    return;
  }
  // Remove the resource from the Task Manager.
  TaskManagerChildProcessResource* resource = iter->second;
  task_manager_->RemoveResource(resource);
  // Remove it from the provider.
  resources_.erase(iter);
  // Remove it from our pid map.
  std::map<int, TaskManagerChildProcessResource*>::iterator pid_iter =
      pid_to_resources_.find(resource->process_id());
  DCHECK(pid_iter != pid_to_resources_.end());
  if (pid_iter != pid_to_resources_.end())
    pid_to_resources_.erase(pid_iter);

  // Finally, delete the resource.
  delete resource;
}

void TaskManagerChildProcessResourceProvider::AddToTaskManager(
    ChildProcessInfo child_process_info) {
  TaskManagerChildProcessResource* resource =
      new TaskManagerChildProcessResource(child_process_info);
  resources_[child_process_info] = resource;
  pid_to_resources_[resource->process_id()] = resource;
  task_manager_->AddResource(resource);
}

// The ChildProcessInfo::Iterator has to be used from the IO thread.
void TaskManagerChildProcessResourceProvider::RetrieveChildProcessInfo() {
  for (BrowserChildProcessHost::Iterator iter; !iter.Done(); ++iter) {
    // Only add processes which are already started, since we need their handle.
    if ((*iter)->handle() != base::kNullProcessHandle)
      existing_child_process_info_.push_back(**iter);
  }
  // Now notify the UI thread that we have retrieved information about child
  // processes.
  ChromeThread::PostTask(
      ChromeThread::UI, FROM_HERE,
      NewRunnableMethod(this,
          &TaskManagerChildProcessResourceProvider::ChildProcessInfoRetreived));
}

// This is called on the UI thread.
void TaskManagerChildProcessResourceProvider::ChildProcessInfoRetreived() {
  std::vector<ChildProcessInfo>::const_iterator iter;
  for (iter = existing_child_process_info_.begin();
       iter != existing_child_process_info_.end(); ++iter) {
    Add(*iter);
  }
  existing_child_process_info_.clear();
}

////////////////////////////////////////////////////////////////////////////////
// TaskManagerExtensionProcessResource class
////////////////////////////////////////////////////////////////////////////////

SkBitmap* TaskManagerExtensionProcessResource::default_icon_ = NULL;

TaskManagerExtensionProcessResource::TaskManagerExtensionProcessResource(
    ExtensionHost* extension_host)
    : extension_host_(extension_host) {
  if (!default_icon_) {
    ResourceBundle& rb = ResourceBundle::GetSharedInstance();
    default_icon_ = rb.GetBitmapNamed(IDR_PLUGIN);
  }
  process_handle_ = extension_host_->render_process_host()->GetHandle();
  pid_ = base::GetProcId(process_handle_);
  std::wstring extension_name(UTF8ToWide(GetExtension()->name()));
  DCHECK(!extension_name.empty());

  int message_id =
      GetExtension()->is_app() ?
          (extension_host_->profile()->IsOffTheRecord() ?
              IDS_TASK_MANAGER_APP_INCOGNITO_PREFIX :
              IDS_TASK_MANAGER_APP_PREFIX) :
          (extension_host_->profile()->IsOffTheRecord() ?
              IDS_TASK_MANAGER_EXTENSION_INCOGNITO_PREFIX :
              IDS_TASK_MANAGER_EXTENSION_PREFIX);
  title_ = l10n_util::GetStringF(message_id, extension_name);
}

TaskManagerExtensionProcessResource::~TaskManagerExtensionProcessResource() {
}

std::wstring TaskManagerExtensionProcessResource::GetTitle() const {
  return title_;
}

SkBitmap TaskManagerExtensionProcessResource::GetIcon() const {
  return *default_icon_;
}

base::ProcessHandle TaskManagerExtensionProcessResource::GetProcess() const {
  return process_handle_;
}

const Extension* TaskManagerExtensionProcessResource::GetExtension() const {
  return extension_host_->extension();
}

////////////////////////////////////////////////////////////////////////////////
// TaskManagerExtensionProcessResourceProvider class
////////////////////////////////////////////////////////////////////////////////

TaskManagerExtensionProcessResourceProvider::
    TaskManagerExtensionProcessResourceProvider(TaskManager* task_manager)
    : task_manager_(task_manager),
      updating_(false) {
}

TaskManagerExtensionProcessResourceProvider::
    ~TaskManagerExtensionProcessResourceProvider() {
}

TaskManager::Resource* TaskManagerExtensionProcessResourceProvider::GetResource(
    int origin_pid,
    int render_process_host_id,
    int routing_id) {
  std::map<int, TaskManagerExtensionProcessResource*>::iterator iter =
      pid_to_resources_.find(origin_pid);
  if (iter != pid_to_resources_.end())
    return iter->second;
  else
    return NULL;
}

void TaskManagerExtensionProcessResourceProvider::StartUpdating() {
  DCHECK(!updating_);
  updating_ = true;

  // Add all the existing ExtensionHosts.
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  for (ProfileManager::const_iterator it = profile_manager->begin();
       it != profile_manager->end(); ++it) {
    ExtensionProcessManager* process_manager =
        (*it)->GetExtensionProcessManager();
    if (process_manager) {
      ExtensionProcessManager::const_iterator jt;
      for (jt = process_manager->begin(); jt != process_manager->end(); ++jt)
        AddToTaskManager(*jt);
    }

    // If we have an incognito profile active, include the split-mode incognito
    // extensions.
    if (BrowserList::IsOffTheRecordSessionActive()) {
      ExtensionProcessManager* process_manager =
          (*it)->GetOffTheRecordProfile()->GetExtensionProcessManager();
      if (process_manager) {
      ExtensionProcessManager::const_iterator jt;
      for (jt = process_manager->begin(); jt != process_manager->end(); ++jt)
        AddToTaskManager(*jt);
      }
    }
  }

  // Register for notifications about extension process changes.
  registrar_.Add(this, NotificationType::EXTENSION_PROCESS_CREATED,
                 NotificationService::AllSources());
  registrar_.Add(this, NotificationType::EXTENSION_PROCESS_TERMINATED,
                 NotificationService::AllSources());
  registrar_.Add(this, NotificationType::EXTENSION_HOST_DESTROYED,
                 NotificationService::AllSources());
}

void TaskManagerExtensionProcessResourceProvider::StopUpdating() {
  DCHECK(updating_);
  updating_ = false;

  // Unregister for notifications about extension process changes.
  registrar_.Remove(this, NotificationType::EXTENSION_PROCESS_CREATED,
                    NotificationService::AllSources());
  registrar_.Remove(this, NotificationType::EXTENSION_PROCESS_TERMINATED,
                    NotificationService::AllSources());
  registrar_.Remove(this, NotificationType::EXTENSION_HOST_DESTROYED,
                    NotificationService::AllSources());

  // Delete all the resources.
  STLDeleteContainerPairSecondPointers(resources_.begin(), resources_.end());

  resources_.clear();
  pid_to_resources_.clear();
}

void TaskManagerExtensionProcessResourceProvider::Observe(
    NotificationType type,
    const NotificationSource& source,
    const NotificationDetails& details) {
  switch (type.value) {
    case NotificationType::EXTENSION_PROCESS_CREATED:
      AddToTaskManager(Details<ExtensionHost>(details).ptr());
      break;
    case NotificationType::EXTENSION_PROCESS_TERMINATED:
    case NotificationType::EXTENSION_HOST_DESTROYED:
      RemoveFromTaskManager(Details<ExtensionHost>(details).ptr());
      break;
    default:
      NOTREACHED() << "Unexpected notification.";
      return;
  }
}

void TaskManagerExtensionProcessResourceProvider::AddToTaskManager(
    ExtensionHost* extension_host) {
  // Don't add dead extension processes.
  if (!extension_host->IsRenderViewLive())
    return;

  TaskManagerExtensionProcessResource* resource =
      new TaskManagerExtensionProcessResource(extension_host);
  DCHECK(resources_.find(extension_host) == resources_.end());
  resources_[extension_host] = resource;
  pid_to_resources_[resource->process_id()] = resource;
  task_manager_->AddResource(resource);
}

void TaskManagerExtensionProcessResourceProvider::RemoveFromTaskManager(
    ExtensionHost* extension_host) {
  if (!updating_)
    return;
  std::map<ExtensionHost*, TaskManagerExtensionProcessResource*>
      ::iterator iter = resources_.find(extension_host);
  if (iter == resources_.end())
    return;

  // Remove the resource from the Task Manager.
  TaskManagerExtensionProcessResource* resource = iter->second;
  task_manager_->RemoveResource(resource);

  // Remove it from the provider.
  resources_.erase(iter);

  // Remove it from our pid map.
  std::map<int, TaskManagerExtensionProcessResource*>::iterator pid_iter =
      pid_to_resources_.find(resource->process_id());
  DCHECK(pid_iter != pid_to_resources_.end());
  if (pid_iter != pid_to_resources_.end())
    pid_to_resources_.erase(pid_iter);

  // Finally, delete the resource.
  delete resource;
}

////////////////////////////////////////////////////////////////////////////////
// TaskManagerNotificationResource class
////////////////////////////////////////////////////////////////////////////////

SkBitmap* TaskManagerNotificationResource::default_icon_ = NULL;

TaskManagerNotificationResource::TaskManagerNotificationResource(
    BalloonHost* balloon_host)
    : balloon_host_(balloon_host) {
  if (!default_icon_) {
    ResourceBundle& rb = ResourceBundle::GetSharedInstance();
    default_icon_ = rb.GetBitmapNamed(IDR_PLUGIN);
  }
  process_handle_ = balloon_host_->render_view_host()->process()->GetHandle();
  pid_ = base::GetProcId(process_handle_);
  title_ = UTF16ToWide(l10n_util::GetStringFUTF16(
      IDS_TASK_MANAGER_NOTIFICATION_PREFIX,
      balloon_host_->GetSource()));
}

TaskManagerNotificationResource::~TaskManagerNotificationResource() {
}

SkBitmap TaskManagerNotificationResource::GetIcon() const {
  return *default_icon_;
}

base::ProcessHandle TaskManagerNotificationResource::GetProcess() const {
  return process_handle_;
}

////////////////////////////////////////////////////////////////////////////////
// TaskManagerNotificationResourceProvider class
////////////////////////////////////////////////////////////////////////////////

TaskManagerNotificationResourceProvider::
    TaskManagerNotificationResourceProvider(TaskManager* task_manager)
    : task_manager_(task_manager),
      updating_(false) {
}

TaskManagerNotificationResourceProvider::
    ~TaskManagerNotificationResourceProvider() {
}

TaskManager::Resource* TaskManagerNotificationResourceProvider::GetResource(
    int origin_pid,
    int render_process_host_id,
    int routing_id) {
  // TODO(johnnyg): provide resources by pid if necessary.
  return NULL;
}

void TaskManagerNotificationResourceProvider::StartUpdating() {
  DCHECK(!updating_);
  updating_ = true;

  // Add all the existing BalloonHosts.
  BalloonCollection* collection =
      g_browser_process->notification_ui_manager()->balloon_collection();
  const BalloonCollection::Balloons& balloons = collection->GetActiveBalloons();
  for (BalloonCollection::Balloons::const_iterator it = balloons.begin();
       it != balloons.end(); ++it) {
    AddToTaskManager((*it)->view()->GetHost());
  }

  // Register for notifications about extension process changes.
  registrar_.Add(this, NotificationType::NOTIFY_BALLOON_CONNECTED,
                 NotificationService::AllSources());
  registrar_.Add(this, NotificationType::NOTIFY_BALLOON_DISCONNECTED,
                 NotificationService::AllSources());
}

void TaskManagerNotificationResourceProvider::StopUpdating() {
  DCHECK(updating_);
  updating_ = false;

  // Unregister for notifications about extension process changes.
  registrar_.Remove(this, NotificationType::NOTIFY_BALLOON_CONNECTED,
                    NotificationService::AllSources());
  registrar_.Remove(this, NotificationType::NOTIFY_BALLOON_DISCONNECTED,
                    NotificationService::AllSources());

  // Delete all the resources.
  STLDeleteContainerPairSecondPointers(resources_.begin(), resources_.end());
  resources_.clear();
}

void TaskManagerNotificationResourceProvider::Observe(
    NotificationType type,
    const NotificationSource& source,
    const NotificationDetails& details) {
  switch (type.value) {
    case NotificationType::NOTIFY_BALLOON_CONNECTED:
      AddToTaskManager(Source<BalloonHost>(source).ptr());
      break;
    case NotificationType::NOTIFY_BALLOON_DISCONNECTED:
      RemoveFromTaskManager(Source<BalloonHost>(source).ptr());
      break;
    default:
      NOTREACHED() << "Unexpected notification.";
      return;
  }
}

void TaskManagerNotificationResourceProvider::AddToTaskManager(
    BalloonHost* balloon_host) {
  TaskManagerNotificationResource* resource =
      new TaskManagerNotificationResource(balloon_host);
  DCHECK(resources_.find(balloon_host) == resources_.end());
  resources_[balloon_host] = resource;
  task_manager_->AddResource(resource);
}

void TaskManagerNotificationResourceProvider::RemoveFromTaskManager(
    BalloonHost* balloon_host) {
  if (!updating_)
    return;
  std::map<BalloonHost*, TaskManagerNotificationResource*>::iterator iter =
      resources_.find(balloon_host);
  if (iter == resources_.end())
    return;

  // Remove the resource from the Task Manager.
  TaskManagerNotificationResource* resource = iter->second;
  task_manager_->RemoveResource(resource);

  // Remove it from the map.
  resources_.erase(iter);

  // Finally, delete the resource.
  delete resource;
}

////////////////////////////////////////////////////////////////////////////////
// TaskManagerBrowserProcessResource class
////////////////////////////////////////////////////////////////////////////////

SkBitmap* TaskManagerBrowserProcessResource::default_icon_ = NULL;

TaskManagerBrowserProcessResource::TaskManagerBrowserProcessResource()
    : title_() {
  pid_ = base::GetCurrentProcId();
  bool success = base::OpenPrivilegedProcessHandle(pid_, &process_);
  DCHECK(success);
#if defined(OS_WIN)
  if (!default_icon_) {
    HICON icon = GetAppIcon();
    if (icon) {
      ICONINFO icon_info = {0};
      BITMAP bitmap_info = {0};

      GetIconInfo(icon, &icon_info);
      GetObject(icon_info.hbmMask, sizeof(bitmap_info), &bitmap_info);

      gfx::Size icon_size(bitmap_info.bmWidth, bitmap_info.bmHeight);
      default_icon_ = IconUtil::CreateSkBitmapFromHICON(icon, icon_size);
    }
  }
#elif defined(OS_LINUX)
  if (!default_icon_) {
    ResourceBundle& rb = ResourceBundle::GetSharedInstance();
    default_icon_ = rb.GetBitmapNamed(IDR_PRODUCT_LOGO_16);
  }
#elif defined(OS_MACOSX)
  if (!default_icon_) {
    // IDR_PRODUCT_LOGO_16 doesn't quite look like chrome/mac's icns icon. Load
    // the real app icon (requires a nsimage->skbitmap->nsimage conversion :-().
    default_icon_ = new SkBitmap(gfx::AppplicationIconAtSize(16));
  }
#else
  // TODO(port): Port icon code.
  NOTIMPLEMENTED();
#endif  // defined(OS_WIN)
}

TaskManagerBrowserProcessResource::~TaskManagerBrowserProcessResource() {
  base::CloseProcessHandle(process_);
}

// TaskManagerResource methods:
std::wstring TaskManagerBrowserProcessResource::GetTitle() const {
  if (title_.empty()) {
    title_ = l10n_util::GetString(IDS_TASK_MANAGER_WEB_BROWSER_CELL_TEXT);
  }
  return title_;
}

SkBitmap TaskManagerBrowserProcessResource::GetIcon() const {
  return *default_icon_;
}

size_t TaskManagerBrowserProcessResource::SqliteMemoryUsedBytes() const {
  return static_cast<size_t>(sqlite3_memory_used());
}

base::ProcessHandle TaskManagerBrowserProcessResource::GetProcess() const {
  return base::GetCurrentProcessHandle();  // process_;
}

////////////////////////////////////////////////////////////////////////////////
// TaskManagerBrowserProcessResourceProvider class
////////////////////////////////////////////////////////////////////////////////

TaskManagerBrowserProcessResourceProvider::
    TaskManagerBrowserProcessResourceProvider(TaskManager* task_manager)
    : updating_(false),
      task_manager_(task_manager) {
}

TaskManagerBrowserProcessResourceProvider::
    ~TaskManagerBrowserProcessResourceProvider() {
}

TaskManager::Resource* TaskManagerBrowserProcessResourceProvider::GetResource(
    int origin_pid,
    int render_process_host_id,
    int routing_id) {
  if (origin_pid != resource_.process_id()) {
    return NULL;
  }

  return &resource_;
}

void TaskManagerBrowserProcessResourceProvider::StartUpdating() {
  task_manager_->AddResource(&resource_);
}

void TaskManagerBrowserProcessResourceProvider::StopUpdating() {
}