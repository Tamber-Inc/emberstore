#pragma once

// emberstore — transactional, Miro-backed on-disk storage.
//
//   Database      — a directory of typed stores, opened by name.
//   Document<T>   — one reflected value persisted as a whole JSON file.
//   Collection<T> — Firestore-style documents keyed by external string id.
//   Query<T>      — where / orderBy / limit over a collection.
//
// All write atomically (temp + rename, optionally fsync'd), so a crash never
// leaves a torn or empty file behind. Message-thread only.

#include "Collection.h"
#include "Database.h"
#include "Document.h"
#include "Query.h"
