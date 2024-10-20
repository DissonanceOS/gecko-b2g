/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "mozilla/dom/FileSystemRequestParent.h"

#include "CreateDirectoryTask.h"
#include "CreateFileTask.h"
#include "GetDirectoryListingTask.h"
#include "GetFileOrDirectoryTask.h"
#include "RemoveTask.h"
#include "CopyOrMoveToTask.h"
#include "RenameToTask.h"
#include "GetFilesTask.h"

#include "mozilla/dom/BlobImpl.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/FileSystemBase.h"
#include "mozilla/dom/FileSystemSecurity.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "mozilla/dom/OSFileSystem.h"
#include "mozilla/Preferences.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Unused.h"
#include "nsProxyRelease.h"

using namespace mozilla::ipc;

namespace mozilla::dom {

FileSystemRequestParent::FileSystemRequestParent() : mDestroyed(false) {
  AssertIsOnBackgroundThread();
}

FileSystemRequestParent::~FileSystemRequestParent() {
  AssertIsOnBackgroundThread();
}

#define FILESYSTEM_REQUEST_PARENT_DISPATCH_ENTRY(name)                \
  case FileSystemParams::TFileSystem##name##Params: {                 \
    const FileSystem##name##Params& p = aParams;                      \
    mFileSystem = FileSystemBase::DeserializeDOMPath(p.filesystem()); \
    MOZ_ASSERT(mFileSystem);                                          \
    mTask = name##TaskParent::Create(mFileSystem, p, this, rv);       \
    if (NS_WARN_IF(rv.Failed())) {                                    \
      rv.SuppressException();                                         \
      return false;                                                   \
    }                                                                 \
    break;                                                            \
  }

bool FileSystemRequestParent::Initialize(const FileSystemParams& aParams) {
  AssertIsOnBackgroundThread();

  ErrorResult rv;

  switch (aParams.type()) {
    FILESYSTEM_REQUEST_PARENT_DISPATCH_ENTRY(CopyOrMoveTo)
    FILESYSTEM_REQUEST_PARENT_DISPATCH_ENTRY(CreateDirectory)
    FILESYSTEM_REQUEST_PARENT_DISPATCH_ENTRY(CreateFile)
    FILESYSTEM_REQUEST_PARENT_DISPATCH_ENTRY(GetDirectoryListing)
    FILESYSTEM_REQUEST_PARENT_DISPATCH_ENTRY(GetFileOrDirectory)
    FILESYSTEM_REQUEST_PARENT_DISPATCH_ENTRY(GetFiles)
    FILESYSTEM_REQUEST_PARENT_DISPATCH_ENTRY(Remove)
    FILESYSTEM_REQUEST_PARENT_DISPATCH_ENTRY(RenameTo)

    default: {
      MOZ_CRASH("not reached");
      break;
    }
  }

  if (NS_WARN_IF(!mTask || !mFileSystem)) {
    // Should never reach here.
    return false;
  }

  if (mFileSystem->PermissionCheckType() !=
      FileSystemBase::ePermissionCheckNotRequired) {
    nsAutoCString access;
    mTask->GetPermissionAccessType(access);

    mPermissionName = mFileSystem->GetPermission();
    mPermissionName.Append(':');
    mPermissionName.Append(access);
  }

  return true;
}

namespace {

class CheckPermissionRunnable final : public Runnable {
 public:
  CheckPermissionRunnable(
      already_AddRefed<ThreadsafeContentParentHandle> aParent,
      FileSystemRequestParent* aActor, FileSystemTaskParentBase* aTask,
      const nsAString& aPath)
      : Runnable("dom::CheckPermissionRunnable"),
        mContentHandle(aParent),
        mActor(aActor),
        mTask(aTask),
        mPath(aPath),
        mBackgroundEventTarget(GetCurrentSerialEventTarget()) {
    AssertIsInMainProcess();
    AssertIsOnBackgroundThread();

    MOZ_ASSERT(mContentHandle);
    MOZ_ASSERT(mActor);
    MOZ_ASSERT(mTask);
    MOZ_ASSERT(mBackgroundEventTarget);
  }

  NS_IMETHOD
  Run() override {
    if (NS_IsMainThread()) {
      if (!mozilla::Preferences::GetBool("dom.filesystem.pathcheck.disabled",
                                         false)) {
        RefPtr<FileSystemSecurity> fss = FileSystemSecurity::Get();
        if (NS_WARN_IF(!fss || !fss->ContentProcessHasAccessTo(
                                   mContentHandle->ChildID(), mPath))) {
          AssertIsOnMainThread();
          if (RefPtr<ContentParent> contentParent =
                  mContentHandle->GetContentParent()) {
            contentParent->KillHard("This path is not allowed.");
          }
          return NS_OK;
        }
      }

      return mBackgroundEventTarget->Dispatch(this, NS_DISPATCH_NORMAL);
    }

    AssertIsOnBackgroundThread();

    // It can happen that this actor has been destroyed in the meantime we were
    // on the main-thread.
    if (!mActor->Destroyed()) {
      mTask->Start();
    }

    return NS_OK;
  }

 private:
  ~CheckPermissionRunnable() {
    NS_ProxyRelease("CheckPermissionRunnable::mActor", mBackgroundEventTarget,
                    mActor.forget());
  }

  RefPtr<ThreadsafeContentParentHandle> mContentHandle;
  RefPtr<FileSystemRequestParent> mActor;
  RefPtr<FileSystemTaskParentBase> mTask;
  const nsString mPath;

  nsCOMPtr<nsIEventTarget> mBackgroundEventTarget;
};

}  // namespace

void FileSystemRequestParent::Start() {
  AssertIsInMainProcess();
  AssertIsOnBackgroundThread();

  MOZ_ASSERT(!mDestroyed);
  MOZ_ASSERT(mFileSystem);
  MOZ_ASSERT(mTask);

  nsAutoString path;
  if (NS_WARN_IF(NS_FAILED(mTask->GetTargetPath(path)))) {
    (void)Send__delete__(this,
                         FileSystemErrorResponse(NS_ERROR_DOM_SECURITY_ERR));
    return;
  }

  RefPtr<ThreadsafeContentParentHandle> parent =
      BackgroundParent::GetContentParentHandle(Manager());

  // If the ThreadsafeContentParentHandle is null we are dealing with a
  // same-process actor.
  if (!parent) {
    mTask->Start();
    return;
  }

  RefPtr<Runnable> runnable =
      new CheckPermissionRunnable(parent.forget(), this, mTask, path);
  NS_DispatchToMainThread(runnable);
}

void FileSystemRequestParent::ActorDestroy(ActorDestroyReason aWhy) {
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(!mDestroyed);

  if (!mFileSystem) {
    return;
  }

  mFileSystem->Shutdown();
  mFileSystem = nullptr;
  mTask = nullptr;
  mDestroyed = true;
}

}  // namespace mozilla::dom
