// Copyright 2017 The etcd Authors
// Copyright 2017 Wu Tao
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <memory>

#include "exception.h"
#include "logging.h"
#include "storage.h"
#include "unstable.h"

#include <silly/disallow_copying.h>

namespace yaraft {

class RaftLog {
  __DISALLOW_COPYING__(RaftLog);

 public:
  explicit RaftLog(Storage* storage) : storage_(storage), lastApplied_(0) {
    auto s = storage_->FirstIndex();
    FATAL_NOT_OK(s, "Storage::FirstIndex");

    uint64_t firstIndex = s.GetValue();
    commitIndex_ = firstIndex - 1;
    lastApplied_ = firstIndex - 1;

    s = storage_->LastIndex();
    FATAL_NOT_OK(s, "Storage::LastIndex");

    uint64_t lastIndex = s.GetValue();
    unstable_.offset = lastIndex + 1;
  }

  // Raft determines which of two logs is more up-to-date
  // by comparing the index and term of the last entries in the
  // logs. If the logs have last entries with different terms, then
  // the log with the later term is more up-to-date. If the logs
  // end with the same term, then whichever log is longer is
  // more up-to-date. (Raft paper 5.4.1)
  bool IsUpToDate(uint64_t index, uint64_t term) {
    return (term > LastTerm()) || (term == LastTerm() && index >= LastIndex());
  }

  uint64_t CommitIndex() const {
    return commitIndex_;
  }

  StatusWith<uint64_t> Term(uint64_t index) const {
    // the valid index range is [index of dummy entry, last index]
    auto dummyIndex = FirstIndex() - 1;
    if (index > LastIndex() || index < dummyIndex) {
      return Status::Make(Error::OutOfBound);
    }

    uint64_t term = unstable_.MaybeTerm(index);
    if (term) {
      return term;
    }

    auto s = storage_->Term(index);
    if (s.IsOK()) {
      return s.GetValue();
    }

    auto errorCode = s.GetStatus().Code();
    if (errorCode == Error::OutOfBound || errorCode == Error::LogCompacted) {
      return s.GetStatus();
    }

    // unacceptable error
    FATAL_NOT_OK(s, "MemoryStorage::Term");
    return 0;
  }

  uint64_t LastIndex() const {
    if (!unstable_.entries.empty()) {
      return unstable_.entries.rbegin()->index();
    } else if (unstable_.snapshot) {
      return unstable_.snapshot->metadata().index();
    } else {
      auto s = storage_->LastIndex();
      FATAL_NOT_OK(s, "Storage::LastIndex");
      return s.GetValue();
    }
  }

  uint64_t FirstIndex() const {
    if (unstable_.snapshot) {
      // unstable snapshot always precedes all the entries in RaftLog.
      return unstable_.snapshot->metadata().index() + 1;
    }
    auto sw = storage_->FirstIndex();
    FATAL_NOT_OK(sw, "Storage::FirstIndex");
    return sw.GetValue();
  }

  uint64_t LastTerm() const {
    auto s = Term(LastIndex());
    FATAL_NOT_OK(s, "RaftLog::Term");
    return s.GetValue();
  }

  bool HasEntry(uint64_t index, uint64_t term) {
    auto s = Term(index);
    if (s.IsOK()) {
      return s.GetValue() == term;
    }
    return false;
  }

  void Append(pb::Entry e) {
    auto msg = PBMessage().Entries({e}).v;
    Append(msg.mutable_entries()->begin(), msg.mutable_entries()->end());
  }

  void Append(EntryVec vec) {
    auto msg = PBMessage().Entries(vec).v;
    Append(msg.mutable_entries()->begin(), msg.mutable_entries()->end());
  }

  // Appends entries into unstable.
  // Entries between begin and end must be ensured not identical with the existing log entries.
  void Append(EntriesIterator begin, EntriesIterator end) {
    if (begin == end) {
      return;
    }

    if (begin->index() <= commitIndex_) {
#ifdef BUILD_TESTS
      throw RaftError("entry %d conflict with committed entry [committed(%d)]", begin->index(),
                      commitIndex_);
#else
      FMT_LOG(FATAL, "entry %d conflict with committed entry [committed(%d)]", begin->index(),
              commitIndex_);
#endif
    }
    unstable_.TruncateAndAppend(begin, end);
  }

