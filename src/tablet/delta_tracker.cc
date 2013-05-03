// Copyright (c) 2013, Cloudera, inc.
// All rights reserved.

#include <boost/thread/shared_mutex.hpp>
#include <string>
#include <tr1/memory>

#include "gutil/strings/strip.h"
#include "util/env.h"
#include "util/env_util.h"
#include "util/status.h"
#include "tablet/deltafile.h"
#include "tablet/delta_tracker.h"
#include "tablet/layer.h"

namespace kudu { namespace tablet {

using std::string;
using std::tr1::shared_ptr;


DeltaTracker::DeltaTracker(Env *env,
                           const Schema &schema,
                           const string &dir) :
  env_(env),
  schema_(schema),
  dir_(dir),
  open_(false),
  next_deltafile_idx_(0),
  dms_(new DeltaMemStore(schema))
{}


// Open any previously flushed DeltaFiles in this layer
Status DeltaTracker::Open() {
  CHECK(delta_trackers_.empty()) << "should call before opening any readers";
  CHECK(!open_);

  vector<string> children;
  RETURN_NOT_OK(env_->GetChildren(dir_, &children));
  BOOST_FOREACH(const string &child, children) {
    // Skip hidden files (also '.' and '..')
    if (child[0] == '.') continue;

    string absolute_path = env_->JoinPathSegments(dir_, child);

    string suffix;
    if (TryStripPrefixString(child, Layer::kDeltaPrefix, &suffix)) {
      // The file should be named 'delta_<N>'. N here is the index
      // of the delta file (indicating the order in which it was flushed).
      uint32_t deltafile_idx;
      if (!safe_strtou32(suffix.c_str(), &deltafile_idx)) {
        return Status::IOError(string("Bad delta file: ") + absolute_path);
      }

      gscoped_ptr<DeltaFileReader> dfr;
      Status s = DeltaFileReader::Open(env_, absolute_path, schema_, &dfr);
      if (!s.ok()) {
        LOG(ERROR) << "Failed to open delta file " << absolute_path << ": "
                   << s.ToString();
        return s;
      }
      LOG(INFO) << "Successfully opened delta file " << absolute_path;

      delta_trackers_.push_back(shared_ptr<DeltaTrackerInterface>(dfr.release()));

      next_deltafile_idx_ = std::max(next_deltafile_idx_,
                                     deltafile_idx + 1);
    } else if (TryStripPrefixString(child, Layer::kColumnPrefix, &suffix)) {
      // expected: column data
    } else {
      LOG(WARNING) << "ignoring unknown file: " << absolute_path;
    }
  }

  return Status::OK();
}


void DeltaTracker::CollectTrackers(vector<shared_ptr<DeltaTrackerInterface> > *deltas) const {
  boost::lock_guard<boost::shared_mutex> lock(component_lock_);
  deltas->assign(delta_trackers_.begin(), delta_trackers_.end());
  deltas->push_back(dms_);
}


ColumnwiseIterator *DeltaTracker::WrapIterator(const shared_ptr<ColumnwiseIterator> &base,
                                               const MvccSnapshot &mvcc_snap) const
{
  std::vector<shared_ptr<DeltaTrackerInterface> > deltas;
  CollectTrackers(&deltas);
  shared_ptr<DeltaIteratorInterface> merged_deltas(
    DeltaIteratorMerger::Create(deltas, base->schema(), mvcc_snap));

  return new DeltaApplier<ColumnwiseIterator>(base, merged_deltas);
}

RowwiseIterator *DeltaTracker::WrapIterator(const shared_ptr<RowwiseIterator> &base,
                                            const MvccSnapshot &mvcc_snap) const
{
  std::vector<shared_ptr<DeltaTrackerInterface> > deltas;
  CollectTrackers(&deltas);
  shared_ptr<DeltaIteratorInterface> merged_deltas(
    DeltaIteratorMerger::Create(deltas, base->schema(), mvcc_snap));

  return new DeltaApplier<RowwiseIterator>(base, merged_deltas);
}


void DeltaTracker::Update(txid_t txid, rowid_t row_idx, const RowChangeList &update) {
  // TODO: can probably lock this more fine-grained.
  boost::shared_lock<boost::shared_mutex> lock(component_lock_);

  dms_->Update(txid, row_idx, update);
}


Status DeltaTracker::FlushDMS(const DeltaMemStore &dms,
                              gscoped_ptr<DeltaFileReader> *dfr) {
  int deltafile_idx = next_deltafile_idx_++;
  string path = Layer::GetDeltaPath(dir_, deltafile_idx);

  // Open file for write.
  shared_ptr<WritableFile> out;
  Status s = env_util::OpenFileForWrite(env_, path, &out);
  if (!s.ok()) {
    LOG(WARNING) << "Unable to open output file for delta level " <<
      deltafile_idx << " at path " << path << ": " << 
      s.ToString();
    return s;
  }
  DeltaFileWriter dfw(schema_, out);

  s = dfw.Start();
  if (!s.ok()) {
    LOG(WARNING) << "Unable to start delta file writer for path " <<
      path;
    return s;
  }
  RETURN_NOT_OK(dms.FlushToFile(&dfw));
  RETURN_NOT_OK(dfw.Finish());
  LOG(INFO) << "Flushed delta file: " << path;

  // Now re-open for read
  RETURN_NOT_OK(DeltaFileReader::Open(env_, path, schema_, dfr));
  LOG(INFO) << "Reopened delta file for read: " << path;

  return Status::OK();
}

Status DeltaTracker::Flush() {
  // First, swap out the old DeltaMemStore with a new one,
  // and add it to the list of delta trackers to be reflected
  // in reads.
  shared_ptr<DeltaMemStore> old_dms;
  size_t count;
  {
    // Lock the component_lock_ in exclusive mode.
    // This shuts out any concurrent readers or writers.
    boost::lock_guard<boost::shared_mutex> lock(component_lock_);

    count = dms_->Count();
    if (count == 0) {
      // No need to flush if there are no deltas.
      return Status::OK();
    }

    old_dms = dms_;
    dms_.reset(new DeltaMemStore(schema_));

    delta_trackers_.push_back(old_dms);
  }

  LOG(INFO) << "Flushing " << count << " deltas...";

  // Now, actually flush the contents of the old DMS.
  // TODO: need another lock to prevent concurrent flushers
  // at some point.
  gscoped_ptr<DeltaFileReader> dfr;
  Status s = FlushDMS(*old_dms, &dfr);
  CHECK(s.ok())
    << "Failed to flush DMS: " << s.ToString()
    << "\nTODO: need to figure out what to do with error handling "
    << "if this fails -- we end up with a DeltaMemStore permanently "
    << "in the tracker list. For now, abort.";


  // Now, re-take the lock and swap in the DeltaFileReader in place of
  // of the DeltaMemStore
  {
    boost::lock_guard<boost::shared_mutex> lock(component_lock_);
    size_t idx = delta_trackers_.size() - 1;

    CHECK_EQ(delta_trackers_[idx], old_dms)
      << "Another thread modified the delta tracker list during flush";
    delta_trackers_[idx].reset(dfr.release());
  }

  return Status::OK();

  // TODO: wherever we write stuff, we should write to a tmp path
  // and rename to final path!
}

////////////////////////////////////////////////////////////
// Delta merger
////////////////////////////////////////////////////////////

DeltaIteratorMerger::DeltaIteratorMerger(const vector<shared_ptr<DeltaIteratorInterface> > &iters) :
  iters_(iters)
{}

Status DeltaIteratorMerger::Init() {
  BOOST_FOREACH(const shared_ptr<DeltaIteratorInterface> &iter, iters_) {
    RETURN_NOT_OK(iter->Init());
  }
  return Status::OK();
}

Status DeltaIteratorMerger::SeekToOrdinal(rowid_t idx) {
  BOOST_FOREACH(const shared_ptr<DeltaIteratorInterface> &iter, iters_) {
    RETURN_NOT_OK(iter->SeekToOrdinal(idx));
  }
  return Status::OK();
}

Status DeltaIteratorMerger::PrepareBatch(size_t nrows) {
  BOOST_FOREACH(const shared_ptr<DeltaIteratorInterface> &iter, iters_) {
    RETURN_NOT_OK(iter->PrepareBatch(nrows));
  }
  return Status::OK();
}

Status DeltaIteratorMerger::ApplyUpdates(size_t col_to_apply, ColumnBlock *dst) {
  BOOST_FOREACH(const shared_ptr<DeltaIteratorInterface> &iter, iters_) {
    RETURN_NOT_OK(iter->ApplyUpdates(col_to_apply, dst));
  }
  return Status::OK();
}

string DeltaIteratorMerger::ToString() const {
  string ret;
  ret.append("DeltaIteratorMerger(");

  bool first = true;
  BOOST_FOREACH(const shared_ptr<DeltaIteratorInterface> &iter, iters_) {
    if (!first) {
      ret.append(", ");
    }
    first = false;

    ret.append(iter->ToString());
  }
  ret.append(")");
  return ret;
}


shared_ptr<DeltaIteratorInterface> DeltaIteratorMerger::Create(
  const vector<shared_ptr<DeltaTrackerInterface> > &trackers,
  const Schema &projection,
  const MvccSnapshot &snapshot)
{
  vector<shared_ptr<DeltaIteratorInterface> > delta_iters;

  BOOST_FOREACH(const shared_ptr<DeltaTrackerInterface> &tracker, trackers) {
    shared_ptr<DeltaIteratorInterface> iter(tracker->NewDeltaIterator(projection, snapshot));
    delta_iters.push_back(iter);
  }

  if (delta_iters.size() == 1) {
    // If we only have one input to the "merge", we can just directly
    // return that iterator.
    return delta_iters[0];
  }

  return shared_ptr<DeltaIteratorInterface>(new DeltaIteratorMerger(delta_iters));
}

} // namespace tablet
} // namespace kudu
