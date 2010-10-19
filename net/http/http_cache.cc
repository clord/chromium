// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/http/http_cache.h"

#include <algorithm>

#include "base/compiler_specific.h"

#if defined(OS_POSIX)
#include <unistd.h>
#endif

#include "base/callback.h"
#include "base/format_macros.h"
#include "base/message_loop.h"
#include "base/pickle.h"
#include "base/ref_counted.h"
#include "base/stl_util-inl.h"
#include "base/string_number_conversions.h"
#include "base/string_util.h"
#include "base/stringprintf.h"
#include "net/base/io_buffer.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/base/ssl_host_info.h"
#include "net/disk_cache/disk_cache.h"
#include "net/http/disk_cache_based_ssl_host_info.h"
#include "net/http/http_cache_transaction.h"
#include "net/http/http_network_layer.h"
#include "net/http/http_network_session.h"
#include "net/http/http_request_info.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_response_info.h"
#include "net/http/http_util.h"
#include "net/spdy/spdy_session_pool.h"

namespace net {

HttpCache::DefaultBackend::DefaultBackend(CacheType type,
                                          const FilePath& path,
                                          int max_bytes,
                                          base::MessageLoopProxy* thread)
    : type_(type),
      path_(path),
      max_bytes_(max_bytes),
      thread_(thread) {
}

HttpCache::DefaultBackend::~DefaultBackend() {}

// static
HttpCache::BackendFactory* HttpCache::DefaultBackend::InMemory(int max_bytes) {
  return new DefaultBackend(MEMORY_CACHE, FilePath(), max_bytes, NULL);
}

int HttpCache::DefaultBackend::CreateBackend(disk_cache::Backend** backend,
                                             CompletionCallback* callback) {
  DCHECK_GE(max_bytes_, 0);
  return disk_cache::CreateCacheBackend(type_, path_, max_bytes_, true,
                                        thread_, backend, callback);
}

//-----------------------------------------------------------------------------

HttpCache::ActiveEntry::ActiveEntry(disk_cache::Entry* e)
    : disk_entry(e),
      writer(NULL),
      will_process_pending_queue(false),
      doomed(false) {
}

HttpCache::ActiveEntry::~ActiveEntry() {
  if (disk_entry) {
    disk_entry->Close();
    disk_entry = NULL;
  }
}

//-----------------------------------------------------------------------------

// This structure keeps track of work items that are attempting to create or
// open cache entries or the backend itself.
struct HttpCache::PendingOp {
  PendingOp() : disk_entry(NULL), backend(NULL), writer(NULL), callback(NULL) {}
  ~PendingOp() {}

  disk_cache::Entry* disk_entry;
  disk_cache::Backend* backend;
  WorkItem* writer;
  CompletionCallback* callback;  // BackendCallback.
  WorkItemList pending_queue;
};

//-----------------------------------------------------------------------------

// The type of operation represented by a work item.
enum WorkItemOperation {
  WI_CREATE_BACKEND,
  WI_OPEN_ENTRY,
  WI_CREATE_ENTRY,
  WI_DOOM_ENTRY
};

// A work item encapsulates a single request to the backend with all the
// information needed to complete that request.
class HttpCache::WorkItem {
 public:
  WorkItem(WorkItemOperation operation, Transaction* trans, ActiveEntry** entry)
      : operation_(operation), trans_(trans), entry_(entry), callback_(NULL),
        backend_(NULL) {}
  WorkItem(WorkItemOperation operation, Transaction* trans,
           CompletionCallback* cb, disk_cache::Backend** backend)
      : operation_(operation), trans_(trans), entry_(NULL), callback_(cb),
        backend_(backend) {}
  ~WorkItem() {}

  // Calls back the transaction with the result of the operation.
  void NotifyTransaction(int result, ActiveEntry* entry) {
    DCHECK(!entry || entry->disk_entry);
    if (entry_)
      *entry_ = entry;
    if (trans_)
      trans_->io_callback()->Run(result);
  }

  // Notifies the caller about the operation completion. Returns true if the
  // callback was invoked.
  bool DoCallback(int result, disk_cache::Backend* backend) {
    if (backend_)
      *backend_ = backend;
    if (callback_) {
      callback_->Run(result);
      return true;
    }
    return false;
  }

  WorkItemOperation operation() { return operation_; }
  void ClearTransaction() { trans_ = NULL; }
  void ClearEntry() { entry_ = NULL; }
  void ClearCallback() { callback_ = NULL; }
  bool Matches(Transaction* trans) const { return trans == trans_; }
  bool IsValid() const { return trans_ || entry_ || callback_; }