  void CommitTo(uint64_t to) {
    if (to > commitIndex_) {
      if (LastIndex() < to) {
#ifdef BUILD_TESTS
        throw RaftError(
            "tocommit(%d) is out of range [lastIndex(%d)]. Was the raft log corrupted, "
            "truncated, or lost?",
            to, LastIndex());
#else
        FMT_SLOG(FATAL,
                 "tocommit(%d) is out of range [lastIndex(%d)]. Was the raft log corrupted, "
                 "truncated, or lost?",
                 to, LastIndex());
#endif
      }
      commitIndex_ = to;
    }
  }

  // FindConflict finds the index of the conflict.
  // It returns the first pair of conflicting entries between the existing
  // entries and the given entries, if there are any.
  // If there is no conflicting entries, and the existing entries contains
  // all the given entries, `end` will be returned.
  // If there is no conflicting entries, but the given entries contains new
  // entries, the iterator of the first new entry will be returned.
  // An entry is considered to be conflicting if it has the same index but
  // a different term.
  // The first entry MUST have an index equal to the argument 'from'.
  // The index of the given entries MUST be continuously increasing.
  EntriesIterator FindConflict(EntriesIterator begin, EntriesIterator end) {
    for (auto it = begin; it != end; it++) {
      if (!HasEntry(it->index(), it->term())) {
        if (it->index() <= LastIndex()) {
          FMT_SLOG(INFO, "found conflict at index %d [existing term: %d, conflicting term: %d]",
                   it->index(), ZeroTermOnErrCompacted(it->index()), it->term());
        }
        return it;
      }
    }

    // returns `end` if no conflict found
    return end;
  }

  // MaybeAppend returns false and set newLastIndex=0 if the entries cannot be appended. Otherwise,
  // it returns true and set newLastIndex = last index of new entries = prevLogIndex + len(entries).
  bool MaybeAppend(pb::Message& m, uint64_t* newLastIndex) {
    uint64_t prevLogIndex = m.index();
    uint64_t prevLogTerm = m.logterm();

    if (HasEntry(prevLogIndex, prevLogTerm)) {
      *newLastIndex = prevLogIndex + m.entries_size();

      if (m.entries_size() > 0) {
        // An entry in raft log that doesn't exist in MsgApp is defined as conflicted.
        // MaybeAppend deletes the conflicted entry and all that follows it from raft log,
        // and append new entries from MsgApp.
        auto begin = m.mutable_entries()->begin();
        auto end = m.mutable_entries()->end();

        if (UNLIKELY(begin->index() != prevLogIndex + 1)) {
          FMT_LOG(ERROR, "unexpected gap between prevlog and newlog [newlog: {}, prevlog: {}]",
                  begin->index(), prevLogIndex);
        }

        auto conflicted = FindConflict(begin, end);
        if (conflicted != end) {
          Append(conflicted, end);
        }
      }
      return true;
    }
    *newLastIndex = 0;
    return false;
  }

  StatusWith<EntryVec> Entries(uint64_t lo, uint64_t maxSize) {
    uint64_t lastIdx = LastIndex();
    // TODO: test this
    if (lo > LastIndex()) {
      return EntryVec();
    }
    return Entries(lo, lastIdx + 1, maxSize);
  }

  // the caller MUST be RaftLog::Entries(uint64_t lo, uint64_t hi, uint64_t maxSize)
  Status MustCheckOutOfBounds(uint64_t lo, uint64_t hi) {
    if (lo > hi) {
      FMT_SLOG(FATAL, "invalid slice %d > %d", lo, hi);
    }

    uint64_t fi = FirstIndex();
    if (lo < fi) {
      return Status::Make(Error::LogCompacted, "[RaftLog::MustCheckOutOfBound]");
    }

    uint64_t li = LastIndex();
    uint64_t length = li + 1 - fi;
    if (lo < fi || hi > fi + length) {
      FMT_SLOG(FATAL, "slice[%d,%d) out of bound [%d,%d]", lo, hi, fi, li);
    }

    return Status::OK();
  }

