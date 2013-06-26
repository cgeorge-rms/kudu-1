// Copyright (c) 2013, Cloudera, inc.

#ifndef KUDU_TABLET_MUTATION_H
#define KUDU_TABLET_MUTATION_H

#include <string>

#include "common/row_changelist.h"
#include "common/schema.h"
#include "gutil/macros.h"
#include "gutil/port.h"
#include "util/memory/arena.h"
#include "util/slice.h"
#include "tablet/mvcc.h"

namespace kudu {
namespace tablet {

// A single mutation associated with a row.
// This object also acts as a node in a linked list connected to other
// mutations in the row.
//
// This is a variable-length object.
class Mutation {
 public:
  Mutation() { }

  // Create a new Mutation object with a copy of the given changelist.
  // The object is allocated from the provided Arena.
  template<class ArenaType>
  static Mutation *CreateInArena(
    ArenaType *arena, txid_t txid, const RowChangeList &rcl);

  RowChangeList changelist() const {
    return RowChangeList(Slice(changelist_data_, changelist_size_));
  }

  txid_t txid() const { return txid_; }
  const Mutation *next() const { return next_; }
  void set_next(Mutation *next) {
    next_ = next;
  }

  // Return a stringified version of the given list of mutations.
  // This should only be used for debugging/logging.
  static string StringifyMutationList(const Schema &schema, const Mutation *head);

  // Append this mutation to the list at the given pointer.
  void AppendToList(Mutation **list);

 private:
  friend class MSRow;
  friend class MemRowSet;

  DISALLOW_COPY_AND_ASSIGN(Mutation);

  // The transaction ID which made this mutation. If this transaction is not
  // committed in the snapshot of the reader, this mutation should be ignored.
  txid_t txid_;

  // Link to the next mutation on this row
  Mutation *next_;

  uint32_t changelist_size_;

  // The actual encoded RowChangeList
  char changelist_data_[0];
};

template<class ArenaType>
inline Mutation *Mutation::CreateInArena(
  ArenaType *arena, txid_t txid, const RowChangeList &rcl) {

  size_t size = sizeof(Mutation) + rcl.slice().size();
  void *storage = arena->AllocateBytesAligned(size, BASE_PORT_H_ALIGN_OF(Mutation));
  CHECK(storage) << "failed to allocate storage from arena";
  Mutation *ret = new(storage) Mutation();
  ret->txid_ = txid;
  ret->next_ = NULL;
  ret->changelist_size_ = rcl.slice().size();
  memcpy(ret->changelist_data_, rcl.slice().data(), rcl.slice().size());
  return ret;
}


} // namespace tablet
} // namespace kudu

#endif