 private:
  WorkItemOperation operation_;
  Transaction* trans_;
  ActiveEntry** entry_;
  CompletionCallback* callback_;  // User callback.
  disk_cache::Backend** backend_;
};

//-----------------------------------------------------------------------------

// This class is a specialized type of CompletionCallback that allows us to
// pass multiple arguments to the completion routine.
class HttpCache::BackendCallback : public CallbackRunner<Tuple1<int> > {
 public:
  BackendCallback(HttpCache* cache, PendingOp* pending_op)
      : cache_(cache), pending_op_(pending_op) {}
  ~BackendCallback() {}

  virtual void RunWithParams(const Tuple1<int>& params) {
    if (cache_) {
      cache_->OnIOComplete(params.a, pending_op_);
    } else {
      // The callback was cancelled so we should delete the pending_op that
      // was used with this callback.
      delete pending_op_;
    }
    delete this;
  }

  void Cancel() {
    cache_ = NULL;
  }

 private:
  HttpCache* cache_;
  PendingOp* pending_op_;
  DISALLOW_COPY_AND_ASSIGN(BackendCallback);
};

//-----------------------------------------------------------------------------

// This class encapsulates a transaction whose only purpose is to write metadata
// to a given entry.
class HttpCache::MetadataWriter {
 public:
  explicit MetadataWriter(HttpCache::Transaction* trans)
      : transaction_(trans),
        ALLOW_THIS_IN_INITIALIZER_LIST(
            callback_(this, &MetadataWriter::OnIOComplete)) {}
  ~MetadataWriter() {}

  // Implementes the bulk of HttpCache::WriteMetadata.
  void Write(const GURL& url, base::Time expected_response_time, IOBuffer* buf,
             int buf_len);

 private:
  void VerifyResponse(int result);
  void SelfDestroy();
  void OnIOComplete(int result);

  scoped_ptr<HttpCache::Transaction> transaction_;
  bool verified_;
  scoped_refptr<IOBuffer> buf_;
  int buf_len_;
  base::Time expected_response_time_;
  CompletionCallbackImpl<MetadataWriter> callback_;
  HttpRequestInfo request_info_;
  DISALLOW_COPY_AND_ASSIGN(MetadataWriter);
};

void HttpCache::MetadataWriter::Write(const GURL& url,
                                      base::Time expected_response_time,
                                      IOBuffer* buf, int buf_len) {
  DCHECK_GT(buf_len, 0);
  DCHECK(buf);
  DCHECK(buf->data());
  request_info_.url = url;
  request_info_.method = "GET";
  request_info_.load_flags = LOAD_ONLY_FROM_CACHE;

  expected_response_time_ = expected_response_time;
  buf_ = buf;
  buf_len_ = buf_len;
  verified_ = false;

  int rv = transaction_->Start(&request_info_, &callback_, BoundNetLog());
  if (rv != ERR_IO_PENDING)
    VerifyResponse(rv);
}

void HttpCache::MetadataWriter::VerifyResponse(int result) {
  verified_ = true;
  if (result != OK)
    return SelfDestroy();

  const HttpResponseInfo* response_info = transaction_->GetResponseInfo();
  DCHECK(response_info->was_cached);
  if (response_info->response_time != expected_response_time_)
    return SelfDestroy();

  result = transaction_->WriteMetadata(buf_, buf_len_, &callback_);
  if (result != ERR_IO_PENDING)
    SelfDestroy();
}

void HttpCache::MetadataWriter::SelfDestroy() {
  delete this;
}

void HttpCache::MetadataWriter::OnIOComplete(int result) {
  if (!verified_)
    return VerifyResponse(result);
  SelfDestroy();
}

//-----------------------------------------------------------------------------

class HttpCache::SSLHostInfoFactoryAdaptor : public SSLHostInfoFactory {
 public:
  SSLHostInfoFactoryAdaptor(HttpCache* http_cache)
      : http_cache_(http_cache) {
  }

  SSLHostInfo* GetForHost(const std::string& hostname) {
    return new DiskCacheBasedSSLHostInfo(hostname, http_cache_);
  }