  // Returns a slice of log entries from lo through hi-1, inclusive.
  // FirstIndex <= lo < hi <= LastIndex + 1
  StatusWith<EntryVec> Entries(uint64_t lo, uint64_t hi, uint64_t maxSize) {
    RETURN_NOT_OK_APPEND(MustCheckOutOfBounds(lo, hi), "[RaftLog::Entries]");
    if (lo == hi) {
      return EntryVec();
    }

    uint64_t uOffset = unstable_.offset;
    EntryVec ret;

    // retrieve from memory storage
    if (lo < uOffset) {
      auto s = storage_->Entries(lo, std::min(hi, uOffset), &maxSize);

      if (s.GetStatus().Code() == Error::LogCompacted) {
        return s;
      } else {
        FATAL_NOT_OK(s, "[RaftLog::Entries]");
      }
      ret = std::move(s.GetValue());

      // check if ret has reached the size limitation
      if (ret.size() < std::min(hi, uOffset) - lo) {
        return ret;
      }
    }

    // retrieve from unstable
    if (hi > uOffset) {
      lo = std::max(lo, uOffset);
      unstable_.CopyTo(ret, lo, hi, maxSize);
    }

    return ret;
  }

  uint64_t LastApplied() const {
    return lastApplied_;
  }

  void ApplyTo(uint64_t i) {
    LOG_ASSERT(i != 0);
    if (commitIndex_ < i || i < lastApplied_) {
      FMT_SLOG(FATAL, "applied(%d) is out of range [prevApplied(%d), committed(%d)]", i,
               lastApplied_, commitIndex_);
    }
    lastApplied_ = i;
  }

  uint64_t ZeroTermOnErrCompacted(uint64_t index) const {
    auto st = Term(index);
    if (!st.IsOK()) {
      if (st.GetStatus().Code() == Error::LogCompacted) {
        return 0;
      }
      FMT_LOG(FATAL, "fail to get term for index: {}, error: {}, last_index: {}", index,
              st.ToString(), LastIndex());
    }
    return st.GetValue();
  }

  // REQUIRED: snap.metadata().index > CommittedIndex
  // REQUIRED: there's no existing log entry the same as {index: snap.metadata.index, term:
  // snap.metadata.term}.
  void Restore(pb::Snapshot& snap) {
    FMT_SLOG(INFO, "log [%s] starts to restore snapshot [index: %d, term: %d]", ToString(),
             snap.metadata().index(), snap.metadata().term());
    commitIndex_ = snap.metadata().index();
    unstable_.Restore(snap);
  }

  // TODO: use shared_ptr to reduce copying
  StatusWith<pb::Snapshot> Snapshot() const {
    if (unstable_.snapshot) {
      return *unstable_.snapshot;
    }
    return storage_->Snapshot();
  }

  std::string ToString() const {
    return fmt::sprintf("committed=%d, applied=%d, unstable.offset=%d, len(unstable.Entries)=%d",
                        commitIndex_, lastApplied_, unstable_.offset, unstable_.entries.size());
  }

 public:
  /// Used with caution

  EntryVec AllEntries() {
    auto s = Entries(FirstIndex(), std::numeric_limits<uint64_t>::max());
    FATAL_NOT_OK(s, "RaftLog::Entries");
    return s.GetValue();
  }

  Unstable& GetUnstable() {
    return unstable_;
  }

 private:
  friend class RaftLogTest;

  // storage contains all stable entries since the last snapshot.
  std::unique_ptr<Storage> storage_;

  // unstable contains all unstable entries and snapshot.
  // they will be saved into storage.
  Unstable unstable_;

  /// The following variables are volatile states kept on all servers, as referenced in raft paper
  /// Figure 2.
  /// Invariant: commitIndex >= lastApplied.

  // Index of highest log entry known to be committed (initialized to 0, increases monotonically)
  uint64_t commitIndex_;
  // Index of highest log entry applied to state machine (initialized to 0, increases monotonically)
  uint64_t lastApplied_;
};

}  // namespace yaraft