 private:
  HttpCache* const http_cache_;
};

//-----------------------------------------------------------------------------

HttpCache::HttpCache(HostResolver* host_resolver,
                     DnsRRResolver* dnsrr_resolver,
                     ProxyService* proxy_service,
                     SSLConfigService* ssl_config_service,
                     HttpAuthHandlerFactory* http_auth_handler_factory,
                     HttpNetworkDelegate* network_delegate,
                     NetLog* net_log,
                     BackendFactory* backend_factory)
    : backend_factory_(backend_factory),
      building_backend_(false),
      mode_(NORMAL),
      ssl_host_info_factory_(new SSLHostInfoFactoryAdaptor(
            ALLOW_THIS_IN_INITIALIZER_LIST(this))),
      network_layer_(HttpNetworkLayer::CreateFactory(host_resolver,
          dnsrr_resolver, ssl_host_info_factory_.get(),
          proxy_service, ssl_config_service,
          http_auth_handler_factory, network_delegate, net_log)),
      ALLOW_THIS_IN_INITIALIZER_LIST(task_factory_(this)),
      enable_range_support_(true) {
}

HttpCache::HttpCache(HttpNetworkSession* session,
                     BackendFactory* backend_factory)
    : backend_factory_(backend_factory),
      building_backend_(false),
      mode_(NORMAL),
      network_layer_(HttpNetworkLayer::CreateFactory(session)),
      ALLOW_THIS_IN_INITIALIZER_LIST(task_factory_(this)),
      enable_range_support_(true) {
}

HttpCache::HttpCache(HttpTransactionFactory* network_layer,
                     BackendFactory* backend_factory)
    : backend_factory_(backend_factory),
      building_backend_(false),
      mode_(NORMAL),
      network_layer_(network_layer),
      ALLOW_THIS_IN_INITIALIZER_LIST(task_factory_(this)),
      enable_range_support_(true) {
}

HttpCache::~HttpCache() {
  // If we have any active entries remaining, then we need to deactivate them.
  // We may have some pending calls to OnProcessPendingQueue, but since those
  // won't run (due to our destruction), we can simply ignore the corresponding
  // will_process_pending_queue flag.
  while (!active_entries_.empty()) {
    ActiveEntry* entry = active_entries_.begin()->second;
    entry->will_process_pending_queue = false;
    entry->pending_queue.clear();
    entry->readers.clear();
    entry->writer = NULL;
    DeactivateEntry(entry);
  }

  STLDeleteElements(&doomed_entries_);

  PendingOpsMap::iterator pending_it = pending_ops_.begin();
  for (; pending_it != pending_ops_.end(); ++pending_it) {
    // We are not notifying the transactions about the cache going away, even
    // though they are waiting for a callback that will never fire.
    PendingOp* pending_op = pending_it->second;
    delete pending_op->writer;
    bool delete_pending_op = true;
    if (building_backend_) {
      // If we don't have a backend, when its construction finishes it will
      // deliver the callbacks.
      BackendCallback* callback =
          static_cast<BackendCallback*>(pending_op->callback);
      if (callback) {
        // The callback will delete the pending operation.
        callback->Cancel();
        delete_pending_op = false;
      }
    } else {
      delete pending_op->callback;
    }

    STLDeleteElements(&pending_op->pending_queue);
    if (delete_pending_op)
      delete pending_op;
  }
}

int HttpCache::GetBackend(disk_cache::Backend** backend,
                          CompletionCallback* callback) {
  DCHECK(callback != NULL);

  if (disk_cache_.get()) {
    *backend = disk_cache_.get();
    return OK;
  }

  return CreateBackend(backend, callback);
}

disk_cache::Backend* HttpCache::GetCurrentBackend() {
  return disk_cache_.get();
}

int HttpCache::CreateTransaction(scoped_ptr<HttpTransaction>* trans) {
  // Do lazy initialization of disk cache if needed.
  if (!disk_cache_.get())
    CreateBackend(NULL, NULL);  // We don't care about the result.

  trans->reset(new HttpCache::Transaction(this, enable_range_support_));
  return OK;
}

HttpCache* HttpCache::GetCache() {
  return this;
}

HttpNetworkSession* HttpCache::GetSession() {
  net::HttpNetworkLayer* network =
      static_cast<net::HttpNetworkLayer*>(network_layer_.get());
  return network->GetSession();
}

void HttpCache::Suspend(bool suspend) {
  network_layer_->Suspend(suspend);
}

// static
bool HttpCache::ParseResponseInfo(const char* data, int len,
                                  HttpResponseInfo* response_info,
                                  bool* response_truncated) {
  // This block is here just to debug a Linux-only crash (bug 56449).
  // TODO(rvargas): Remove this.
  if (len < 4)
    return false;
  int payload_size = *reinterpret_cast<const int*>(data);
  if (payload_size < 4)
    return false;

  Pickle pickle(data, len);
  return response_info->InitFromPickle(pickle, response_truncated);
}

void HttpCache::WriteMetadata(const GURL& url,
                              base::Time expected_response_time, IOBuffer* buf,
                              int buf_len) {
  if (!buf_len)
    return;

  // Do lazy initialization of disk cache if needed.
  if (!disk_cache_.get())
    CreateBackend(NULL, NULL);  // We don't care about the result.

  HttpCache::Transaction* trans =
      new HttpCache::Transaction(this, enable_range_support_);
  MetadataWriter* writer = new MetadataWriter(trans);

  // The writer will self destruct when done.
  writer->Write(url, expected_response_time, buf, buf_len);
}

void HttpCache::CloseCurrentConnections() {
  net::HttpNetworkLayer* network =
      static_cast<net::HttpNetworkLayer*>(network_layer_.get());
  HttpNetworkSession* session = network->GetSession();
  if (session) {
    session->FlushSocketPools();
    if (session->spdy_session_pool())
      session->spdy_session_pool()->CloseCurrentSessions();
  }
}

//-----------------------------------------------------------------------------

int HttpCache::CreateBackend(disk_cache::Backend** backend,
                             CompletionCallback* callback) {
  if (!backend_factory_.get())
    return ERR_FAILED;

  building_backend_ = true;

  scoped_ptr<WorkItem> item(new WorkItem(WI_CREATE_BACKEND, NULL, callback,
                                         backend));

  // This is the only operation that we can do that is not related to any given
  // entry, so we use an empty key for it.
  PendingOp* pending_op = GetPendingOp("");
  if (pending_op->writer) {
    if (callback)
      pending_op->pending_queue.push_back(item.release());
    return ERR_IO_PENDING;
  }

  DCHECK(pending_op->pending_queue.empty());

  pending_op->writer = item.release();
  BackendCallback* my_callback = new BackendCallback(this, pending_op);
  pending_op->callback = my_callback;

  int rv = backend_factory_->CreateBackend(&pending_op->backend, my_callback);
  if (rv != ERR_IO_PENDING) {
    pending_op->writer->ClearCallback();
    my_callback->Run(rv);
  }

  return rv;
}

int HttpCache::GetBackendForTransaction(Transaction* trans) {
  if (disk_cache_.get())
    return OK;

  if (!building_backend_)
    return ERR_FAILED;

  WorkItem* item = new WorkItem(WI_CREATE_BACKEND, trans, NULL, NULL);
  PendingOp* pending_op = GetPendingOp("");
  DCHECK(pending_op->writer);
  pending_op->pending_queue.push_back(item);
  return ERR_IO_PENDING;
}

// Generate a key that can be used inside the cache.
std::string HttpCache::GenerateCacheKey(const HttpRequestInfo* request) {
  // Strip out the reference, username, and password sections of the URL.
  std::string url = HttpUtil::SpecForRequest(request->url);

  DCHECK(mode_ != DISABLE);
  if (mode_ == NORMAL) {
    // No valid URL can begin with numerals, so we should not have to worry
    // about collisions with normal URLs.
    if (request->upload_data && request->upload_data->identifier()) {
      url.insert(0, base::StringPrintf("%" PRId64 "/",
                                       request->upload_data->identifier()));
    }
    return url;
  }

  // In playback and record mode, we cache everything.

  // Lazily initialize.
  if (playback_cache_map_ == NULL)
    playback_cache_map_.reset(new PlaybackCacheMap());

  // Each time we request an item from the cache, we tag it with a
  // generation number.  During playback, multiple fetches for the same
  // item will use the same generation number and pull the proper
  // instance of an URL from the cache.
  int generation = 0;
  DCHECK(playback_cache_map_ != NULL);
  if (playback_cache_map_->find(url) != playback_cache_map_->end())
    generation = (*playback_cache_map_)[url];
  (*playback_cache_map_)[url] = generation + 1;

  // The key into the cache is GENERATION # + METHOD + URL.
  std::string result = base::IntToString(generation);
  result.append(request->method);
  result.append(url);
  return result;
}

int HttpCache::DoomEntry(const std::string& key, Transaction* trans) {
  // Need to abandon the ActiveEntry, but any transaction attached to the entry
  // should not be impacted.  Dooming an entry only means that it will no
  // longer be returned by FindActiveEntry (and it will also be destroyed once
  // all consumers are finished with the entry).
  ActiveEntriesMap::iterator it = active_entries_.find(key);
  if (it == active_entries_.end()) {
    return AsyncDoomEntry(key, trans);
  }

  ActiveEntry* entry = it->second;
  active_entries_.erase(it);

  // We keep track of doomed entries so that we can ensure that they are
  // cleaned up properly when the cache is destroyed.
  doomed_entries_.insert(entry);

  entry->disk_entry->Doom();
  entry->doomed = true;

  DCHECK(entry->writer || !entry->readers.empty());
  return OK;
}

int HttpCache::AsyncDoomEntry(const std::string& key, Transaction* trans) {
  DCHECK(trans);
  WorkItem* item = new WorkItem(WI_DOOM_ENTRY, trans, NULL);
  PendingOp* pending_op = GetPendingOp(key);
  if (pending_op->writer) {
    pending_op->pending_queue.push_back(item);
    return ERR_IO_PENDING;
  }

  DCHECK(pending_op->pending_queue.empty());

  pending_op->writer = item;
  BackendCallback* my_callback = new BackendCallback(this, pending_op);
  pending_op->callback = my_callback;

  int rv = disk_cache_->DoomEntry(key, my_callback);
  if (rv != ERR_IO_PENDING) {
    item->ClearTransaction();
    my_callback->Run(rv);
  }

  return rv;
}

void HttpCache::FinalizeDoomedEntry(ActiveEntry* entry) {
  DCHECK(entry->doomed);
  DCHECK(!entry->writer);
  DCHECK(entry->readers.empty());
  DCHECK(entry->pending_queue.empty());

  ActiveEntriesSet::iterator it = doomed_entries_.find(entry);
  DCHECK(it != doomed_entries_.end());
  doomed_entries_.erase(it);

  delete entry;
}

HttpCache::ActiveEntry* HttpCache::FindActiveEntry(const std::string& key) {
  ActiveEntriesMap::const_iterator it = active_entries_.find(key);
  return it != active_entries_.end() ? it->second : NULL;
}

HttpCache::ActiveEntry* HttpCache::ActivateEntry(
    const std::string& key,
    disk_cache::Entry* disk_entry) {
  DCHECK(!FindActiveEntry(key));
  ActiveEntry* entry = new ActiveEntry(disk_entry);
  active_entries_[key] = entry;
  return entry;
}

void HttpCache::DeactivateEntry(ActiveEntry* entry) {
  DCHECK(!entry->will_process_pending_queue);
  DCHECK(!entry->doomed);
  DCHECK(!entry->writer);
  DCHECK(entry->disk_entry);
  DCHECK(entry->readers.empty());
  DCHECK(entry->pending_queue.empty());

  std::string key = entry->disk_entry->GetKey();
  if (key.empty())
    return SlowDeactivateEntry(entry);

  ActiveEntriesMap::iterator it = active_entries_.find(key);
  DCHECK(it != active_entries_.end());
  DCHECK(it->second == entry);

  active_entries_.erase(it);
  delete entry;
}

// We don't know this entry's key so we have to find it without it.
void HttpCache::SlowDeactivateEntry(ActiveEntry* entry) {
  for (ActiveEntriesMap::iterator it = active_entries_.begin();
       it != active_entries_.end(); ++it) {
    if (it->second == entry) {
      active_entries_.erase(it);
      delete entry;
      break;
    }
  }
}

HttpCache::PendingOp* HttpCache::GetPendingOp(const std::string& key) {
  DCHECK(!FindActiveEntry(key));

  PendingOpsMap::const_iterator it = pending_ops_.find(key);
  if (it != pending_ops_.end())
    return it->second;

  PendingOp* operation = new PendingOp();
  pending_ops_[key] = operation;
  return operation;
}

void HttpCache::DeletePendingOp(PendingOp* pending_op) {
  std::string key;
  if (pending_op->disk_entry)
    key = pending_op->disk_entry->GetKey();

  if (!key.empty()) {
    PendingOpsMap::iterator it = pending_ops_.find(key);
    DCHECK(it != pending_ops_.end());
    pending_ops_.erase(it);
  } else {
    for (PendingOpsMap::iterator it = pending_ops_.begin();
         it != pending_ops_.end(); ++it) {
      if (it->second == pending_op) {
        pending_ops_.erase(it);
        break;
      }
    }
  }
  DCHECK(pending_op->pending_queue.empty());

  delete pending_op;
}

int HttpCache::OpenEntry(const std::string& key, ActiveEntry** entry,
                         Transaction* trans) {
  ActiveEntry* active_entry = FindActiveEntry(key);
  if (active_entry) {
    *entry = active_entry;
    return OK;
  }

  WorkItem* item = new WorkItem(WI_OPEN_ENTRY, trans, entry);
  PendingOp* pending_op = GetPendingOp(key);
  if (pending_op->writer) {
    pending_op->pending_queue.push_back(item);
    return ERR_IO_PENDING;
  }

  DCHECK(pending_op->pending_queue.empty());

  pending_op->writer = item;
  BackendCallback* my_callback = new BackendCallback(this, pending_op);
  pending_op->callback = my_callback;

  int rv = disk_cache_->OpenEntry(key, &(pending_op->disk_entry), my_callback);
  if (rv != ERR_IO_PENDING) {
    item->ClearTransaction();
    my_callback->Run(rv);
  }

  return rv;
}

int HttpCache::CreateEntry(const std::string& key, ActiveEntry** entry,
                           Transaction* trans) {
  DCHECK(!FindActiveEntry(key));

  WorkItem* item = new WorkItem(WI_CREATE_ENTRY, trans, entry);
  PendingOp* pending_op = GetPendingOp(key);
  if (pending_op->writer) {
    pending_op->pending_queue.push_back(item);
    return ERR_IO_PENDING;
  }

  DCHECK(pending_op->pending_queue.empty());

  pending_op->writer = item;
  BackendCallback* my_callback = new BackendCallback(this, pending_op);
  pending_op->callback = my_callback;

  int rv = disk_cache_->CreateEntry(key, &(pending_op->disk_entry),
                                    my_callback);
  if (rv != ERR_IO_PENDING) {
    item->ClearTransaction();
    my_callback->Run(rv);
  }

  return rv;
}

void HttpCache::DestroyEntry(ActiveEntry* entry) {
  if (entry->doomed) {
    FinalizeDoomedEntry(entry);
  } else {
    DeactivateEntry(entry);
  }
}

int HttpCache::AddTransactionToEntry(ActiveEntry* entry, Transaction* trans) {
  DCHECK(entry);
  DCHECK(entry->disk_entry);

  // We implement a basic reader/writer lock for the disk cache entry.  If
  // there is already a writer, then everyone has to wait for the writer to
  // finish before they can access the cache entry.  There can be multiple
  // readers.
  //
  // NOTE: If the transaction can only write, then the entry should not be in
  // use (since any existing entry should have already been doomed).

  if (entry->writer || entry->will_process_pending_queue) {
    entry->pending_queue.push_back(trans);
    return ERR_IO_PENDING;
  }

  if (trans->mode() & Transaction::WRITE) {
    // transaction needs exclusive access to the entry
    if (entry->readers.empty()) {
      entry->writer = trans;
    } else {
      entry->pending_queue.push_back(trans);
      return ERR_IO_PENDING;
    }
  } else {
    // transaction needs read access to the entry
    entry->readers.push_back(trans);
  }

  // We do this before calling EntryAvailable to force any further calls to
  // AddTransactionToEntry to add their transaction to the pending queue, which
  // ensures FIFO ordering.
  if (!entry->writer && !entry->pending_queue.empty())
    ProcessPendingQueue(entry);

  return OK;
}

void HttpCache::DoneWithEntry(ActiveEntry* entry, Transaction* trans,
                              bool cancel) {
  // If we already posted a task to move on to the next transaction and this was
  // the writer, there is nothing to cancel.
  if (entry->will_process_pending_queue && entry->readers.empty())
    return;

  if (entry->writer) {
    DCHECK(trans == entry->writer);

    // Assume there was a failure.
    bool success = false;
    if (cancel) {
      DCHECK(entry->disk_entry);
      // This is a successful operation in the sense that we want to keep the
      // entry.
      success = trans->AddTruncatedFlag();
    }
    DoneWritingToEntry(entry, success);
  } else {
    DoneReadingFromEntry(entry, trans);
  }
}

void HttpCache::DoneWritingToEntry(ActiveEntry* entry, bool success) {
  DCHECK(entry->readers.empty());

  entry->writer = NULL;

  if (success) {
    ProcessPendingQueue(entry);
  } else {
    DCHECK(!entry->will_process_pending_queue);

    // We failed to create this entry.
    TransactionList pending_queue;
    pending_queue.swap(entry->pending_queue);

    entry->disk_entry->Doom();
    DestroyEntry(entry);

    // We need to do something about these pending entries, which now need to
    // be added to a new entry.
    while (!pending_queue.empty()) {
      // ERR_CACHE_RACE causes the transaction to restart the whole process.
      pending_queue.front()->io_callback()->Run(ERR_CACHE_RACE);
      pending_queue.pop_front();
    }
  }
}

void HttpCache::DoneReadingFromEntry(ActiveEntry* entry, Transaction* trans) {
  DCHECK(!entry->writer);

  TransactionList::iterator it =
      std::find(entry->readers.begin(), entry->readers.end(), trans);
  DCHECK(it != entry->readers.end());

  entry->readers.erase(it);

  ProcessPendingQueue(entry);
}

void HttpCache::ConvertWriterToReader(ActiveEntry* entry) {
  DCHECK(entry->writer);
  DCHECK(entry->writer->mode() == Transaction::READ_WRITE);
  DCHECK(entry->readers.empty());

  Transaction* trans = entry->writer;

  entry->writer = NULL;
  entry->readers.push_back(trans);

  ProcessPendingQueue(entry);
}

LoadState HttpCache::GetLoadStateForPendingTransaction(
      const Transaction* trans) {
  ActiveEntriesMap::const_iterator i = active_entries_.find(trans->key());
  if (i == active_entries_.end()) {
    // If this is really a pending transaction, and it is not part of
    // active_entries_, we should be creating the backend or the entry.
    return LOAD_STATE_WAITING_FOR_CACHE;
  }

  Transaction* writer = i->second->writer;
  return writer ? writer->GetWriterLoadState() : LOAD_STATE_WAITING_FOR_CACHE;
}

void HttpCache::RemovePendingTransaction(Transaction* trans) {
  ActiveEntriesMap::const_iterator i = active_entries_.find(trans->key());
  bool found = false;
  if (i != active_entries_.end())
    found = RemovePendingTransactionFromEntry(i->second, trans);

  if (found)
    return;

  if (building_backend_) {
    PendingOpsMap::const_iterator j = pending_ops_.find("");
    if (j != pending_ops_.end())
      found = RemovePendingTransactionFromPendingOp(j->second, trans);

    if (found)
      return;
  }

  PendingOpsMap::const_iterator j = pending_ops_.find(trans->key());
  if (j != pending_ops_.end())
    found = RemovePendingTransactionFromPendingOp(j->second, trans);

  if (found)
    return;

  ActiveEntriesSet::iterator k = doomed_entries_.begin();
  for (; k != doomed_entries_.end() && !found; ++k)
    found = RemovePendingTransactionFromEntry(*k, trans);

  DCHECK(found) << "Pending transaction not found";
}

bool HttpCache::RemovePendingTransactionFromEntry(ActiveEntry* entry,
                                                  Transaction* trans) {
  TransactionList& pending_queue = entry->pending_queue;

  TransactionList::iterator j =
      find(pending_queue.begin(), pending_queue.end(), trans);
  if (j == pending_queue.end())
    return false;

  pending_queue.erase(j);
  return true;
}

bool HttpCache::RemovePendingTransactionFromPendingOp(PendingOp* pending_op,
                                                      Transaction* trans) {
  if (pending_op->writer->Matches(trans)) {
    pending_op->writer->ClearTransaction();
    pending_op->writer->ClearEntry();
    return true;
  }
  WorkItemList& pending_queue = pending_op->pending_queue;

  WorkItemList::iterator it = pending_queue.begin();
  for (; it != pending_queue.end(); ++it) {
    if ((*it)->Matches(trans)) {
      delete *it;
      pending_queue.erase(it);
      return true;
    }
  }
  return false;
}

void HttpCache::ProcessPendingQueue(ActiveEntry* entry) {
  // Multiple readers may finish with an entry at once, so we want to batch up
  // calls to OnProcessPendingQueue.  This flag also tells us that we should
  // not delete the entry before OnProcessPendingQueue runs.
  if (entry->will_process_pending_queue)
    return;
  entry->will_process_pending_queue = true;

  MessageLoop::current()->PostTask(
      FROM_HERE,
      task_factory_.NewRunnableMethod(&HttpCache::OnProcessPendingQueue,
                                      entry));
}

void HttpCache::OnProcessPendingQueue(ActiveEntry* entry) {
  entry->will_process_pending_queue = false;
  DCHECK(!entry->writer);

  // If no one is interested in this entry, then we can de-activate it.
  if (entry->pending_queue.empty()) {
    if (entry->readers.empty())
      DestroyEntry(entry);
    return;
  }

  // Promote next transaction from the pending queue.
  Transaction* next = entry->pending_queue.front();
  if ((next->mode() & Transaction::WRITE) && !entry->readers.empty())
    return;  // Have to wait.

  entry->pending_queue.erase(entry->pending_queue.begin());

  int rv = AddTransactionToEntry(entry, next);
  if (rv != ERR_IO_PENDING) {
    next->io_callback()->Run(rv);
  }
}

void HttpCache::OnIOComplete(int result, PendingOp* pending_op) {
  WorkItemOperation op = pending_op->writer->operation();

  // Completing the creation of the backend is simpler than the other cases.
  if (op == WI_CREATE_BACKEND)
    return OnBackendCreated(result, pending_op);

  scoped_ptr<WorkItem> item(pending_op->writer);
  bool fail_requests = false;

  ActiveEntry* entry = NULL;
  std::string key;
  if (result == OK) {
    if (op == WI_DOOM_ENTRY) {
      // Anything after a Doom has to be restarted.
      fail_requests = true;
    } else if (item->IsValid()) {
      key = pending_op->disk_entry->GetKey();
      entry = ActivateEntry(key, pending_op->disk_entry);
    } else {
      // The writer transaction is gone.
      if (op == WI_CREATE_ENTRY)
        pending_op->disk_entry->Doom();
      pending_op->disk_entry->Close();
      pending_op->disk_entry = NULL;
      fail_requests = true;
    }
  }

  // We are about to notify a bunch of transactions, and they may decide to
  // re-issue a request (or send a different one). If we don't delete
  // pending_op, the new request will be appended to the end of the list, and
  // we'll see it again from this point before it has a chance to complete (and
  // we'll be messing out the request order). The down side is that if for some
  // reason notifying request A ends up cancelling request B (for the same key),
  // we won't find request B anywhere (because it would be in a local variable
  // here) and that's bad. If there is a chance for that to happen, we'll have
  // to move the callback used to be a CancelableCallback. By the way, for this
  // to happen the action (to cancel B) has to be synchronous to the
  // notification for request A.
  WorkItemList pending_items;
  pending_items.swap(pending_op->pending_queue);
  DeletePendingOp(pending_op);

  item->NotifyTransaction(result, entry);

  while (!pending_items.empty()) {
    item.reset(pending_items.front());
    pending_items.pop_front();

    if (item->operation() == WI_DOOM_ENTRY) {
      // A queued doom request is always a race.
      fail_requests = true;
    } else if (result == OK) {
      entry = FindActiveEntry(key);
      if (!entry)
        fail_requests = true;
    }

    if (fail_requests) {
      item->NotifyTransaction(ERR_CACHE_RACE, NULL);
      continue;
    }

    if (item->operation() == WI_CREATE_ENTRY) {
      if (result == OK) {
        // A second Create request, but the first request succeded.
        item->NotifyTransaction(ERR_CACHE_CREATE_FAILURE, NULL);
      } else {
        if (op != WI_CREATE_ENTRY) {
          // Failed Open followed by a Create.
          item->NotifyTransaction(ERR_CACHE_RACE, NULL);
          fail_requests = true;
        } else {
          item->NotifyTransaction(result, entry);
        }
      }
    } else {
      if (op == WI_CREATE_ENTRY && result != OK) {
        // Failed Create followed by an Open.
        item->NotifyTransaction(ERR_CACHE_RACE, NULL);
        fail_requests = true;
      } else {
        item->NotifyTransaction(result, entry);
      }
    }
  }
}

void HttpCache::OnBackendCreated(int result, PendingOp* pending_op) {
  scoped_ptr<WorkItem> item(pending_op->writer);
  WorkItemOperation op = item->operation();
  DCHECK_EQ(WI_CREATE_BACKEND, op);

  // We don't need the callback anymore.
  pending_op->callback = NULL;
  disk_cache::Backend* backend = pending_op->backend;

  if (backend_factory_.get()) {
    // We may end up calling OnBackendCreated multiple times if we have pending
    // work items. The first call saves the backend and releases the factory,
    // and the last call clears building_backend_.
    backend_factory_.reset();  // Reclaim memory.
    if (result == OK)
      disk_cache_.reset(backend);
  }

  if (!pending_op->pending_queue.empty()) {
    WorkItem* pending_item = pending_op->pending_queue.front();
    pending_op->pending_queue.pop_front();
    DCHECK_EQ(WI_CREATE_BACKEND, pending_item->operation());

    // We want to process a single callback at a time, because the cache may
    // go away from the callback.
    pending_op->writer = pending_item;

    MessageLoop::current()->PostTask(
        FROM_HERE,
        task_factory_.NewRunnableMethod(&HttpCache::OnBackendCreated,
                                        result, pending_op));
  } else {
    building_backend_ = false;
    DeletePendingOp(pending_op);
  }

  // The cache may be gone when we return from the callback.
  if (!item->DoCallback(result, backend))
    item->NotifyTransaction(result, NULL);
}

}  // namespace net
